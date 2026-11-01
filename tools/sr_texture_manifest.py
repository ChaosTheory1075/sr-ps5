#!/usr/bin/env python3
"""The PC HD texture packs as a list, for the PS5 app to install them from the user's own files.

    sr_texture_manifest.py --game 1 --x360 <Saints Row's files> --pack <pack folder> [--pack ...]
                              --game 2 --x360 <Saints Row 2's files> --pack <pack folder> ...
                              --out package/sr-textures.txt

The packs are TexMod packages (.tpf, tools/tpf.py): DDS files named by TexMod's hash of the PC
texture they replace (uMod's CRC32 of its base level, without the final inversion). The PC
textures are the Xbox 360's, untiled and little-endian (tools/x360_textures.py), so every 360
texture's hash says which replacement is its. The app's HD texture replacer
(ci/fw-files/src/graphics/vulkan/hd_textures.cpp) finds a replacement by the XXH3-64 of the
texture's base level as the game loads it into memory - its Pure3D payload - seeded with its format,
size and tiling: the list gives that, and where the replacement's bytes are in the user's package,
so the app writes <xxh3>.dds from it on the console.

Not every PC texture was compressed as the 360's (none of Saints Row's, about half of Saints Row
2's): those replacements are matched by picture instead - a 16x16 thumbnail of each from its DXT
blocks' colours (tools/tex_signature.py), against the 360 textures of the same shape: the closest
one if it's clearly closer than any different one. The list holds no game or pack data:

    P <pack> <game> <size> <file name>        a .tpf (found in install/ by name and size)
    T <pack> <offset> <stored> <size> <method> <zip crc32> <xxh3>
                                              a texture: the zip entry's encrypted bytes at
                                              <offset> (in the file), <stored> long; deflated if
                                              <method> is 8; <size> and <zip crc32> once unpacked

All numbers but <game> and <method> are hex. A later package's replacement of the same texture
wins, as in TexMod's list.
"""
import argparse
import collections
import glob
import os
import sys

import numpy as np
import xxhash
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tex_signature as ts  # noqa: E402
import tpf  # noqa: E402
import x360_textures as xt  # noqa: E402


# picture matching: the mean colour difference (0..255) of the closest 360 texture at most
# MAX_DISTANCE, within SAME of it the same picture, and any other one at least RATIO times as far
# (a retexture shifts the colours, so a right match can be a few points off; a wrong one is never
# alone: other textures of the same kind are about as close - checked on contact sheets,
# tools/texture_match_sheet.py)
MAX_DISTANCE, SAME, RATIO = 12.0, 0.5, 1.45


def tile_offset(x, y, pitch, log):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F))


def guest_size(t):
    """Xenia's GetGuestTextureLayout base.level_data_extent_bytes (2D, one slice, base not packed)."""
    bpb, block = xt.FORMATS[t.fmt]
    bw = 4 if block else 1
    x_ext, y_ext = (t.w + bw - 1) // bw, (t.h + bw - 1) // bw
    pitch_blocks = (((t.pitch + bw - 1) // bw) + 31) & ~31
    log = bpb.bit_length() - 1
    if t.tiled:
        up = tile_offset((x_ext - 1) & ~31, (y_ext - 1) & ~31, pitch_blocks, log)
        return up + (0xA00 if log == 0 else 0xC00 if log == 1 else 0x400 << log)
    return pitch_blocks * bpb * (y_ext - 1) + bpb * x_ext


def runtime_key(t):
    """hd_textures.cpp's hash of the texture as loaded, or None if it can't be told here."""
    # (the base is in the packed mip tail only at 16 texels and under; 2D, one slice, only)
    if min(t.w, t.h) <= 16 or (t.fetch[5] >> 9) & 3 != 1 or t.fetch[2] >> 26:
        return None
    size = guest_size(t)
    if size > len(t.data):
        return None
    seed = t.fmt | t.w << 8 | t.h << 24 | t.tiled << 40
    return xxhash.xxh3_64_intdigest(t.data[:size], seed=seed)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', type=int, action='append', required=True)
    ap.add_argument('--x360', action='append', required=True)
    ap.add_argument('--pack', action='append', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--matches', help='also list the picture matches (to check them): tpf|crc|distance|where')
    a = ap.parse_args()
    lines = [
        '# SAINTS ROW: ReInstated HD textures: the PC texture packs\' replacements for the Xbox 360 textures',
        '# (tools/sr_texture_manifest.py). P <pack> <game> <size> <file>; T <pack> <offset> <stored> <size>',
        '# <method> <zip crc32> <xxh3>.']
    pack_id = 0
    matches = []
    for game, x360, packs in zip(a.game, a.x360, a.pack):
        # every pack's textures by TexMod hash (the later package's winning)
        wanted = {}
        tpfs = []
        tpfs_by_id = {}
        for folder in packs:
            for path in sorted(glob.glob(os.path.join(folder, '**', '*.tpf'), recursive=True)):
                t = tpf.Tpf(path)
                tpfs.append((pack_id, path, t))
                tpfs_by_id[pack_id] = t
                lines.append(f'P {pack_id:x} {game} {os.path.getsize(path):x} {os.path.basename(path)}')
                for e in t.entries:
                    wanted[e['crc']] = (pack_id, e)
                pack_id += 1
        # the 360 textures, each one's TexMod hash and runtime key
        keys = collections.defaultdict(set)
        seen = 0
        skipped = 0
        sigs, sig_keys, aspects, sig_where = [], [], [], []  # (the 360 textures by picture)
        files = sorted(p for p in glob.glob(os.path.join(x360, '**', '*'), recursive=True)
                       if os.path.isfile(p) and p.lower().endswith(('.rcf', '.p3d'))
                       and 'audio' not in os.path.basename(p).lower() and 'movies' not in os.path.basename(p).lower())
        for path in files:
            for t in xt.textures(path):
                seen += 1
                p = t.pc()
                if p is None:
                    continue
                c = zlib.crc32(p) ^ 0xFFFFFFFF
                if c not in wanted:
                    if t.fmt in (0x12, 0x13, 0x14) and min(t.w, t.h) >= 32:
                        k = runtime_key(t)
                        if k is not None:
                            sigs.append(ts.signature(p, (t.w + 3) // 4, (t.h + 3) // 4, xt.FORMATS[t.fmt][0]))
                            sig_keys.append(k)
                            aspects.append(t.w / t.h)
                            sig_where.append(t.where)
                    continue
                k = runtime_key(t)
                if k is None:
                    skipped += 1
                    continue
                keys[c].add(k)
            print(f'game {game}: {os.path.basename(path)}: {seen} textures, {len(keys)} of {len(wanted)} replacements '
                  f'matched', file=sys.stderr, flush=True)
        # the rest, by picture
        by_crc = len(keys)
        if sigs:
            sig = np.stack(sigs)
            asp = np.array(aspects)
            for c, (pid, e) in wanted.items():
                if c in keys:
                    continue
                r = ts.dds_signature(tpfs_by_id[pid].read(e))
                if not r:
                    continue
                s0, w, h = r
                idx = np.nonzero(np.abs(asp - w / h) < 1e-3)[0]
                if not len(idx):
                    continue
                d = np.abs(sig[idx] - s0).mean((1, 2, 3))
                order = np.argsort(d)
                best = d[order[0]]
                if best > MAX_DISTANCE:
                    continue
                # (the same picture several times over: all of them; another one nearly as close: none)
                same = [i for i in order[:8] if d[i] <= best + SAME]
                other = [i for i in order if d[i] > best + SAME]
                if other and d[other[0]] < best * RATIO + SAME:
                    continue
                for i in same:
                    keys[c].add(sig_keys[idx[i]])
                    w0 = sig_where[idx[i]]
                    matches.append(f"{tpfs_by_id[pid].path}|{c:08X}|{d[i]:.2f}|{w0[0]}|{w0[1]:x}|{w0[2]:x}")
        written = 0
        for c, ks in keys.items():
            pid, e = wanted[c]
            for k in sorted(ks):
                lines.append(f"T {pid:x} {e['offset']:x} {e['csize']:x} {e['size']:x} {e['method']} {e['zcrc']:x} "
                             f"{k:016x}")
                written += 1
        unmatched = collections.Counter(os.path.basename(p) for pid, p, t in tpfs for e in t.entries
                                        if e['crc'] not in keys)
        print(f'game {game}: {len(keys)} of {len(wanted)} replacements ({by_crc} by hash, {len(keys) - by_crc} by '
              f'picture) for {written} textures ({skipped} skipped: '
              f'base in the mip tail or not 2D); unmatched by package: {dict(unmatched)}', file=sys.stderr)
    open(a.out, 'w', newline='\n').write('\n'.join(lines) + '\n')
    if a.matches:
        open(a.matches, 'w').write('\n'.join(matches) + '\n')


if __name__ == '__main__':
    main()
