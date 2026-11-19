"""The modern sounds' library from PsiLok's picks: res/sounds/XX_N.wav, sounds.ini and CREDITS.txt.

    python tools/sfxlib/build.py PICKS.txt [OUT]

PICKS.txt is what the audition page's "Copy my picks" gives ("F5 Sword: s1, m1 | a note"): each ticked candidate
(build/sfxlib-cand/XX/s1.wav ...) becomes a variant of that effect, in the order ticked, up to four. An effect
marked NONE, or not in the file, gets nothing: it stays the game's own. OUT (default res/sounds) is rewritten
whole. sounds.ini carries, for the effects that had one, the first modern set's rules for sounds asked for in
quick succession (how many may overlap; how soon one may start again).
"""
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CAND = os.path.join(ROOT, 'build', 'sfxlib-cand')
# sound ID: (how many may sound at once, seconds before it may start again) -- from the synthesized set's recipes
RULES = {'F5': (3, 0), 'B7': (1, 0), 'F2': (1, 0), 'F3': (2, 0), 'E8': (3, 0.045), 'B9': (3, 0.1), 'DD': (1, 0.3), 'C6': (2, 0.4),
         'BA': (2, 0.05), 'D5': (1, 0.2), 'B5': (2, 0.05), 'BE': (2, 0.1), 'CC': (1, 0.3), 'BB': (1, 0.2), 'B6': (2, 0.05),
         'C4': (1, 0.25), 'D7': (2, 0.05), 'BD': (1, 0.2), 'B8': (1, 0.05), 'C7': (3, 0), 'CD': (1, 0.5), 'C2': (1, 0.25),
         'BC': (1, 0.25), 'CB': (1, 0.4), 'C5': (2, 0.15), 'D0': (2, 0.1), 'C0': (2, 0.15), 'D3': (3, 0.03), 'BF': (1, 0.2),
         'CE': (1, 0.35), 'C3': (1, 0.3), 'C1': (1, 0.25)}
DEFAULT = (2, 0.05)


def main():
    picks_path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'res', 'sounds')
    man = json.load(open(os.path.join(CAND, 'candidates.json'), encoding='utf-8'))
    picks = {}
    for line in open(picks_path, encoding='utf-8'):
        m = re.match(r'\s*\$?([0-9A-Fa-f]{2})\b[^:]*:\s*([^|]*)', line)
        if not m:
            continue
        keys = [k.strip().lower() for k in m.group(2).split(',') if re.fullmatch(r'\s*[sm]\d+\s*', k, re.I)]
        if keys:
            picks[m.group(1).upper()] = keys[:4]
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    ini = ["; The modern sounds' library (runtime/sfx.h): XX_1.wav .. XX_4.wav for the game's sound ID XX.",
           '; XX = gain dB, how many may sound at once, seconds before it may start again',
           '; Made by tools/sfxlib/build.py from the picks; what each is made from: CREDITS.txt', '']
    credits = ["The modern sounds: what they are made from", '=' * 42, '',
               'Every sound here is made from recordings their authors gave to the public domain (CC0 1.0,',
               'https://creativecommons.org/publicdomain/zero/1.0/), found on OpenGameArt.org: cut, layered and',
               'levelled for this game (tools/sfxlib). No credit is owed under CC0; it is given gladly.', '']
    authors = {}
    for sid in sorted(picks):
        e = man.get(sid)
        if not e:
            print(sid, 'is not an effect with candidates: skipped')
            continue
        by = {c['file'][:-4]: c for c in e['candidates']}
        n = 0
        credits.append(f"${sid} {e['name']}")
        for key in picks[sid]:
            c = by.get(key)
            if not c:
                print(f'{sid}: no candidate {key}: skipped')
                continue
            n += 1
            shutil.copy2(os.path.join(CAND, sid, c['file']), os.path.join(out, f'{sid}_{n}.wav'))
            for s in c['sources']:
                credits.append(f"  {sid}_{n}.wav: {os.path.basename(s['file'])} by {s['author']} (https://opengameart.org/content/{s['pack']})")
                authors.setdefault(s['author'], set()).add(s['pack'])
        if n:
            ov, gap = RULES.get(sid, DEFAULT)
            ini.append(f'{sid} = 0, {ov}, {gap:g}')
        else:
            credits.pop()
    credits += ['', 'Thank you: ' + ', '.join(sorted(authors, key=str.lower)) + '.', '']
    open(os.path.join(out, 'sounds.ini'), 'w', encoding='utf-8', newline='\n').write('\n'.join(ini) + '\n')
    open(os.path.join(out, 'CREDITS.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(credits))
    files = [f for f in os.listdir(out) if f.endswith('.wav')]
    print(f"{out}: {len({f[:2] for f in files})} effects, {len(files)} files, {sum(os.path.getsize(os.path.join(out, f)) for f in files) / 1e6:.1f} MB")


if __name__ == '__main__':
    main()
