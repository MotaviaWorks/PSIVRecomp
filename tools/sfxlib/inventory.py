"""Every audio file in the downloaded CC0 packs (build/sfxlib-src), measured -> build/sfxlib-src/inventory.json.

    python tools/sfxlib/inventory.py

For each file: its pack, how long it sounds (first to last sample within 50 dB of its peak), peak and loudest
quarter second, where its energy sits (five bands, centroid), how noisy it is (spectral flatness), how fast it
starts and dies (attack: time to the envelope's peak; decay: from there to 20 dB down), and its strongest pitch.
Chosen by these and by the names: nobody here can hear them (PsiLok picks by ear from the shortlists).
Packs marked "exclude" in sources.json are left out.
"""
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, 'build', 'sfxlib-src')
FF = r"C:\Users\Ajsko\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1-full_build\bin\ffmpeg.exe"
SR = 44100
EXT = ('.wav', '.ogg', '.flac', '.mp3', '.m4a')


def decode(path, sr=SR):
    r = subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-i', path, '-ac', '2', '-ar', str(sr), '-f', 'f32le', '-'],
                       capture_output=True)
    return np.frombuffer(r.stdout, np.float32).reshape(-1, 2).astype(np.float64)


def measure(path):
    try:
        a = decode(path)
    except Exception as e:                               # noqa: BLE001
        return {'error': str(e)}
    if len(a) < 64:
        return {'error': 'empty'}
    mono = a.mean(axis=1)
    m = np.abs(a).max(axis=1)
    peak = float(m.max())
    if peak < 1e-5:
        return {'error': 'silent'}
    live = np.nonzero(m > peak * 10 ** (-50 / 20))[0]
    a0, a1 = int(live[0]), int(live[-1])
    body = mono[a0:a1 + 1]
    win = int(0.25 * SR)
    loud = max(float(np.sqrt(np.mean(body[i:i + win] ** 2))) for i in range(0, max(1, len(body) - win), max(1, win // 4)))
    n = min(len(body), 1 << 18)
    spec = np.abs(np.fft.rfft(body[:n] * np.hanning(n))) ** 2 + 1e-18
    f = np.fft.rfftfreq(n, 1 / SR)
    tot = spec.sum()
    bands = [float(spec[(f >= lo) & (f < hi)].sum() / tot) for lo, hi in ((0, 150), (150, 600), (600, 2500), (2500, 8000), (8000, 22050))]
    k = int(0.005 * SR)
    env = np.sqrt(np.mean(body[:len(body) // k * k].reshape(-1, k) ** 2, axis=1)) if len(body) >= k else np.array([abs(body).max()])
    ip = int(np.argmax(env))
    after = np.nonzero(env[ip:] < env[ip] * 0.1)[0]
    lr = float(np.corrcoef(a[a0:a1 + 1, 0], a[a0:a1 + 1, 1])[0, 1]) if a1 - a0 > 100 and a[:, 0].std() > 0 and a[:, 1].std() > 0 else 1.0
    return {'secs': round((a1 - a0 + 1) / SR, 3), 'file_secs': round(len(a) / SR, 3), 'lead': round(a0 / SR, 3),
            'peak_db': round(20 * np.log10(peak), 1), 'loud_db': round(20 * np.log10(loud + 1e-9), 1),
            'bands': [round(b, 3) for b in bands], 'centroid': round(float((f * spec).sum() / tot)),
            'flat': round(float(np.exp(np.mean(np.log(spec))) / np.mean(spec)), 4),
            'attack': round(ip * 0.005, 3), 'decay': round((int(after[0]) if len(after) else len(env) - ip) * 0.005, 3),
            'pitch': round(float(f[int(np.argmax(spec[1:])) + 1])), 'stereo': round(1.0 - lr, 2)}


def main():
    packs = json.load(open(os.path.join(HERE, 'sources.json'), encoding='utf-8'))['packs']
    skip = {p['slug'] for p in packs if p.get('exclude')}
    author = {p['slug']: p['author'] for p in packs}
    files = []
    for slug in sorted(os.listdir(SRC)):
        d = os.path.join(SRC, slug, 'files')
        if slug in skip or not os.path.isdir(d):
            continue
        for base, _, names in os.walk(d):
            for nm in names:
                if nm.lower().endswith(EXT):
                    files.append((slug, os.path.join(base, nm)))
    print(len(files), 'audio files in', len({s for s, _ in files}), 'packs', flush=True)
    with ThreadPoolExecutor(6) as ex:
        res = list(ex.map(lambda sf: measure(sf[1]), files))
    out = []
    for (slug, path), r in zip(files, res):
        out.append(dict(pack=slug, author=author.get(slug, '?'), path=os.path.relpath(path, SRC).replace('\\', '/'), **r))
    json.dump(out, open(os.path.join(SRC, 'inventory.json'), 'w', encoding='utf-8'), indent=0)
    bad = [o for o in out if 'error' in o]
    print('measured', len(out) - len(bad), '; unreadable or silent', len(bad))


if __name__ == '__main__':
    main()
