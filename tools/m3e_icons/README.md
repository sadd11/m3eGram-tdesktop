# m3eGram icons: Material Symbols

Telegram's UI icons (`Telegram/Resources/icons`) are replaced with glyphs of the
**Material Symbols Rounded** font (Apache License 2.0, google/material-design-icons).

Icons are masks (white shape on black, scales 1x / 2x / 3x with identical canvas
sizes), so the tool renders each glyph into the original canvas, fitted to the
bounding box of the original shape.

    pip install pillow
    python generate.py --dry-run     # validate mapping.json
    python generate.py --preview     # contact sheets (old | new) in ./preview
    python generate.py --apply       # back up originals to ./original_icons, write new PNGs
    python generate.py --restore     # restore the originals
    python generate.py --report      # list icons without a mapping

`mapping.json` maps `path/under/icons` (no extension) to a glyph name, a list of
fallback names, or `{ "g": name, "fill": 0|1, "scale": 1.0, "weight": 300 }`.
Solid originals (play, pause, comments, settings list, folders) use the FILL=1 glyph.

The font is downloaded on first run and is not committed. Only the generated PNGs are.
Not touched: SVG icons, logos, shadows, backgrounds, illustrations, bubble tails.

## App icon

    python make_app_icon.py --preview   # app_icon_preview.png only
    python make_app_icon.py --apply     # rewrite Telegram/Resources/art/icon*.png and icon256.ico

The icon (taskbar, window, exe) is an M3 squircle with a tonal gradient and a vector paper plane.
Originals are backed up to ./original_icons/app.
