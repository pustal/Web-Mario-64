#!/usr/bin/env python3
"""Draws the Web Mario 64 logo and the webOS app icons.

The artwork is an original illustration in a soft, flat style (like the
Animal Crossing for webOS icon): a warm sky with clouds, a golden star,
rolling green hills with a fairy-tale castle on the left one, and the app's
name in chunky, multicoloured, extruded lettering.

It is written as SVG (docs/images/logo.svg, the source) and rendered with
Chromium (Python Playwright), then shrunk with Pillow:
  docs/images/logo.png   512 x 512 (README)
  webos/largeIcon.png    130 x 130
  webos/icon.png          80 x  80

The lettering uses Titan One (Rodrigo Fuenzalida, SIL Open Font License 1.1),
read from a font file given on the command line; it is not part of the
repository. One way to get it: npm pack @fontsource/titan-one, then pass
package/files/titan-one-latin-400-normal.woff2.

Usage: python3 scripts/make-logo.py <titan-one.woff2>
"""
import base64
import math
import pathlib
import sys

from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIZE = 512          # SVG units (the logo's own size)
RENDER = 1024       # rendered at twice the size, then shrunk (smoother edges)
CORNER = 104        # rounded corners of the icon


def star_points(cx: float, cy: float, outer: float, inner: float, rotation: float = -90.0) -> str:
    """The ten points of a five-pointed star, as an SVG points list."""
    points = []
    for i in range(10):
        radius = outer if i % 2 == 0 else inner
        angle = math.radians(rotation + i * 36)
        points.append(f"{cx + radius * math.cos(angle):.1f},{cy + radius * math.sin(angle):.1f}")
    return " ".join(points)


def sky_and_scenery() -> str:
    """Everything but the lettering."""
    star = star_points(392, 118, 62, 28)
    star_highlight = star_points(386, 108, 30, 13)
    return f"""
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#8fd3f4"/>
      <stop offset="0.55" stop-color="#bfe6f0"/>
      <stop offset="0.85" stop-color="#ffe3a8"/>
    </linearGradient>
    <linearGradient id="hillBack" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#7cc96b"/>
      <stop offset="1" stop-color="#4fa34f"/>
    </linearGradient>
    <linearGradient id="hillLeft" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#9adf7a"/>
      <stop offset="1" stop-color="#5db657"/>
    </linearGradient>
    <linearGradient id="hillFront" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#8bd66f"/>
      <stop offset="1" stop-color="#4c9e48"/>
    </linearGradient>
    <linearGradient id="wall" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#fffaf0"/>
      <stop offset="1" stop-color="#eadfca"/>
    </linearGradient>
    <linearGradient id="roof" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#f05a50"/>
      <stop offset="1" stop-color="#c8333a"/>
    </linearGradient>
    <radialGradient id="glow" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#fff6c8" stop-opacity="0.9"/>
      <stop offset="1" stop-color="#fff6c8" stop-opacity="0"/>
    </radialGradient>
    <clipPath id="card"><rect width="{SIZE}" height="{SIZE}" rx="{CORNER}"/></clipPath>
  </defs>
  <g clip-path="url(#card)">
    <rect width="{SIZE}" height="{SIZE}" fill="url(#sky)"/>

    <!-- clouds -->
    <g fill="#fffaf0" opacity="0.92">
      <ellipse cx="96" cy="118" rx="62" ry="24"/>
      <ellipse cx="128" cy="100" rx="38" ry="26"/>
      <ellipse cx="74" cy="104" rx="28" ry="20"/>
      <ellipse cx="248" cy="196" rx="52" ry="18"/>
      <ellipse cx="270" cy="184" rx="30" ry="18"/>
    </g>

    <!-- the star, with a soft glow -->
    <circle cx="392" cy="118" r="104" fill="url(#glow)"/>
    <polygon points="{star}" fill="#ffc928" stroke="#ffc928" stroke-width="18" stroke-linejoin="round"/>
    <polygon points="{star}" fill="none" stroke="#e59a12" stroke-width="5" stroke-linejoin="round" opacity="0.55"/>
    <polygon points="{star_highlight}" fill="#fff1a6" stroke="#fff1a6" stroke-width="6" stroke-linejoin="round" opacity="0.85"/>

    <!-- hills: one far on the right, the castle's on the left, the meadow in front -->
    <ellipse cx="420" cy="372" rx="230" ry="120" fill="url(#hillBack)"/>
    <ellipse cx="120" cy="352" rx="210" ry="128" fill="url(#hillLeft)"/>
    <ellipse cx="256" cy="520" rx="400" ry="150" fill="url(#hillFront)"/>
    <path d="M -10 306 Q 60 268 150 274" fill="none" stroke="#c4f0a0" stroke-width="6" stroke-linecap="round" opacity="0.7"/>

    {castle()}
  </g>
"""


def castle() -> str:
    """A small fairy-tale castle standing on the left hill: cream walls, red roofs."""
    return """
    <g transform="translate(118 232)">
      <!-- shadow on the grass -->
      <ellipse cx="0" cy="12" rx="78" ry="10" fill="#3f8a3f" opacity="0.35"/>
      <!-- side towers -->
      <rect x="-70" y="-58" width="30" height="70" rx="3" fill="url(#wall)"/>
      <rect x="40" y="-58" width="30" height="70" rx="3" fill="url(#wall)"/>
      <path d="M -76 -56 L -55 -104 L -34 -56 Z" fill="url(#roof)" stroke="#a82a31" stroke-width="2" stroke-linejoin="round"/>
      <path d="M 34 -56 L 55 -104 L 76 -56 Z" fill="url(#roof)" stroke="#a82a31" stroke-width="2" stroke-linejoin="round"/>
      <!-- the main hall -->
      <rect x="-46" y="-40" width="92" height="52" rx="3" fill="url(#wall)"/>
      <path d="M -52 -38 L 0 -70 L 52 -38 Z" fill="url(#roof)" stroke="#a82a31" stroke-width="2" stroke-linejoin="round"/>
      <!-- the central tower -->
      <rect x="-17" y="-104" width="34" height="76" rx="3" fill="url(#wall)"/>
      <path d="M -23 -102 L 0 -150 L 23 -102 Z" fill="url(#roof)" stroke="#a82a31" stroke-width="2" stroke-linejoin="round"/>
      <!-- flag -->
      <line x1="0" y1="-150" x2="0" y2="-170" stroke="#7a5a3a" stroke-width="2.5" stroke-linecap="round"/>
      <path d="M 1 -170 L 17 -165 L 1 -160 Z" fill="#ffc928"/>
      <!-- windows, door, the round window of the tower -->
      <circle cx="0" cy="-82" r="9" fill="#ffd968" stroke="#c99a2e" stroke-width="2"/>
      <path d="M -55 -38 v -8 a 4 4 0 0 1 8 0 v 8 Z" fill="#6b84c9"/>
      <path d="M 47 -38 v -8 a 4 4 0 0 1 8 0 v 8 Z" fill="#6b84c9"/>
      <path d="M -30 -20 v -8 a 4 4 0 0 1 8 0 v 8 Z" fill="#6b84c9"/>
      <path d="M 22 -20 v -8 a 4 4 0 0 1 8 0 v 8 Z" fill="#6b84c9"/>
      <path d="M -11 12 v -18 a 11 11 0 0 1 22 0 v 18 Z" fill="#8a5a36"/>
      <!-- a path down the hill -->
      <path d="M -6 13 Q -14 34 -2 58 L 10 58 Q 0 34 6 13 Z" fill="#f2dfa8" opacity="0.9"/>
    </g>
"""


def lettering() -> str:
    """'WEB' small on top, 'MARIO' big with one colour per letter, then '64', all extruded."""
    out = []
    # "WEB": cream with a dark outline, like a ribbon over the meadow.
    out.append(text_block("WEB", 262, 304, 44, ["#fff7e0"] * 3, tilt=[-4, 0, 4], depth=4))
    # "MARIO": an arch of chunky, multicoloured letters.
    colours = ["#e8383d", "#2f7de1", "#ffc928", "#36b24a", "#e8383d"]
    out.append(text_block("MARIO", 256, 402, 104, colours, tilt=[-12, -6, 0, 6, 12], arch=18, depth=8))
    # "64": big, yellow and blue.
    out.append(text_block("64", 256, 476, 76, ["#ffc928", "#2f7de1"], tilt=[-6, 6], depth=7))
    return "\n".join(out)


# Advance of Titan One's capitals and digits, in ems (measured), plus the outline's room.
ADVANCE = {"W": 1.02, "E": 0.66, "B": 0.72, "M": 0.98, "A": 0.80, "R": 0.74, "I": 0.40, "O": 0.82, "6": 0.70, "4": 0.72}
GAP = 0.08  # ems between letters (the thick outlines need it)


def text_block(word: str, cx: float, baseline: float, size: float, colours: list,
               tilt: list, arch: float = 0.0, depth: int = 6) -> str:
    """One word, letter by letter: an extruded dark body, a white rim, the coloured face and a highlight."""
    n = len(word)
    widths = [(ADVANCE.get(letter, 0.7) + GAP) * size for letter in word]
    left = cx - sum(widths) / 2
    parts = []
    for i, letter in enumerate(word):
        offset = (i - (n - 1) / 2)
        x = left + sum(widths[:i]) + widths[i] / 2
        # The arch: letters away from the centre sit a little lower.
        y = baseline + arch * (offset / max(1, (n - 1) / 2)) ** 2
        angle = tilt[i]
        colour = colours[i]
        face = f'font-family="Titan One" font-size="{size}" text-anchor="middle"'
        group = [f'<g transform="translate({x:.1f} {y:.1f}) rotate({angle})">']
        # Extrusion: the letter repeated downwards and to the right in a dark tone.
        for d in range(depth, 0, -1):
            group.append(f'<text {face} x="{d * 0.45:.2f}" y="{d:.2f}" fill="#3a2a5a" stroke="#3a2a5a" '
                         f'stroke-width="{size * 0.16:.1f}" stroke-linejoin="round">{letter}</text>')
        # The outline and a thin light rim, then the face.
        group.append(f'<text {face} fill="{colour}" stroke="#3a2a5a" stroke-width="{size * 0.16:.1f}" '
                     f'stroke-linejoin="round" paint-order="stroke">{letter}</text>')
        group.append(f'<text {face} fill="{colour}" stroke="#ffffff" stroke-width="{size * 0.045:.1f}" '
                     f'stroke-linejoin="round" paint-order="stroke" stroke-opacity="0.8">{letter}</text>')
        # A soft highlight across the top of the face.
        group.append(f'<text {face} fill="url(#shine)">{letter}</text>')
        group.append("</g>")
        parts.append("\n".join(group))
    return "\n".join(parts)


def svg(font_url: str) -> str:
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">
  <style>@font-face {{ font-family: "Titan One"; src: url("{font_url}"); }}</style>
  <defs>
    <linearGradient id="shine" x1="0" y1="0" x2="0" y2="1" gradientUnits="objectBoundingBox">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.55"/>
      <stop offset="0.45" stop-color="#ffffff" stop-opacity="0.08"/>
      <stop offset="0.5" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
  </defs>
  {sky_and_scenery()}
  <g clip-path="url(#card)">
    <!-- the meadow darkens a little under the lettering, so it reads at small sizes -->
    <ellipse cx="256" cy="430" rx="250" ry="92" fill="#2f6e33" opacity="0.18"/>
    {lettering()}
  </g>
</svg>"""


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    font_path = pathlib.Path(sys.argv[1]).resolve()
    font_url = "data:font/woff2;base64," + base64.b64encode(font_path.read_bytes()).decode()

    images = ROOT / "docs" / "images"
    images.mkdir(parents=True, exist_ok=True)
    # The source keeps a reference to the font by name only (no embedded font in the repository).
    (images / "logo.svg").write_text(svg("titan-one-latin-400-normal.woff2"), encoding="utf-8")

    rendered = images / "logo-render.png"
    page_html = f'<html><body style="margin:0;background:transparent">{svg(font_url)}</body></html>'
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        page = browser.new_page(viewport={"width": SIZE, "height": SIZE}, device_scale_factor=RENDER / SIZE)
        page.set_content(page_html)
        page.evaluate("document.fonts.ready")
        page.wait_for_timeout(300)
        page.locator("svg").screenshot(path=str(rendered), omit_background=True)
        browser.close()

    art = Image.open(rendered).convert("RGBA")
    rendered.unlink()
    art.resize((512, 512), Image.LANCZOS).save(images / "logo.png", optimize=True)
    art.resize((130, 130), Image.LANCZOS).save(ROOT / "webos" / "largeIcon.png", optimize=True)
    art.resize((80, 80), Image.LANCZOS).save(ROOT / "webos" / "icon.png", optimize=True)
    print("wrote docs/images/logo.svg, docs/images/logo.png, webos/largeIcon.png, webos/icon.png")


if __name__ == "__main__":
    main()
