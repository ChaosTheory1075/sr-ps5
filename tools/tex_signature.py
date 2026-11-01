#!/usr/bin/env python3
"""Small colour thumbnails of DXT textures, from their blocks' endpoint colours: to tell which
Xbox 360 texture a PC pack's replacement is when their bytes differ (compressed apart).

    signature(blocks, bw, bh, bpb)  ->  (S, S, 3) float32, the blocks' mean colours over an S x S grid
"""
import struct

import numpy as np

S = 16


def _rgb565(c):
    c = c.astype(np.int32)
    return np.stack([(c >> 11) & 31, (c >> 5) & 63, c & 31], -1).astype(np.float32) * \
        np.array([255 / 31, 255 / 63, 255 / 31], np.float32)


def block_colours(data, bw, bh, bpb):
    """(bh, bw, 3): each 4x4 block's mean of its two endpoint colours."""
    a = np.frombuffer(data[:bw * bh * bpb], np.uint8).reshape(bh, bw, bpb)
    o = 0 if bpb == 8 else 8
    c0 = a[:, :, o].astype(np.uint16) | (a[:, :, o + 1].astype(np.uint16) << 8)
    c1 = a[:, :, o + 2].astype(np.uint16) | (a[:, :, o + 3].astype(np.uint16) << 8)
    return (_rgb565(c0) + _rgb565(c1)) * 0.5


def shrink(img):
    """Area-average an (h, w, 3) grid to S x S."""
    h, w = img.shape[:2]
    ys = (np.arange(S + 1) * h) // S
    xs = (np.arange(S + 1) * w) // S
    out = np.zeros((S, S, 3), np.float32)
    for i in range(S):
        for j in range(S):
            blk = img[ys[i]:max(ys[i + 1], ys[i] + 1), xs[j]:max(xs[j + 1], xs[j] + 1)]
            out[i, j] = blk.reshape(-1, 3).mean(0)
    return out


def signature(data, bw, bh, bpb):
    return shrink(block_colours(data, bw, bh, bpb))


def dds_signature(b):
    """(signature, width, height) of a DXT1/DXT5 DDS, from a mip of 64 blocks across or so."""
    if b[:4] != b'DDS ':
        return None
    h, w = struct.unpack_from('<II', b, 12)
    mips = max(1, struct.unpack_from('<I', b, 28)[0])
    fourcc = b[84:88]
    bpb = {b'DXT1': 8, b'DXT3': 16, b'DXT5': 16}.get(fourcc)
    if not bpb:
        return None
    off = 128
    lw, lh = w, h
    for level in range(mips):
        bw, bh = max(1, (lw + 3) // 4), max(1, (lh + 3) // 4)
        size = bw * bh * bpb
        if max(bw, bh) <= 64 or level == mips - 1:
            return signature(b[off:off + size], bw, bh, bpb), w, h
        off += size
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    return None
