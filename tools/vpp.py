#!/usr/bin/env python3
"""Volition packfiles (Saints Row's .vpp_xbox2, version 3, and Saints Row 2's, version 4): list or
extract.

    vpp.py <packfile> list
    vpp.py <packfile> extract <out dir> [name filter, e.g. .lua]

Big-endian on the Xbox 360. The header's first 0x800 bytes: the descriptor (0x51890ACE), the version,
then at 0x14C the flags (1: compressed with zlib; 2: condensed - all files one zlib stream), the file
count, the package size and the index, names and extensions sections' sizes, the data's
uncompressed and compressed sizes. The sections follow, each aligned to 2048 bytes: the index (28
bytes a file: name and extension offsets, -, data offset, size, compressed size, -), the names, the
extensions, the data. Version 3 has no extensions section (the names carry them): its sizes at
0x154 are the count, package, index and names sizes, then the data's uncompressed and compressed.
"""
import os
import struct
import sys
import zlib


def align(v, a=2048):
    return (v + a - 1) // a * a


def read(path):
    d = open(path, "rb").read()
    magic, version = struct.unpack_from(">II", d, 0)
    if magic != 0x51890ACE or version not in (3, 4):
        sys.exit(f"{path}: not a version 3 or 4 packfile ({magic:08X} v{version})")
    if version == 4:
        flags, _, count, _size, isize, nsize, esize, usize, csize = struct.unpack_from(">9I", d, 0x14C)
    else:
        flags, _, count, _size, isize, nsize, usize, csize = struct.unpack_from(">8I", d, 0x14C)
        esize = 0
    index = 0x800
    names = align(index + isize)
    exts = align(names + nsize)
    data = align(exts + esize) if esize else exts
    cstr = lambda o: d[o:d.index(b"\0", o)].decode("latin-1")
    files = []
    for i in range(count):
        no, eo, _, off, size, comp, _ = struct.unpack_from(">7I", d, index + i * 28)
        if version == 4:
            files.append((cstr(names + no) + "." + cstr(exts + eo), off, size, comp))
        else:
            files.append((cstr(names + no), struct.unpack_from(">I", d, index + i * 28 + 8)[0], size, comp))
    blob = None
    if flags & 2:  # condensed: one stream
        blob = zlib.decompressobj().decompress(d[data:data + csize]) if flags & 1 else d[data:data + usize]
    # (compressed, not condensed: each file's stream follows the last, aligned to 2048 bytes; the
    # index's offsets are the uncompressed ones)
    where = {}
    pos = data
    for f in files:
        where[f[0]] = pos
        pos = align(pos + (f[3] if flags & 1 else f[2]))

    def get(f):
        name, off, size, comp = f
        if blob is not None:
            return blob[off:off + size]
        if flags & 1:
            return zlib.decompressobj().decompress(d[where[name]:where[name] + comp])
        return d[data + off:data + off + size]
    return flags, files, get


def main():
    flags, files, get = read(sys.argv[1])
    if sys.argv[2] == "list":
        print(f"flags {flags:X}, {len(files)} files")
        for name, off, size, comp in files:
            print(f"{size:10} {name}")
        return
    out = sys.argv[3]
    want = sys.argv[4].lower() if len(sys.argv) > 4 else ""
    os.makedirs(out, exist_ok=True)
    n = 0
    for f in files:
        if want in f[0].lower():
            open(os.path.join(out, f[0]), "wb").write(get(f))
            n += 1
    print(f"{n} files to {out}")


if __name__ == "__main__":
    main()
