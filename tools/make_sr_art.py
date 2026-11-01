#!/usr/bin/env python3
"""SAINTS ROW: ReInstated's art, from the user's pictures (kept out of git, like the music):

    make_sr_art.py <Extras folder> <out: app-assets folder> <font.ttf>

- hub/background1.jpg, hub/background2.jpg: the menu's backgrounds for Saints Row and Saints Row 2
- hub/logo1.png, hub/logo2.png: "Saints Row" and "Saints Row 2" in chrome, like the games' logos
- sce_sys/icon0.png (512x512) and pic0.dds / pic1.dds (3840x2160 BC7): the home screen's, with
  "Saints Row: ReInstated" on them

The font is Pirata One (SIL Open Font License), a blackletter like the games' logo; the chrome,
outline and purple are drawn here.
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ci"))
import make_sce_art  # noqa: E402  (its BC7 encoder and DDS header)

PURPLE = [(232, 160, 255), (170, 70, 235), (96, 22, 168), (190, 110, 250)]
CHROME = [(255, 255, 255), (214, 214, 220), (96, 98, 108), (238, 238, 244)]


def gradient(w, h, stops):
    """A top-to-bottom gradient through `stops` (the chrome: light, a dark band, light again)."""
    g = Image.new("RGB", (1, h))
    n = len(stops) - 1
    for y in range(h):
        t = y / max(1, h - 1) * n
        i = min(n - 1, int(t))
        f = t - i
        a, b = stops[i], stops[i + 1]
        g.putpixel((0, y), tuple(int(a[k] + (b[k] - a[k]) * f) for k in range(3)))
    return g.resize((w, h))


def logo(font_path, text, height, stops=CHROME):
    """Transparent lettering `height` pixels tall (trimmed): the gradient, a dark outline and a soft
    shadow."""
    font = ImageFont.truetype(font_path, height)
    l, t, r, b = font.getbbox(text)
    pad = height // 3
    w, h = r - l + pad * 2, b - t + pad * 2
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).text((pad - l, pad - t), text, font=font, fill=255)
    edge = mask.filter(ImageFilter.MaxFilter(max(3, (height // 22) | 1)))
    shadow = edge.filter(ImageFilter.GaussianBlur(height / 12))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(Image.new("RGBA", (w, h), (0, 0, 0, 230)), (0, 0), shadow)
    out.paste(Image.new("RGBA", (w, h), (18, 8, 26, 255)), (0, 0), edge)
    bb = mask.getbbox()
    fill = Image.new("RGB", (w, h))
    fill.paste(gradient(bb[2] - bb[0], bb[3] - bb[1], stops), (bb[0], bb[1]))
    out.paste(fill.convert("RGBA"), (0, 0), mask)
    # a thin highlight along the letters' top edges
    hi = ImageChops.subtract(mask, ImageChops.offset(mask, 0, max(1, height // 60)))
    out.paste(Image.new("RGBA", (w, h), (255, 255, 255, 255)), (0, 0), hi.point(lambda v: v // 2))
    return out.crop(out.getbbox())


def title_block(font_path, width):
    """Chrome "Saints Row:" over a purple "ReInstated", `width` wide."""
    top = logo(font_path, "Saints Row:", 400)
    sub = logo(font_path, "ReInstated", 400, PURPLE)
    s = width / top.width
    top = top.resize((width, int(top.height * s)), Image.LANCZOS)
    sw = int(width * 0.74)
    sub = sub.resize((sw, int(sub.height * sw / sub.width)), Image.LANCZOS)
    lap = int(top.height * 0.22)
    out = Image.new("RGBA", (width, top.height + sub.height - lap), (0, 0, 0, 0))
    out.alpha_composite(top, (0, 0))
    out.alpha_composite(sub, ((width - sw) // 2, top.height - lap))
    return out


def cover(img, w, h, focus=0.5, vfocus=0.5):
    s = max(w / img.width, h / img.height)
    img = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x = round((img.width - w) * focus)
    y = round((img.height - h) * vfocus)
    return img.crop((x, y, x + w, y + h))


def main():
    extras, out, font_path = sys.argv[1:4]
    art = os.path.join(extras, "Launcher")
    hub, sce = os.path.join(out, "hub"), os.path.join(out, "sce_sys")
    os.makedirs(hub, exist_ok=True)
    os.makedirs(sce, exist_ok=True)
    b1 = Image.open(os.path.join(art, "SaintsRow1Background.jpg")).convert("RGB")
    b2 = Image.open(os.path.join(art, "SaintsRow2Background.jpg")).convert("RGB")
    t1 = Image.open(os.path.join(art, "SaintsRow1Title.jpg")).convert("RGB")
    t2 = Image.open(os.path.join(art, "SaintsRow2Title.jpg")).convert("RGB")
    fleur = Image.open(os.path.join(art, "SaintsRowLogo.png")).convert("RGB")

    # the menu's backgrounds (1920x1080) and logos
    cover(b1, 1920, 1080).save(os.path.join(hub, "background1.jpg"), quality=92)
    cover(b2, 1920, 1080).save(os.path.join(hub, "background2.jpg"), quality=92)
    logo(font_path, "Saints Row", 300).save(os.path.join(hub, "logo1.png"))
    logo(font_path, "Saints Row 2", 300).save(os.path.join(hub, "logo2.png"))

    # the home screen: the Saints Row crew at the left, Saints Row 2's boss at the right (the logo-free
    # picture), the name over the dark middle (the crew picture's logo is off the crop)
    W, H = make_sce_art.W, make_sce_art.H
    left = cover(t1, W // 2 + 500, H, 0.40)
    right = cover(b1, W // 2 + 500, H, 0.5, 0.0)
    bg = Image.new("RGB", (W, H))
    bg.paste(right, (W - right.width, 0))
    seam = Image.new("L", (left.width, 1))
    for x in range(left.width):
        seam.putpixel((x, 0), int(255 * min(1.0, max(0.0, (left.width - x) / 1000))))
    bg.paste(left, (0, 0), seam.resize((left.width, H)))
    shade = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shade).rectangle([0, int(H * 0.58), W, H], fill=190)
    ImageDraw.Draw(shade).rectangle([0, 0, W, int(H * 0.12)], fill=120)
    shade = shade.filter(ImageFilter.GaussianBlur(170))
    bg = Image.composite(Image.new("RGB", (W, H), (8, 0, 14)), bg, shade)
    title = title_block(font_path, 2000)
    bg = bg.convert("RGBA")
    bg.alpha_composite(title, ((W - title.width) // 2, H - title.height - 150))
    bg = bg.convert("RGB")
    bg.resize((1920, 1080), Image.LANCZOS).save(os.path.join(extras, "tmp", "pic0-preview.jpg"), quality=88)
    data = make_sce_art.bc7_mode6(bg)
    for name in ("pic0.dds", "pic1.dds"):
        with open(os.path.join(sce, name), "wb") as f:
            f.write(make_sce_art.DDS_HEADER + data)

    # the icon: the Saints' fleur-de-lis, the name over its lower half
    icon = cover(fleur, 512, 512, 0.5, 0.2)
    band = Image.new("L", (512, 512), 0)
    ImageDraw.Draw(band).rectangle([0, 290, 512, 512], fill=170)
    band = band.filter(ImageFilter.GaussianBlur(40))
    icon = Image.composite(Image.new("RGB", (512, 512), (10, 0, 18)), icon, band).convert("RGBA")
    t = title_block(font_path, 470)
    icon.alpha_composite(t, ((512 - t.width) // 2, 512 - t.height - 22))
    icon.convert("RGB").save(os.path.join(sce, "icon0.png"), optimize=True)
    print("hub/background1.jpg background2.jpg logo1.png logo2.png, sce_sys/icon0.png pic0.dds pic1.dds")


if __name__ == "__main__":
    main()
