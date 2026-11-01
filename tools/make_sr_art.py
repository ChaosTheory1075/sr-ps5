#!/usr/bin/env python3
"""SAINTS ROW: ReInstated's art, from the user's pictures (kept out of git, like the music):

    make_sr_art.py <Extras folder> <out: app-assets folder> <font.ttf>

- hub/background1.jpg, hub/background2.jpg: the menu's backgrounds for Saints Row and Saints Row 2
- hub/logo1.png, hub/logo2.png: "[SAINTS ROW]" and "[SAINTS ROW 2]", worn like the games' own logos
- sce_sys/icon0.png (512x512) and pic0.dds / pic1.dds (3840x2160 BC7): the home screen's, with
  "SAINTS ROW: ReInstated" on them

The font is Black Ops One (SIL Open Font License): heavy and wide like the games' lettering; the
wear (specks and scratches eaten out of it) is drawn here.
"""
import os
import random
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ci"))
import make_sce_art  # noqa: E402  (its BC7 encoder and DDS header)

GREY = (232, 230, 226)
RED = (200, 24, 30)


def worn(mask, seed, amount=1.0):
    """The lettering worn: specks and short scratches taken out of `mask` (L), more at the bottom."""
    rnd = random.Random(seed)
    w, h = mask.size
    holes = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(holes)
    for _ in range(int(w * h / 520 * amount)):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        # (worse toward the bottom, as the logo's)
        if rnd.random() > 0.35 + 0.65 * (y / h):
            continue
        r = rnd.uniform(0.4, 1.7) * h / 220
        d.ellipse([x - r, y - r * rnd.uniform(0.5, 1.2), x + r, y + r], fill=255)
    for _ in range(int(w / 18 * amount)):
        x, y = rnd.uniform(0, w), rnd.uniform(h * 0.3, h)
        ln = rnd.uniform(4, 22) * h / 220
        d.line([x, y, x + ln, y + rnd.uniform(-2, 2)], fill=255, width=max(1, int(h / 260)))
    holes = holes.filter(ImageFilter.GaussianBlur(0.6))
    return ImageChops.subtract(mask, holes)


def text_mask(font, parts, pad):
    """The parts [(text, colour)] side by side: a colour layer and its mask."""
    widths = [font.getbbox(t)[2] for t, _ in parts]
    asc, desc = font.getmetrics()
    w, h = sum(widths) + pad * 2, asc + desc + pad * 2
    mask = Image.new("L", (w, h), 0)
    colour = Image.new("RGB", (w, h), GREY)
    x = pad
    for (t, c), tw in zip(parts, widths):
        m = Image.new("L", (w, h), 0)
        ImageDraw.Draw(m).text((x, pad), t, font=font, fill=255)
        mask = ImageChops.lighter(mask, m)
        colour.paste(Image.new("RGB", (w, h), c), (0, 0), m)
        x += tw
    return colour, mask


def logo(font_path, parts, height, seed):
    """Transparent worn lettering `height` pixels tall (trimmed), with a soft dark shadow."""
    font = ImageFont.truetype(font_path, height)
    colour, mask = text_mask(font, parts, height // 4)
    mask = worn(mask, seed)
    shadow = mask.filter(ImageFilter.GaussianBlur(height / 14)).point(lambda v: int(v * 0.8))
    out = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    out.paste(Image.new("RGBA", mask.size, (0, 0, 0, 255)), (0, 0), shadow)
    out.paste(colour.convert("RGBA"), (0, 0), mask)
    return out.crop(out.getbbox())


def title_block(font_path, width, seed):
    """"SAINTS ROW:" over a red "ReInstated", `width` wide."""
    top = logo(font_path, [("SAINTS ROW:", GREY)], 400, seed)
    sub = logo(font_path, [("ReInstated", RED)], 400, seed + 1)
    s = width / top.width
    top = top.resize((width, int(top.height * s)), Image.LANCZOS)
    sw = int(width * 0.78)
    sub = sub.resize((sw, int(sub.height * sw / sub.width)), Image.LANCZOS)
    out = Image.new("RGBA", (width, top.height + sub.height - int(top.height * 0.12)), (0, 0, 0, 0))
    out.alpha_composite(top, (0, 0))
    out.alpha_composite(sub, (width - sw, top.height - int(top.height * 0.12)))
    return out


def cover(img, w, h, focus=0.5):
    s = max(w / img.width, h / img.height)
    img = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x = round((img.width - w) * focus)
    y = (img.height - h) // 2
    return img.crop((x, y, x + w, y + h))


def darken_left(img, strength=0.75, reach=0.55):
    w, h = img.size
    grad = Image.new("L", (w, 1))
    for x in range(w):
        t = max(0.0, 1 - x / (w * reach))
        grad.putpixel((x, 0), int(255 * strength * t))
    grad = grad.resize((w, h))
    return Image.composite(Image.new("RGB", (w, h), (0, 0, 0)), img, grad)


def main():
    extras, out, font_path = sys.argv[1:4]
    hub, sce = os.path.join(out, "hub"), os.path.join(out, "sce_sys")
    os.makedirs(hub, exist_ok=True)
    os.makedirs(sce, exist_ok=True)
    p1 = Image.open(os.path.join(extras, "Saints Row1Background2.jpg")).convert("RGB")
    p2 = Image.open(os.path.join(extras, "Saints Row2Background.webp")).convert("RGB")

    # the menu's backgrounds (1920x1080) and logos
    cover(p1, 1920, 1080, 0.5).save(os.path.join(hub, "background1.jpg"), quality=92)
    cover(p2, 1920, 1080, 0.5).save(os.path.join(hub, "background2.jpg"), quality=92)
    logo(font_path, [("[SAINTS ROW]", GREY)], 300, 1).save(os.path.join(hub, "logo1.png"))
    logo(font_path, [("[SAINTS ROW", GREY), ("2", RED), ("]", GREY)], 300, 2).save(os.path.join(hub, "logo2.png"))

    # the home screen: Alex at the left, Heller at the right, the name over the dark middle
    W, H = make_sce_art.W, make_sce_art.H
    left = cover(p1, W // 2 + 400, H, 0.62)
    right = cover(p2, W // 2 + 400, H, 0.62)
    bg = Image.new("RGB", (W, H))
    bg.paste(right, (W - right.width, 0))
    seam = Image.new("L", (left.width, H))
    for x in range(left.width):
        seam.paste(int(255 * min(1.0, max(0.0, (left.width - x) / 800))), (x, 0, x + 1, H))
    bg.paste(left, (0, 0), seam)
    shade = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shade).rectangle([0, int(H * 0.62), W, H], fill=150)
    shade = shade.filter(ImageFilter.GaussianBlur(160))
    bg = Image.composite(Image.new("RGB", (W, H), (0, 0, 0)), bg, shade)
    title = title_block(font_path, 1900, 7)
    bg = bg.convert("RGBA")
    bg.alpha_composite(title, ((W - title.width) // 2, H - title.height - 170))
    bg = bg.convert("RGB")
    bg.resize((1920, 1080), Image.LANCZOS).save(os.path.join(extras, "tmp", "pic0-preview.jpg"), quality=88)
    data = make_sce_art.bc7_mode6(bg)
    for name in ("pic0.dds", "pic1.dds"):
        with open(os.path.join(sce, name), "wb") as f:
            f.write(make_sce_art.DDS_HEADER + data)

    # the icon: Alex's hood, the name under it
    icon = cover(p1, 512, 512, 0.62)
    band = Image.new("L", (512, 512), 0)
    ImageDraw.Draw(band).rectangle([0, 300, 512, 512], fill=200)
    band = band.filter(ImageFilter.GaussianBlur(40))
    icon = Image.composite(Image.new("RGB", (512, 512), (0, 0, 0)), icon, band).convert("RGBA")
    t = title_block(font_path, 452, 9)
    icon.alpha_composite(t, (30, 512 - t.height - 28))
    icon.convert("RGB").save(os.path.join(sce, "icon0.png"), optimize=True)
    print("hub/background1.jpg background2.jpg logo1.png logo2.png, sce_sys/icon0.png pic0.dds pic1.dds")


if __name__ == "__main__":
    main()
