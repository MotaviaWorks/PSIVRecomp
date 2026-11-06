"""Hear and see the game's own sound effects next to the port's.

Renders each effect alone (psiv_headless --music ID:60, the jukebox) and
describes it: how long it sounds, its loudest quarter second, where its
energy sits (spectral centroid) and how that moves, and how noisy it is
(spectral flatness). Spectrograms go into one sheet per run, so an effect
and its replacement can be held side by side.

    python tools/sfx_study.py E8,B9,C6               the originals
    python tools/sfx_study.py E8,B9 --synth claw,die  ... with the port's recipes beside them
Output: build/sfxstudy/ (WAVs, sheet.png, study.json). Game audio: never commit.
"""
import argparse
import json
import subprocess
import wave
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / 'build' / 'native-Release' / 'psiv_headless.exe'
OUT = ROOT / 'build' / 'sfxstudy'
SR = 53267
START = 62 * 888.6313 / SR                               # the request (frame 62 of the jukebox run)


def load(path):
    with wave.open(str(path), 'rb') as w:
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, w.getnchannels())
        return w.getframerate(), a.astype(np.float64) / 32768.0


def original(sid):
    p = OUT / f'orig_{sid:02X}.wav'
    if not p.exists():
        subprocess.run([str(EXE), '--music', f'{sid:X}:60', '--frames', '330', '--wav', str(p)], cwd=ROOT,
                       capture_output=True)
    sr, a = load(p)
    a = a[int(START * sr):]
    return sr, a - np.median(a, axis=0)                  # the chips' idle DC offset out


def synth(name):
    d = OUT / 'synth'
    d.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(EXE), '--sfx-dump', str(d)], cwd=ROOT, capture_output=True)
    return load(d / f'{name}_v1.wav')


def describe(sr, a):
    m = np.abs(a).max(axis=1)
    peak = float(m.max()) or 1e-9
    live = np.nonzero(m > peak * 10 ** (-50 / 20))[0]
    dur = (live[-1] / sr) if len(live) else 0.0
    mono = a.mean(axis=1)
    win = int(0.25 * sr)
    best = max((float(np.sqrt(np.mean(mono[i:i + win] ** 2))) for i in range(0, max(1, len(mono) - win), win // 4)),
               default=0.0)
    hop, n = 512, 2048
    cents, flats = [], []
    for i in range(0, max(1, min(len(mono), int(dur * sr) + n) - n), hop):
        seg = mono[i:i + n] * np.hanning(n)
        s = np.abs(np.fft.rfft(seg)) + 1e-12
        f = np.fft.rfftfreq(n, 1 / sr)
        if s.sum() < 1e-6:
            continue
        cents.append(float((f * s).sum() / s.sum()))
        flats.append(float(np.exp(np.mean(np.log(s))) / np.mean(s)))
    return {'seconds': round(dur, 3), 'peak_db': round(20 * np.log10(peak), 1),
            'loud250_db': round(20 * np.log10(best + 1e-9), 1),
            'centroid_hz': [round(c) for c in cents[::max(1, len(cents) // 8)][:8]],
            'flatness': round(float(np.mean(flats)) if flats else 0.0, 3)}


def spectrogram(sr, a, secs, label, w=520, h=200):
    mono = a.mean(axis=1)[:int(secs * sr)]
    n, hop = 1024, max(1, int(secs * sr / w))
    cols = []
    for i in range(w):
        seg = mono[i * hop:i * hop + n]
        if len(seg) < n:
            seg = np.pad(seg, (0, n - len(seg)))
        cols.append(np.abs(np.fft.rfft(seg * np.hanning(n))))
    S = 20 * np.log10(np.array(cols).T + 1e-9)
    f = np.fft.rfftfreq(n, 1 / sr)
    rows = np.geomspace(60, 16000, h)[::-1]               # log frequency, high at the top
    idx = np.clip(np.searchsorted(f, rows), 0, len(f) - 1)
    S = S[idx]
    S = np.clip((S + 30) / 60, 0, 1)                      # -30..+30 dB of the FFT's scale
    rgb = np.stack([S ** 0.6 * 255, S ** 1.2 * 230, (1 - S) * 60 + S ** 3 * 200], axis=-1).astype(np.uint8)
    im = Image.fromarray(rgb, 'RGB')
    d = ImageDraw.Draw(im)
    for hz in (100, 1000, 10000):
        y = int(np.argmin(np.abs(rows - hz)))
        d.line((0, y, 6, y), fill=(255, 255, 255))
        d.text((8, y - 6), f'{hz // 1000}k' if hz >= 1000 else str(hz), fill=(200, 200, 200))
    for k in range(1, int(secs / 0.25) + 1):
        x = int(k * 0.25 / secs * w)
        d.line((x, h - 6, x, h), fill=(200, 200, 200))
    d.text((60, 4), label, fill=(255, 240, 160))
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ids', help='hex IDs, comma-separated')
    ap.add_argument('--synth', help='recipe names to set beside them, comma-separated (same order)')
    ap.add_argument('--secs', type=float, default=1.6)
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    ids = [int(x, 16) for x in a.ids.split(',')]
    names = a.synth.split(',') if a.synth else []
    tiles, study = [], {}
    for k, sid in enumerate(ids):
        sr, x = original(sid)
        d = describe(sr, x)
        study[f'{sid:02X}'] = d
        tiles.append(spectrogram(sr, x, a.secs, f'${sid:02X} original  {d["seconds"]} s  loud {d["loud250_db"]} dB'))
        if k < len(names) and names[k]:
            sr2, y = synth(names[k])
            d2 = describe(sr2, y)
            study[f'{sid:02X}:{names[k]}'] = d2
            tiles.append(spectrogram(sr2, y, a.secs, f'{names[k]} (port)  {d2["seconds"]} s  loud {d2["loud250_db"]} dB'))
        print(f'${sid:02X}', d)
    cols = 2 if names else 3
    W, H = tiles[0].size
    sheet = Image.new('RGB', (cols * (W + 6), ((len(tiles) + cols - 1) // cols) * (H + 6)), (10, 10, 10))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * (W + 6), (i // cols) * (H + 6)))
    sheet.save(OUT / 'sheet.png')
    (OUT / 'study.json').write_text(json.dumps(study, indent=1))
    print('sheet:', OUT / 'sheet.png')


if __name__ == '__main__':
    main()
