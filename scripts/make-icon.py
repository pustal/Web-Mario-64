#!/usr/bin/env python3
# Usage: scripts/make-icon.py webos [logo.png]   (needs Pillow: pip install pillow)
"""Draws the Web Mario 64 app icon as original pixel art: rolling green hills
under a banded sky with clouds and a golden star, with a red ribbon carrying the
name ("WEB MARIO" over "64") so the app is easy to spot in the webOS app list.
No characters, logos or game artwork are used; the letters are font8x8 (public
domain, the same font the app draws its own text with, read from
src/port/BitmapFont.cpp). The scene is rendered on a cell grid and upscaled 2x
with nearest-neighbour; the ribbon and letters are drawn at the final size, so
they stay crisp on the TV. After SonicWebMania's and Sonic 3 A.I.R. for webOS's
make-icon.py (the ribbon and the lettering)."""
import math, os, re, sys
from PIL import Image

SKY = [(40, 70, 190), (60, 110, 225), (92, 150, 245), (136, 192, 255), (190, 226, 255)]
CLOUD, CLOUD_SHADE = (250, 252, 255), (210, 226, 248)
HILL_BACK, HILL_BACK_DARK = (60, 170, 90), (44, 140, 72)
HILL, HILL_DARK, HILL_LIGHT = (70, 196, 70), (46, 150, 52), (130, 226, 96)
STAR, STAR_LIGHT, STAR_DARK = (255, 214, 40), (255, 244, 150), (226, 150, 20)

HORIZON = 0.56     # Where the far hills meet the sky.
RIBBON_TOP = 0.60  # Top of the name ribbon, on the hills.


def in_star(u, v, cx, cy, outer, inner):
    """Whether (u, v) is inside a five-pointed star centred at (cx, cy), one point up."""
    angle = math.atan2(v - cy, u - cx) + math.pi / 2
    dist = math.hypot(u - cx, v - cy)
    sector = (angle % (2 * math.pi / 5)) / (2 * math.pi / 5)  # 0 at a point, 0.5 between two
    edge = outer + (inner - outer) * (1 - abs(sector - 0.5) * 2)
    # The edge between a point and the next notch is close enough to a straight line at this size.
    return dist <= edge


def pixel(u, v):
    """Colour at normalised position (u, v) in [0,1), or None for transparent."""
    # Rounded-square icon mask.
    r = 0.18
    cx = min(max(u, r), 1 - r); cy = min(max(v, r), 1 - r)
    if (u - cx) ** 2 + (v - cy) ** 2 > r * r:
        return None

    # The star, upper right, lit from the top left.
    sx, sy = 0.66, 0.26
    if in_star(u, v, sx, sy, 0.17, 0.075):
        if in_star(u, v, sx, sy, 0.10, 0.045):
            return STAR_LIGHT if u + v < sx + sy else STAR
        return STAR_DARK if u + v > sx + sy + 0.06 else STAR

    # Near hills: two rounded hills with stripes, in front of the far ones.
    for (hx, top, rad) in [(0.22, 0.52, 0.34), (0.86, 0.60, 0.30)]:
        if (u - hx) ** 2 + ((v - top - rad) * 0.9) ** 2 < rad * rad and v >= top:
            if (u - hx) ** 2 + ((v - top - rad) * 0.9) ** 2 > (rad * 0.82) ** 2 and v < top + rad * 0.5:
                return HILL_LIGHT  # The lit rim.
            return HILL_DARK if int((u - hx + 1) * 22) % 3 == 0 else HILL
    # Far hills along the horizon.
    far = HORIZON - 0.05 * math.sin(u * 7.5) - 0.03 * math.sin(u * 19)
    if v >= far:
        return HILL_BACK_DARK if int(v * 40) % 4 == 0 else HILL_BACK

    # Clouds.
    for (cx2, cy2, rad) in [(0.20, 0.18, 0.07), (0.29, 0.16, 0.06), (0.36, 0.19, 0.045), (0.14, 0.40, 0.045), (0.22, 0.42, 0.035)]:
        d = (u - cx2) ** 2 + ((v - cy2) * 1.6) ** 2
        if d < rad * rad:
            return CLOUD_SHADE if v > cy2 + rad * 0.25 else CLOUD

    # Banded sky.
    return SKY[min(len(SKY) - 1, int(v / HORIZON * len(SKY)))]

# ---- The name ribbon ----------------------------------------------------------

RIBBON = (206, 32, 44)
RIBBON_LIGHT = (240, 78, 76)
RIBBON_DARK = (130, 14, 30)
RIBBON_FOLD = (96, 8, 22)
TITLE_FILL = [(255, 236, 92), (255, 206, 40), (248, 150, 24)]  # top to bottom
SUBTITLE_FILL = (255, 255, 255)
OUTLINE = (24, 14, 50)


def load_font():
    """Glyphs of font8x8 from the app's BitmapFont.cpp: {char: 8 row bytes, bit 0 = leftmost}."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source = open(os.path.join(root, "src", "port", "BitmapFont.cpp"), encoding="utf-8").read()
    rows = re.findall(r"\{ (0x[0-9A-F]{2}(?:, 0x[0-9A-F]{2}){7}) \}", source)
    return {chr(0x20 + i): [int(v, 16) for v in row.split(", ")] for i, row in enumerate(rows)}


FONT = load_font()


def text_mask(text, scale, bold):
    """
    Set of (x, y) pixels of @text at @scale (bold: each glyph pixel also one to
    the right). Letters are spaced by their drawn width, not the font's fixed
    8-pixel cell, so narrow letters (I) and wide ones (M) get the same gap.
    """
    pixels = set()
    gap = scale  # One font pixel between letters.
    pen = 0
    for ch in text:
        columns = [gx for bits in FONT[ch] for gx in range(8) if bits >> gx & 1]
        if not columns:  # A space: a gap three font pixels wide.
            pen += 3 * scale
            continue
        first, last = min(columns), max(columns)
        for gy, bits in enumerate(FONT[ch]):
            for gx in range(first, last + 1):
                if bits >> gx & 1:
                    for sy in range(scale):
                        for sx in range(scale + (1 if bold else 0)):
                            pixels.add((pen + (gx - first) * scale + sx, gy * scale + sy))
        pen += (last - first + 1) * scale + (1 if bold else 0) + gap
    return pixels


def bounds(mask):
    xs = [x for x, _ in mask]; ys = [y for _, y in mask]
    return min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def draw_text(img, mask, left, top, fill):
    """Draws @mask at (left, top) with a 1-pixel outline; @fill is a colour or a top-to-bottom list."""
    x0, y0, w, h = bounds(mask)
    outline = {(x + dx, y + dy) for x, y in mask for dx in (-1, 0, 1) for dy in (-1, 0, 1)} - mask
    for x, y in outline:
        img.putpixel((left + x - x0, top + y - y0), OUTLINE + (255,))
    for x, y in mask:
        colour = fill[min(len(fill) - 1, (y - y0) * len(fill) // h)] if isinstance(fill, list) else fill
        img.putpixel((left + x - x0, top + y - y0), colour + (255,))


def draw_ribbon(img, top, height):
    """A banner across the icon: lit top edge, shaded bottom edge, folded ends that hang lower."""
    size = img.width
    fold_w, drop = max(4, size // 12), max(2, height // 5)
    for y in range(top, top + height + drop):
        for x in range(size):
            if img.getpixel((x, y))[3] == 0:
                continue  # Outside the rounded icon.
            in_band = top <= y < top + height and fold_w <= x < size - fold_w
            in_fold = (x < fold_w or x >= size - fold_w) and top + drop <= y < top + height + drop
            if in_band:
                rel = y - top
                colour = RIBBON_LIGHT if rel < max(1, height // 10) else RIBBON_DARK if rel >= height - max(1, height // 8) else RIBBON
            elif in_fold:
                # The folded-back ends, darker, with a notch cut into the outer edge.
                mid = top + drop + height // 2
                notch = (x < fold_w and x < abs(y - mid) * fold_w // (height // 2 + 1) - fold_w // 2 + 2) or \
                        (x >= size - fold_w and size - 1 - x < abs(y - mid) * fold_w // (height // 2 + 1) - fold_w // 2 + 2)
                if notch:
                    continue
                colour = RIBBON_FOLD
            else:
                continue
            img.putpixel((x, y), colour + (255,))


def add_name(img, title_scale, subtitle_scale):
    """The ribbon with "WEB MARIO" over "64", centred horizontally on the lower part of the icon."""
    size = img.width
    # Bold from 2x up: at 1x the extra column would close the gap between letters.
    title    = text_mask("WEB MARIO", title_scale, title_scale >= 2)
    subtitle = text_mask("64", subtitle_scale, subtitle_scale >= 2)
    _, _, tw, th = bounds(title)
    _, _, sw, sh = bounds(subtitle)
    gap = max(1, title_scale)
    pad = max(2, title_scale + 1)
    height = th + gap + sh + 2 * pad
    top = int(size * RIBBON_TOP)
    draw_ribbon(img, top, height)
    # The name in white, the big "64" in gold.
    draw_text(img, title, (size - tw) // 2, top + pad, SUBTITLE_FILL)
    draw_text(img, subtitle, (size - sw) // 2, top + pad + th + gap, TITLE_FILL)


def render(cells, scale, title_scale, subtitle_scale, path):
    img = Image.new("RGBA", (cells, cells), (0, 0, 0, 0))
    for y in range(cells):
        for x in range(cells):
            c = pixel((x + 0.5) / cells, (y + 0.5) / cells)
            if c is not None:
                img.putpixel((x, y), c + (255,))
    img = img.resize((cells * scale, cells * scale), Image.NEAREST)
    add_name(img, title_scale, subtitle_scale)
    img.save(path)


out = sys.argv[1]
# (cells, upscale, scale of "WEB MARIO", scale of "64")
render(40, 2, 1, 1, f"{out}/icon.png")         # 80x80
render(65, 2, 1, 2, f"{out}/largeIcon.png")    # 130x130
if len(sys.argv) > 2:
    render(128, 2, 3, 4, sys.argv[2])          # 256x256, the README logo

