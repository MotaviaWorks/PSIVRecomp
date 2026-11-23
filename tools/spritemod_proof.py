#!/usr/bin/env python3
"""A sprite file for the port's sprite mods (runtime/spritemod.c): a placeholder figure of our own, drawn by
this script, walking in the four directions with as many in-between pictures as asked for, and at the
game's own pixels or finer.

  python tools/spritemod_proof.py OUT.spr --target KEY [--size 16x32] [--frames 4] [--mult 4] [--res 1]
                                  [--colours BODY,SHADE,LEG,SKIN] [--sheet OUT.png]

KEY is whose sprite the file stands for, as the port lists them for whoever is on screen
(PSIV_SPRITEMOD_LOG=1): R290900 for one of the party, A1B2C3D4 for a kind of townsperson. FRAMES is the
game's own count for the walk (four holds to a cycle); MULT how many pictures stand for each of them (1, 2
or 4). RES is the pixels to each of the game's, along a side (1, 2 or 4): the figure is drawn at that
detail, not enlarged. COLOURS picks its colours in the sprite's palette line, to tell several apart.

The file: a line "PSIVSPR3", a line "W H FRAMES MULT RES KEY" (W and H in the game's pixels), then for each
facing (down, up, right, left), each frame and each of its pictures, (W RES) x (H RES) bytes: 0 nothing,
1-15 a colour of the palette line. (With RES 1 it writes the older "PSIVSPR2", "W H FRAMES MULT KEY".)
"""
import argparse, math

OUTLINE, EYE = 1, 1                                    # colours of the palette line, picked by eye


def figure(w, h, facing, phase, colours, k=1):
    """one picture, k pixels to each of the game's: `phase` 0..1 through the walk's cycle. Places and sizes
    are in the game's pixels; the shapes are drawn at the finer grid."""
    body, shade, leg, skin = colours
    W, H = w * k, h * k
    px = [[0] * W for _ in range(H)]

    def rect(x0, y0, x1, y1, c):                          # the game's pixels x0..x1, y0..y1, their whole blocks
        for y in range(max(0, int(round(y0 * k))), min(H, int(round((y1 + 1) * k)))):
            for x in range(max(0, int(round(x0 * k))), min(W, int(round((x1 + 1) * k)))):
                px[y][x] = c

    def dot(x, y, c):
        rect(round(x), round(y), round(x), round(y), c)

    def disc(cx, cy, r, c):
        ox, oy, rr = (cx + 0.5) * k, (cy + 0.5) * k, r * k
        for y in range(max(0, int(oy - rr - 1)), min(H, int(oy + rr + 2))):
            for x in range(max(0, int(ox - rr - 1)), min(W, int(ox + rr + 2))):
                if (x + 0.5 - ox) ** 2 + (y + 0.5 - oy) ** 2 <= rr * rr + 0.3 * k * k:
                    px[y][x] = c

    swing = math.sin(2 * math.pi * phase)                # a leg forward, then the other
    bob = -abs(math.cos(2 * math.pi * phase)) * 1.4      # up as the legs pass
    if k == 1:
        bob = round(bob)
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
        steps = 8 * k
        for i in range(steps + 1):
            t = i / steps
            x = (cx + (0 if side else sign * 1.6)) * (1 - t) + fx * t
            y = hip * (1 - t) + (foot - lift) * t
            rect(x - 0.6, y, x + 0.6 - 1 + 1.0 / k, y - 1 + 1.0 / k, leg)
        rect(fx - 1.2, foot - lift, fx + 1.2, foot - lift + 1, OUTLINE)
    # body
    top = h * 0.36 + bob
    rect(cx - 3.4, top, cx + 3.4, hip, OUTLINE)
    rect(cx - 2.6, top + 1, cx + 2.6, hip - 1, body)
    rect(cx - 2.6, hip - 3, cx + 2.6, hip - 1, shade)
    if k > 1:                                             # a belt one fine pixel high, and a button: detail only the finer grid has
        rect(cx - 2.6, hip - 3, cx + 2.6, hip - 4 + 1.0 / k, OUTLINE)
        disc(cx, top + 2.5, 0.45, skin)
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
    eye = 0.5 if k > 1 else 0.3
    if facing == 0:
        disc(cx - 1.5, hy, eye, EYE)
        disc(cx + 1.5, hy, eye, EYE)
    elif facing == 2:
        disc(cx + 2, hy, eye, EYE)
    elif facing == 3:
        disc(cx - 2, hy, eye, EYE)
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
    p.add_argument('--res', type=int, default=1, choices=(1, 2, 4), help="pixels to each of the game's, along a side")
    p.add_argument('--colours', default='6,4,9,14', help='body, shade, leg, skin: colours 1-15 of the palette line')
    p.add_argument('--sheet', help='also a picture of every frame (needs Pillow)')
    a = p.parse_args()
    w, h = (int(v) for v in a.size.lower().split('x'))
    colours = tuple(int(v) for v in a.colours.split(','))
    if a.target[:1] == 'B':
        # one of the party standing in battle: a single picture of 48 x 48, the figure seen from behind
        fig = figure(12, 24, 1, 0.0, colours, 2)          # 24 x 48, in the middle of the block
        pic = [[0] * 12 + row + [0] * 12 for row in fig]
        with open(a.out, 'wb') as f:
            f.write(('PSIVSPR2\n48 48 1 1 %s\n' % a.target).encode('ascii'))
            f.write(bytes(v for row in pic for v in row))
        print('%s: for %s, 48 x 48, a battle stance' % (a.out, a.target))
        return
    if a.target[:1] not in ('R', 'A'):
        raise SystemExit('--target is a key as the log lists it: R..., A... or B...')
    pics = []
    with open(a.out, 'wb') as f:
        if a.res == 1:
            f.write(('PSIVSPR2\n%d %d %d %d %s\n' % (w, h, a.frames, a.mult, a.target)).encode('ascii'))
        else:
            f.write(('PSIVSPR3\n%d %d %d %d %d %s\n' % (w, h, a.frames, a.mult, a.res, a.target)).encode('ascii'))
        for facing in range(4):
            for g in range(a.frames):
                for k in range(a.mult):
                    pic = figure(w, h, facing, (g + k / a.mult) / a.frames, colours, a.res)
                    pics.append(pic)
                    f.write(bytes(v for row in pic for v in row))
    print('%s: for %s, %d x %d, %d frames x %d pictures a facing, %d to a pixel' % (a.out, a.target, w, h, a.frames, a.mult, a.res))
    if a.sheet:
        from PIL import Image
        grey = [(0, 0, 0, 0)] + [(40 + i * 14, 40 + i * 14, 60 + i * 12, 255) for i in range(1, 16)]
        per = a.frames * a.mult
        W, H = w * a.res, h * a.res
        im = Image.new('RGBA', (per * (W + 2), 4 * (H + 2)), (30, 34, 60, 255))
        for i, pic in enumerate(pics):
            for y in range(H):
                for x in range(W):
                    if pic[y][x]:
                        im.putpixel(((i % per) * (W + 2) + 1 + x, (i // per) * (H + 2) + 1 + y), grey[pic[y][x]])
        z = max(1, 4 // a.res)
        im.resize((im.width * z, im.height * z), Image.NEAREST).save(a.sheet)


if __name__ == '__main__':
    main()
