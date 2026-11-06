"""Battles from battery saves, for trying the battle menus with every party.

For each save: Continue (slot 1), walk back and forth on the field until a
random battle starts (placing the party a few steps off first if a step
would enter a town), then run probes with the modern battle menus on:

  strip   Command: the first member's commands
  tech    ... Right, C: their technique list
  skill   ... Right, Right, C: their skill list
  fight   Command, then Attack and a target for everyone: the round plays

Screenshots go to OUT (default build/probe): {probe}{NN}_{offset}.png, the
offset in logical frames from the battle's start. The movies are kept
(s{NN}_{probe}.movie) and work with tools/bizhawk/check.py --movie M --sram S,
S being the save the battle was found from (listed in OUT/found.json).

Saves can be the port's, BizHawk's or Gens / Kega Fusion's .srm. Keep
saves you collect out of git ("imported saves/" is ignored).

  python tools/battle_probe.py --list "imported saves/PSIV Saves"
  python tools/battle_probe.py "imported saves/PSIV Saves" --probes fight --only 8,25
"""
import argparse, json, os, re, struct, subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / 'build' / 'native-Release' / 'psiv_headless.exe'
if not EXE.exists():
    EXE = ROOT / 'build' / 'native' / 'psiv_headless'
CONTINUE = ['290 S 4', '460 S 4', '560 C 2', '620 C 2']   # title -> Continue -> slot 1; the field from ~700
WALKS = {'LR': ('L', 'R'), 'UD': ('U', 'D'), 'RL': ('R', 'L'), 'DU': ('D', 'U')}
NUDGES = ((0, 0), (0, 64), (0, -64), (64, 0), (-64, 0), (64, 64), (-64, -64))
NAMES = ['Chaz', 'Alys', 'Hahn', 'Rune', 'Gryz', 'Rika', 'Demi', 'Wren', 'Raja', 'Kyra', 'Seth']
WORLDS = ['Motavia', 'Dezolis', 'Rykros', 'Zelan', 'Kuran', 'Air Castle']
SLOT = 0x100                                   # slot 1 in the port's save: RAM $FFF100-$FFFAFF


def port_save(b: bytes) -> bytes:
    """any of the layouts (see bus_sram_import in runtime/bus.c) -> the port's 8 KB"""
    mark = b'PHANTASY STAR 4'
    if len(b) == 0x4000:
        return bytes(b[1::2])
    if b[2:17] != mark and b[5:35:2] == mark:
        return bytes(b[1::2]).ljust(0x2000, b'\0')
    return bytes(b[:0x2000]).ljust(0x2000, b'\0')


def saves_in(folder: Path):
    """(number, path): *.srm under the folder, numbered by the digits ending their folder or name"""
    out = []
    for p in sorted(folder.rglob('*.srm')):
        m = re.search(r'(\d+)$', p.parent.name) or re.search(r'(\d+)$', p.stem)
        out.append((int(m.group(1)) if m else len(out) + 1, p))
    return out


def describe(save: bytes) -> str:
    s = save[SLOT:SLOT + 0xA00]
    w = lambda o: struct.unpack('>H', s[o:o + 2])[0]
    party = [f'{NAMES[c]} {w(0x400 + c * 0x80 + 8)}' for c in s[0x30A:0x30F] if c < len(NAMES)]
    world = WORLDS[s[0x300]] if s[0x300] < len(WORLDS) else str(s[0x300])
    return f'{world:8} map {w(0x304):3X} at {w(0x306)},{w(0x308)}  {", ".join(party)}  ({struct.unpack(">I", s[0x338:0x33C])[0]} meseta)'


def run(args):
    return subprocess.run([str(EXE)] + [str(a) for a in args], capture_output=True, text=True, cwd=ROOT).stdout


def nudged(base: Path, out: Path, n: int, dx: int, dy: int) -> Path:
    """the save with the party placed (dx, dy) pixels away; the slot's
    checksum (at byte $14) is the 16-bit sum of its $A00 bytes"""
    b = bytearray(base.read_bytes())
    if dx or dy:
        for o, d in ((0x306, dx), (0x308, dy)):
            v = int.from_bytes(b[SLOT + o:SLOT + o + 2], 'big') + d
            b[SLOT + o:SLOT + o + 2] = v.to_bytes(2, 'big')
        b[0x14:0x16] = (sum(b[SLOT:SLOT + 0xA00]) & 0xFFFF).to_bytes(2, 'big')
    p = out / f'save{n:02d}_{dx}_{dy}.srm'
    p.write_bytes(b)
    return p


def walk_lines(walk, until):
    a, b = WALKS[walk]
    lines, t, k = [f'760 {a} 24'], 760, 1
    while t + 24 < until:
        t += 24
        lines.append(f'{t} {b if k % 2 else a} 24')
        k += 1
    return lines


def find(n, base: Path, out: Path):
    for dx, dy in NUDGES:
        srm = nudged(base, out, n, dx, dy)
        for walk in WALKS:
            lines = walk_lines(walk, 760 + 3600)
            mv = out / f'search{n:02d}.movie'
            mv.write_text('\n'.join(['# psiv movie v1'] + CONTINUE + lines) + '\n')
            # Game_Mode_Index: 8 load a field map, $C field, $10 load a battle, $14 battle
            log = run(['--sram', srm, '--movie', mv, '--frames', 6000, '--ramlog', 'FFEF00.w'])
            start = None
            for m in re.finditer(r'ram (\d+): EF00=(\w+)', log):
                f, mode = int(m.group(1)), int(m.group(2), 16)
                if f < 760:
                    continue
                if mode == 8:
                    break                               # walked into a town or a cave
                if mode == 0x10:
                    start = f
                    break
            if start is None:
                continue
            cut = []
            for l in lines:
                s, k, d = l.split()
                if int(s) >= start:
                    break
                cut.append(f'{s} {k} {min(int(d), start - int(s))}')
            return {'srm': str(srm), 'walk': cut, 'start': start, 'how': f'{walk}, nudged {dx},{dy}'}
    return None


def probe(n, f, name, out: Path, font: str):
    st = f['start']
    if name == 'strip':
        keys, shots = [(160, 'C')], [230]
    elif name == 'tech':
        keys, shots = [(160, 'C'), (230, 'R'), (270, 'C')], [330]
    elif name == 'skill':
        keys, shots = [(160, 'C'), (230, 'R'), (260, 'R'), (300, 'C')], [360]
    else:                                               # everyone attacks
        keys, t = [(160, 'C')], 230
        for _ in range(5):
            keys += [(t, 'C'), (t + 40, 'C')]
            t += 90
        shots = list(range(t, t + 900, 60))
    mv = out / f's{n:02d}_{name}.movie'
    lines = (['# psiv movie v1', f'# battle probe "{name}" (start {st}; save {f["srm"]})'] + CONTINUE + f['walk']
             + [f'{st + d} {k} 2' for d, k in keys] + [f'{st + shots[-1] + 60} - 1'])
    mv.write_text('\n'.join(lines) + '\n')
    args = ['--sram', f['srm'], '--movie', mv, '--frames', st + shots[-1] + 1500, '--battle-ui', '--text', font]
    for d in shots:
        args += ['--shot', f'{st + d}:{out / f"{name}{n:02d}_{d}.png"}']
    run(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder', type=Path, help='a folder of battery saves (searched recursively)')
    ap.add_argument('--list', action='store_true', help='only list what each save holds')
    ap.add_argument('--only', help='save numbers, e.g. 8,25')
    ap.add_argument('--probes', default='strip,tech,skill,fight')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'probe')
    ap.add_argument('--font', default='C:/Windows/Fonts/segoeui.ttf:3', help='--text FONT[:SCALE] for the menus')
    a = ap.parse_args()
    saves = saves_in(a.folder)
    if a.only:
        keep = {int(x) for x in a.only.split(',')}
        saves = [(n, p) for n, p in saves if n in keep]
    if a.list:
        for n, p in saves:
            print(f'{n:3d}  {describe(port_save(p.read_bytes()))}')
        return
    a.out.mkdir(parents=True, exist_ok=True)
    bases = {}
    for n, p in saves:
        bases[n] = a.out / f'save{n:02d}.srm'
        bases[n].write_bytes(port_save(p.read_bytes()))
    found_path = a.out / 'found.json'
    found = json.loads(found_path.read_text()) if found_path.exists() else {}
    todo = [n for n, _ in saves if str(n) not in found]
    with ThreadPoolExecutor(8) as ex:
        for n, f in zip(todo, ex.map(lambda n: find(n, bases[n], a.out), todo)):
            found[str(n)] = f
            print(f'save {n:02d}: ' + (f"battle at logical {f['start']} ({f['how']})" if f else 'no battle'), flush=True)
    found_path.write_text(json.dumps(found, indent=1))
    jobs = [(n, found[str(n)], pr) for n, _ in saves if found.get(str(n)) for pr in a.probes.split(',')]
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(lambda j: probe(*j, a.out, a.font), jobs))
    print(f'{len(jobs)} probes; screenshots in {a.out}')


if __name__ == '__main__':
    main()
