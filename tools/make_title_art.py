#!/usr/bin/env python3
"""The port's own title art: a Roman numeral IV beside the game's logo and a RECOMP wordmark under
it (runtime/titleart.c draws them on the title screen in Enhanced mode).

  python tools/make_title_art.py          res/title/*.png -> runtime/titleart_data.h
  python tools/make_title_art.py --draw   first redraw the PNGs from the shapes below

The PNGs are the source: touch them up in any pixel editor, then run this. Every pixel is either
transparent or one of the title screen's own colours (its palette line 0, below), because the port
draws the art with the palette the game has loaded: it fades with the screen. The shapes are the
port's own, drawn here; nothing comes from the ROM.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "res" / "title"
OUT = ROOT / "runtime" / "titleart_data.h"

# The title screen's palette line 0, fully lit (index -> RGB): white, the logo's gold ramp, black,
# the subtitle's teal ramp.
PAL = {1: (255, 255, 255), 2: (255, 255, 72), 3: (255, 218, 0), 4: (255, 182, 0), 5: (218, 145, 0),
       6: (218, 109, 0), 7: (182, 72, 0), 8: (145, 36, 0), 9: (0, 0, 0),
       10: (218, 255, 255), 11: (145, 218, 255), 12: (72, 182, 182), 13: (0, 145, 109), 14: (0, 109, 72),
       15: (0, 72, 36)}

# name, file, where its top left goes on the 320x224 screen
PIECES = [("IV", "iv.png", 216, 85),            # right of STAR, on its line
          ("RECOMP", "recomp.png", 215, 120)]   # under the numeral

SS = 8                                           # shapes are drawn this much larger, then reduced


def raster(polys, w, h):
    big = Image.new("L", (w * SS, h * SS), 0)
    d = ImageDraw.Draw(big)
    for p in polys:
        d.polygon([(x * SS, y * SS) for x, y in p], fill=255)
    small = big.resize((w, h), Image.BOX)
    return [[small.getpixel((x, y)) >= 128 for x in range(w)] for y in range(h)]


def run(m, x, y, dx, dy):
    """how many pixels of the shape follow (x, y) in one direction"""
    n, h, w = 0, len(m), len(m[0])
    x += dx
    y += dy
    while 0 <= x < w and 0 <= y < h and m[y][x]:
        n += 1
        x += dx
        y += dy
    return n


def outline(a):
    """a black line round the shape"""
    h, w = len(a), len(a[0])
    res = [row[:] for row in a]
    for y in range(h):
        for x in range(w):
            if a[y][x]:
                continue
            near = [a[yy][xx] for yy in range(max(0, y - 1), min(h, y + 2)) for xx in range(max(0, x - 1), min(w, x + 2))]
            if any(near):
                res[y][x] = 9
    return res


def shade_gold(m):
    """lit from the upper left: bright to dark across each stroke, a white glint down the tall
    ones, the under-edges a shade darker"""
    h, w = len(m), len(m[0])
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if not m[y][x]:
                continue
            l, r, u, d = run(m, x, y, -1, 0), run(m, x, y, 1, 0), run(m, x, y, 0, -1), run(m, x, y, 0, 1)
            upright = l + r <= u + d                 # an upright stroke is shaded across, a flat one down
            n, i = (l + r + 1, l) if upright else (u + d + 1, u)
            idx = 2 + round(i / (n - 1) * 5) if n > 1 else 3      # 2 (yellow) .. 7 (brown)
            if upright and n >= 5 and i == 1 and u + d + 1 >= 8 and 0.18 <= u / (u + d + 1) <= 0.5:
                idx = 1                              # the glint
            if d == 0 and idx < 7 and (not upright or n <= 3):
                idx += 1
            out[y][x] = min(idx, 8)
    return outline(out)


def numeral():
    ox, oy = 2, 1                                    # room for the black line

    def at(pts):
        return [(x + ox, y + oy) for x, y in pts]

    polys = [
        # I: a slim stem, wedge serifs drawn out to points
        at([(2.5, 2), (7.5, 2), (7.5, 30), (2.5, 30)]),
        at([(-1, 0), (11, 0), (8.2, 2.4), (7.5, 6), (2.5, 6), (1.8, 2.4)]),
        at([(2.5, 26), (7.5, 26), (8.2, 29.6), (11, 32), (-1, 32), (1.8, 29.6)]),
        # V: a heavy left arm, a hairline right one, meeting in a point
        at([(14, 2), (19.5, 2), (25.2, 23.5), (30.3, 2), (33, 2), (25.9, 32.4), (23.8, 32.4)]),
        at([(11.5, 0), (22, 0), (20, 2.4), (19.5, 5), (14.2, 5), (13.6, 2.4)]),
        at([(28, 0), (35.5, 0), (33.4, 2.4), (33, 4), (30.3, 4), (30, 2.4)]),
    ]
    return shade_gold(raster(polys, 39, 34))


FONT = {
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
}


def wordmark(text="RECOMP", ramp=(10, 10, 11, 11, 12, 12, 13), gap=2):
    """small capitals, light to deep teal down each letter (the subtitle's colours)"""
    cols = []
    for ch in text:
        g = FONT[ch]
        cols += [[g[y][x] == "1" for y in range(7)] for x in range(len(g[0]))] + [[False] * 7] * gap
    cols = cols[:-gap]
    out = [[0] * (len(cols) + 2) for _ in range(9)]
    for x, c in enumerate(cols):
        for y in range(7):
            if c[y]:
                out[y + 1][x + 1] = ramp[y]
    return outline(out)


def save(a, path):
    im = Image.new("RGBA", (len(a[0]), len(a)), (0, 0, 0, 0))
    for y, row in enumerate(a):
        for x, c in enumerate(row):
            if c:
                im.putpixel((x, y), PAL[c] + (255,))
    im.save(path)


def load(path):
    index = {rgb: i for i, rgb in PAL.items()}
    im = Image.open(path).convert("RGBA")
    rows = []
    for y in range(im.height):
        row = ""
        for x in range(im.width):
            r, g, b, alpha = im.getpixel((x, y))
            if alpha == 0:
                row += "0"
            elif alpha == 255 and (r, g, b) in index:
                row += "%X" % index[(r, g, b)]
            else:
                sys.exit(f"{path.name}: pixel {x},{y} = {(r, g, b, alpha)} is not transparent or one of the title's colours")
        rows.append(row)
    return rows


def main():
    if "--draw" in sys.argv[1:]:
        ART.mkdir(parents=True, exist_ok=True)
        save(numeral(), ART / "iv.png")
        save(wordmark(), ART / "recomp.png")
    lines = ["/* Generated by tools/make_title_art.py from the PNGs in res/title: edit those, not this.",
             " * The port's own title art (titleart.c): each digit a colour of the title screen's",
             " * palette line 0 (0 = nothing; 1 white, 2-8 the logo's gold, 9 black, A-F the",
             " * subtitle's teal). */", ""]
    for name, file, x, y in PIECES:
        rows = load(ART / file)
        lines.append(f"static const char *const TITLE_{name}_ROWS[] = {{")
        lines += [f'    "{r}",' for r in rows]
        lines.append("};")
        lines.append(f"static const TitleArt TITLE_{name} = {{{x}, {y}, {len(rows[0])}, {len(rows)}, TITLE_{name}_ROWS}};")
        lines.append("")
        print(f"{file}: {len(rows[0])}x{len(rows)} at {x},{y}")
    OUT.write_text("\n".join(lines), encoding="ascii", newline="\n")
    print("wrote", OUT.relative_to(ROOT))


if __name__ == "__main__":
    main()
