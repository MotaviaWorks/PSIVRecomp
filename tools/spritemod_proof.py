#!/usr/bin/env python3
"""A sprite file for the port's sprite-mod proof (runtime/spritemod.c): a placeholder figure of our own,
drawn by this script, walking in the four directions with as many in-between pictures as asked for.

  python tools/spritemod_proof.py OUT.spr --target KEY [--size 16x32] [--frames 4] [--mult 4]
                                  [--colours BODY,SHADE,LEG,SKIN] [--sheet OUT.png]

KEY is whose sprite the file stands for, as the port lists them for whoever is on screen
(PSIV_SPRITEMOD_LOG=1): R290900 for one of the party, A1B2C3D4 for a kind of townsperson. FRAMES is the game's
own count for the walk (four holds to a cycle); MULT how many pictures stand for each of them (1, 2 or 4).
COLOURS picks the figure's colours in the sprite's palette line, to tell several figures apart.

The file: a line "PSIVSPR2", a line "W H FRAMES MULT KEY", then for each facing (down, up, right,
left), each frame and each of its pictures, W x H bytes: 0 nothing, 1-15 a colour of the palette line.
"""
import argparse, math

OUTLINE, EYE = 1, 1                                    # colours of the palette line, picked by eye


def figure(w, h, facing, phase, colours):
    """one picture: `phase` 0..1 through the walk's cycle"""
    body, shade, leg, skin = colours
    px = [[0] * w for _ in range(h)]

    def dot(x, y, c):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < w and 0 <= y < h:
            px[y][x] = c

    def rect(x0, y0, x1, y1, c):
        for y in range(int(round(y0)), int(round(y1)) + 1):
            for x in range(int(round(x0)), int(round(x1)) + 1):
                dot(x, y, c)

    def disc(cx, cy, r, c):
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r + 0.3:
                    dot(x, y, c)

    swing = math.sin(2 * math.pi * phase)                # a leg forward, then the other
    bob = -abs(math.cos(2 * math.pi * phase)) * 1.4      # up as the legs pass
    cx = (w - 1) / 2
    foot = h - 2
    hip = h * 0.66 + bob
    side = facing in (2, 3)
    # legs
    for sign in (-1, 1):
        if side:
            fx = cx + sign * swing * 3.2
            lift = max(0.0, -sign * math.cos(2 * math.pi * phase)) * 2.0
        else:
            fx = cx + sign * 2.2
            lift = max(0.0, sign * swing) * 2.6
        steps = 8
        for i in range(steps + 1):
            t = i / steps
            x = (cx + (0 if side else sign * 1.6)) * (1 - t) + fx * t
            y = hip * (1 - t) + (foot - lift) * t
            rect(x - 0.6, y, x + 0.6, y, leg)
        rect(fx - 1.2, foot - lift, fx + 1.2, foot - lift + 1, OUTLINE)
    # body
    top = h * 0.36 + bob
    rect(cx - 3.4, top, cx + 3.4, hip, OUTLINE)
    rect(cx - 2.6, top + 1, cx + 2.6, hip - 1, body)
    rect(cx - 2.6, hip - 3, cx + 2.6, hip - 1, shade)
    # arms, against the legs
    for sign in (-1, 1):
        if side:
            ax = cx - sign * swing * 2.6
            rect(ax - 0.6, top + 2, ax + 0.6, top + 7, skin if sign > 0 else shade)
        else:
            ay = top + 2 + max(0.0, -sign * swing) * 1.5
            rect(cx + sign * 4 - 0.6, ay, cx + sign * 4 + 0.6, ay + 5, skin)
    # head
    hy = h * 0.2 + bob
    disc(cx, hy, 4.4, OUTLINE)
    disc(cx, hy, 3.5, skin)
    if facing == 0:
        dot(cx - 1.5, hy, EYE)
        dot(cx + 1.5, hy, EYE)
    elif facing == 2:
        dot(cx + 2, hy, EYE)
    elif facing == 3:
        dot(cx - 2, hy, EYE)
    else:                                                 # the back of the head
        rect(cx - 2.5, hy - 3, cx + 2.5, hy - 1, shade)
    return px


def main():
    p = argparse.ArgumentParser()
    p.add_argument('out')
    p.add_argument('--target', required=True, help="the character's key, as the port's log lists it (R... or A...)")
    p.add_argument('--size', default='16x32')
    p.add_argument('--frames', type=int, default=4)
    p.add_argument('--mult', type=int, default=4)
    p.add_argument('--colours', default='6,4,9,14', help='body, shade, leg, skin: colours 1-15 of the palette line')
    p.add_argument('--sheet', help='also a picture of every frame (needs Pillow)')
    a = p.parse_args()
    w, h = (int(v) for v in a.size.lower().split('x'))
    colours = tuple(int(v) for v in a.colours.split(','))
    pics = []
    with open(a.out, 'wb') as f:
        if a.target[:1] not in ('R', 'A'):
            raise SystemExit('--target is a key as the log lists it: R... or A...')
        f.write(('PSIVSPR2\n%d %d %d %d %s\n' % (w, h, a.frames, a.mult, a.target)).encode('ascii'))
        for facing in range(4):
            for g in range(a.frames):
                for k in range(a.mult):
                    pic = figure(w, h, facing, (g + k / a.mult) / a.frames, colours)
                    pics.append(pic)
                    f.write(bytes(v for row in pic for v in row))
    print('%s: for %s, %d x %d, %d frames x %d pictures a facing' % (a.out, a.target, w, h, a.frames, a.mult))
    if a.sheet:
        from PIL import Image
        grey = [(0, 0, 0, 0)] + [(40 + i * 14, 40 + i * 14, 60 + i * 12, 255) for i in range(1, 16)]
        per = a.frames * a.mult
        im = Image.new('RGBA', (per * (w + 2), 4 * (h + 2)), (30, 34, 60, 255))
        for i, pic in enumerate(pics):
            for y in range(h):
                for x in range(w):
                    if pic[y][x]:
                        im.putpixel(((i % per) * (w + 2) + 1 + x, (i // per) * (h + 2) + 1 + y), grey[pic[y][x]])
        im.resize((im.width * 4, im.height * 4), Image.NEAREST).save(a.sheet)


if __name__ == '__main__':
    main()
