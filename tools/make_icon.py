"""The port's icon: an original four-point star on a window of the game's own
colours -- the navy fill and the grey bevel of its dialogue windows ($00246D;
$6D6D6D / $B6B6B6), the star in the Mega Drive's palette steps (multiples of
$24) -- drawn here from geometry. Nothing is taken from the game's art or its
logo.

  python tools/make_icon.py [--preview DIR]

writes res/psiv.ico (16-256 px, each size drawn at that size), res/psiv.png
(512 px, for the README) and runtime/icon.h (64 px, the window's icon where
the exe's own isn't used); --preview also writes every size on dark and light.
"""
import argparse, os
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAVY = (0x00, 0x24, 0x6D)
BEVEL_DARK = (0x6D, 0x6D, 0x6D)
BEVEL_LIGHT = (0xB6, 0xB6, 0xB6)
HALO = (0xFF, 0xDA, 0x48)
STAR = [(0.00, (0xFF, 0xFF, 0xFF)), (0.12, (0xFF, 0xFF, 0xFF)), (0.32, (0xFF, 0xFF, 0xB6)),
        (0.60, (0xFF, 0xDA, 0x48)), (1.00, (0xFF, 0xB6, 0x00))]
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def ramp(t, stops):
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,))
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        m = (t >= t0) & (t <= t1)
        k = ((t - t0) / (t1 - t0))[m][:, None]
        out[m] = np.array(c0) * (1 - k) + np.array(c1) * k
    return out


def rounded_box(u, v, half, r):
    """signed distance to a rounded square (negative inside)"""
    qx, qy = np.abs(u) - (half - r), np.abs(v) - (half - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def render(S):
    """the icon at S x S, drawn at that size (supersampled), RGBA"""
    ss = 8 if S <= 64 else 4
    N = S * ss
    g = (np.arange(N) + 0.5) / N * 2 - 1
    v, u = np.meshgrid(g, g, indexing='ij')
    px = 2.0 / S                                    # one output pixel, in these units
    rgba = np.zeros((N, N, 4))

    # the window: navy, a soft light behind the star, the game's grey bevel
    d = rounded_box(u, v, 1 - max(0.04, 0.5 * px), 0.20)
    inside = d < 0
    t = -d / (max(0.075, 3 * px) if S >= 24 else 2 * px)   # 0 at the edge .. 1 inside the bevel
    col = np.zeros((N, N, 3)) + NAVY
    col += (np.array([0x24, 0x48, 0x91]) - np.array(NAVY)) * np.exp(-(u ** 2 + v ** 2) / 0.30)[..., None] * 0.55
    if S >= 24:
        col[(t < 0.25)] = BEVEL_DARK
        col[(t >= 0.25) & (t < 0.75)] = BEVEL_LIGHT
        col[(t >= 0.75) & (t < 1)] = BEVEL_DARK
    else:                                           # tiny: one light line
        col[t < 1] = BEVEL_LIGHT
    rgba[..., :3] = col
    rgba[..., 3] = inside

    def star(cx, cy, a, b, p, alpha=1.0, lit=True, halo=0.0, tint=(0xFF, 0xFF, 0xFF)):
        """a four-point star: |x/a|^p + |y/b|^p < 1 (p < 1 draws its sides in)"""
        inn = (np.abs((u - cx) / a) ** p + np.abs((v - cy) / b) ** p) < 1
        if halo:
            h = np.exp(-np.hypot(u - cx, v - cy) ** 2 / (2 * (max(a, b) * 0.40) ** 2)) * halo * inside
            rgba[..., :3] = rgba[..., :3] * (1 - h[..., None]) + np.array(HALO) * h[..., None]
        c = ramp(np.hypot((u - cx) / a, (v - cy) / b) ** 0.8, STAR) if lit else np.array(tint)
        w = (inn * alpha)[..., None]
        rgba[..., :3] = rgba[..., :3] * (1 - w) + c * w

    k = 1.0 if S >= 24 else 1.08                    # small sizes: the star a little bigger
    star(0.0, 0.0, 0.70 * k, 0.84 * k, 0.46, halo=0.22 if S >= 32 else 0.0)
    if S >= 32:                                     # two small sparkles
        star(0.50, -0.52, 0.16, 0.20, 0.42, alpha=0.95, lit=False, tint=(0xFF, 0xFF, 0xDA))
        star(-0.52, 0.55, 0.11, 0.14, 0.42, alpha=0.8, lit=False, tint=(0xFF, 0xFF, 0xDA))
    if S >= 48:                                     # and faint dust
        for cx, cy, s in ((-0.55, -0.35, 0.05), (0.62, 0.30, 0.045), (-0.25, -0.66, 0.04), (0.30, 0.68, 0.04)):
            star(cx, cy, s, s, 0.5, alpha=0.6, lit=False, tint=(0xDA, 0xDA, 0xFF))

    # down to S x S (a box filter over premultiplied alpha)
    pre = rgba.copy()
    pre[..., :3] *= pre[..., 3:4]
    pre = pre.reshape(S, ss, S, ss, 4).mean(axis=(1, 3))
    a = pre[..., 3:4]
    rgb = np.where(a > 0, pre[..., :3] / np.maximum(a, 1e-6), 0)
    out = np.concatenate([rgb, a * 255], axis=2)
    return Image.fromarray(np.clip(np.round(out), 0, 255).astype(np.uint8), 'RGBA')


def write_header(img, path):
    px = np.array(img).astype(np.uint32)
    argb = (px[..., 3] << 24) | (px[..., 0] << 16) | (px[..., 1] << 8) | px[..., 2]
    vals = [f'0x{v:08X}' for v in argb.flatten()]
    rows = [', '.join(vals[i:i + 8]) for i in range(0, len(vals), 8)]
    with open(path, 'w', newline='\n') as f:
        f.write('/* The window icon, made by tools/make_icon.py (an original star in the\n'
                ' * game\'s window colours): 64 x 64, ARGB 0xAARRGGBB. */\n')
        f.write('#ifndef PSIV_ICON_H\n#define PSIV_ICON_H\n\n#include <stdint.h>\n\n')
        f.write(f'#define PSIV_ICON_SIZE {img.width}\n')
        f.write(f'static const uint32_t psiv_icon[{img.width} * {img.height}] = {{\n    ')
        f.write(',\n    '.join(rows))
        f.write('\n};\n\n#endif\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--preview', help='also write every size, on dark and light, to this folder')
    args = ap.parse_args()
    imgs = {s: render(s) for s in SIZES}
    os.makedirs(os.path.join(ROOT, 'res'), exist_ok=True)
    imgs[256].save(os.path.join(ROOT, 'res', 'psiv.ico'), sizes=[(s, s) for s in SIZES],
                   append_images=[imgs[s] for s in SIZES if s != 256])
    render(512).save(os.path.join(ROOT, 'res', 'psiv.png'))
    write_header(imgs[64], os.path.join(ROOT, 'runtime', 'icon.h'))
    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        W = sum(SIZES) + 12 * len(SIZES) + 12
        sheet = Image.new('RGBA', (W, 2 * (256 + 24)), (32, 32, 36, 255))
        sheet.paste(Image.new('RGBA', (W, 256 + 24), (236, 236, 240, 255)), (0, 256 + 24))
        x = 12
        for s in SIZES:
            for row in (0, 1):
                sheet.alpha_composite(imgs[s], (x, row * (256 + 24) + 12 + (256 - s)))
            x += s + 12
        sheet.save(os.path.join(args.preview, 'icon_sizes.png'))
    print('wrote res/psiv.ico, res/psiv.png, runtime/icon.h')


if __name__ == '__main__':
    main()
