#!/usr/bin/env python3
"""
m3eGram app icon (taskbar / window / exe): M3 Expressive squircle with a tonal
gradient and the Material Symbols "send" glyph.

    python make_app_icon.py --preview   # only writes ./app_icon_preview.png
    python make_app_icon.py --apply     # backs up the originals, rewrites
                                        # Telegram/Resources/art/icon*.png and icon256.ico
"""
import argparse
import os
import shutil

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.normpath(os.path.join(HERE, "..", "..", "Telegram", "Resources", "art"))
BACKUP = os.path.join(HERE, "original_icons", "app")
FONT = os.path.join(HERE, "MaterialSymbolsRounded.ttf")
CODEPOINTS = os.path.join(HERE, "MaterialSymbolsRounded.codepoints")

MASTER = 2048  # supersampled master size
TOP = (122, 200, 255)  # gradient start (top-left)
BOTTOM = (0, 97, 164)  # gradient end (bottom-right)


def glyph_codepoint(name):
    for line in open(CODEPOINTS, encoding="utf-8"):
        parts = line.split()
        if len(parts) == 2 and parts[0] == name:
            return int(parts[1], 16)
    raise SystemExit("glyph not found: " + name)


def squircle_mask(size, power=5.0):
    """Superellipse |x|^n + |y|^n = 1: the 'continuous corner' app-icon shape."""
    mask = Image.new("L", (size, size), 0)
    px = mask.load()
    half = size / 2.0
    for y in range(size):
        dy = abs((y + 0.5 - half) / half)
        for x in range(size):
            dx = abs((x + 0.5 - half) / half)
            if dx ** power + dy ** power <= 1.0:
                px[x, y] = 255
    return mask


def diagonal_gradient(size, a, b):
    base = Image.new("RGB", (size, size))
    px = base.load()
    scale = 2.0 * (size - 1)
    for y in range(size):
        for x in range(size):
            t = (x + y) / scale
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    return base


def build_master():
    size = MASTER
    # the squircle is built at 1/4 size and upscaled for speed, edges stay smooth
    small = squircle_mask(size // 4)
    mask = small.resize((size, size), Image.LANCZOS).filter(ImageFilter.GaussianBlur(1.2))

    icon = diagonal_gradient(size // 8, TOP, BOTTOM).resize((size, size), Image.BICUBIC).convert("RGBA")

    # soft glow in the upper-left corner (no hard edge)
    glow = Image.new("L", (size, size), 0)
    ImageDraw.Draw(glow).ellipse((-size * 0.3, -size * 0.5, size * 0.85, size * 0.45), fill=60)
    glow = glow.filter(ImageFilter.GaussianBlur(size * 0.10))
    icon = Image.composite(Image.new("RGBA", (size, size), (255, 255, 255, 255)), icon, glow)

    # paper plane, drawn as a vector so it stays crisp and has no font seams
    body = [(0.06, 0.50), (0.94, 0.12), (0.72, 0.90), (0.50, 0.64), (0.37, 0.84), (0.35, 0.60)]
    fold = [(0.35, 0.60), (0.50, 0.64), (0.37, 0.84)]
    box = size * 0.58
    ox = (size - box) / 2 - size * 0.012
    oy = (size - box) / 2 + size * 0.006

    def pts(points):
        return [(ox + px * box, oy + py * box) for px, py in points]

    def rounded_polygon(points, width):
        layer = Image.new("L", (size, size), 0)
        draw = ImageDraw.Draw(layer)
        p = pts(points)
        draw.polygon(p, fill=255)
        draw.line(p + [p[0]], fill=255, width=width, joint="curve")
        for x, y in p:  # round every corner
            draw.ellipse((x - width / 2, y - width / 2, x + width / 2, y + width / 2), fill=255)
        return layer

    plane = rounded_polygon(body, int(size * 0.03))
    crease = rounded_polygon(fold, int(size * 0.012))

    shadow = plane.filter(ImageFilter.GaussianBlur(size * 0.02))
    shadow = ImageChops.offset(shadow, 0, int(size * 0.018)).point(lambda v: int(v * 0.35))
    icon = Image.composite(Image.new("RGBA", (size, size), (0, 40, 90, 255)), icon, shadow)
    icon = Image.composite(Image.new("RGBA", (size, size), (255, 255, 255, 255)), icon, plane)
    # the fold is a slightly darker tint of the plane
    fold_mask = ImageChops.multiply(crease, plane)
    icon = Image.composite(Image.new("RGBA", (size, size), (196, 224, 250, 255)), icon, fold_mask)

    icon.putalpha(mask)
    return icon


def targets():
    result = []
    for name in sorted(os.listdir(ART)):
        if name.startswith("icon") and name.endswith(".png") and "green" not in name:
            if name == "icon_round512@2x.png":
                continue  # separate round macOS variant, left alone
            result.append(name)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()

    master = build_master()
    if args.preview or not args.apply:
        sheet = Image.new("RGBA", (1120, 300), (30, 32, 34, 255))
        x = 20
        for s in (256, 128, 64, 48, 32, 16):
            sheet.alpha_composite(master.resize((s, s), Image.LANCZOS), (x, 20))
            x += s + 24
        sheet.save(os.path.join(HERE, "app_icon_preview.png"))
        print("preview written")
        if not args.apply:
            return

    os.makedirs(BACKUP, exist_ok=True)
    written = 0
    for name in targets() + ["icon256.ico"]:
        src = os.path.join(ART, name)
        backup = os.path.join(BACKUP, name)
        if os.path.exists(src) and not os.path.exists(backup):
            shutil.copy2(src, backup)
    for name in targets():
        size = Image.open(os.path.join(BACKUP, name)).size[0]
        master.resize((size, size), Image.LANCZOS).save(os.path.join(ART, name), optimize=True)
        written += 1
    ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    master.resize((256, 256), Image.LANCZOS).save(os.path.join(ART, "icon256.ico"), format="ICO", sizes=ico_sizes)
    print("written", written, "png files + icon256.ico")


if __name__ == "__main__":
    main()
