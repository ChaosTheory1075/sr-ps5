#!/usr/bin/env python3
"""The Xbox 360 textures in Saints Row's and Saints Row 2's archives, with what they'd be on PC.

Radical's Pure3D keeps a texture's image as chunk 0x19002 (big-endian): a payload of the 360's
52-byte D3D texture header (its fetch constant at +28) and the data as the GPU reads it: tiled, in
8-in-16 byte order. The same art went into the PC versions untiled, little-endian: so the 360 base
level, untiled and byte-swapped, is the PC one's, and TexMod's hash of it (CRC32 of the base
level) is the hash a PC texture pack names its replacement by.

As a module: textures(path) yields Texture records for every one in an archive (.rcf) or Pure3D
file; Texture.pc_crc() is the TexMod hash, Texture.guest() the base level as it is in guest memory.
"""
import os
import re
import struct
import zlib

import numpy as np

# fetch constant formats: (bytes per 4x4 block, or per texel when not a block format; block?)
FORMATS = {0x12: (8, True), 0x13: (16, True), 0x14: (16, True), 0x31: (16, True), 0x7B: (8, True),
           0x06: (4, False)}
NAMES = {0x12: 'DXT1', 0x13: 'DXT3', 0x14: 'DXT5', 0x31: 'DXN', 0x7B: 'DXT5A', 0x06: '8888'}


def tiled_offsets(bw, bh, pitch_blocks, log_bpb):
    """Xenia's TiledOffset2DInner for every (by, bx): byte offsets, shape (bh, bw)."""
    y, x = np.mgrid[0:bh, 0:bw].astype(np.int64)
    width = pitch_blocks
    macro = ((x >> 5) + (y >> 5) * (width >> 5)) << (log_bpb + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F))


class Texture:
    def __init__(self, payload, where):
        self.payload = payload
        self.where = where  # (archive, entry offset, payload offset in the entry)
        fetch = struct.unpack('>6I', payload[28:52])
        self.fetch = fetch
        self.tiled = fetch[0] >> 31
        self.pitch = ((fetch[0] >> 22) & 0x1ff) * 32  # texels
        self.fmt = fetch[1] & 0x3f
        self.endian = (fetch[1] >> 6) & 3
        self.w = (fetch[2] & 0x1fff) + 1
        self.h = ((fetch[2] >> 13) & 0x1fff) + 1
        self.data = payload[52:]

    def ok(self):
        return (self.fmt in FORMATS and 4 <= self.w <= 8192 and 4 <= self.h <= 8192 and self.pitch >= 32 and
                self.endian in (0, 1, 2))

    def _swapped(self, data):
        a = np.frombuffer(data[:len(data) // 4 * 4], np.uint8)
        if self.endian == 1:
            return a.reshape(-1, 2)[:, ::-1].reshape(-1)
        if self.endian == 2:
            return a.reshape(-1, 4)[:, ::-1].reshape(-1)
        return a

    def pc(self):
        """The base level as on PC (untiled, little-endian), or None if the data's too short."""
        bpb, block = FORMATS[self.fmt]
        if block:
            bw, bh, pb = (self.w + 3) // 4, (self.h + 3) // 4, self.pitch // 4
        else:
            bw, bh, pb = self.w, self.h, self.pitch
        log = bpb.bit_length() - 1
        d = self._swapped(self.data)
        if self.tiled:
            pbt = max((pb + 31) & ~31, 32)
            offs = tiled_offsets(bw, bh, pbt, log)
        else:
            y, x = np.mgrid[0:bh, 0:bw].astype(np.int64)
            offs = (y * pb + x) << log
        if offs.max() + bpb > len(d):
            return None
        idx = (offs.reshape(-1, 1) + np.arange(bpb)).reshape(-1)
        return d[idx].tobytes()

    def pc_crc(self):
        p = self.pc()
        return None if p is None else zlib.crc32(p) & 0xFFFFFFFF


def entries(path):
    """(entry offset, data) of an archive's entries (RZ-unpacked), or the file itself."""
    f = open(path, 'rb')
    h = f.read(0x3c)
    if not h.startswith(b'ATG CORE CEMENT LIBRARY'):
        f.seek(0)
        yield 0, f.read()
        return
    dir_off, _, _, _, _, count = struct.unpack('>6I', h[0x24:0x3c])
    f.seek(dir_off)
    ents = [struct.unpack('>3I', f.read(12)) for _ in range(count)]
    for _, off, size in sorted(ents, key=lambda e: e[1]):
        f.seek(off)
        d = f.read(size)
        if d[:2] == b'RZ':
            try:
                d = rz(d)
            except Exception:
                continue
        yield off, d


def rz(d):
    """RZ: "RZ", then blocks of zlib streams (one, mostly)."""
    out = []
    do = zlib.decompressobj()
    out.append(do.decompress(d[16:]))
    rest = do.unused_data
    while rest[:1] == b'\x78':
        do = zlib.decompressobj()
        out.append(do.decompress(rest))
        rest = do.unused_data
    return b''.join(out)


CHUNK = re.compile(rb'\x00\x01\x90\x02', re.S)


def textures(path):
    for eoff, d in entries(path):
        for m in CHUNK.finditer(d):
            q = m.start()
            if q + 16 > len(d):
                continue
            dsize, tsize, plen = struct.unpack('>III', d[q + 4:q + 16])
            if plen < 64 or q + 16 + plen > len(d) or tsize < plen or tsize > plen + 64:
                continue
            t = Texture(d[q + 16:q + 16 + plen], (path, eoff, q + 16))
            if t.ok():
                yield t
