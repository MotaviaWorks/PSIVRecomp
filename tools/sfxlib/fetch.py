"""Download the modern sounds' raw material: the CC0 packs listed in sources.json.

    python tools/sfxlib/fetch.py            every pack not here yet
    python tools/sfxlib/fetch.py SLUG ...   only these

Each pack goes into its own folder, build/sfxlib-src/<slug>/ (build/ is never committed), with the files as
downloaded and SHA256.txt beside them; archives are unpacked into <slug>/files/ (.7z with the py7zr module). Nothing in a pack is run. Run it only for packs PsiLok has said yes to.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEST = os.path.join(ROOT, 'build', 'sfxlib-src')
BASE = 'https://opengameart.org/sites/default/files/'


def fetch(url, path):
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0 (sound library fetch)'})
    with urllib.request.urlopen(req, timeout=120) as r, open(path + '.part', 'wb') as f:
        shutil.copyfileobj(r, f)
    os.replace(path + '.part', path)


def unpack(path, into):
    os.makedirs(into, exist_ok=True)
    low = path.lower()
    if low.endswith('.zip'):
        with zipfile.ZipFile(path) as z:
            for m in z.infolist():                       # (no paths that climb out of the folder)
                target = os.path.realpath(os.path.join(into, m.filename))
                if not target.startswith(os.path.realpath(into) + os.sep) and target != os.path.realpath(into):
                    raise SystemExit(f'{path}: a path outside the folder: {m.filename}')
            z.extractall(into)
    elif low.endswith('.7z'):
        import py7zr                                     # (pip install py7zr: this PC has no 7-Zip)
        with py7zr.SevenZipFile(path) as z:
            for name in z.getnames():
                target = os.path.realpath(os.path.join(into, name))
                if not target.startswith(os.path.realpath(into) + os.sep) and target != os.path.realpath(into):
                    raise SystemExit(f'{path}: a path outside the folder: {name}')
            z.extractall(into)
    else:
        shutil.copy2(path, into)


def main():
    packs = json.load(open(os.path.join(HERE, 'sources.json'), encoding='utf-8'))['packs']
    only = set(sys.argv[1:])
    for p in packs:
        if only and p['slug'] not in only:
            continue
        folder = os.path.join(DEST, p['slug'])
        os.makedirs(folder, exist_ok=True)
        sums = []
        for name, size in p['files']:
            local = os.path.join(folder, urllib.parse.unquote(name))
            if not os.path.exists(local):
                print(f"{p['slug']}: {urllib.parse.unquote(name)} ({size})", flush=True)
                fetch(BASE + name, local)
                unpack(local, os.path.join(folder, 'files'))
            sums.append(f"{hashlib.sha256(open(local, 'rb').read()).hexdigest()}  {os.path.basename(local)}")
        with open(os.path.join(folder, 'SHA256.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(f"{p['title']} by {p['author']} ({p['license']})\nhttps://opengameart.org/content/{p['slug']}\n" + '\n'.join(sums) + '\n')
    print('done:', DEST)


if __name__ == '__main__':
    main()
