#!/usr/bin/env python3
"""Contact sheets of sr_texture_manifest.py's picture matches, to check them by eye:
each 360 texture beside the PC pack's replacement it was given, with their distance.

    texture_match_sheet.py <matches.txt> <out prefix> [per sheet]
"""
import collections
import io
import os
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tpf  # noqa: E402
import x360_textures as xt  # noqa: E402

FOURCC = {0x12: b'DXT1', 0x13: b'DXT3', 0x14: b'DXT5'}


def dds(w, h, fourcc, data):
    hd = struct.pack('<4s7I44x2I4s5I', b'DDS ', 124, 0x81007, h, w, len(data), 0, 1, 32, 4, fourcc, 0, 0, 0, 0, 0)
    hd += struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    return Image.open(io.BytesIO(hd + data)).convert('RGB')


def main():
    rows = [l.rstrip('\n').split('|') for l in open(sys.argv[1]) if l.strip()]
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 48
    by_entry = collections.defaultdict(list)
    for i, r in enumerate(rows):
        by_entry[(r[3], int(r[4], 16))].append(i)
    left = {}
    for (path, eoff), items in by_entry.items():
        want = {int(rows[i][5], 16): i for i in items}
        for t in xt.textures(path):
            if t.where[1] == eoff and t.where[2] in want:
                try:
                    left[want[t.where[2]]] = dds(t.w, t.h, FOURCC[t.fmt], t.pc())
                except Exception:
                    pass
    packs = {}
    cell = 160
    for s in range(0, len(rows), per):
        chunk = list(range(s, min(s + per, len(rows))))
        cols = 6
        sheet = Image.new('RGB', (cols * (2 * cell + 16), ((len(chunk) + cols - 1) // cols) * (cell + 20)), (40, 40, 40))
        d = ImageDraw.Draw(sheet)
        for n, i in enumerate(chunk):
            r = rows[i]
            if r[0] not in packs:
                packs[r[0]] = tpf.Tpf(r[0])
            t = packs[r[0]]
            e = next(e for e in t.entries if e['crc'] == int(r[1], 16))
            try:
                rep = Image.open(io.BytesIO(t.read(e))).convert('RGB')
            except Exception:
                rep = Image.new('RGB', (8, 8), (255, 0, 255))
            x, y = (n % cols) * (2 * cell + 16), (n // cols) * (cell + 20)
            for k, im in enumerate((left.get(i), rep)):
                if im is None:
                    continue
                im = im.copy()
                im.thumbnail((cell, cell))
                sheet.paste(im, (x + k * cell, y + 18))
            d.text((x, y + 2), f'{os.path.basename(r[0])} {r[1]} d={r[2]}', fill=(255, 255, 0))
        sheet.save(f'{sys.argv[2]}{s // per:02d}.png')
        print(f'{sys.argv[2]}{s // per:02d}.png', len(chunk))


if __name__ == '__main__':
    main()
