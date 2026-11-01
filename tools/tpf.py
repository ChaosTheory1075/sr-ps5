#!/usr/bin/env python3
"""TexMod packages (.tpf): a zip archive, XOR-ed with 0x3FA43FA4 (per little-endian 32-bit word,
the tail bytes with its low byte), its entries ZipCrypto-encrypted with TexMod's fixed password.
texmod.def lists the textures: "0x<CRC32 of the PC texture's base level>|<file in the archive>".

    tpf.py <file.tpf> list            the def's entries, with each one's DDS header
    tpf.py <file.tpf> get <outdir>    every DDS, as <crc>.dds

As a module: Tpf(path) has .entries [{'crc', 'name', 'offset', 'csize', 'size', 'method', 'zcrc'}]
(offset: the encrypted data's, in the decoded archive) and .read(entry).
"""
import os
import struct
import sys
import zlib

XOR = 0x3FA43FA4
PASSWORD = bytes([0x73, 0x2A, 0x63, 0x7D, 0x5F, 0x0A, 0xA6, 0xBD, 0x7D, 0x65, 0x7E, 0x67, 0x61, 0x2A, 0x7F,
                  0x7F, 0x74, 0x61, 0x67, 0x5B, 0x60, 0x70, 0x45, 0x74, 0x5C, 0x22, 0x74, 0x5D, 0x6E, 0x6A,
                  0x73, 0x41, 0x77, 0x6E, 0x46, 0x47, 0x77, 0x49, 0x0C, 0x4B, 0x46, 0x6F])


def unxor(data):
    n = len(data) // 4
    import numpy as np
    words = np.frombuffer(data, dtype='<u4', count=n) ^ np.uint32(XOR)
    tail = bytes(b ^ (XOR & 0xFF) for b in data[n * 4:])
    return words.tobytes() + tail


CRC_TABLE = [0] * 256
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    CRC_TABLE[i] = c


class ZipCrypto:
    def __init__(self, pw):
        self.k = [0x12345678, 0x23456789, 0x34567890]
        for b in pw:
            self.update(b)

    def update(self, b):
        k = self.k
        k[0] = CRC_TABLE[(k[0] ^ b) & 0xFF] ^ (k[0] >> 8)
        k[1] = (k[1] + (k[0] & 0xFF)) & 0xFFFFFFFF
        k[1] = (k[1] * 134775813 + 1) & 0xFFFFFFFF
        k[2] = CRC_TABLE[(k[2] ^ (k[1] >> 24)) & 0xFF] ^ (k[2] >> 8)

    def decrypt(self, data):
        out = bytearray(len(data))
        for i, c in enumerate(data):
            t = (self.k[2] | 2) & 0xFFFF
            p = c ^ (((t * (t ^ 1)) >> 8) & 0xFF)
            self.update(p)
            out[i] = p
        return bytes(out)


class Tpf:
    def __init__(self, path):
        self.path = path
        self.data = unxor(open(path, 'rb').read())
        d = self.data
        # the central directory, from the end record
        eocd = d.rfind(b'PK\x05\x06')
        if eocd < 0:
            raise ValueError(f'{path}: no zip end record')
        n, cd_size, cd_off = struct.unpack_from('<HII', d, eocd + 10)
        files = {}
        p = cd_off
        for _ in range(n):
            (sig, _, _, flags, method, _, _, zcrc, csize, size, nlen, xlen, clen, _, _, _,
             lho) = struct.unpack_from('<IHHHHHHIIIHHHHHII', d, p)
            name = d[p + 46:p + 46 + nlen].decode('latin1')
            lnlen, lxlen = struct.unpack_from('<HH', d, lho + 26)
            files[name.lower()] = {'name': name, 'offset': lho + 30 + lnlen + lxlen, 'csize': csize, 'size': size,
                                   'method': method, 'zcrc': zcrc, 'flags': flags}
            p += 46 + nlen + xlen + clen
        self.files = files
        self.entries = []
        defe = files.get('texmod.def')
        if defe:
            for line in self.read(defe).decode('latin1').splitlines():
                if '|' not in line:
                    continue
                h, name = line.split('|', 1)
                name = name.strip().replace('\\', '/').lstrip('./')
                f = files.get(name.lower()) or next((v for k, v in files.items() if k.endswith(name.lower())), None)
                if f:
                    self.entries.append(dict(f, crc=int(h.strip(), 16) & 0xFFFFFFFF))

    def read(self, f):
        raw = self.data[f['offset']:f['offset'] + f['csize']]
        if f['flags'] & 1:
            raw = ZipCrypto(PASSWORD).decrypt(raw)[12:]
        if f['method'] == 8:
            return zlib.decompress(raw, -15)
        return raw


def dds_info(b):
    if b[:4] != b'DDS ':
        return 'not DDS'
    h, w, mips = struct.unpack_from('<III', b, 12)[0], struct.unpack_from('<I', b, 16)[0], struct.unpack_from('<I', b, 28)[0]
    fourcc = b[84:88]
    bits = struct.unpack_from('<I', b, 88)[0]
    return f'{w}x{h} mips {mips} {fourcc!r} {bits}bpp'


if __name__ == '__main__':
    t = Tpf(sys.argv[1])
    if sys.argv[2] == 'list':
        print(len(t.entries), 'textures,', len(t.files), 'files')
        for e in t.entries[:int(sys.argv[3]) if len(sys.argv) > 3 else 20]:
            print(f"{e['crc']:08X} {e['name']} {e['size']} {dds_info(t.read(e)[:128])}")
    elif sys.argv[2] == 'get':
        os.makedirs(sys.argv[3], exist_ok=True)
        for e in t.entries:
            open(os.path.join(sys.argv[3], f"{e['crc']:08X}.dds"), 'wb').write(t.read(e))
