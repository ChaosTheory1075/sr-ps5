"""Radical's RCF archives (ATG CORE CEMENT LIBRARY): entries with their names, unpacked."""
import re
import struct
import zlib


def entries(path):
    """[(name, offset, size)] in file order."""
    with open(path, 'rb') as f:
        h = f.read(0x3c)
        do, _, no, ns, _, n = struct.unpack('>6I', h[0x24:0x3c])
        f.seek(do)
        ents = sorted((struct.unpack('>3I', f.read(12)) for _ in range(n)), key=lambda e: e[1])
        f.seek(no)
        nt = f.read(ns)
    # (each name: its length with the NUL as a little-endian u32, then the name)
    names, p = [], 8
    while p + 4 <= len(nt):
        ln = struct.unpack('<I', nt[p:p + 4])[0]
        name = nt[p + 4:p + 3 + ln]
        if 4 < ln < 512 and nt[p + 3 + ln:p + 4 + ln] == b'\0' and all(32 <= c < 127 for c in name):
            names.append(name.decode('latin-1').replace(chr(92), '/'))
            p += 4 + ln
        else:
            p += 1
    if len(names) != len(ents):
        raise ValueError(f'{path}: {len(names)} names for {len(ents)} entries')
    return [(nm, o, s) for nm, (_, o, s) in zip(names, ents)]


def unpack(raw):
    """An entry's data, unpacked if it's an RZ or CFX one."""
    if raw[:2] == b'RZ':
        return zlib.decompressobj().decompress(raw[16:])
    if raw[:3] == b'CFX':
        return b'GFX' + raw[3:8] + zlib.decompressobj().decompress(raw[8:])
    return raw
