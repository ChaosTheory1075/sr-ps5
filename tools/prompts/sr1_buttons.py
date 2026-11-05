#!/usr/bin/env python3
"""Saints Row's PlayStation button pictures, for the texture hook (ps5/sr_prompts.cpp).

    sr1_buttons.py <pegs dir> <out: prompts1.bin> [preview.png]

<pegs dir> holds px_base, interface-backend and interface-ld_ctrl (.peg_xbox2, from misc.vpp_xbox2 /
pegfiles). Saints Row draws its button prompts from these (the font's button characters are blank
placeholders the text code puts these pictures over):
- px_base: px_btna/b/x/y (the help text's buttons), px_btnselect
- interface-backend: hud_sheet_02 (y_button), hud_sheet_10 (the combo buttons), hud_sheet_15 (a/b/x
  buttons, LB/RB/LT/RT, the sticks, BACK/START)
- interface-ld_ctrl: the controller screen (its face buttons, guide button and callouts)
The rectangles are bitmap_sheets.xtbl's.

The peg ("VKEG", version 8): at 0x10 the count; from 0x18, 0x48-byte entries: the data's offset (from
the file's start), width, height, format (0x191 DXT3, 0x192 DXT5; 16-bit swapped, tiled as the Xbox
360's GPU reads them), the name at +0x17. The game loads a texture's data as it is in the file, so its
base level is the fingerprint: prompts1.bin ("SRTX", count, then {FNV-1a 64 of the base level, its
size, offset of the replacement}) holds the patched base level. Blocks the pictures don't touch keep
their bytes.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(__file__))
from x360tex import block_offsets, dxt5_block, encode_block, rgb565, swap16  # noqa: E402

FONT = "C:/Windows/Fonts/arialbd.ttf"


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def dxt3_block(b):
    abits = int.from_bytes(b[0:8], 'little')
    c0, c1 = struct.unpack('<HH', b[8:12])
    cbits = int.from_bytes(b[12:16], 'little')
    p0, p1 = rgb565(c0), rgb565(c1)
    pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
    px = np.zeros((4, 4, 4), np.uint8)
    for i in range(16):
        px[i // 4, i % 4, :3] = pal[(cbits >> (2 * i)) & 3]
        px[i // 4, i % 4, 3] = ((abits >> (4 * i)) & 15) * 17
    return px


def encode_dxt3(px):
    a = px[:, :, 3].reshape(16).astype(np.int32)
    abits = 0
    for i in range(16):
        abits |= ((int(a[i]) + 8) // 17) << (4 * i)
    return abits.to_bytes(8, 'little') + encode_block(px)[8:16]


def base_size(w, h):
    """The base level's size as the GPU's texture cache takes it: tiled, 32x32 blocks at a time."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    return ((bw + 31) // 32 * 32) * ((bh + 31) // 32 * 32) * 16


def entries(d):
    n = struct.unpack_from(">H", d, 0x10)[0]
    out = {}
    for i in range(n):
        e = 0x18 + i * 0x48
        off, w, h, fmt = struct.unpack_from(">IHHH", d, e)
        name = d[e + 0x17:e + 0x17 + 40].split(b"\0")[0].decode("latin-1")
        out[name] = dict(off=off, w=w, h=h, fmt=fmt)
    return out


class Texture:
    def __init__(self, d, e):
        self.e = e
        self.size = base_size(e['w'], e['h'])
        self.orig = d[e['off']:e['off'] + self.size]
        self.data = bytearray(swap16(self.orig))
        self.offs = block_offsets(dict(tiled=1, pitch=e['w'], w=e['w'], h=e['h']))
        dec = dxt3_block if e['fmt'] == 0x191 else dxt5_block
        img = np.zeros(((e['h'] + 3) // 4 * 4, (e['w'] + 3) // 4 * 4, 4), np.uint8)
        for (by, bx), o in self.offs.items():
            img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = dec(bytes(self.data[o:o + 16]))
        self.img = Image.fromarray(img, "RGBA")
        self.dirty = []

    def paste(self, rect, picture, clear=True):
        x, y, w, h = rect
        if clear:
            self.img.paste(Image.new("RGBA", (w, h), (0, 0, 0, 0)), (x, y))
        self.img.alpha_composite(picture.resize((w, h), Image.LANCZOS), (x, y))
        self.dirty.append(rect)

    def build(self):
        arr = np.array(self.img)
        enc = encode_dxt3 if self.e['fmt'] == 0x191 else encode_block
        for (by, bx), o in self.offs.items():
            if not any(x - 4 < bx * 4 + 4 and bx * 4 < x + w + 4 and y - 4 < by * 4 + 4 and by * 4 < y + h + 4
                       for x, y, w, h in self.dirty):
                continue
            blk = np.zeros((4, 4, 4), np.uint8)
            part = arr[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4]
            blk[:part.shape[0], :part.shape[1]] = part
            self.data[o:o + 16] = enc(blk)
        return swap16(bytes(self.data))


# ---- the pictures ----
S = 8  # (drawn large, scaled down: smooth edges)
FACE = {"cross": (124, 178, 232), "circle": (255, 102, 102), "square": (214, 145, 224), "triangle": (64, 226, 160)}


def face(kind, size=64, disabled=False):
    img = Image.new("RGBA", (size * S, size * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c, r = size * S / 2, size * S / 2 - S * 1.5
    d.ellipse([c - r, c - r, c + r, c + r], fill=(22, 22, 26, 255), outline=(235, 235, 235, 255), width=int(S * 2.2))
    colour = (120, 120, 120) if disabled else FACE[kind]
    s, lw = r * 0.48, int(S * size / 14)
    if kind == "cross":
        d.line([c - s, c - s, c + s, c + s], fill=colour, width=lw)
        d.line([c - s, c + s, c + s, c - s], fill=colour, width=lw)
    elif kind == "circle":
        d.ellipse([c - s, c - s, c + s, c + s], outline=colour, width=lw)
    elif kind == "square":
        d.rectangle([c - s * 0.9, c - s * 0.9, c + s * 0.9, c + s * 0.9], outline=colour, width=lw)
    else:
        t = s * 1.05
        d.polygon([(c, c - t), (c + t * 0.95, c + t * 0.65), (c - t * 0.95, c + t * 0.65)], outline=colour, width=lw)
    return img


def label(text, w, h, shape="pill", disabled=False):
    img = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = S * 2
    box = [pad, pad, w * S - pad, h * S - pad]
    if shape == "circle":
        side = min(w, h) * S - 2 * pad
        x0, y0 = (w * S - side) / 2, (h * S - side) / 2
        box = [x0, y0, x0 + side, y0 + side]
        d.ellipse(box, fill=(22, 22, 26, 255), outline=(235, 235, 235, 255), width=int(S * 2))
    else:
        d.rounded_rectangle(box, radius=(box[3] - box[1]) // 3, fill=(22, 22, 26, 255),
                            outline=(235, 235, 235, 255), width=int(S * 2))
    bw, bh = box[2] - box[0], box[3] - box[1]
    size = int(bh * 0.62)
    font = ImageFont.truetype(FONT, size)
    while size > 8 and d.textbbox((0, 0), text, font=font)[2] > bw * 0.84:
        size -= 2
        font = ImageFont.truetype(FONT, size)
    l, t, r, b = d.textbbox((0, 0), text, font=font)
    colour = (130, 130, 130, 255) if disabled else (240, 240, 240, 255)
    d.text((box[0] + (bw - (r - l)) / 2 - l, box[1] + (bh - (b - t)) / 2 - t), text, font=font, fill=colour)
    return img


def two(a, b, w, h):
    img = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    half = w // 2
    img.alpha_composite(label(a, half, h).resize((half * S, h * S)), (0, 0))
    img.alpha_composite(label(b, w - half, h).resize(((w - half) * S, h * S)), (half * S, 0))
    return img


def main():
    pegs, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[3] if len(sys.argv) > 3 else None
    load = lambda n: open(os.path.join(pegs, n + ".peg_xbox2"), "rb").read()
    textures = []

    # px_base: one picture a texture (the whole texture is the picture)
    d = load("px_base")
    es = entries(d)
    for name, pic in (("px_btna.tga", lambda w, h: face("cross")), ("px_btnb.tga", lambda w, h: face("circle")),
                      ("px_btnx.tga", lambda w, h: face("square")), ("px_btny.tga", lambda w, h: face("triangle")),
                      ("px_btnselect.tga", lambda w, h: label("TOUCH", w, h))):
        e = es[name]
        t = Texture(d, e)
        t.paste((0, 0, e['w'], e['h']), pic(e['w'], e['h']))
        textures.append((name, t))

    # the HUD sheets (bitmap_sheets.xtbl's rectangles)
    d = load("interface-backend")
    es = entries(d)
    sheets = {
        "hud_sheet_02.tga": [((472, 385, 34, 36), face("triangle")), ((476, 421, 34, 36), face("triangle", disabled=True))],
        "hud_sheet_10.tga": [((100, 101, 60, 60), face("cross")), ((160, 101, 60, 60), face("circle")),
                             ((100, 161, 60, 60), face("square")), ((160, 161, 60, 60), face("triangle")),
                             ((432, 101, 64, 64), label("L3", 64, 64, "circle")), ((214, 323, 24, 24), face("square"))],
        "hud_sheet_15.tga": [((64, 451, 34, 36), face("cross")), ((98, 451, 34, 36), face("cross", disabled=True)),
                             ((132, 451, 34, 36), face("circle")), ((166, 453, 34, 36), face("circle", disabled=True)),
                             ((200, 453, 34, 36), face("square")), ((234, 453, 34, 36), face("square", disabled=True)),
                             ((0, 436, 64, 36), two("L2", "L1", 64, 36)), ((0, 475, 64, 36), two("R2", "R1", 64, 36)),
                             ((0, 383, 45, 45), label("L3", 45, 45, "circle")), ((45, 383, 45, 45), label("R3", 45, 45, "circle")),
                             ((90, 383, 44, 34), label("L1", 44, 34)), ((134, 383, 44, 34), label("R1", 44, 34)),
                             ((90, 417, 44, 34), label("L2", 44, 34)), ((134, 417, 44, 34), label("R2", 44, 34)),
                             ((178, 418, 35, 35), label("OPT", 35, 35, "circle")),
                             ((178, 383, 35, 35), label("PAD", 35, 35, "circle"))],
    }
    for name, parts in sheets.items():
        t = Texture(d, es[name])
        for rect, pic in parts:
            t.paste(rect, pic)
        textures.append((name, t))

    # the controller screen: the callouts, and the photo's face and guide buttons
    d = load("interface-ld_ctrl")
    e = entries(d)["ld_ctrl.tga"]
    t = Texture(d, e)
    for rect, pic in (((323, 0, 111, 58), label("L1", 111, 58)), ((323, 58, 109, 57), label("R1", 109, 57)),
                      ((434, 0, 44, 37), label("OPT", 44, 37)), ((434, 37, 44, 38), label("PAD", 44, 38)),
                      ((323, 115, 55, 50), face("square")), ((378, 115, 53, 47), face("cross")),
                      ((323, 165, 55, 51), face("triangle")), ((378, 162, 51, 49), face("circle")),
                      ((432, 75, 52, 61), label("R2", 52, 61)), ((432, 138, 51, 63), label("L2", 51, 63))):
        t.paste(rect, pic)
    for rect, pic in (((248, 66, 24, 24), face("triangle")), ((222, 89, 24, 24), face("square")),
                      ((272, 89, 24, 24), face("circle")), ((246, 110, 24, 24), face("cross")),
                      ((145, 86, 30, 30), label("PS", 30, 30, "circle"))):
        t.paste(rect, pic, clear=False)  # (on the photo)
    textures.append(("ld_ctrl.tga", t))

    pieces, sheet = [], []
    for name, t in textures:
        rep = t.build()
        assert len(rep) == len(t.orig) == t.size, name
        pieces.append((fnv1a64(t.orig), t.size, rep))
        sheet.append(t.img)
        print(f"{name}: {t.e['w']}x{t.e['h']} fmt {t.e['fmt']:x}, base {t.size:#x}, fnv {fnv1a64(t.orig):016X}")
    blob = bytearray(b"SRTX" + struct.pack("<I", len(pieces)))
    at = 8 + 16 * len(pieces)
    data = b""
    for h, size, rep in pieces:
        blob += struct.pack("<QII", h, size, at + len(data))
        data += rep
    open(out, "wb").write(bytes(blob) + data)
    if preview:
        W = sum(i.width for i in sheet) + 8 * len(sheet)
        img = Image.new("RGBA", (W, max(i.height for i in sheet)), (60, 40, 70, 255))
        x = 0
        for i in sheet:
            img.alpha_composite(i, (x, 0))
            x += i.width + 8
        img.save(preview)
    print(f"{len(pieces)} textures, {out} ({len(blob) + len(data)} bytes)")


if __name__ == "__main__":
    main()
