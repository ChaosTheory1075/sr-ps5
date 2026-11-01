#!/usr/bin/env python3
# x360tex.py: Xbox 360 texture payloads (52-byte D3D header + data) <-> RGBA images.
#   x360tex.py decode <payload.bin>... <outdir>   (PNG per payload, plus sheet.png)
# As a module: read_header, decode, encode (DXT5 only, the button glyphs' format).
import os, struct, sys
import numpy as np


def read_header(p):
    fetch = struct.unpack('>6I', p[28:52])
    tiled = fetch[0] >> 31
    pitch = ((fetch[0] >> 22) & 0x1ff) * 32
    fmt = fetch[1] & 0x3f
    endian = (fetch[1] >> 6) & 3
    w = (fetch[2] & 0x1fff) + 1
    h = ((fetch[2] >> 13) & 0x1fff) + 1
    return dict(tiled=tiled, pitch=pitch, fmt=fmt, endian=endian, w=w, h=h)


def tiled_offset(x, y, width, log_bpp):  # Xenia's TiledOffset2DInner: blocks -> bytes
    macro = ((x >> 5) + (y >> 5) * (width >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpp
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F))


def block_offsets(hd):
    """Byte offset of each 4x4 block (by, bx) in the data."""
    bw, bh = (hd['w'] + 3) // 4, (hd['h'] + 3) // 4
    pb = max(hd['pitch'], 32) // 4  # the pitch, in blocks
    out = {}
    for by in range(bh):
        for bx in range(bw):
            if hd['tiled']:
                out[(by, bx)] = tiled_offset(bx, by, max(pb, 32), 4)
            else:
                out[(by, bx)] = (by * pb + bx) * 16
    return out


def swap16(b):
    a = bytearray(b)
    a[0::2], a[1::2] = b[1::2], b[0::2]
    return bytes(a)


def rgb565(c):
    return np.array([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], np.int32)


def dxt5_block(b):
    a0, a1 = b[0], b[1]
    abits = int.from_bytes(b[2:8], 'little')
    al = [a0, a1] + ([(6 - i) * a0 // 7 + (1 + i) * a1 // 7 for i in range(6)] if a0 > a1 else
                     [(4 - i) * a0 // 5 + (1 + i) * a1 // 5 for i in range(4)] + [0, 255])
    c0, c1 = struct.unpack('<HH', b[8:12])
    cbits = int.from_bytes(b[12:16], 'little')
    p0, p1 = rgb565(c0), rgb565(c1)
    pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
    px = np.zeros((4, 4, 4), np.uint8)
    for i in range(16):
        px[i // 4, i % 4, :3] = pal[(cbits >> (2 * i)) & 3]
        px[i // 4, i % 4, 3] = al[(abits >> (3 * i)) & 7]
    return px


def decode(p):
    hd = read_header(p)
    assert hd['fmt'] == 0x14, 'not DXT5: %x' % hd['fmt']
    data = p[52:]
    if hd['endian'] == 1:
        data = swap16(data)
    img = np.zeros(((hd['h'] + 3) // 4 * 4, (hd['w'] + 3) // 4 * 4, 4), np.uint8)
    for (by, bx), o in block_offsets(hd).items():
        img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = dxt5_block(data[o:o + 16])
    return img[:hd['h'], :hd['w']], hd


def encode_block(px):
    """A 4x4 RGBA block as DXT5 (range fit: good enough for flat glyph art)."""
    a = px[:, :, 3].reshape(16).astype(np.int32)
    a0, a1 = int(a.max()), int(a.min())
    if a0 == a1:
        abits = 0
    else:
        al = [a0, a1] + [(6 - i) * a0 // 7 + (1 + i) * a1 // 7 for i in range(6)]
        abits = 0
        for i in range(16):
            abits |= int(np.argmin([abs(int(a[i]) - v) for v in al])) << (3 * i)
    rgb = px[:, :, :3].reshape(16, 3).astype(np.int32)
    w = a > 8
    pts = rgb[w] if w.any() else rgb
    lum = pts @ np.array([2, 4, 1])
    hi, lo = pts[lum.argmax()], pts[lum.argmin()]
    q = lambda c: ((int(c[0]) * 31 + 127) // 255 << 11) | ((int(c[1]) * 63 + 127) // 255 << 5) | ((int(c[2]) * 31 + 127) // 255)
    c0, c1 = q(hi), q(lo)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        cbits = 0
    else:
        p0, p1 = rgb565(c0), rgb565(c1)
        pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
        cbits = 0
        for i in range(16):
            cbits |= int(np.argmin([int(((rgb[i] - c) ** 2).sum()) for c in pal])) << (2 * i)
    return bytes([a0, a1]) + abits.to_bytes(6, 'little') + struct.pack('<HH', c0, c1) + cbits.to_bytes(4, 'little')


def encode(img, like):
    """`img` (h, w, 4) into a payload laid out as `like` (same header, tiling and size)."""
    hd = read_header(like)
    data = bytearray(swap16(like[52:]) if hd['endian'] == 1 else like[52:])
    for (by, bx), o in block_offsets(hd).items():
        blk = np.zeros((4, 4, 4), np.uint8)
        part = img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4]
        blk[:part.shape[0], :part.shape[1]] = part
        data[o:o + 16] = encode_block(blk)
    data = bytes(data)
    return like[:52] + (swap16(data) if hd['endian'] == 1 else data)


if __name__ == '__main__' and sys.argv[1] == 'decode':
    from PIL import Image
    out = sys.argv[-1]
    os.makedirs(out, exist_ok=True)
    tiles = []
    for path in sys.argv[2:-1]:
        img, hd = decode(open(path, 'rb').read())
        name = os.path.basename(path).split('.')[0]
        Image.fromarray(img, 'RGBA').save(os.path.join(out, name + '.png'))
        tiles.append((name, img))
    # a sheet on grey, each scaled x2 with its name
    from PIL import ImageDraw
    cw, ch = 280, 300
    cols = 6
    sheet = Image.new('RGB', (cw * cols, ch * ((len(tiles) + cols - 1) // cols)), (70, 70, 70))
    d = ImageDraw.Draw(sheet)
    for i, (name, img) in enumerate(tiles):
        im = Image.fromarray(img, 'RGBA')
        im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
        x, y = (i % cols) * cw, (i // cols) * ch
        sheet.paste(im, (x + 8, y + 24), im)
        d.text((x + 8, y + 4), name, fill=(255, 255, 0))
    sheet.save(os.path.join(out, 'sheet.png'))
