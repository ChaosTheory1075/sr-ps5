#!/usr/bin/env python3
"""PlayStation button prompts for SAINTS ROW: ReInstated (sr-recomp/src/ps5/sr_prompts.cpp).

The games' button hints, as they are on the disc:
  Saints Row    characters of its own fonts (Saints RowBody360, Saints RowTitle360 in the Scaleform
               movies fonts_latin.gfx and gfxfontlib.gfx): its strings say "PRESS \xb3 to ...", and
               \xb3 is drawn as the Y button. Also a few textures (buttonhint, button_a/b).
  Saints Row 2  small textures (HUD_360_A_up.dds ...) its strings ask for by name ("/bju/" is
               HUD_360_A_up), and the buttonhint textures.

This draws the DualSense's over each (cross, circle, square, triangle; L1 R1 L2 R2; the touch pad
and OPTIONS for BACK and START), in the archive entry that holds it, and writes prompts<N>.bin: for
each archive, the byte ranges that differ and their new bytes. Packed entries (RZ, CFX) are packed
again into the same space. The app writes those bytes over what the game reads from its files.

  make_prompts.py <game 1|2> <out.bin> <preview.png> <game folder>

Nothing of the game's goes into the repository: the .bin is built from the user's own files.
"""
import glob, math, os, re, struct, sys, zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcf  # noqa: E402
import swf  # noqa: E402
import x360tex  # noqa: E402

# The textures, by name, per game.
TEXTURES = {
    1: {
        'buttonhint__I18': 'cross', 'buttonhint__I31': 'cross',
        'buttonhint__I1A': 'circle', 'buttonhint__I33': 'circle',
        'buttonhint__I1C': 'square', 'buttonhint__I35': 'square',
        'buttonhint__I1E': 'triangle', 'buttonhint__I37': 'triangle',
        'buttonhint__I20': 'L1', 'buttonhint__I39': 'L1',
        'buttonhint__I22': 'R1', 'buttonhint__I3B': 'R1',
        'buttonhint__I24': 'L2', 'buttonhint__I3D': 'L2',
        'buttonhint__I26': 'R2', 'buttonhint__I3F': 'R2',
        'button_a': 'cross', 'button_b': 'circle',
    },
    2: {
        'buttonhint__IA': 'cross', 'buttonhint__I2D': 'cross',
        'buttonhint__ID': 'circle', 'buttonhint__I30': 'circle',
        'buttonhint__I10': 'square', 'buttonhint__I33': 'square',
        'buttonhint__I12': 'triangle', 'buttonhint__I35': 'triangle',
        'buttonhint__I15': 'L1', 'buttonhint__I38': 'L1',
        'buttonhint__I17': 'R1', 'buttonhint__I3A': 'R1',
        'buttonhint__I19': 'L2', 'buttonhint__I3C': 'L2',
        'buttonhint__I1B': 'R2', 'buttonhint__I3E': 'R2',
        'buttonhint__I28': 'L3',
        'HUD_360_A_up': 'cross', 'HUD_360_B_up': 'circle', 'HUD_360_X_up': 'square',
        'HUD_360_Y_up': 'triangle', 'HUD_360_LB_up': 'L1', 'HUD_360_RB_up': 'R1',
        'HUD_360_LT_up': 'L2', 'HUD_360_RT_up': 'R2', 'HUD_360_LT_RT': 'L2 R2',
        'HUD_360_back': 'touchpad', 'HUD_360_start': 'options',
    },
}
# Saints Row's font glyphs (fonts named here), by character.
FONTS = ('Saints RowBody360', 'Saints RowTitle360')
CHARS = {0x25CA: 'cross', 0xB1: 'circle', 0xA3: 'square', 0xB3: 'triangle', 0xB9: 'L1', 0xA5: 'R1',
         0xBB: 'L2', 0xBC: 'R2', 0xB7: 'touchpad', 0xB8: 'options'}

# The DualSense's symbol colours.
COLOURS = {'cross': (124, 178, 232), 'circle': (255, 102, 102), 'square': (227, 155, 227),
           'triangle': (64, 226, 160)}
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'


# ---- textures ----

def footprint(img):
    a = img[:, :, 3] > 60
    ys, xs = np.nonzero(a)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def symbol(d, glyph, cx, cy, r, lw, c):
    if glyph == 'cross':
        d.line([cx - r, cy - r, cx + r, cy + r], fill=c, width=lw)
        d.line([cx - r, cy + r, cx + r, cy - r], fill=c, width=lw)
    elif glyph == 'circle':
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=c, width=lw)
    elif glyph == 'square':
        d.rectangle([cx - r * 0.9, cy - r * 0.9, cx + r * 0.9, cy + r * 0.9], outline=c, width=lw)
    else:
        rr = r * 1.1
        pts = [(cx, cy - rr), (cx + rr * 0.95, cy + rr * 0.68), (cx - rr * 0.95, cy + rr * 0.68)]
        d.line(pts + [pts[0], pts[1]], fill=c, width=lw, joint='curve')


def face(img, glyph):
    """A dark round button with a light rim and its coloured symbol, in the original's footprint."""
    h, w = img.shape[:2]
    x0, y0, x1, y1 = footprint(img)
    S = 8  # drawn large, then scaled down (smooth edges)
    im = Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([x0 * S, y0 * S, x1 * S - 1, y1 * S - 1], fill=(225, 225, 225, 255))
    pad = max(1, (x1 - x0) // 14) * S
    d.ellipse([x0 * S + pad, y0 * S + pad, x1 * S - 1 - pad, y1 * S - 1 - pad], fill=(24, 24, 26, 255))
    cx, cy = (x0 + x1) * S / 2, (y0 + y1) * S / 2
    r = min(x1 - x0, y1 - y0) * S * 0.25
    lw = max(S, int(min(x1 - x0, y1 - y0) * S * 0.11))
    symbol(d, glyph, cx, cy, r, lw, COLOURS[glyph] + (255,))
    return np.array(im.resize((w, h), Image.LANCZOS))


def pill(img, glyph):
    """BACK and START (a light-rimmed dark pill): the touch pad, or OPTIONS' three lines."""
    h, w = img.shape[:2]
    out = img.copy()
    x0, y0, x1, y1 = footprint(img)
    S = 8
    im = Image.fromarray(out, 'RGBA').resize((w * S, h * S), Image.NEAREST)
    d = ImageDraw.Draw(im)
    rgb = img[:, :, :3].astype(np.int32)
    luma = (rgb * [3, 6, 1]).sum(2) // 10
    solid = img[:, :, 3] > 200
    dark = solid & (luma < 90)
    fill = tuple(int(v) for v in np.median(img[dark][:, :3], axis=0)) if dark.any() else (24, 24, 26)
    light = solid & (luma > 160)
    ink = tuple(int(v) for v in np.median(img[light][:, :3], axis=0)) if light.any() else (225, 225, 225)
    # the inside cleared, the symbol drawn in the rim's colour
    mx, my = (x1 - x0) * 0.22, (y1 - y0) * 0.24
    d.rectangle([(x0 + mx) * S, (y0 + my) * S, (x1 - mx) * S, (y1 - my) * S], fill=fill + (255,))
    cx, cy = (x0 + x1) * S / 2, (y0 + y1) * S / 2
    u = min(x1 - x0, y1 - y0) * S
    if glyph == 'touchpad':
        d.rounded_rectangle([cx - u * 0.26, cy - u * 0.16, cx + u * 0.26, cy + u * 0.16], radius=u * 0.05,
                            outline=ink + (255,), width=max(S, int(u * 0.07)))
    else:
        for k in (-1, 0, 1):
            y = cy + k * u * 0.13
            d.line([cx - u * 0.2, y, cx + u * 0.2, y], fill=ink + (255,), width=max(S, int(u * 0.07)))
    return np.array(im.resize((w, h), Image.LANCZOS))


def plate(img, label):
    """The original's bumper/trigger plate with its letters painted out and `label` drawn (in the
    letters' colour: dark letters on a light plate, or light on dark)."""
    h, w = img.shape[:2]
    out = img.copy()
    rgb = img[:, :, :3].astype(np.int32)
    luma = (rgb * [3, 6, 1]).sum(2) // 10
    solid = img[:, :, 3] > 200
    x0, y0, x1, y1 = footprint(img)
    inner = np.zeros_like(solid)
    mx, my = max(1, (x1 - x0) // 6), max(1, (y1 - y0) // 6)
    inner[y0 + my:y1 - my, x0 + mx:x1 - mx] = True
    core = solid & inner
    bg = np.median(luma[core]) if core.any() else 200
    letters = core & (np.abs(luma - bg) > 70)
    if letters.any():
        ys, xs = np.nonzero(letters)
        lx0, ly0, lx1, ly1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
        ink = tuple(int(v) for v in np.median(img[letters][:, :3], axis=0))
    else:
        lx0, ly0, lx1, ly1 = x0 + mx, y0 + my, x1 - mx, y1 - my
        ink = (16, 16, 16) if bg > 128 else (235, 235, 235)
    plain = core & ~letters
    fill = np.median(img[plain][:, :3], axis=0).astype(np.uint8) if plain.any() else np.array([128] * 3, np.uint8)
    grow = 1
    box = np.zeros_like(solid)
    box[max(0, ly0 - grow):ly1 + grow, max(0, lx0 - grow):lx1 + grow] = True
    out[box & solid, :3] = fill
    S = 8
    im = Image.fromarray(out, 'RGBA').resize((w * S, h * S), Image.NEAREST)
    d = ImageDraw.Draw(im)
    size = int((ly1 - ly0) * S * 1.5)
    font = ImageFont.truetype(FONT, max(8, size))
    while font.getbbox(label)[2] - font.getbbox(label)[0] > (lx1 - lx0 + 2 * grow + 2) * S and size > 8:
        size -= S // 2
        font = ImageFont.truetype(FONT, size)
    bb = font.getbbox(label)
    tx = (lx0 + lx1) * S / 2 - (bb[0] + bb[2]) / 2
    ty = (ly0 + ly1) * S / 2 - (bb[1] + bb[3]) / 2
    d.text((tx, ty), label, font=font, fill=ink + (255,))
    return np.array(im.resize((w, h), Image.LANCZOS))


def dark_plate(img, label):
    """Saints Row 2's HUD bumpers and triggers (white letters on a dark plate, a white rim): the
    inside cleared to the plate's colour and `label` drawn in white. "L2 R2": two plates."""
    h, w = img.shape[:2]
    labels = label.split()
    if len(labels) > 1:
        half = w // len(labels)
        return np.concatenate([dark_plate(img[:, i * half:(i + 1) * half].copy(), lb)
                               for i, lb in enumerate(labels)], axis=1)
    x0, y0, x1, y1 = footprint(img)
    b = max(2, round(min(x1 - x0, y1 - y0) * 0.16))
    S = 8
    im = Image.fromarray(img, 'RGBA').resize((w * S, h * S), Image.NEAREST)
    d = ImageDraw.Draw(im)
    d.rectangle([(x0 + b) * S, (y0 + b) * S, (x1 - b) * S - 1, (y1 - b) * S - 1], fill=(20, 20, 22, 255))
    bw, bh = (x1 - x0 - 2 * b) * S, (y1 - y0 - 2 * b) * S
    size = int(bh * 1.05)
    font = ImageFont.truetype(FONT, size)
    while font.getbbox(label)[2] - font.getbbox(label)[0] > bw * 0.92 and size > 8:
        size -= 2
        font = ImageFont.truetype(FONT, size)
    bb = font.getbbox(label)
    tx = (x0 + x1) * S / 2 - (bb[0] + bb[2]) / 2
    ty = (y0 + y1) * S / 2 - (bb[1] + bb[3]) / 2
    d.text((tx, ty), label, font=font, fill=(240, 240, 240, 255))
    return np.array(im.resize((w, h), Image.LANCZOS))


def draw_texture(img, glyph, name=''):
    up = img[::-1].copy()  # (stored upside down)
    if name.startswith('HUD_360') and glyph[0] in 'LR':
        new = dark_plate(up, glyph)
    elif glyph in COLOURS:
        new = face(up, glyph)
    elif glyph in ('touchpad', 'options'):
        new = pill(up, glyph)
    else:
        new = plate(up, glyph)
    return new[::-1].copy()


def patch_textures(d, wanted, sheet):
    """The wanted Pure3D textures in an entry's data redrawn (same size); the new data."""
    if not any(n.encode() in d for n in wanted):
        return d
    out = bytearray(d)
    for m in re.finditer(rb'\x00\x01\x90\x00(....)(....)(.)', d, re.S):
        ln = m.group(3)[0]
        name = d[m.end():m.end() + ln].rstrip(b'\0').decode('latin-1').split('.')[0]
        if name not in wanted:
            continue
        q = d.find(b'\x00\x01\x90\x02', m.end())
        plen = struct.unpack('>I', d[q + 12:q + 16])[0]
        p = d[q + 16:q + 16 + plen]
        img, hd = x360tex.decode(p)
        rep = x360tex.encode(draw_texture(img, wanted[name], name), p)
        assert len(rep) == len(p)
        out[q + 16:q + 16 + plen] = rep
        back, _ = x360tex.decode(rep)
        sheet.append((name + ' ' + wanted[name], img[::-1], back[::-1]))
    return bytes(out)


# ---- font glyphs ----

def flat(c):
    """A contour's points (curves by their ends and controls), for areas and bounds."""
    pts = [c[0]]
    for e in c[1:]:
        pts += [(e[1], e[2])] if e[0] == 'L' else [(e[1], e[2]), (e[3], e[4])]
    return pts


def area(c):
    p = flat(c)
    return sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p))) / 2


def oriented(c, sign):
    """The contour, reversed if its winding isn't `sign`'s."""
    if (area(c) > 0) == (sign > 0):
        return c
    pts = [c[0]] + [e for e in c[1:]]
    # rebuild backwards: each edge's end becomes the next start
    nodes = [c[0]] + [(e[1], e[2]) if e[0] == 'L' else (e[3], e[4]) for e in c[1:]]
    out = [nodes[-1]]
    for i in range(len(c) - 1, 0, -1):
        e = c[i]
        to = nodes[i - 1]
        out.append(('L', to[0], to[1]) if e[0] == 'L' else ('Q', e[1], e[2], to[0], to[1]))
    del pts
    return out


def circle(cx, cy, r, n=8):
    k = r / math.cos(math.pi / n)
    c = [(round(cx + r), round(cy))]
    for i in range(1, n + 1):
        a0, a1 = 2 * math.pi * (i - 1) / n, 2 * math.pi * i / n
        am = (a0 + a1) / 2
        c.append(('Q', round(cx + k * math.cos(am)), round(cy + k * math.sin(am)),
                  round(cx + r * math.cos(a1)), round(cy + r * math.sin(a1))))
    return c


def poly(pts):
    pts = [(round(x), round(y)) for x, y in pts]
    return [pts[0]] + [('L', x, y) for x, y in pts[1:] + pts[:1]]


def rect(x0, y0, x1, y1):
    return poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def text_contours(label, x0, y0, x1, y1):
    """`label` in DejaVu Sans Bold as quadratic contours, fitted (centred) in the box."""
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.ttLib import TTFont
    tt = TTFont(FONT)
    gs, cmap, hmtx = tt.getGlyphSet(), tt.getBestCmap(), tt['hmtx']
    raw, pen_x = [], 0
    for ch in label:
        g = cmap[ord(ch)]
        pen = DecomposingRecordingPen(gs)
        gs[g].draw(pen)
        cur = None
        for op, args in pen.value:
            if op == 'moveTo':
                cur = [(args[0][0] + pen_x, args[0][1])]
                raw.append(cur)
            elif op == 'lineTo':
                cur.append(('L', args[0][0] + pen_x, args[0][1]))
            elif op == 'qCurveTo':
                pts = [(x + pen_x, y) for x, y in args]
                for i in range(len(pts) - 1):
                    ctl = pts[i]
                    end = pts[i + 1] if i == len(pts) - 2 else ((pts[i][0] + pts[i + 1][0]) / 2,
                                                                 (pts[i][1] + pts[i + 1][1]) / 2)
                    cur.append(('Q', ctl[0], ctl[1], end[0], end[1]))
            elif op == 'closePath':
                s = cur[0]
                e = cur[-1]
                end = (e[1], e[2]) if e[0] == 'L' else (e[3], e[4])
                if end != s:
                    cur.append(('L', s[0], s[1]))
        pen_x += hmtx[cmap[ord(ch)]][0]
    xs = [p[0] for c in raw for p in flat(c)]
    ys = [p[1] for c in raw for p in flat(c)]
    bw, bh = max(xs) - min(xs), max(ys) - min(ys)
    sc = min((x1 - x0) / bw, (y1 - y0) / bh)
    ox = (x0 + x1) / 2 - (min(xs) + max(xs)) / 2 * sc
    oy = (y0 + y1) / 2 + (min(ys) + max(ys)) / 2 * sc  # (y down)

    def tr(x, y):
        return round(ox + x * sc), round(oy - y * sc)
    out = []
    for c in raw:
        n = [tr(*c[0])]
        for e in c[1:]:
            n.append(('L',) + tr(e[1], e[2]) if e[0] == 'L' else ('Q',) + tr(e[1], e[2]) + tr(e[3], e[4]))
        out.append(n)
    return out


def glyph_contours(kind, x0, y0, x1, y1, sign):
    """The DualSense's glyph in the box (y down): outer contours wound `sign`, holes the other way."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    D = min(x1 - x0, y1 - y0)
    R = D / 2
    o = lambda c: oriented(c, sign)  # noqa: E731
    h = lambda c: oriented(c, -sign)  # noqa: E731
    if kind in COLOURS:
        t = D * 0.1
        cs = [o(circle(cx, cy, R)), h(circle(cx, cy, R - t))]
        r, lw = D * 0.24, D * 0.1
        if kind == 'cross':
            # a plus (arms r * 1.3 long, lw wide) turned 45 degrees: one outline, no overlaps
            a, k = lw / 2, r * 1.3
            plus = [(a, -k), (a, -a), (k, -a), (k, a), (a, a), (a, k), (-a, k), (-a, a), (-k, a),
                    (-k, -a), (-a, -a), (-a, -k)]
            s2 = math.sqrt(0.5)
            cs.append(o(poly([(cx + (x - y) * s2, cy + (x + y) * s2) for x, y in plus])))
        elif kind == 'circle':
            cs += [o(circle(cx, cy, r)), h(circle(cx, cy, r - lw))]
        elif kind == 'square':
            s = r * 0.9
            cs += [o(rect(cx - s, cy - s, cx + s, cy + s)), h(rect(cx - s + lw, cy - s + lw, cx + s - lw, cy + s - lw))]
        else:
            rr = r * 1.15
            top, rgt, lft = (cx, cy - rr), (cx + rr * 0.95, cy + rr * 0.62), (cx - rr * 0.95, cy + rr * 0.62)
            gx, gy = cx, cy - rr * 0.0
            # the inner triangle: the outer one shrunk towards its centroid by the line's width
            ccx, ccy = (top[0] + rgt[0] + lft[0]) / 3, (top[1] + rgt[1] + lft[1]) / 3
            inr = 1 - lw * 2.0 / (rr * 1.6)
            inner = [(ccx + (x - ccx) * inr, ccy + (y - ccy) * inr) for x, y in (top, rgt, lft)]
            cs += [o(poly([top, rgt, lft])), h(poly(inner))]
            del gx, gy
        return cs
    if kind in ('touchpad', 'options'):
        w, hh = (x1 - x0), D * 0.62
        t = D * 0.09
        X0, X1, Y0, Y1 = cx - w / 2, cx + w / 2, cy - hh / 2, cy + hh / 2
        cs = [o(rect(X0, Y0, X1, Y1)), h(rect(X0 + t, Y0 + t, X1 - t, Y1 - t))]
        if kind == 'touchpad':
            cs.append(o(rect(cx - w * 0.22, cy - hh * 0.18, cx + w * 0.22, cy + hh * 0.18)))
        else:
            for k in (-1, 0, 1):
                y = cy + k * hh * 0.22
                cs.append(o(rect(cx - w * 0.2, y - t * 0.45, cx + w * 0.2, y + t * 0.45)))
        return cs
    # L1 R1 L2 R2: the label, as the Xbox's letters were
    cs = text_contours(kind, x0 + (x1 - x0) * 0.04, cy - D * 0.36, x1 - (x1 - x0) * 0.04, cy + D * 0.36)
    # TrueType's outers are clockwise y-up: counter-clockwise here; flip all if that's not `sign`
    big = max(cs, key=lambda c: abs(area(c)))
    if (area(big) > 0) != (sign > 0):
        cs = [oriented(c, -1 if area(c) > 0 else 1) for c in cs]
    return cs


def patch_fonts(d, glyph_sheet):
    """Saints Row's button characters in a movie's fonts redrawn; the movie, the same size."""
    if d[:3] != b'GFX':
        return d
    out = bytearray(d)
    changed = False
    for start, hl, t, body in swf.tags(d):
        if t != 75:
            continue
        f = swf.Font(t, body)
        if f.name not in FONTS:
            continue
        for i, code in enumerate(f.codes):
            kind = CHARS.get(code)
            if not kind:
                continue
            old = swf.parse_shape(f.shapes[i], 0)
            if not old:
                continue
            pts = [p for c in old for p in flat(c)]
            x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
            y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts)
            sign = 1 if area(max(old, key=lambda c: abs(area(c)))) > 0 else -1
            new = glyph_contours(kind, x0, y0, x1, y1, sign)
            f.shapes[i] = swf.write_shape(new, swf.first_fill(f.shapes[i]))
            assert swf.parse_shape(f.shapes[i], 0)
            glyph_sheet.append((f'{f.name[9:]} {code:04x} {kind}', old, new))
        nb = f.body(keep_size=len(body))
        out[start + hl:start + hl + len(body)] = nb
        changed = True
    return bytes(out) if changed else d


def render(contours, S=96):
    xs = [p[0] for c in contours for p in flat(c)]
    ys = [p[1] for c in contours for p in flat(c)]
    x0, y0, sc = min(xs), min(ys), (S - 8) / max(max(xs) - min(xs), max(ys) - min(ys), 1)
    im = Image.new('L', (S, S), 0)
    for c in contours:  # (even-odd)
        m = Image.new('L', (S, S), 0)
        pts, (x, y) = [], c[0]
        pts.append((x, y))
        for e in c[1:]:
            if e[0] == 'L':
                pts.append((e[1], e[2]))
            else:
                for tt in np.linspace(0, 1, 8)[1:]:
                    pts.append(((1 - tt) ** 2 * x + 2 * (1 - tt) * tt * e[1] + tt * tt * e[3],
                                (1 - tt) ** 2 * y + 2 * (1 - tt) * tt * e[2] + tt * tt * e[4]))
            x, y = pts[-1]
        ImageDraw.Draw(m).polygon([(4 + (px - x0) * sc, 4 + (py - y0) * sc) for px, py in pts], fill=255)
        im = Image.fromarray(np.array(im) ^ np.array(m))
    return im


# ---- Saints Row 2's logos (SKIP INTRO) ----

def skip_logos(d):
    """legalloading.gfx with the Radical logo's frame going straight to the end of the logos (the
    frame that moves on to the menu); the movie, the same size. None if it isn't that movie."""
    if d[:3] != b'GFX' or b'FE_logo_rad' not in d:
        return None
    frame, logo, outro = 0, None, None
    for start, hl, t, body in swf.tags(d):
        if t == 1:
            frame += 1
        elif t == 12 and b'FE_logo_rad' in body:
            logo = (start + hl, len(body))
        elif t == 12 and b'TimelineOnOutroCompleted' in body:
            outro = frame
    if logo is None or outro is None:
        return None
    at, n = logo
    # ActionGotoFrame(outro), ActionPlay, ActionEnd (the rest of the old actions: ends too)
    act = b'\x81\x02\x00' + struct.pack('<H', outro) + b'\x06\x00'
    out = bytearray(d)
    out[at:at + n] = act + b'\0' * (n - len(act))
    return bytes(out)


# ---- the archives ----

def deflate_exact(data, size):
    """A zlib stream of `data` exactly `size` bytes long, or None if it won't fit. (The games read a
    packed entry to its end: a shorter stream with padding after it never finishes unpacking.) The
    data is deflated but for its last few bytes, which go in stored blocks, and empty stored blocks
    (5 bytes each) take up what's left."""
    tail_max = min(64, len(data))
    adler = struct.pack('>I', zlib.adler32(data))
    for level in (9, 8, 7, 6, 5, 4):
        base = zlib.compressobj(level)
        head = base.compress(data[:len(data) - tail_max])
        for t in range(tail_max + 1):
            c = base.copy()
            out = head + c.compress(data[len(data) - tail_max:len(data) - t]) + c.flush(zlib.Z_FULL_FLUSH)
            last = b'\x01' + struct.pack('<HH', t, t ^ 0xFFFF) + data[len(data) - t:]  # final, stored
            room = size - (len(out) + len(last) + 4)
            if room >= 0 and room % 5 == 0:
                s = out + b'\x00\x00\x00\xff\xff' * (room // 5) + last + adler
                z = zlib.decompressobj()
                assert z.decompress(s) == data and z.eof and not z.unused_data and len(s) == size
                return s
            if room < -64:
                break  # (this level's too big)
    return None


def repack(raw, new):
    """An entry packed as `raw` was, from its new unpacked data, exactly raw's size; None if too big."""
    if raw[:2] == b'RZ':
        s = deflate_exact(new, len(raw) - 16)
        return raw[:16] + s if s else None
    if raw[:3] == b'CFX':
        s = deflate_exact(new[8:], len(raw) - 8)
        return raw[:8] + s if s else None
    return new if len(new) == len(raw) else None


def runs(a, b, base, gap=64):
    """(offset, bytes) of where b differs from a, nearby differences joined."""
    diff = np.nonzero(np.frombuffer(a, np.uint8) != np.frombuffer(b, np.uint8))[0]
    out = []
    if not len(diff):
        return out
    s = e = int(diff[0])
    for i in diff[1:]:
        i = int(i)
        if i - e > gap:
            out.append((base + s, b[s:e + 1]))
            s = i
        e = i
    out.append((base + s, b[s:e + 1]))
    return out


def main():
    game, out_bin, preview, folder = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
    wanted = TEXTURES[game]
    patches = []  # (archive, offset, bytes, kind: 0 the prompts, 1 SKIP INTRO)
    sheet, glyph_sheet = [], []
    for path in sorted(glob.glob(os.path.join(folder, '*.rcf'))):
        arc = os.path.basename(path)
        f = open(path, 'rb')
        for name, o, s in rcf.entries(path):
            f.seek(o)
            raw = f.read(s)
            try:
                d = rcf.unpack(raw)
            except zlib.error:
                continue
            new = patch_textures(d, wanted, sheet)
            if game == 1:
                new = patch_fonts(new, glyph_sheet)
            kind = 0
            if game == 2 and new == d:
                skipped = skip_logos(d)
                if skipped:
                    new, kind = skipped, 1
            if new == d:
                continue
            packed = repack(raw, new)
            if packed is None:
                print(f'{arc} {name}: no room to pack it again - left as it is')
                continue
            assert rcf.unpack(packed)[:len(new)] == new
            rs = runs(raw, packed, o)
            patches += [(arc, off, data, kind) for off, data in rs]
            print(f'{arc} {name}: {sum(len(x) for _, x in rs)} bytes in {len(rs)} pieces' +
                  (' (SKIP INTRO)' if kind else ''))
    # prompts.bin: "PRPF", count; per patch: archive name (32 bytes), offset, size, data offset,
    # kind (u32); then the data
    blob = bytearray(b'PRPF' + struct.pack('<I', len(patches)))
    data_at = len(blob) + 48 * len(patches)
    table, data = bytearray(), bytearray()
    for arc, off, b, kind in patches:
        table += arc.encode().ljust(32, b'\0') + struct.pack('<IIII', off, len(b), data_at + len(data), kind)
        data += b
    blob += table + data
    open(out_bin, 'wb').write(bytes(blob))
    print(len(patches), 'pieces,', len(blob), 'bytes ->', out_bin)
    # the preview: originals and replacements
    cells = [(lab, Image.fromarray(a, 'RGBA'), Image.fromarray(b, 'RGBA')) for lab, a, b in sheet]
    cells += [(lab, render(a).convert('RGBA'), render(b).convert('RGBA')) for lab, a, b in glyph_sheet]
    cw, ch = 300, 150
    im = Image.new('RGB', (cw * 4, ch * max(1, (len(cells) + 3) // 4)), (70, 70, 70))
    d = ImageDraw.Draw(im)
    for i, (label, a, b) in enumerate(cells):
        x, y = (i % 4) * cw, (i // 4) * ch
        for j, t in enumerate((a, b)):
            k = max(1, min(4, 120 // max(t.width, t.height)))
            t = t.resize((t.width * k, t.height * k), Image.NEAREST)
            im.paste(t, (x + 8 + j * 140, y + 22), t)
        d.text((x + 8, y + 4), label, fill=(255, 255, 0))
    im.save(preview)


main()
