#!/usr/bin/env python3
"""
m3eGram: replace Telegram Desktop UI icons with Material Symbols glyphs.

Telegram icons are *masks*: white shape on black background, three scales
(name.png, name@2x.png, name@3x.png) that must keep exactly the same pixel
sizes. This tool renders a Material Symbols (Rounded) glyph into the same
canvas, fitted to the bounding box of the original shape, so layouts stay put.

    python generate.py --dry-run          # validate mapping.json only
    python generate.py --preview          # contact sheets (old | new) in ./preview
    python generate.py --apply            # back up originals, write new icons
    python generate.py --restore          # put the original icons back
    python generate.py --report           # list icons that have no mapping
    add  --only substr  to limit to icons whose path contains `substr`

Needs Pillow. The font is downloaded on first run (Apache License 2.0,
https://github.com/google/material-design-icons).
"""
import argparse
import json
import os
import shutil
import sys
import urllib.request

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.normpath(os.path.join(HERE, "..", "..", "Telegram", "Resources", "icons"))
BACKUP = os.path.join(HERE, "original_icons")
PREVIEW = os.path.join(HERE, "preview")
FONT_PATH = os.path.join(HERE, "MaterialSymbolsRounded.ttf")
CP_PATH = os.path.join(HERE, "MaterialSymbolsRounded.codepoints")
BASE_URL = "https://github.com/google/material-design-icons/raw/master/variablefont/"
FONT_URL = BASE_URL + "MaterialSymbolsRounded%5BFILL%2CGRAD%2Copsz%2Cwght%5D"
SCALES = ["", "@2x", "@3x"]
EM = 512  # glyphs are rendered at this em size, then downscaled
FIT = 0.94  # Material glyphs read heavier than Telegram's, so sit slightly smaller
FILL_GROUPS = ("settings/", "folders/")  # these were solid icons


def ensure_font():
    for path, ext in ((FONT_PATH, ".ttf"), (CP_PATH, ".codepoints")):
        if not os.path.exists(path):
            print("downloading", os.path.basename(path), "...")
            urllib.request.urlretrieve(FONT_URL + ext, path)


def load_codepoints():
    result = {}
    with open(CP_PATH, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2:
                result[parts[0]] = int(parts[1], 16)
    return result


def load_mapping(codepoints):
    with open(os.path.join(HERE, "mapping.json"), encoding="utf-8-sig") as f:
        raw = json.load(f)
    mapping, invalid = {}, []
    for key, value in raw.items():
        if key.startswith("_"):
            continue
        opts = {}
        if isinstance(value, dict):
            opts = dict(value)
            names = opts.pop("g")
        else:
            names = value
        names = [names] if isinstance(names, str) else list(names)
        found = next((n for n in names if n in codepoints), None)
        if found is None:
            invalid.append((key, names))
            continue
        mapping[key] = (found, opts)
    return mapping, invalid


def source_path(rel, suffix):
    """Original icon: the backup if it exists, otherwise the file in the tree."""
    name = rel.replace("/", os.sep) + suffix + ".png"
    backup = os.path.join(BACKUP, name)
    return backup if os.path.exists(backup) else os.path.join(ICONS, name)


_glyphs = {}


def render_glyph(cp, fill, weight):
    key = (cp, fill, weight)
    if key not in _glyphs:
        font = ImageFont.truetype(FONT_PATH, EM)
        font.set_variation_by_axes([fill, 0, 24, weight])
        pad = EM // 2
        image = Image.new("L", (EM + 2 * pad, EM + 2 * pad), 0)
        ImageDraw.Draw(image).text((pad, pad), chr(cp), font=font, fill=255)
        bbox = image.getbbox()
        _glyphs[key] = image.crop(bbox) if bbox else None
    return _glyphs[key]


def analyze(path):
    image = Image.open(path)
    mode = image.mode
    rgba = image.convert("RGBA")
    alpha = rgba.getchannel("A")
    alpha_type = alpha.getextrema()[0] < 250  # real transparency => alpha mask
    mask = alpha if alpha_type else rgba.getchannel("B")  # Telegram reads byte 0
    bbox = mask.point(lambda v: 255 if v > 48 else 0).getbbox()
    return rgba.size, mode, alpha_type, bbox


def pick_weight(rel, opts):
    if "weight" in opts:
        return int(opts["weight"])
    _, _, _, bbox = analyze(source_path(rel, ""))
    if not bbox:
        return 400
    small = min(bbox[2] - bbox[0], bbox[3] - bbox[1])
    # Telegram's own icons are thin, so stay lighter than the Material default.
    return 300 if small >= 17 else 400 if small >= 12 else 500


def pick_fill(rel, opts):
    """Solid originals (play, pause, comment, ...) get the FILL=1 glyph."""
    if "fill" in opts:
        return int(opts["fill"])
    if rel.startswith(FILL_GROUPS):
        return 1
    path = source_path(rel, "@3x")
    if not os.path.exists(path):
        path = source_path(rel, "")
    _, _, _, bbox = analyze(path)
    if not bbox:
        return 0
    image = Image.open(path).convert("RGBA")
    alpha = image.getchannel("A")
    mask = alpha if alpha.getextrema()[0] < 250 else image.getchannel("B")
    crop = mask.crop(bbox)
    solid = sum(1 for v in crop.getdata() if v > 128)
    return 1 if solid / float(crop.width * crop.height) >= 0.45 else 0


def build(rel, suffix, cp, opts, weight):
    path = source_path(rel, suffix)
    if not os.path.exists(path):
        return None
    (width, height), mode, alpha_type, bbox = analyze(path)
    if not bbox:
        return None
    glyph = render_glyph(cp, pick_fill(rel, opts), weight)
    if glyph is None:
        return None
    gw, gh = glyph.size
    mult = float(opts.get("scale", 1.0)) * FIT
    bw, bh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    scale = min(bw * mult / gw, bh * mult / gh)
    scale = min(scale, min(width, height) / EM)  # em never larger than canvas
    nw, nh = max(1, round(gw * scale)), max(1, round(gh * scale))
    small = glyph.resize((nw, nh), Image.LANCZOS)
    cx, cy = (bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2
    out = Image.new("L", (width, height), 0)
    out.paste(small, (round(cx - nw / 2), round(cy - nh / 2)))
    if alpha_type:
        black = Image.new("L", (width, height), 0)
        return Image.merge("RGBA", (black, black, black, out))
    if mode == "L":
        return out
    if mode == "RGBA":
        return Image.merge("RGBA", (out, out, out, Image.new("L", (width, height), 255)))
    return Image.merge("RGB", (out, out, out))


def to_gray(image):
    return image.convert("RGBA").getchannel("B") if image.mode != "L" else image


def preview(items):
    os.makedirs(PREVIEW, exist_ok=True)
    for old in os.listdir(PREVIEW):
        if old.startswith("sheet_"):
            os.remove(os.path.join(PREVIEW, old))
    tile, cols, per_sheet = 112, 8, 48
    font = ImageFont.load_default()
    sheets = [items[i:i + per_sheet] for i in range(0, len(items), per_sheet)]
    for number, chunk in enumerate(sheets, 1):
        rows = (len(chunk) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * tile * 2, rows * (tile + 16)), (38, 40, 46))
        draw = ImageDraw.Draw(sheet)
        for index, (rel, (name, opts)) in enumerate(chunk):
            weight = pick_weight(rel, opts)
            suffix = "@3x" if os.path.exists(source_path(rel, "@3x")) else ""
            new = build(rel, suffix, load_codepoints_cache[name], opts, weight)
            old = Image.open(source_path(rel, suffix))
            x = (index % cols) * tile * 2
            y = (index // cols) * (tile + 16)
            for k, image in enumerate((old, new)):
                if image is None:
                    continue
                gray = to_gray(image)
                if gray.width > tile - 8 or gray.height > tile - 8:
                    gray.thumbnail((tile - 8, tile - 8), Image.LANCZOS)
                colored = Image.merge("RGB", (gray, gray, gray))
                sheet.paste(colored, (x + k * tile + (tile - gray.width) // 2, y + (tile - gray.height) // 2))
            draw.text((x + 4, y + tile - 2), rel.split("/")[-1][:30] + " > " + name[:18], font=font, fill=(150, 170, 200))
        sheet.save(os.path.join(PREVIEW, "sheet_%02d.png" % number))
    print("preview sheets:", len(sheets), "->", PREVIEW)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--preview", action="store_true")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--restore", action="store_true")
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--only", default="")
    args = parser.parse_args()

    if args.restore:
        count = 0
        for root, _, files in os.walk(BACKUP):
            for file in files:
                dst = os.path.join(ICONS, os.path.relpath(os.path.join(root, file), BACKUP))
                shutil.copy2(os.path.join(root, file), dst)
                count += 1
        print("restored", count, "files")
        return

    ensure_font()
    global load_codepoints_cache
    load_codepoints_cache = load_codepoints()
    mapping, invalid = load_mapping(load_codepoints_cache)
    items = [(k, v) for k, v in mapping.items() if args.only in k]
    missing = [k for k, _ in items if not os.path.exists(source_path(k, ""))]

    print("mapped icons: %d, invalid glyph names: %d, missing files: %d" % (len(mapping), len(invalid), len(missing)))
    for key, names in invalid:
        print("  INVALID glyph:", key, names)
    for key in missing:
        print("  MISSING file:", key)
    items = [(k, v) for k, v in items if k not in missing]

    if args.report:
        mapped = set(mapping)
        for root, _, files in os.walk(ICONS):
            for file in sorted(files):
                if file.endswith(".png") and "@" not in file:
                    rel = os.path.relpath(os.path.join(root, file), ICONS)[:-4].replace(os.sep, "/")
                    if rel not in mapped:
                        print("  unmapped:", rel)
    if args.preview:
        preview(items)
    if args.apply:
        written = 0
        for rel, (name, opts) in items:
            weight = pick_weight(rel, opts)
            for suffix in SCALES:
                new = build(rel, suffix, load_codepoints_cache[name], opts, weight)
                if new is None:
                    continue
                relname = rel.replace("/", os.sep) + suffix + ".png"
                target = os.path.join(ICONS, relname)
                backup = os.path.join(BACKUP, relname)
                if not os.path.exists(backup):
                    os.makedirs(os.path.dirname(backup), exist_ok=True)
                    shutil.copy2(target, backup)
                new.save(target, optimize=True)
                written += 1
        print("written", written, "files")


if __name__ == "__main__":
    sys.exit(main())
