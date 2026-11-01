#!/usr/bin/env python3
"""A frame the desktop build dumped (SR_DUMP_DIR: frame_N.raw + frame_N.txt) as a PNG.

    sr_frame.py <frame_N.raw> [out.png]

The dump is the frontbuffer's guest memory: 32-bit pixels (k_8_8_8_8 / k_2_10_10_10), tiled the
way the Xbox 360's GPU keeps textures (rex::graphics::texture_util::GetTiledOffset2D), big-endian.
"""
import sys

import numpy as np
from PIL import Image


def tiled_offset_2d(x, y, pitch, bpb_log2):
    # (numpy arrays of x and y; rex texture_util GetTiledOffset2D)
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F))


def main():
    raw_path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else raw_path[:-4] + ".png"
    width, height, pitch, fmt = (int(v) for v in open(raw_path[:-4] + ".txt").read().split())
    data = np.frombuffer(open(raw_path, "rb").read(), dtype=np.uint8)
    ys, xs = np.mgrid[0:height, 0:width]
    offsets = tiled_offset_2d(xs.astype(np.int64), ys.astype(np.int64), pitch, 2)
    offsets = np.clip(offsets, 0, len(data) - 4)
    px = np.stack([data[offsets + i] for i in range(4)], axis=-1)
    if fmt == 6:  # k_8_8_8_8, stored big-endian as A R G B? (the 360 swaps: B G R A in memory order 8in32)
        rgb = px[..., [1, 2, 3]] if False else px[..., [2, 1, 0]]
    else:  # k_2_10_10_10 and others: the top 8 bits of each 10-bit channel
        v = (px[..., 0].astype(np.uint32) << 24 | px[..., 1].astype(np.uint32) << 16 |
             px[..., 2].astype(np.uint32) << 8 | px[..., 3].astype(np.uint32))
        rgb = np.stack([(v >> 2) & 0xFF, (v >> 12) & 0xFF, (v >> 22) & 0xFF], axis=-1).astype(np.uint8)
    Image.fromarray(rgb.astype(np.uint8)).save(out)
    print(out, width, height, fmt)


if __name__ == "__main__":
    main()
