#!/usr/bin/env python3
"""Rasterise Sora into the anti-aliased bitmap fonts the firmware draws with.

Writes src/render/fonts.h and src/render/fonts.cpp (checked in).

    pip install pillow fonttools
    python3 tools/make_fonts.py                 # finds Sora in ~/Library/Fonts
    python3 tools/make_fonts.py --font-dir DIR  # or point it at the .ttf files

PIXEL_ASPECT must match DISPLAY_PIXEL_ASPECT in include/config.h.
"""
import argparse
import math
import os
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

PIXEL_ASPECT = 1.273
SUPERSAMPLE = 8

# Latin-1 + Latin Extended-A + typographic punctuation; glyphs Sora lacks are dropped.
TEXT_CHARS = (
    [chr(c) for c in range(0x20, 0x7F)]
    + [chr(c) for c in range(0xA0, 0x180)]
    + [chr(c) for c in (0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D,
                        0x2022, 0x2026, 0x20AC, 0x2122)]
)
CLOCK_CHARS = list("0123456789:- ")
MICRO_CHARS = [chr(c) for c in range(0x20, 0x7F)] + ["\u00b7", "\u2014", "\u00b0"]

# (C name, file, size px, characters, tabular digits, alpha gamma, extra spacing px)
FONTS = [
    ("sora_title", "Sora-SemiBold.ttf", 14, TEXT_CHARS, False, 0.85, 0.3),
    # The artist, beside the title at the same size, just lighter.
    ("sora_title_regular", "Sora-Regular.ttf", 14, TEXT_CHARS, False, 0.85, 0.3),
    ("sora_artist", "Sora-Medium.ttf", 11, TEXT_CHARS, False, 0.80, 0.5),
    ("sora_clock", "Sora-Light.ttf", 58, CLOCK_CHARS, True, 0.90, 0.0),
    # Labels on the screensavers: dial numerals, indicator words, fine print.
    ("sora_micro", "Sora-Regular.ttf", 9, MICRO_CHARS, False, 0.70, 0.7),
]

DEFAULT_FONT_DIRS = [
    os.path.expanduser("~/Library/Fonts"),
    "/Library/Fonts",
    os.path.expanduser("~/.local/share/fonts"),
    "/usr/share/fonts/truetype/sora",
]


def find_font(name, dirs):
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    sys.exit(f"Can't find {name}. Install Sora (fonts.google.com/specimen/Sora) "
             f"or pass --font-dir.")


def render_glyph(font, ch, gamma):
    """Return (advance_px, left, top, width, height, alpha rows 0..15)."""
    sx = SUPERSAMPLE * PIXEL_ASPECT
    sy = SUPERSAMPLE
    advance = font.getlength(ch) / sx
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    if x1 <= x0 or y1 <= y0:
        return advance, 0, 0, 0, 0, []

    left = math.floor(x0 / sx)
    right = math.ceil(x1 / sx)
    top = math.floor(y0 / sy)
    bottom = math.ceil(y1 / sy)
    w, h = right - left, bottom - top

    hi = Image.new("L", (round(w * sx), h * sy), 0)
    ImageDraw.Draw(hi).text((-left * sx, -top * sy), ch, font=font,
                            fill=255, anchor="ls")
    lo = hi.resize((w, h), Image.BOX)
    px = lo.load()
    rows = [[round(((px[x, y] / 255.0) ** gamma) * 15) for x in range(w)]
            for y in range(h)]

    # Trim rows/columns that quantised to nothing.
    while rows and not any(rows[0]):
        rows.pop(0)
        top += 1
    while rows and not any(rows[-1]):
        rows.pop()
    if not rows:
        return advance, 0, 0, 0, 0, []
    while not any(r[0] for r in rows):
        rows = [r[1:] for r in rows]
        left += 1
    while not any(r[-1] for r in rows):
        rows = [r[:-1] for r in rows]
    return advance, left, top, len(rows[0]), len(rows), rows


def build_font(cname, path, size, chars, tabular, gamma, tracking):
    cmap = TTFont(path).getBestCmap()
    font = ImageFont.truetype(path, size * SUPERSAMPLE)
    chars = sorted({c for c in chars if ord(c) in cmap or c == " "}, key=ord)

    glyphs = {c: render_glyph(font, c, gamma) for c in chars}

    if tabular:
        digits = [c for c in "0123456789" if c in glyphs]
        widest = max(glyphs[c][0] for c in digits)
        for c in digits:
            adv, left, top, w, h, rows = glyphs[c]
            glyphs[c] = (widest, left + round((widest - adv) / 2), top, w, h, rows)

    cap_top = glyphs["H"][2] if "H" in glyphs else glyphs["0"][2]
    ascent, descent = font.getmetrics()

    bitmap = bytearray()
    table = []
    for c in chars:
        adv, left, top, w, h, rows = glyphs[c]
        offset = len(bitmap)
        for r in rows:
            r = r + [0] * (len(r) & 1)
            for i in range(0, len(r), 2):
                bitmap.append((r[i] << 4) | r[i + 1])
        table.append((ord(c), round((adv + tracking) * 16), w, h, left, top, offset, c))

    meta = dict(
        cap=-cap_top,
        ascent=round(ascent / SUPERSAMPLE),
        descent=round(descent / SUPERSAMPLE),
    )
    return bitmap, table, meta


def c_char_comment(c):
    # repr() always ends in a quote, so a backslash can't splice the next line.
    return repr(c) if c.isprintable() and c != " " else f"U+{ord(c):04X}"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font-dir", action="append", default=[])
    args = ap.parse_args()
    dirs = args.font_dir + DEFAULT_FONT_DIRS

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "src", "render")

    cpp = ["// Generated by tools/make_fonts.py. Do not edit; rerun the script.",
           "// Sora (c) The Sora Project Authors, SIL Open Font License 1.1.",
           "", '#include "fonts.h"', ""]
    hdr = ["// Generated by tools/make_fonts.py. Do not edit; rerun the script.",
           "#pragma once", "", '#include "font.h"', ""]

    for cname, fname, size, chars, tabular, gamma, tracking in FONTS:
        path = find_font(fname, dirs)
        bitmap, table, meta = build_font(cname, path, size, chars, tabular, gamma, tracking)

        cpp.append(f"// {fname} at {size} px, {len(table)} glyphs, {len(bitmap)} bytes")
        cpp.append(f"static const uint8_t {cname}_bitmap[] = {{")
        for i in range(0, len(bitmap), 24):
            cpp.append("    " + ",".join(f"0x{b:02X}" for b in bitmap[i:i + 24]) + ",")
        cpp.append("};")
        cpp.append(f"static const Glyph {cname}_glyphs[] = {{")
        for code, adv, w, h, left, top, offset, c in table:
            cpp.append(f"    {{0x{code:04X}, {adv}, {w}, {h}, {left}, {top}, {offset}}},"
                       f"  // {c_char_comment(c)}")
        cpp.append("};")
        cpp.append(f"const Font {cname} = {{{cname}_bitmap, {cname}_glyphs, {len(table)}, "
                   f"{meta['cap']}, {meta['ascent']}, {meta['descent']}}};")
        cpp.append("")
        hdr.append(f"extern const Font {cname};  // {fname}, {size} px")
        print(f"{cname}: {fname} {size}px, {len(table)} glyphs, {len(bitmap)} bytes")

    with open(os.path.join(out_dir, "fonts.cpp"), "w") as f:
        f.write("\n".join(cpp))
    with open(os.path.join(out_dir, "fonts.h"), "w") as f:
        f.write("\n".join(hdr) + "\n")


if __name__ == "__main__":
    main()
