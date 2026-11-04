#!/usr/bin/env python3
"""List the sprites in a port dump and find overlapping opaque pixels.

    python tools/bizhawk/sprites.py build/bizhawk/stuckbox.port

Reads PREFIX.vram.bin and PREFIX.regs.bin (VDP registers + the VDP's sprite
cache, written by psiv_headless --dump-at / --replay). Uses the cache for
each sprite's Y, size and link and VRAM for its pattern and X, like the
hardware, and reports every pixel where two sprites' opaque pixels meet
(which sets the VDP's collision flag), including a 32-pixel margin each side.
"""
import sys

prefix = sys.argv[1]
v = open(prefix + ".vram.bin", "rb").read()
regs = open(prefix + ".regs.bin", "rb").read()
reg, cache = regs[:24], regs[24:]
h40 = bool(reg[12] & 0x81)
width = 320 if h40 else 256
base = (reg[5] << 9) & (0xFC00 if h40 else 0xFE00)
print(f"sprite table ${base:04X} (reg 5 = ${reg[5]:02X}), {'H40' if h40 else 'H32'}")

sprites, link, n = [], 0, 0
while True:
    c = cache[link * 8: link * 8 + 4]
    s = v[base + link * 8: base + link * 8 + 8]
    sy = ((c[0] << 8 | c[1]) & 0x3FF) - 128
    vs, hs = (c[2] & 3) + 1, ((c[2] >> 2) & 3) + 1
    attr = s[4] << 8 | s[5]
    sx = ((s[6] << 8 | s[7]) & 0x1FF) - 128
    sprites.append((link, sx, sy, hs * 8, vs * 8, attr))
    link = c[3] & 0x7F
    n += 1
    if not link or n >= (80 if h40 else 64):
        break
for s in sprites:
    print(f"  #{s[0]:2d}  x {s[1]:4d}  y {s[2]:4d}  {s[3]}x{s[4]}  attr ${s[5]:04X}")


def px(attr, tx, ty, w, h):
    if attr & 0x0800:
        tx = w - 1 - tx
    if attr & 0x1000:
        ty = h - 1 - ty
    tile = (attr & 0x7FF) + (tx >> 3) * (h // 8) + (ty >> 3)
    b = v[(tile * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)) & 0xFFFF]
    return (b & 0xF) if (tx & 1) else (b >> 4)


hits = []
for y in range(224):
    seen = {}
    for (i, sx, sy, w, h, attr) in [s for s in sprites if s[2] <= y < s[2] + s[4]]:
        for cx in range(w):
            x = sx + cx
            if -32 <= x < width + 32 and px(attr, cx, y - sy, w, h):
                if x in seen:
                    hits.append((y, x, seen[x], i))
                else:
                    seen[x] = i
print(f"overlapping opaque pixels: {len(hits)}", hits[:8])
