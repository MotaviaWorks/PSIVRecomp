"""The Discord mark for the launcher's Discord button: its outline (the usual 24-unit icon
path) drawn into a 64 x 64 coverage mask, written as runtime/discord_mark.h.

Discord and its logo are trademarks of Discord Inc.; the mark is used unchanged, in white
on Discord's own blue, only as the link to the project's Discord server.

    python tools/make_discord_mark.py [preview.png]
"""
import math
import os
import re
import sys

from PIL import Image, ImageDraw

PATH = ("M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 "
        "1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 "
        "19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 "
        "0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 "
        "1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943"
        ".2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c"
        ".1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c"
        ".3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 "
        "00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569"
        "-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 "
        "2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 "
        "0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z")
N = 64
NUM = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def arc(p0, rx, ry, rot, large, sweep, p1, steps=24):
    """an SVG arc as points (the end point included)"""
    (x0, y0), (x1, y1) = p0, p1
    if rx == 0 or ry == 0:
        return [p1]
    phi = math.radians(rot)
    c, s = math.cos(phi), math.sin(phi)
    dx, dy = (x0 - x1) / 2, (y0 - y1) / 2
    xp, yp = c * dx + s * dy, -s * dx + c * dy
    rx, ry = abs(rx), abs(ry)
    lam = xp * xp / (rx * rx) + yp * yp / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = max(0.0, rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp)
    k = math.sqrt(num / (rx * rx * yp * yp + ry * ry * xp * xp)) * (-1 if large == sweep else 1)
    cxp, cyp = k * rx * yp / ry, -k * ry * xp / rx
    cx, cy = c * cxp - s * cyp + (x0 + x1) / 2, s * cxp + c * cyp + (y0 + y1) / 2
    a0 = math.atan2((yp - cyp) / ry, (xp - cxp) / rx)
    a1 = math.atan2((-yp - cyp) / ry, (-xp - cxp) / rx)
    d = a1 - a0
    if sweep and d < 0:
        d += 2 * math.pi
    if not sweep and d > 0:
        d -= 2 * math.pi
    out = []
    for i in range(1, steps + 1):
        a = a0 + d * i / steps
        out.append((cx + rx * math.cos(a) * c - ry * math.sin(a) * s, cy + rx * math.cos(a) * s + ry * math.sin(a) * c))
    out[-1] = p1
    return out


def cubic(p0, p1, p2, p3, steps=20):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        out.append(tuple(u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d
                         for a, b, c, d in zip(p0, p1, p2, p3)))
    return out


def subpaths(path):
    """the path's closed outlines, as lists of points"""
    out, pts, pos, start, i, cmd = [], [], (0.0, 0.0), (0.0, 0.0), 0, None

    def num():
        nonlocal i
        m = NUM.match(path, i)
        while not m:                                 # spaces and commas between numbers
            i += 1
            m = NUM.match(path, i)
        i = m.end()
        return float(m.group())

    def flag():
        nonlocal i
        while path[i] in ' ,':
            i += 1
        i += 1
        return int(path[i - 1])

    while i < len(path):
        ch = path[i]
        if ch in ' ,':
            i += 1
            continue
        if ch.isalpha():
            cmd = ch
            i += 1
            if cmd in 'zZ':
                if pts:
                    out.append(pts)
                pts, pos = [], start
                continue
        rel = cmd.islower()
        ox, oy = pos if rel else (0.0, 0.0)
        if cmd in 'mM':
            if pts:
                out.append(pts)
            pos = start = (ox + num(), oy + num())
            pts = [pos]
            cmd = 'l' if rel else 'L'                # further pairs are lines
        elif cmd in 'lL':
            pos = (ox + num(), oy + num())
            pts.append(pos)
        elif cmd in 'cC':
            p1, p2, p3 = ((ox + num(), oy + num()) for _ in range(3))
            pts += cubic(pos, p1, p2, p3)
            pos = p3
        elif cmd in 'aA':
            rx, ry, rot = num(), num(), num()
            large, sweep = flag(), flag()
            end = (ox + num(), oy + num())
            pts += arc(pos, rx, ry, rot, large, sweep, end)
            pos = end
        else:
            raise ValueError(cmd)
    if pts:
        out.append(pts)
    return out


def mask(n=N, ss=16):
    subs = subpaths(PATH)
    xs = [x for s in subs for x, _ in s]
    ys = [y for s in subs for _, y in s]
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    k = (n - 2) * ss / max(w, h)                     # a pixel of air all round
    ox, oy = (n * ss - w * k) / 2 - min(xs) * k, (n * ss - h * k) / 2 - min(ys) * k
    im = Image.new('L', (n * ss, n * ss), 0)
    d = ImageDraw.Draw(im)
    for j, s in enumerate(subs):                     # the body, then its two eyes cut out
        d.polygon([(x * k + ox, y * k + oy) for x, y in s], fill=255 if j == 0 else 0)
    return im.resize((n, n), Image.LANCZOS)


if __name__ == '__main__':
    im = mask()
    here = os.path.dirname(os.path.abspath(__file__))
    data = im.tobytes()
    rows = [', '.join(str(b) for b in data[r * N:(r + 1) * N]) for r in range(N)]
    with open(os.path.join(here, '..', 'runtime', 'discord_mark.h'), 'w', newline='\n') as f:
        f.write('/* The Discord mark as a %d x %d coverage mask, for the launcher\'s Discord button\n'
                ' * (tools/make_discord_mark.py). Discord and its logo are trademarks of Discord Inc.;\n'
                ' * used unchanged, only as the link to the project\'s Discord server. */\n'
                '#ifndef PSIV_DISCORD_MARK_H\n#define PSIV_DISCORD_MARK_H\n'
                '#define DISCORD_MARK_N %d\n'
                'static const unsigned char DISCORD_MARK[DISCORD_MARK_N * DISCORD_MARK_N] = {\n' % (N, N, N))
        f.write(',\n'.join('    ' + r for r in rows))
        f.write('\n};\n#endif\n')
    if len(sys.argv) > 1:
        big = Image.new('RGB', (N * 6, N * 6), (88, 101, 242))
        big.paste(Image.new('RGB', big.size, (255, 255, 255)), (0, 0), im.resize(big.size, Image.NEAREST))
        big.save(sys.argv[1])
    print('written runtime/discord_mark.h')
