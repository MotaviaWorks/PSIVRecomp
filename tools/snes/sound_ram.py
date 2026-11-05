"""Find where a SNES game keeps its instrument samples, for a new
instrument-bank recipe (runtime/snesbank.c).

1. Dump the sound RAM while the game plays (BizHawk, bsnes core, with a
   scratch copy of BizHawk's config so its own settings, recent files and
   saves are left alone):

     python tools/snes/sound_ram.py dump ROM.smc --frames 600,1200,2400 [--press 900:Start]

2. Find the sample directory in a dump (the DSP's DIR page: 4-byte entries,
   start and loop, pointing at BRR samples), and each sample's bytes in the
   ROM -- if the game stores them raw, that's where a recipe reads them:

     python tools/snes/sound_ram.py find ROM.smc build/snes/<game>/aram_01200.bin

3. Walk a chain of Lufia II-style records from an address the search found:

     python tools/snes/sound_ram.py chain ROM.smc 0x0C3828

Dumps go to build/snes/<rom name>/. The ROM is only read.
"""
import argparse, json, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIZHAWK = Path(os.environ.get('PSIV_BIZHAWK', r'H:\Emulation\08 - Tools\BizHawk\EmuHawk.exe'))


def rom_bytes(path):
    b = Path(path).read_bytes()
    return b[len(b) % 1024:]                    # a copier's 512-byte header


def dump(a):
    out = ROOT / 'build' / 'snes' / Path(a.rom).stem
    out.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((BIZHAWK.parent / 'config.ini').read_text(encoding='utf-8-sig'))
    for e in cfg['PathEntries']['Paths']:
        if e.get('System') == 'SNES' and e.get('Type') == 'Base':
            e['Path'] = str(out / 'bizhawk').replace('\\', '/')
    cfg['PreferredCores']['SNES'] = 'BSNESv115+'        # it has the sound RAM
    (out / 'bizhawk').mkdir(exist_ok=True)
    (out / 'bizhawk' / 'config.ini').write_text(json.dumps(cfg, indent=2), encoding='utf-8')
    env = dict(os.environ, SNES_OUT=str(out), SNES_FRAMES=a.frames, SNES_PRESS=a.press or '')
    subprocess.run([str(BIZHAWK), f'--config={out / "bizhawk" / "config.ini"}',
                    f'--lua={ROOT / "tools" / "snes" / "aram_dump.lua"}', str(Path(a.rom).resolve())],
                   cwd=BIZHAWK.parent, env=env, timeout=1800)
    if (out / 'error.txt').exists():
        sys.exit((out / 'error.txt').read_text())
    print('dumps in', out)


def chain_end(ram, start, limit=0x8000):
    o = start
    while o + 9 <= len(ram) and o - start < limit:
        h = ram[o]
        if h >> 4 > 12:
            return None
        o += 9
        if h & 1:
            return o
    return None


def find(a):
    rom, ram = rom_bytes(a.rom), Path(a.aram).read_bytes()

    def entries(page):
        out = []
        for i in range(128):
            p = page * 256 + i * 4
            if p + 4 > len(ram):
                break
            start, loop = ram[p] | ram[p + 1] << 8, ram[p + 2] | ram[p + 3] << 8
            end = chain_end(ram, start) if 0x200 <= start < 0xFF00 else None
            if end and end - start >= 9 * 6 and start <= loop < end and (loop - start) % 9 == 0:
                out.append((i, start, loop, end))
        return out

    # the page whose entries cover the most sample bytes
    page = max(range(256), key=lambda p: sum(e[3] - e[1] for e in entries(p)))
    print(f'sample directory at ${page:02X}00 in sound RAM')
    for i, start, loop, end in entries(page):
        chunk = ram[start:end]
        at = rom.find(chunk[:32])
        whole = at >= 0 and rom[at:at + len(chunk)] == chunk
        print(f'  #{i:02X} ${start:04X}-${end:04X} {(end - start) // 9:4d} blocks, loop block {(loop - start) // 9:4d}: '
              + (f'in the ROM at ${at:06X}' + ('' if whole else ' (start only)') if at >= 0 else 'not in the ROM as is (packed?)')
              + (f'; 8 bytes before: {rom[at - 8:at].hex(" ")}' if at >= 8 else ''))


def chain(a):
    rom = rom_bytes(a.rom)
    o, i = int(a.address, 0), 0
    while True:
        if rom[o:o + 8] == bytes([8, 0, 0, 0, 0, 0, 0, 0]):
            print(f'  {i:2d} ${o:06X} (empty slot)')
            o += 8
            i += 1
            continue
        ln, lp = rom[o] | rom[o + 1] << 8, rom[o + 2] | rom[o + 3] << 8
        if ln < 18 or ln % 9 or ln > 0x8000 or lp > ln or lp % 9 or chain_end(rom, o + 8, ln + 9) != o + 8 + ln:
            break
        t = rom[o + 6] + rom[o + 7] / 256
        print(f'  {i:2d} ${o:06X} {ln // 9:4d} blocks, loop block {lp // 9:4d}{"" if lp < ln else " (once)"}, '
              f'ADSR {rom[o + 4]:02X} {rom[o + 5]:02X}, tuning {t:.3f} = {2000 / t if t else 0:.1f} Hz at 32 kHz')
        o += 8 + ln
        i += 1
    print(f'{i} records, ending at ${o:06X}')


ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
sub = ap.add_subparsers(dest='cmd', required=True)
d = sub.add_parser('dump'); d.add_argument('rom'); d.add_argument('--frames', default='600,1200,2400,3600'); d.add_argument('--press')
f = sub.add_parser('find'); f.add_argument('rom'); f.add_argument('aram')
c = sub.add_parser('chain'); c.add_argument('rom'); c.add_argument('address')
a = ap.parse_args()
{'dump': dump, 'find': find, 'chain': chain}[a.cmd](a)
