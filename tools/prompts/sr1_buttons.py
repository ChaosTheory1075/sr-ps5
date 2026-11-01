#!/usr/bin/env python3
"""Saints Row's PlayStation button glyphs: its Xbox 360 button pictures (px_base.peg_xbox2, from
misc.vpp_xbox2 / pegfiles: px_btna, px_btnb, px_btnx, px_btny, px_btnselect) redrawn as the
DualSense's (cross, circle, square, triangle, the touch pad), for the texture hook
(ps5/sr_prompts.cpp). Its L1/L2/R1/R2 pictures already say so.

    sr1_buttons.py <px_base.peg_xbox2> <out: prompts1.bin> [preview.png]

The peg: "GEKV", version 8; at 0x10 the count; from 0x18, 0x48-byte entries: the data's offset,
width, height, format (0x191: DXT5, 16-bit swapped, tiled as the Xbox 360's GPU reads it; its
base level 16 KB however small), the name at +0x17. The game loads it into memory as it is in the file, so the bytes there are its
fingerprint: prompts1.bin ("SRTX", count, then {FNV-1a 64 of the original, size, offset of the
replacement}) holds the PlayStation picture in the same layout for each.
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, __import__("os").path.dirname(__file__))
import numpy as np  # noqa: E402
from x360tex import block_offsets, dxt5_block, encode_block, swap16  # noqa: E402


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def entries(d):
    n = struct.unpack_from(">H", d, 0x10)[0]
    out = []
    for i in range(n):
        e = 0x18 + i * 0x48
        off, w, h, fmt = struct.unpack_from(">IHHH", d, e)
        name = d[e + 0x17:e + 0x17 + 40].split(b"\0")[0].decode()
        out.append(dict(name=name, off=off, w=w, h=h, fmt=fmt))
    return out


REGION = 0x4000  # (a texture's data in the peg: 16 KB, however small)


def decode(data, w, h):
    data = swap16(data)
    img = np.zeros(((h + 3) // 4 * 4, (w + 3) // 4 * 4, 4), np.uint8)
    for (by, bx), o in block_offsets(dict(tiled=1, pitch=w, w=w, h=h)).items():
        img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = dxt5_block(data[o:o + 16])
    return Image.fromarray(img[:h, :w], "RGBA")


def encode(img, like, w, h):
    data = bytearray(swap16(like))
    arr = np.array(img.convert("RGBA"))
    for (by, bx), o in block_offsets(dict(tiled=1, pitch=w, w=w, h=h)).items():
        blk = np.zeros((4, 4, 4), np.uint8)
        part = arr[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4]
        blk[:part.shape[0], :part.shape[1]] = part
        data[o:o + 16] = encode_block(blk)
    return swap16(bytes(data))


# The DualSense's face buttons: the symbol's colour on a dark disc, as PlayStation prompts look.
FACE = {"px_btna.tga": ("cross", (124, 178, 232)), "px_btnb.tga": ("circle", (255, 102, 102)),
        "px_btnx.tga": ("square", (214, 145, 224)), "px_btny.tga": ("triangle", (64, 226, 160))}
LABEL = {}


def draw_face(orig, kind, colour):
    w, h = orig.size
    S = 8  # (drawn large, scaled down: smooth edges)
    img = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy, r = w * S / 2, h * S / 2, min(w, h) * S / 2 - S
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(20, 20, 24, 255), outline=(235, 235, 235, 255),
              width=int(S * 1.4))
    s, lw = r * 0.48, int(S * 2.6)
    if kind == "cross":
        d.line([cx - s, cy - s, cx + s, cy + s], fill=colour, width=lw)
        d.line([cx - s, cy + s, cx + s, cy - s], fill=colour, width=lw)
    elif kind == "circle":
        d.ellipse([cx - s, cy - s, cx + s, cy + s], outline=colour, width=lw)
    elif kind == "square":
        d.rectangle([cx - s * 0.9, cy - s * 0.9, cx + s * 0.9, cy + s * 0.9], outline=colour, width=lw)
    else:
        t = s * 1.05
        d.polygon([(cx, cy - t), (cx + t * 0.95, cy + t * 0.65), (cx - t * 0.95, cy + t * 0.65)], outline=colour,
                  width=lw)
    return img.resize((w, h), Image.LANCZOS)


def draw_label(orig, label, font_path):
    """L1/L2/R1/R2 (and OPTIONS) on the 360's shoulder-button shape: its own outline kept, the
    letters redrawn."""
    w, h = orig.size
    S = 8
    img = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = S * 2
    d.rounded_rectangle([pad, pad, w * S - pad, h * S - pad], radius=(h * S) // 3, fill=(20, 20, 24, 255),
                        outline=(235, 235, 235, 255), width=int(S * 1.4))
    size = int(h * S * (0.62 if len(label) <= 2 else 0.36))
    font = ImageFont.truetype(font_path, size)
    l, t, r, b = d.textbbox((0, 0), label, font=font)
    d.text(((w * S - (r - l)) / 2 - l, (h * S - (b - t)) / 2 - t), label, font=font, fill=(240, 240, 240, 255))
    return img.resize((w, h), Image.LANCZOS)


def main():
    peg, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[3] if len(sys.argv) > 3 else None
    font_path = sys.argv[4] if len(sys.argv) > 4 else "C:/Windows/Fonts/arialbd.ttf"
    d = open(peg, "rb").read()
    pieces, sheet = [], []
    for e in entries(d):
        name = e["name"]
        if name not in FACE and name != "px_btnselect.tga" or e["fmt"] != 0x191:
            continue
        orig = d[e["off"]:e["off"] + REGION]
        img = decode(orig, e["w"], e["h"])
        new = draw_face(img, *FACE[name]) if name in FACE else draw_label(img, "TOUCH", font_path)
        rep = encode(new, orig, e["w"], e["h"])
        # (fingerprints of the first 1, 2, 4, 8 and 16 KB: whichever base size the GPU's cache asks)
        for size in (0x400, 0x800, 0x1000, 0x2000, 0x4000):
            pieces.append((fnv1a64(orig[:size]), size, rep[:size]))
        sheet.append((img, decode(rep, e["w"], e["h"])))
        print(f"{name}: {e['w']}x{e['h']}, fnv(16 KB) {fnv1a64(orig):016X}")
    blob = bytearray(b"SRTX" + struct.pack("<I", len(pieces)))
    at = 8 + 16 * len(pieces)
    data = b""
    for h, size, rep in pieces:
        blob += struct.pack("<QII", h, size, at + len(data))
        data += rep
    open(out, "wb").write(bytes(blob) + data)
    if preview:
        W = 64 * 2 + 8
        img = Image.new("RGBA", (W * 2, 72 * len(sheet)), (60, 40, 70, 255))
        for i, (a, b) in enumerate(sheet):
            img.alpha_composite(a.resize((a.width * 2, a.height * 2)), (4, i * 72))
            img.alpha_composite(b.resize((b.width * 2, b.height * 2)), (W + 4, i * 72))
        img.save(preview)
    print(f"{len(pieces)} glyphs, {out}")


if __name__ == "__main__":
    main()
