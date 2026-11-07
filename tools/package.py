"""Package a build for testers: the program, its libraries, the bundled font,
the tester README (docs/TESTERS.md) and the third-party licences. No game
data: no ROM, saves, samples, music or art -- players bring their own ROM.

  python tools/package.py windows BUILD_DIR OUT_DIR [--sdl-license FILE]
  python tools/package.py linux   BUILD_DIR OUT_DIR [--sdl-license FILE]

windows -> OUT_DIR/PSIV-Recomp-<version>-windows-x64.zip   (folder "PSIV Recomp")
linux   -> OUT_DIR/PSIV-Recomp-<version>-linux-x86_64.tar.gz (folder "psiv-recomp")

The version comes from the build (CMakeCache: PSIV_VERSION, else the
commit); the file name drops the "+commit" part.
"""
import argparse, os, re, shutil, subprocess, sys, tarfile, tempfile, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def version(build):
    """the version the build was made with: its CMakeCache, else git"""
    try:
        cache = open(os.path.join(build, 'CMakeCache.txt'), encoding='utf-8').read()
        m = re.search(r'^PSIV_VERSION:STRING=(.+)$', cache, re.M)
        if m and m.group(1).strip():
            return m.group(1).strip()
    except OSError:
        pass
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True,
                            text=True).stdout.strip()
    return '0.1.0-alpha+' + commit if commit else '0.1.0-alpha'


def stb_license():
    text = open(os.path.join(ROOT, 'third_party', 'stb', 'stb_truetype.h'), encoding='utf-8').read()
    start = text.index('This software is available under 2 licenses')
    return text[start:].rstrip().rstrip('*/').rstrip() + '\n'


def licenses(dest, sdl_license):
    os.makedirs(dest, exist_ok=True)
    files = {
        'SDL3.txt': sdl_license or os.path.join(ROOT, 'third_party', 'SDL3-3.4.20', 'LICENSE.txt'),
        'ymfm.txt': os.path.join(ROOT, 'third_party', 'ymfm', 'LICENSE'),
        'z80.h.txt': os.path.join(ROOT, 'third_party', 'chips', 'LICENSE'),
        'NotoSans-OFL.txt': os.path.join(ROOT, 'res', 'fonts', 'OFL.txt'),
    }
    for name, src in files.items():
        shutil.copyfile(src, os.path.join(dest, name))
    with open(os.path.join(dest, 'stb_truetype.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('stb_truetype.h -- Sean Barrett and contributors\n\n' + stb_license())


def stage(kind, build, folder, sdl_license):
    os.makedirs(folder)
    if kind == 'windows':
        for f in ('psiv.exe', 'SDL3.dll'):
            shutil.copy2(os.path.join(build, f), folder)
    else:
        shutil.copy2(os.path.join(build, 'psiv'), folder)
        os.chmod(os.path.join(folder, 'psiv'), 0o755)
        shutil.copyfile(os.path.join(ROOT, 'res', 'psiv.png'), os.path.join(folder, 'psiv.png'))
    fonts = os.path.join(folder, 'fonts')
    os.makedirs(fonts)
    for f in ('NotoSans-Regular.ttf', 'OFL.txt'):
        shutil.copyfile(os.path.join(ROOT, 'res', 'fonts', f), os.path.join(fonts, f))
    nl = '\r\n' if kind == 'windows' else '\n'
    readme = open(os.path.join(ROOT, 'docs', 'TESTERS.md'), encoding='utf-8').read()
    with open(os.path.join(folder, 'README.txt'), 'w', encoding='utf-8', newline=nl) as f:
        f.write(readme)
    licenses(os.path.join(folder, 'LICENSES'), sdl_license)
    # the port's own licence (GPL-3.0): it travels with every copy
    text = open(os.path.join(ROOT, 'LICENSE'), encoding='utf-8').read()
    with open(os.path.join(folder, 'LICENSE.txt'), 'w', encoding='utf-8', newline=nl) as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kind', choices=['windows', 'linux'])
    ap.add_argument('build')
    ap.add_argument('out')
    ap.add_argument('--sdl-license', help="SDL3's LICENSE.txt (default: third_party/SDL3-3.4.20)")
    args = ap.parse_args()
    ver = version(args.build)
    name_ver = ver.split('+')[0]
    os.makedirs(args.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        if args.kind == 'windows':
            top = 'PSIV Recomp'
            stage('windows', args.build, os.path.join(tmp, top), args.sdl_license)
            dest = os.path.join(args.out, f'PSIV-Recomp-{name_ver}-windows-x64.zip')
            with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for d, _, files in sorted(os.walk(os.path.join(tmp, top))):
                    for f in sorted(files):
                        p = os.path.join(d, f)
                        z.write(p, os.path.relpath(p, tmp))
        else:
            top = 'psiv-recomp'
            stage('linux', args.build, os.path.join(tmp, top), args.sdl_license)
            dest = os.path.join(args.out, f'PSIV-Recomp-{name_ver}-linux-x86_64.tar.gz')
            with tarfile.open(dest, 'w:gz') as t:
                t.add(os.path.join(tmp, top), arcname=top)
    print(f'{dest} ({os.path.getsize(dest) // 1024} KB, version {ver})')


if __name__ == '__main__':
    sys.exit(main())
