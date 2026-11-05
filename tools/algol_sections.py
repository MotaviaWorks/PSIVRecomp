"""A song bar by bar, for choosing where Algol's rock drums switch from the
hi-hat to the ride (RIDE_MAP in runtime/algol.c).

Renders the song on Algol without drums (the band alone) and lists, per
bar: the driver tick from the song's start (what RIDE_MAP counts), the
band's loudness (K-weighted, as BS.1770), and the game's drum hits. The
eighth comes from the snares (as the hat follower finds it); the bars start
at --anchor, a downbeat in song ticks (default: the first drum hit). The
song's loop is found from its FM notes: past `end` it plays on from `loop`.

  python tools/algol_sections.py 8F
  python tools/algol_sections.py 8F --anchor 98 --seconds 150
"""
import argparse, math, os, re, subprocess, sys, tempfile, wave
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SR = 53267
KIND = {1: 'S', 11: 'S', 12: 'S', 2: 'K', 13: 'K', 14: 'K', 3: 'R', 4: 'h', 9: 'o', 5: 'T', 6: 'C', 10: 'C'}


def render(song, seconds, tmp):
    exe = os.path.join(ROOT, 'build', 'native-Release', 'psiv_headless.exe')
    if not os.path.exists(exe):
        exe = os.path.join(ROOT, 'build', 'native', 'psiv_headless')
    with open(os.path.join(tmp, 'algol.ini'), 'w') as f:      # every song analysed as rock (the eighths)
        f.write(f'song {song} = rock\n')
    env = dict(os.environ, PSIV_CHANNELS='3FF', PSIV_ALGOL_LOG='2')
    frames = int(seconds * 60) + 40
    out = subprocess.run([exe, '--rom', os.path.join(ROOT, 'rom', 'psiv.bin'), '--algol', '--frames', str(frames),
                          '--music', f'{song}:30', '--wav', os.path.join(tmp, 'band.wav'),
                          '--ymlog', os.path.join(tmp, 'ym.log')],
                         cwd=tmp, env=env, capture_output=True, text=True)
    if out.returncode:
        sys.exit(out.stderr or out.stdout)
    return out.stdout


def k_weighted(x):
    from scipy.signal import lfilter
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = math.tan(math.pi * f0 / SR)
    Vh, Vb = 10 ** (G / 20), (10 ** (G / 20)) ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = math.tan(math.pi * f0 / SR)
    a0 = 1 + K / Q + K * K
    b2, a2 = [1, -2, 1], [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    return lfilter(b2, a2, lfilter(b1, a1, x, axis=0), axis=0)


def find_loop(notes):
    """notes: sorted (tick, channel, key). The song's loop: the smallest
    period L such that everything from some tick on repeats what was played
    L ticks before, over a stretch of at least half a period (and 20 s).
    Returns (loop, L): from loop + L the song plays on from loop."""
    if len(notes) < 50:
        return None
    last = notes[-1][0]
    seen = set(notes)
    for L in range(192, last):
        later = [n for n in notes if n[0] >= L]
        # the earliest tick from which every note repeats one L ticks before
        t_star = None
        for (t, c, k) in reversed(later):
            if (t - L, c, k) in seen:
                t_star = t
            else:
                break
        if t_star is None:
            continue
        if last - t_star >= max(0.5 * L, 1200):
            return t_star - L, L
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('song')
    ap.add_argument('--seconds', type=float, default=160)
    ap.add_argument('--anchor', type=int, help='a downbeat, in song ticks')
    ap.add_argument('--bar', type=int, default=8, help='eighths per bar')
    a = ap.parse_args()
    song = a.song.upper()

    with tempfile.TemporaryDirectory() as tmp:
        log = render(song, a.seconds, tmp)
        with wave.open(os.path.join(tmp, 'band.wav')) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64).reshape(-1, 2) / 32768
        with open(os.path.join(tmp, 'ym.log')) as f:
            ym = f.read().splitlines()

    # the jukebox's request (frame 32), not the game's own at power-on
    m = next((m_ for m_ in re.finditer(rf'algol: song {song}, \w+ drums \(at ([\d.]+) s', log)
              if float(m_.group(1)) >= 0.5), None)
    if not m:
        sys.exit('the song did not start')
    t0 = float(m.group(1))
    # up to whatever the game plays next (its attract mode, say)
    nxt = re.search(r'algol: song \w+, \w+ drums \(at ([\d.]+) s', log[m.end():])
    t_end = float(nxt.group(1)) if nxt else len(x) / SR
    x = x[:int(t_end * SR)]
    hits = [(float(m_.group(2)), int(m_.group(3)), int(m_.group(1)))
            for m_ in re.finditer(r'algol: drum (\d+) at ([\d.]+) s, song tick (-?\d+)', log)]
    hits = [h for h in hits if t0 <= h[0] < t_end and h[1] >= 0]
    if not hits:
        sys.exit('the song has no drums')
    ym = [l for l in ym if t0 * SR <= int(l.split()[0]) < t_end * SR]
    # seconds <-> song ticks, from the drum hits
    ts = np.array([h[0] for h in hits]); ks = np.array([h[1] for h in hits])
    rate, base = np.polyfit(ts, ks, 1)
    tick_of = lambda t: rate * t + base
    time_of = lambda k: (k - base) / rate
    # the driver ticks of the ym log vs song ticks: the same drum hits
    zr = [(int(p[0]) / SR, int(p[4])) for p in (l.split() for l in ym) if len(p) == 5 and p[1] == 'zram' and p[2] == '1F00' and 1 <= int(p[3], 16) <= 17]
    off = int(round(np.median([zt - tick_of(t) for (t, zt) in zr if t >= t0])))

    # the eighth: the commonest gap between snares, a quarter of it (as algol.c)
    sn = [h[0] for h in hits if KIND.get(h[2]) == 'S']
    gaps = [b - a_ for a_, b in zip(sn, sn[1:]) if 0.4 <= b - a_ <= 2.0]
    if gaps:
        best = max(gaps, key=lambda g: sum(abs(o - g) < 0.03 * g for o in gaps))
        beat2 = np.mean([o for o in gaps if abs(o - best) < 0.03 * best])
        p8 = beat2 / 4
    else:
        ki = [h[0] for h in hits if KIND.get(h[2]) in ('K', 'T')]
        p8 = np.median(np.diff(ki)) if len(ki) > 2 else 0.25
    anchor_t = time_of(a.anchor) if a.anchor is not None else hits[0][0]
    bar_s = p8 * a.bar

    # the loop, from the FM key-ons (channel and pitch registers)
    fn = {}
    notes = []
    for l in ym:
        p = l.split()
        if len(p) != 5 or p[1] not in ('ym0', 'ym1'):
            continue
        part = 0 if p[1] == 'ym0' else 1
        reg, val, tick = int(p[2], 16), int(p[3], 16), int(p[4])
        if 0xA0 <= reg <= 0xA6:
            fn[(part, reg)] = val
        elif part == 0 and reg == 0x28 and val & 0xF0 and (val & 3) != 3:
            c = val & 7
            pt, ci = (1, c - 4) if c >= 4 else (0, c)
            key = (fn.get((pt, 0xA4 + ci), 0), fn.get((pt, 0xA0 + ci), 0))
            notes.append((tick - off, c, key))
    notes = [n for n in notes if n[0] >= 0]
    loop = find_loop(sorted(set(notes)))

    y = k_weighted(x)
    print(f'song {song}: starts {t0:.3f} s; {rate:.3f} ticks/s; eighth {p8:.4f} s = {p8 * rate:.2f} ticks; bar {bar_s:.3f} s')
    if loop:
        print(f'loop: from tick {loop[0]}, every {loop[1]} ticks (end = {loop[0] + loop[1]})')
    else:
        print('loop: not found (render longer?)')
    k0 = math.floor((t0 - anchor_t) / bar_s)
    rows = []
    b = k0
    while True:
        s0, s1 = anchor_t + b * bar_s, anchor_t + (b + 1) * bar_s
        if s1 * SR > len(x):
            break
        if s1 > t0:
            seg = y[int(max(s0, t0) * SR):int(s1 * SR)]
            lk = -0.691 + 10 * math.log10(np.mean(np.sum(seg * seg, axis=1)) + 1e-12)
            hs = [KIND.get(h[2], str(h[2])) for h in hits if s0 - 0.03 <= h[0] < s1 - 0.03]
            near = [h[1] for h in hits if abs(h[0] - s0) < 0.03]
            tick = near[0] if near else int(round(tick_of(s0)))
            rows.append((b - k0, tick, s0, lk, hs))
        b += 1
    med = np.median([r[3] for r in rows])
    for i, tick, s0, lk, hs in rows:
        cnt = ' '.join(f'{k}{hs.count(k)}' for k in 'KSTCRho' if hs.count(k))
        other = ' '.join(h for h in hs if h not in 'KSTCRho')
        mark = '#' * max(0, int((lk - med + 4) * 4))
        print(f'{i:3d} tick {tick:5d} {s0 - t0:6.1f}s  {lk:6.1f} ({lk - med:+.1f})  {mark:<34} {cnt} {other}')


main()
