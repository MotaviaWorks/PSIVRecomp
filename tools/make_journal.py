"""The journal's words (runtime/journal.txt) into runtime/journal_data.h.

    python tools/make_journal.py            write journal_data.h
    python tools/make_journal.py --check    only check journal.txt

journal.txt, a line each ('#' starts a comment; a line that starts with a
space carries on the one before):

    arc NAME                  the part of the game the steps after it are in
    step FLAG FLAG ...        a story step, reached when all its flags are set
                              (A|B: either; START: from the start)
    title / done / next / hint   its words (done: what happened; next: what to
                              do now; hint: help, shown when asked for)

    job NAME                  a Hunters' Guild job (HuntersGuildData's order)
    flags TAKEN COMPLETED FEE FROM UNTIL
    about ...                 what the client wants
    stage FLAG ... | next | hint   after taking it: what to do (the last stage
                              whose flags are set is shown)
    reward ...                completed: how to collect the fee

The flags are the disassembly's EventFlag_ names (ps4.constants.asm); the
header holds their numbers, so building needs no disassembly.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'runtime', 'journal.txt')
OUT = os.path.join(ROOT, 'runtime', 'journal_data.h')
CONST = os.path.join(ROOT, 'disasm', 'ps4disasm', 'ps4.constants.asm')
JT, JA = 4, 3                                  # journal.c: terms in a condition, flags in a term
LIMITS = {'title': 32, 'done': 240, 'next': 170, 'hint': 260, 'about': 220, 'reward': 170, 'arc': 24}


def flag_numbers():
    names = {}
    for line in open(CONST, encoding='latin-1'):
        m = re.match(r'EventFlag_(\w+)\s*=\s*(\$[0-9A-Fa-f]+|\d+)', line)
        if m:
            v = m.group(2)
            names[m.group(1)] = int(v[1:], 16) if v.startswith('$') else int(v)
    return names


def parse(path):
    """the steps and the jobs, and the problems found"""
    lines, problems = [], []
    for no, raw in enumerate(open(path, encoding='utf-8'), 1):
        line = raw.rstrip('\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        if line[0] in ' \t' and lines:
            lines[-1] = (lines[-1][0], lines[-1][1] + ' ' + line.strip())
        else:
            lines.append((no, line.strip()))
    steps, jobs, arc, cur = [], [], '', None
    for no, line in lines:
        key, _, rest = line.partition(' ')
        rest = rest.strip()
        if any(ord(c) > 126 for c in rest):
            problems.append(f'{no}: not ASCII: {rest!r}')
        if key == 'arc':
            arc = rest
        elif key == 'step':
            cur = {'line': no, 'when': rest.split(), 'arc': arc}
            steps.append(cur)
        elif key == 'job':
            cur = {'line': no, 'name': rest, 'stages': []}
            jobs.append(cur)
        elif key in ('title', 'done', 'next', 'hint', 'about', 'reward') and cur is not None:
            if key in cur:
                problems.append(f'{no}: a second {key}')
            cur[key] = rest
        elif key == 'flags' and cur is not None and 'stages' in cur:
            cur['flags'] = rest.split()
        elif key == 'stage' and cur is not None and 'stages' in cur:
            parts = [p.strip() for p in rest.split('|')]
            if len(parts) != 3:
                problems.append(f'{no}: a stage is FLAGS | next | hint')
                parts += [''] * 3
            cur['stages'].append({'line': no, 'when': parts[0].split(), 'next': parts[1], 'hint': parts[2]})
        else:
            problems.append(f'{no}: what is {key!r}?')
    return steps, jobs, problems


def cond(words, names, where, problems):
    """['A', 'B|C'] -> [[a], [b, c]] as numbers (START: no terms)"""
    if words == ['START']:
        return []
    terms = []
    for w in words:
        alts = []
        for name in w.split('|'):
            if name not in names:
                problems.append(f'{where}: no flag {name}')
            else:
                alts.append(names[name])
        if len(alts) > JA:
            problems.append(f'{where}: more than {JA} flags in {w}')
        terms.append(alts[:JA])
    if not terms:
        problems.append(f'{where}: no flags')
    if len(terms) > JT:
        problems.append(f'{where}: more than {JT} terms')
    return terms[:JT]


def c_str(s):
    return '"' + (s or '').replace('\\', '\\\\').replace('"', '\\"') + '"'


def c_cond(terms):
    rows = []
    for t in range(JT):
        alts = terms[t] if t < len(terms) else []
        rows.append('{' + ', '.join(str(alts[a]) if a < len(alts) else '-1' for a in range(JA)) + '}')
    return '{{' + ', '.join(rows) + '}}'


def check_text(item, keys, where, problems):
    for k in keys:
        v = item.get(k)
        if v is None:
            problems.append(f'{where}: no {k}')
        elif len(v) > LIMITS[k]:
            problems.append(f'{where}: {k} is {len(v)} characters (at most {LIMITS[k]})')


def build():
    names = flag_numbers()
    steps, jobs, problems = parse(SRC)
    for s in steps:
        where = f'line {s["line"]}'
        s['terms'] = cond(s['when'], names, where, problems)
        check_text(s, ['title', 'next', 'hint'] + ([] if s['when'] == ['START'] else ['done']), where, problems)
    for j in jobs:
        where = f'job {j["name"]!r}'
        f = j.get('flags', [])
        if len(f) != 5:
            problems.append(f'{where}: flags are TAKEN COMPLETED FEE FROM UNTIL')
            f = (f + ['GuildPlaceholder'] * 5)[:5]
        j['nums'] = [names.get(x, 0) for x in f]
        for x in f:
            if x not in names:
                problems.append(f'{where}: no flag {x}')
        check_text(j, ['about', 'reward'], where, problems)
        if not j['stages']:
            problems.append(f'{where}: no stages')
        for st in j['stages']:
            st['terms'] = cond(st['when'], names, f'line {st["line"]}', problems)
            check_text(st, ['next', 'hint'], f'line {st["line"]}', problems)
            st.setdefault('next', '')
    return steps, jobs, problems


def write(steps, jobs):
    out = ['/* Made by tools/make_journal.py from runtime/journal.txt: edit that, then run it. */', '',
           'static const Step STEPS[] = {']
    for s in steps:
        out.append(f'    {{{c_cond(s["terms"])},')
        out.append(f'     {{{c_str(s["arc"])}, {c_str(s.get("title"))},')
        out.append(f'      {c_str(s.get("done"))},')
        out.append(f'      {c_str(s.get("next"))},')
        out.append(f'      {c_str(s.get("hint"))}}}}},')
    out.append('};')
    for i, j in enumerate(jobs):
        out += ['', f'static const Stage JOB{i}[] = {{']
        for st in j['stages']:
            out.append(f'    {{{c_cond(st["terms"])},')
            out.append(f'     {c_str(st["next"])},')
            out.append(f'     {c_str(st["hint"])}}},')
        out.append('};')
    out += ['', 'static const Job JOBS[] = {']
    for i, j in enumerate(jobs):
        t, c, fee, fr, un = j['nums']
        out.append(f'    {{0x{t:02X}, 0x{c:02X}, 0x{fee:02X}, 0x{fr:02X}, 0x{un:02X}, {c_str(j["name"])},')
        out.append(f'     {c_str(j.get("about"))},')
        out.append(f'     {c_str(j.get("reward"))},')
        out.append(f'     {len(j["stages"])}, JOB{i}}},')
    out.append('};')
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    steps, jobs, problems = build()
    for p in problems:
        print('journal.txt', p)
    if problems:
        sys.exit(1)
    if '--check' not in sys.argv:
        write(steps, jobs)
        print(f'{OUT}: {len(steps)} steps, {len(jobs)} jobs')
    else:
        print(f'journal.txt: {len(steps)} steps, {len(jobs)} jobs, no problems')
