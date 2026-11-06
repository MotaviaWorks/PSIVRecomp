"""Battles and saves from battery saves, played by the test bot.

For each save (a folder of them, searched recursively; any layout the port
reads): find a walk on the world map that meets a random battle without
entering a town (tools/battle_probe.py), then let psiv_headless --bot play:
Continue, seven battles, each with a plan (attack, techniques, skills, items,
a macro, running, a mix), then save in slot 1. Then:

  enhanced  the same input with the port's additions on (two sets: battle
            depth, fog, effects, modern text, battle menus, 16:9, synthesized
            sound effects, your music; and the side panels with Algol): the
            game's RAM must come out the same as in Classic, and screenshots
            are taken in every battle
  reload    the save the bot wrote is loaded and Continued: the field must
            come back, with the slot the bot saved in RAM, and its checksum
            right
  bizhawk   (--bizhawk) the same bot in BizHawk (tools/bizhawk/bot.lua) plays
            its own battles; the port replays BizHawk's trace frame by frame,
            in Classic and with the additions, and the battery saves must be
            identical

  python tools/battle_test.py "imported saves/PSIV Saves"
  python tools/battle_test.py "imported saves/PSIV Saves" --only 8,25 --bizhawk

Results go to OUT (default build/bot): per save sNN.log (the bot's account),
sNN.movie, sNN.port.srm, screenshots in shots/, and results.json.
"""
import argparse, json, os, re, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import battle_probe as bp                      # noqa: E402

ROOT = bp.ROOT
EXE = bp.EXE
FONT = 'C:/Windows/Fonts/segoeui.ttf' if os.name == 'nt' else str(ROOT / 'res' / 'fonts' / 'NotoSans-Regular.ttf')
ENHANCED = {
    'e1': ['--depth', '2:2', '--fog', '1', '--fx', '1', '--text', f'{FONT}:2', '--battle-ui', '--wide', '52',
           '--sfx-synth'] + (['--custom-music', str(ROOT / 'music')] if (ROOT / 'music').is_dir() else []),
    'e2': ['--text', f'{FONT}:2', '--battle-ui', '--wide', '52', '--panels', '--algol', '--sfx-synth'],
}
SLOT = 0x100                                   # slot 1 in the port's save


def run(args, timeout=900):
    t = time.time()
    try:
        p = subprocess.run([str(EXE)] + [str(a) for a in args], capture_output=True, text=True, cwd=ROOT,
                           timeout=timeout)
        return p.returncode, p.stdout + p.stderr, time.time() - t
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        return 'timeout', out, time.time() - t


def fnv(log, game=True):
    m = re.search(r'game RAM FNV-1a (\w+)' if game else r'RAM FNV-1a (\w+), audio', log)
    return m.group(1) if m else None


def battles_of(log):
    """the bot's account: [{n, plan, start, end, outcome, rounds, picks: [...]}]"""
    out, cur = [], None
    for line in log.splitlines():
        m = re.match(r'bot (\d+): battle (\d+) \((\w+)\)', line)
        if m:
            cur = {'n': int(m.group(2)), 'plan': m.group(3), 'start': int(m.group(1)), 'picks': [], 'rounds': []}
            out.append(cur)
            continue
        m = re.match(r'bot (\d+): battle \d+ over after (\d+) rounds?: (.*)', line)
        if m and cur:
            cur.update(end=int(m.group(1)), outcome=m.group(3))
            continue
        m = re.match(r'bot (\d+): round (\d+): (\w+)', line)
        if m and cur:
            cur['rounds'].append(int(m.group(1)))
            continue
        m = re.match(r'bot (\d+):   (.*)', line)
        if m and cur:
            cur['picks'].append((int(m.group(1)), m.group(2)))
    return out


def shot_frames(battles, log):
    """logical frames worth a look: each battle's first menu, its first action
    under way, and its last message; the field; the save message"""
    fr = {800: 'field'}
    for b in battles:
        if b['rounds']:
            fr[b['rounds'][0] + 24] = f"b{b['n']}-menu"
        acts = [t for t, p in b['picks'] if not p.startswith('macro')] or [t for t, _ in b['picks']]
        if acts:
            fr[acts[-1] + 150] = f"b{b['n']}-action"
        if 'end' in b:
            fr[b['end'] - 40] = f"b{b['n']}-end"
    m = re.search(r'bot (\d+): saving', log)
    if m:
        fr[int(m.group(1)) + 300] = 'saved'
    return fr


def check_slot(srm: Path):
    """the port's save: slot 1's checksum (the 16-bit sum of its $A00 bytes)"""
    b = srm.read_bytes()
    s = b[SLOT:SLOT + 0xA00]
    want = int.from_bytes(b[0x14:0x16], 'big')
    return (sum(s) & 0xFFFF) == want, s


def test_save(n, base: Path, out: Path, found: dict, a):
    res = {'save': n, 'what': bp.describe(base.read_bytes())}
    f = found.get(str(n))
    if not f:
        res['skip'] = 'no battle found on a walk from here'
        return res
    walk = f['how'].split(',')[0]
    srm = Path(f['srm'])
    sn = f's{n:02d}' + (f'v{a.vehicle}' if a.vehicle else '')
    log_p, mv, port_srm = out / f'{sn}.log', out / f'{sn}.movie', out / f'{sn}.port.srm'
    rc, log, dt = run(['--sram', srm, '--bot', f'{n}:{walk}:{a.battles}:{a.vehicle}', '--bot-movie', mv,
                       '--sram-out', port_srm, '--frames', 400000])
    log_p.write_text(log)
    res.update(classic_rc=rc, classic_s=round(dt, 1), fnv=fnv(log), fnv_all=fnv(log, False))
    res['battles'] = battles_of(log)
    res['done'] = 'saved; done' in log
    m = re.search(r'frames (\d+), RAM', log)
    frames = int(m.group(1)) if m else 0
    res['frames'] = frames
    if rc != 0 or not res['done']:
        res['problem'] = f'classic run: exit {rc}, ' + ('finished' if res['done'] else 'did not finish')
        return res
    # the same input with the additions
    shots = shot_frames(res['battles'], log)
    sd = out / 'shots'
    sd.mkdir(exist_ok=True)
    for tag, flags in ENHANCED.items():
        if tag not in a.sets:
            continue
        tag_srm = out / f'{sn}.{tag}.srm'
        args = ['--sram', srm, '--movie', mv, '--frames', frames + 1, '--sram-out', tag_srm] + flags
        for fr, name in shots.items():
            args += ['--shot', f'{fr}:{(sd / f"{sn}_{tag}_{name}.png").as_posix()}']
        rc2, log2, dt2 = run(args)
        (out / f'{sn}.{tag}.log').write_text(log2)
        # the game's state: its RAM (less display data) and the battery save it wrote
        same_save = tag_srm.exists() and port_srm.exists() and tag_srm.read_bytes() == port_srm.read_bytes()
        res[tag] = {'rc': rc2, 's': round(dt2, 1), 'fnv': fnv(log2), 'same_save': same_save,
                    'same': fnv(log2) == res['fnv'] and same_save}
    # the save it wrote: checksum, and Continue brings it back
    ok, slot = check_slot(port_srm)
    res['checksum_ok'] = ok
    cont = out / 'continue.movie'
    cont.write_text('# psiv movie v1\n' + '\n'.join(bp.CONTINUE) + '\n')
    dump = out / f'{sn}.reload'
    rc3, log3, _ = run(['--sram', port_srm, '--movie', cont, '--frames', 1000, '--dump-at', 720,
                        '--dump-to', dump.as_posix(),
                        '--shot', f'720:{(sd / f"{sn}_reload.png").as_posix()}'])
    (out / f'{sn}.reload.log').write_text(log3)
    res['reload_rc'] = rc3
    res['reload_field'] = bool(re.search(r'game mode 3\b', log3))
    ram = Path(str(dump) + '.ram.bin')
    if ram.exists():
        r = ram.read_bytes()
        res['reload_slot_diff'] = [f'{0xF100 + i:04X}' for i in range(0xA00) if r[0xF100 + i] != slot[i]]
    return res


def compare_shots(trace: Path, srm, bz_shots, port_dir: Path):
    """BizHawk's screenshots (named by logical frame) against the port's
    replay of the same trace: for each, the port frame within 2 that matches
    best (BizHawk shows lag frames too); pixels off by more than palette
    rounding"""
    from PIL import Image, ImageChops
    lo, hi = int(bz_shots[0].stem), int(bz_shots[-1].stem)
    if port_dir.exists():
        for p in port_dir.glob('*.png'):
            p.unlink()
    port_dir.mkdir(parents=True, exist_ok=True)
    run(['--replay', trace, '--sram', srm, '--shot-every', f'1:{port_dir.as_posix()}',
         '--shot-range', f'{max(1, lo - 2)}-{hi + 2}', '--frames', 10 ** 7])
    worst, same = [], 0
    for s in bz_shots:
        A = Image.open(s).convert('RGB')
        best = None
        for o in (-2, -1, 0, 1, 2):
            q = port_dir / f'{int(s.stem) + o:06d}.png'
            if q.exists():
                B = Image.open(q).convert('RGB')
                if B.size == A.size:
                    d = sum(ImageChops.difference(A, B).convert('L').histogram()[40:])
                    best = d if best is None else min(best, d)
        if best is not None:
            same += best == 0
            worst.append((best, s.stem))
    worst.sort(reverse=True)
    return {'shots': len(bz_shots), 'identical': same, 'worst': worst[:3]}


def emuhawk_running():
    if os.name != 'nt':
        return False
    p = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq EmuHawk.exe', '/NH'], capture_output=True, text=True)
    return 'EmuHawk.exe' in p.stdout


def bizhawk_one(n, f, out: Path, a):
    """the bot in BizHawk from the same save, then the port replays its trace:
    in Classic (check.py, which also compares the battery saves) and with the
    first set of additions"""
    res = {'save': n}
    while emuhawk_running():                   # never a second EmuHawk next to yours
        print('EmuHawk is open; waiting for it to close...', flush=True)
        time.sleep(60)
    walk = f['how'].split(',')[0]
    name = f'bot_s{n:02d}' + (f'v{a.vehicle}' if a.vehicle else '')
    t = time.time()
    # BizHawk's screenshots while a gravity technique's raster effect runs (bot.lua)
    shots = ROOT / 'build' / 'bizhawk' / f'shots_{name}'
    if shots.exists():
        for p in shots.glob('*.png'):
            p.unlink()
    shots.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PSIV_BOT_SHOTS=shots.as_posix())
    try:
        p = subprocess.run([sys.executable, str(ROOT / 'tools' / 'bizhawk' / 'check.py'), '--bot',
                            f'{n}:{walk}:{a.battles}:{a.vehicle}', '--sram', f['srm'], '--name', name],
                           capture_output=True, text=True, cwd=ROOT, timeout=2400, env=env)
        log = p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        log = 'TIMEOUT'
    (out / f'{name}.check.txt').write_text(log)
    res['s'] = round(time.time() - t)
    m = re.search(r'trace ended: all (\d+) logical frames match', log)
    res['frames'] = int(m.group(1)) if m else None
    res['match'] = bool(m)
    m = re.search(r'logical frame (\d+) \(port frame \d+\): MISMATCH', log)
    res['first_mismatch'] = int(m.group(1)) if m else None
    res['battery'] = 'battery save identical' in log
    bot_txt = ROOT / 'build' / 'bizhawk' / f'{name}.bot.txt'
    res['battles'] = battles_of(bot_txt.read_text()) if bot_txt.exists() else []
    res['done'] = bot_txt.exists() and 'saved; done' in bot_txt.read_text()
    trace = ROOT / 'build' / 'bizhawk' / f'{name}.trace'
    bz_shots = sorted(shots.glob('*.png'))
    if res['match'] and bz_shots:
        res['raster'] = compare_shots(trace, f['srm'], bz_shots, out / f'{name}_port_shots')
    if res['match'] and 'e1' in a.sets:
        # the additions bar 16:9 (its sprite table holds the margins' sprites, which the
        # trace would count; the port-side runs check its battery saves instead)
        flags = list(ENHANCED['e1'])
        i = flags.index('--wide')
        del flags[i:i + 2]
        rc, log2, _ = run(['--replay', trace, '--sram', f['srm'], '--keep-going'] + flags)
        (out / f'{name}.e1.txt').write_text(log2)
        m2 = re.search(r'trace ended: (all (\d+) logical frames match|(\d+) of (\d+) logical frames differ)', log2)
        res['e1_rc'] = rc
        res['e1_match'] = bool(m2 and m2.group(1).startswith('all'))
        res['e1_result'] = m2.group(1) if m2 else 'no result'
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder', type=Path)
    ap.add_argument('--only', help='save numbers, e.g. 8,25')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'bot')
    ap.add_argument('--battles', type=int, default=7)
    ap.add_argument('--vehicle', type=int, default=0, help='board a vehicle first: 1 Land Rover, 2 Ice Digger, 3 Hydrofoil')
    ap.add_argument('--sets', default='e1,e2', help='which sets of additions to replay with')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--bizhawk', action='store_true', help='also let the bot play in BizHawk (one save at a time)')
    ap.add_argument('--bizhawk-only', action='store_true', help='only the BizHawk part')
    a = ap.parse_args()
    a.bizhawk |= a.bizhawk_only
    # work from a copy of psiv_headless, so a rebuild during a long run can't
    # pull it away (check.py takes it from PSIV_HEADLESS too)
    global EXE
    a.out.mkdir(parents=True, exist_ok=True)
    snap = a.out / 'psiv_headless_run.exe' if os.name == 'nt' else a.out / 'psiv_headless_run'
    shutil.copy2(EXE, snap)
    EXE = snap
    os.environ['PSIV_HEADLESS'] = str(snap)
    a.sets = a.sets.split(',') if a.sets else []
    saves = bp.saves_in(a.folder)
    if a.only:
        keep = {int(x) for x in a.only.split(',')}
        saves = [(n, p) for n, p in saves if n in keep]
    a.out.mkdir(parents=True, exist_ok=True)
    bases = {}
    for n, p in saves:
        bases[n] = a.out / f'save{n:02d}.srm'
        bases[n].write_bytes(bp.port_save(p.read_bytes()))
    found_p = a.out / 'found.json'
    found = json.loads(found_p.read_text()) if found_p.exists() else {}
    todo = [n for n, _ in saves if str(n) not in found]
    with ThreadPoolExecutor(a.jobs) as ex:
        for n, f in zip(todo, ex.map(lambda n: bp.find(n, bases[n], a.out), todo)):
            found[str(n)] = f
            print(f'save {n:02d}: ' + (f"battle at logical {f['start']} ({f['how']})" if f else 'no battle'), flush=True)
    found_p.write_text(json.dumps(found, indent=1))
    if a.bizhawk:
        bres = []
        bz_p = a.out / (f'bizhawk_v{a.vehicle}.json' if a.vehicle else 'bizhawk.json')
        for n, _ in saves:
            f = found.get(str(n))
            if not f:
                continue
            r = bizhawk_one(n, f, a.out, a)
            bres.append(r)
            bz_p.write_text(json.dumps(bres, indent=1))
            bs = r['battles']
            print(f"bizhawk save {n:02d}: {len(bs)} battles "
                  f"({', '.join(b['plan'] + ':' + b.get('outcome', '?') for b in bs)}), "
                  + (f"all {r['frames']} frames match" if r['match'] else f"MISMATCH at {r['first_mismatch']}")
                  + (', battery save identical' if r['battery'] else ', battery save DIFFERS')
                  + (f", e1 {r.get('e1_result')}" if 'e1_result' in r else '')
                  + (f", gravity frames {r['raster']['identical']}/{r['raster']['shots']} identical "
                     f"(worst {r['raster']['worst'][:1]})" if 'raster' in r else '') + f" ({r['s']} s)", flush=True)
        if a.bizhawk_only:
            return
    results = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(lambda np: test_save(np[0], bases[np[0]], a.out, found, a), saves):
            results.append(r)
            bs = r.get('battles', [])
            line = f"save {r['save']:02d}: "
            if 'skip' in r:
                line += r['skip']
            else:
                line += f"{len(bs)} battles ({', '.join(b['plan'] + ':' + b.get('outcome', '?') for b in bs)})"
                if 'problem' in r:
                    line += '  PROBLEM: ' + r['problem']
                for tag in a.sets:
                    if tag in r:
                        line += f"  {tag} {'same' if r[tag]['same'] else 'DIFFERS'}"
                if 'checksum_ok' in r:
                    line += f"  save {'ok' if r['checksum_ok'] else 'BAD CHECKSUM'}"
                if 'reload_field' in r:
                    line += f"  reload {'ok' if r['reload_field'] else 'FAILED'}"
            print(line, flush=True)
    (a.out / (f'results_v{a.vehicle}.json' if a.vehicle else 'results.json')).write_text(json.dumps(results, indent=1))


if __name__ == '__main__':
    main()
