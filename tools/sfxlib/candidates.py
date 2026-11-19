"""Candidates for each modern sound, from the CC0 packs: build/sfxlib-cand/XX/*.wav and candidates.json.

    python tools/sfxlib/candidates.py [ID ...]

For each effect (briefs.py): up to six single sounds, picked by name and by how well their length suits the
original's, and the layered mixes the brief asks for. Each is trimmed to its first sound, cut to a length that
fits the animation the original was timed to (with a fade), and set to the original's loudness less 1 dB (its
loudest quarter second; peaks kept under -1 dBFS). 44.1 kHz stereo, 16 bit.
Chosen by names and measurements, not by ear: these are for PsiLok to pick from (the audition page).
"""
import json
import os
import re
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, 'build', 'sfxlib-src')
OUT = os.path.join(ROOT, 'build', 'sfxlib-cand')
FF = r"C:\Users\Ajsko\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1-full_build\bin\ffmpeg.exe"
SR = 44100
sys.path.insert(0, HERE)
from briefs import BRIEFS                                   # noqa: E402

ORIG = json.load(open(os.path.join(HERE, 'originals.json'), encoding='utf-8'))['effects']
INV = [o for o in json.load(open(os.path.join(SRC, 'inventory.json'), encoding='utf-8'))
       if 'error' not in o and '__MACOSX' not in o['path'] and '/preview.' not in o['path']]


def decode(path):
    r = subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-i', path, '-ac', '2', '-ar', str(SR), '-f', 'f32le', '-'],
                       capture_output=True)
    return np.frombuffer(r.stdout, np.float32).reshape(-1, 2).astype(np.float64)


def thunder():
    """the thunderclap out of "Rain + Long Thunder" (44 s of rain; the clap near 20 s): its loudest 3.2 s, as a
    sound of its own in the inventory"""
    src = next((o for o in INV if o['path'].endswith('rain-thunder.ogg')), None)
    d = os.path.join(SRC, '_derived')
    p = os.path.join(d, 'thunder_clap.wav')
    if src and not os.path.exists(p):
        os.makedirs(d, exist_ok=True)
        a = decode(os.path.join(SRC, src['path']))
        e = np.convolve((a ** 2).mean(axis=1), np.ones(SR // 4) / (SR // 4), 'same')
        k = max(0, int(np.argmax(e)) - int(0.4 * SR))
        save(p, a[k:k + int(3.2 * SR)])
    if src and os.path.exists(p):
        INV.append(dict(src, path='_derived/thunder_clap.wav', secs=3.2, lead=0.0, note='the clap cut out of ' + src['path']))


def save(path, a):
    a = np.clip(np.round(a * 32767.0), -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(a.tobytes())


def loud250(a):
    p = (a ** 2).mean(axis=1)
    win = int(0.25 * SR)
    if len(p) <= win:
        return 10 * np.log10(p.mean() + 1e-12)
    c = np.concatenate([[0.0], np.cumsum(p)])
    return 10 * np.log10(((c[win:] - c[:-win]) / win).max() + 1e-12)


def length_for(sid):
    """how long a replacement may be: the original's length and a little, never under a third of a second"""
    t = ORIG[sid]['secs']
    return max(0.33, min(t * 1.5 + 0.25, t + 0.9))


def prepare(a, limit):
    """from its first sound, no longer than limit (faded out)"""
    m = np.abs(a).max(axis=1)
    if not len(m) or m.max() < 1e-5:
        return None
    first = int(np.nonzero(m > m.max() * 0.03)[0][0])
    a = a[max(0, first - int(0.002 * SR)):]
    n = min(len(a), int(limit * SR))
    a = a[:n].copy()
    k = min(n, int((0.12 if n >= int(limit * SR) else 0.02) * SR))
    a[n - k:] *= np.linspace(1, 0, k)[:, None]
    a[:int(0.001 * SR)] *= np.linspace(0, 1, int(0.001 * SR))[:, None]
    return a


def level(a, sid):
    target = ORIG[sid]['loud_db'] - 1.0
    g = 10 ** ((target - loud250(a)) / 20)
    pk = np.abs(a).max() * g
    if pk > 0.89:
        g *= 0.89 / pk
    return a * g, round(20 * np.log10(g), 1)


def pool(pattern):
    rx = re.compile(pattern, re.I)
    return [o for o in INV if rx.search('/' + o['path'])]


def pick_singles(sid, patterns, want=6):
    t = ORIG[sid]['secs']
    lim = length_for(sid)
    seen, picks = set(), []
    groups = []
    same = set()                                             # (a pack that holds a sound twice, .wav and .ogg: once)
    for pat in patterns:
        rows = []
        for o in pool(pat):
            key = (os.path.splitext(os.path.basename(o['path']))[0].lower(), o['pack'], round(o['secs'], 1))
            if o['path'] in seen or key in same:
                continue
            same.add(key)
            rows.append(o)
        # the best fit first: long enough to carry the effect, not so long that the cut takes its body
        rows.sort(key=lambda o: (abs(np.log(max(o['secs'], 0.03) / max(t, 0.08))) + (2.0 if o['secs'] > lim * 2.5 else 0.0), o['path']))
        for o in rows:
            seen.add(o['path'])
        groups.append(rows)
    depth = 0
    while len(picks) < want and any(len(g) > depth for g in groups):       # a turn for each pattern
        for g in groups:
            if len(g) > depth and len(picks) < want:
                picks.append(g[depth])
        depth += 1
    return picks


def source(o):
    return {'pack': o['pack'], 'author': o['author'], 'file': o['path']}


def main():
    thunder()
    only = [x.upper() for x in sys.argv[1:]]
    manifest = {}
    for sid, (what, patterns, mixes) in sorted(BRIEFS.items()):
        if only and sid not in only:
            continue
        d = os.path.join(OUT, sid)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        lim = length_for(sid)
        cands = []
        for k, o in enumerate(pick_singles(sid, patterns)):
            a = prepare(decode(os.path.join(SRC, o['path'])), lim)
            if a is None:
                continue
            a, g = level(a, sid)
            name = f's{k + 1}.wav'
            save(os.path.join(d, name), a)
            cands.append({'file': name, 'kind': 'single', 'label': os.path.splitext(os.path.basename(o['path']))[0],
                          'secs': round(len(a) / SR, 2), 'gain_db': g, 'sources': [source(o)]})
        for k, layers in enumerate(mixes):
            parts, srcs, label = [], [], []
            for pat, gain_db, delay in layers:
                rows = pick_singles(sid, [pat], 1)
                if not rows:
                    parts = []
                    break
                a = prepare(decode(os.path.join(SRC, rows[0]['path'])), lim)
                if a is None:
                    parts = []
                    break
                a = a / (10 ** (loud250(a) / 20) + 1e-9) * 10 ** (gain_db / 20)        # each layer to the same loudness, then its gain
                parts.append((a, int(delay * SR)))
                srcs.append(source(rows[0]))
                label.append(os.path.splitext(os.path.basename(rows[0]['path']))[0])
            if not parts:
                continue
            n = min(max(len(a) + dl for a, dl in parts), int(lim * SR))
            mix = np.zeros((n, 2))
            for a, dl in parts:
                m = min(len(a), n - dl)
                if m > 0:
                    mix[dl:dl + m] += a[:m]
            kf = min(n, int(0.08 * SR))
            mix[n - kf:] *= np.linspace(1, 0, kf)[:, None]
            mix, g = level(mix, sid)
            name = f'm{k + 1}.wav'
            save(os.path.join(d, name), mix)
            cands.append({'file': name, 'kind': 'mix', 'label': ' + '.join(label), 'secs': round(n / SR, 2), 'gain_db': g, 'sources': srcs})
        manifest[sid] = {'name': ORIG[sid]['name'], 'what': what, 'original_secs': ORIG[sid]['secs'], 'limit_secs': round(lim, 2),
                         'candidates': cands}
        print(f"{sid} {ORIG[sid]['name']:20s} {len(cands)} candidates: " + ', '.join(c['label'] for c in cands)[:150], flush=True)
    path = os.path.join(OUT, 'candidates.json')
    if only and os.path.exists(path):
        old = json.load(open(path, encoding='utf-8'))
        old.update(manifest)
        manifest = old
    json.dump(manifest, open(path, 'w', encoding='utf-8'), indent=1)
    print(len(manifest), 'effects ->', OUT)


if __name__ == '__main__':
    main()
