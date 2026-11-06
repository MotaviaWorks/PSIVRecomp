"""Which sound effects the battles ask for, and what asks for them.

Replays the test bot's movies (tools/battle_test.py, build/bot/sNN.movie)
with every sound request logged (PSIV_SFX_LOG=1) and the battle's current
command ($FF4146: high byte 1 attack, 2 technique, 3 skill, 4 item; low
byte the ID) and actor ($FF4142: 1-5 party, 6+ enemies) logged as they
change. Each request is put down to the command in force when it came.

    python tools/sfx_census.py                 every movie in build/bot
    python tools/sfx_census.py --only 39,44
"""
import argparse
import json
import re
import subprocess
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / 'build' / 'native-Release' / 'psiv_headless.exe'
BOT = ROOT / 'build' / 'bot'


def sfx_names():
    names = {}
    for line in (ROOT / 'disasm' / 'ps4disasm' / 'ps4.constants.asm').read_text(errors='replace').splitlines():
        m = re.match(r'SFXID_(\w+)\s*=\s*id\(\w+\)\s*;\s*\$([0-9A-F]+)', line)
        if m:
            names[int(m.group(2), 16)] = m.group(1)
    return names


def id_names(prefix):
    names = {}
    for line in (ROOT / 'disasm' / 'ps4disasm' / 'ps4.constants.asm').read_text(errors='replace').splitlines():
        m = re.match(prefix + r'(\w+)\s*=\s*id\(\w+\)\s*;\s*\$?([0-9A-F]+)', line)
        if m and not m.group(1).endswith('_Mask'):
            v = m.group(2)
            names[int(v, 16) if not v.isdigit() or len(v) > 1 and line.split(';')[1].strip().startswith('$') else int(v)] = m.group(1)
    return names


def one(n, srm, frames):
    import os
    env = dict(os.environ, PSIV_SFX_LOG='1')
    p = subprocess.run([str(EXE), '--sram', srm, '--movie', str(BOT / f's{n:02d}.movie'), '--frames', str(frames),
                        '--ramlog', 'FF4146.w,FF4142.w,FFEF00.w', '--sfx-synth'], capture_output=True, text=True, cwd=ROOT, env=env)
    # the RAM log (stdout, by logical frame) and the requests (stderr, by port
    # frame: the logical frame less one) come apart: merge them by frame
    events = []
    for line in p.stdout.splitlines():
        m = re.match(r'ram (\d+): (.*)', line)
        if m:
            events.append((int(m.group(1)) - 1, 0, re.findall(r'(\w{4})=(\w{4})', m.group(2))))
    for line in p.stderr.splitlines():
        m = re.match(r'frame (\d+): sound request (\w\w)', line)
        if m:
            events.append((int(m.group(1)), 1, int(m.group(2), 16)))
    events.sort(key=lambda e: (e[0], e[1]))
    cmd, actor, mode, out = 0, 0, 0, []
    for f, kind, data in events:
        if kind == 0:
            for k, v in data:
                v = int(v, 16)
                if k == '4146':
                    cmd = v
                elif k == '4142':
                    actor = v
                elif k == 'EF00':
                    mode = v
        elif mode == 0x14:                           # in battle
            out.append((f, data, cmd, actor))
    return n, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only')
    a = ap.parse_args()
    found = json.loads((BOT / 'found.json').read_text())
    jobs = []
    for mv in sorted(BOT.glob('s[0-9][0-9].movie')):
        n = int(mv.stem[1:])
        if a.only and n not in {int(x) for x in a.only.split(',')}:
            continue
        log = BOT / f's{n:02d}.log'
        m = re.search(r'frames (\d+), RAM', log.read_text()) if log.exists() else None
        if found.get(str(n)) and m:
            jobs.append((n, found[str(n)]['srm'], int(m.group(1)) + 1))
    sfx = sfx_names()
    techs, skills, items = id_names('TechID_'), id_names('SkillID_'), id_names('ItemID_')
    total = Counter()
    by_cmd = defaultdict(Counter)
    with ThreadPoolExecutor(8) as ex:
        for n, reqs in ex.map(lambda j: one(*j), jobs):
            for f, sid, cmd, actor in reqs:
                total[sid] += 1
                kind, cid = cmd >> 8, cmd & 0xFF
                who = 'party' if 1 <= actor <= 5 else 'enemy' if actor >= 6 else '-'
                what = {1: 'attack', 2: 'tech ' + techs.get(cid, hex(cid)), 3: 'skill ' + skills.get(cid, hex(cid)),
                        4: 'item ' + items.get(cid, hex(cid)), 5: 'defend'}.get(kind, f'cmd {cmd:04X}')
                by_cmd[sid][f'{who} {what}'] += 1
    print(f'{len(jobs)} movies; sound requests in battle:')
    for sid, c in total.most_common():
        top = ', '.join(f'{k} x{v}' for k, v in by_cmd[sid].most_common(4))
        print(f'  ${sid:02X} {sfx.get(sid, "?"):20s} {c:5d}   {top}')
    (BOT / 'sfx_census.json').write_text(json.dumps({f'{k:02X}': {'name': sfx.get(k), 'count': v,
                                                                'by': dict(by_cmd[k].most_common())}
                                                           for k, v in total.items()}, indent=1))


if __name__ == '__main__':
    main()
