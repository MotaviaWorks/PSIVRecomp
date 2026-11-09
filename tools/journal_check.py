"""The journal (runtime/journal.txt) against saves: for each save slot, the
step the journal would show, the steps before it that its flags don't reach
(a step in the wrong place, or one not every player sees), and the guild's
jobs. Saves in story order should never go back a step.

    python tools/journal_check.py SAVE.srm ...      (the port's 8 KB saves, or 16 KB)
    python tools/journal_check.py --jobs SAVE.srm   ... with the jobs
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_journal  # noqa: E402


def slots(path):
    """(slot, event flags as a set, town flags) for each slot in use"""
    d = open(path, 'rb').read()
    if len(d) == 0x4000:
        d = d[1::2]                              # every byte: the odd ones are the save
    for s in range(3):
        b = d[0x100 + s * 0xA00: 0x100 + (s + 1) * 0xA00]
        if len(b) < 0xA00 or not any(b[:0x20]):
            continue
        flags = {i for i in range(256) if b[i >> 3] & (0x80 >> (i & 7))}
        towns = {i for i in range(32) if b[0x60 + (i >> 3)] & (0x80 >> (i & 7))}
        yield s, flags, towns


def holds(terms, flags):
    return all(any(f in flags for f in term) for term in terms)


def main(args):
    jobs_too = '--jobs' in args
    paths = [a for a in args if not a.startswith('--')]
    steps, jobs, problems = make_journal.build()
    for p in problems:
        print('journal.txt', p)
    last, back = -1, 0
    for path in paths:
        for s, flags, towns in slots(path):
            reached = [i for i, st in enumerate(steps) if holds(st['terms'], flags)]
            cur = reached[-1] if reached else -1
            gaps = [i for i in range(cur) if i not in reached]
            name = os.path.basename(path) + (f'.{s}' if s else '')
            title = steps[cur].get('title', '?') if cur >= 0 else '-'
            mark = '  <-- back' if cur < last else ''
            back += cur < last
            print(f'{name:22} step {cur:3} {title}{mark}')
            for g in gaps:
                print(f'{"":22}   not reached: {g} {steps[g].get("title")} ({" ".join(steps[g]["when"])})')
            if jobs_too:
                for j in jobs:
                    t, c, fee, fr, un = j['nums']
                    state = ('done' if fee in flags else 'report back' if c in flags else 'taken' if t in flags
                             else 'offered' if fr in flags and un not in flags else '')
                    if state == 'taken':
                        at = [st for st in j['stages'] if holds(st['terms'], flags)]
                        state += ': ' + (at[-1]['next'] if at else '(no stage)')
                    if state:
                        print(f'{"":22}   job {j["name"]}: {state}')
            last = cur
    print(f'{back} saves went back a step')
    return 1 if problems or back else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
