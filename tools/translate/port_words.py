"""The port's own words (its menus' labels, hints and descriptions) for the translation tools: read out of the sources,
so a new one is in the list the next time this is run.

    python port_words.py            writes them into index.html and psiv_translate.py (both hold them, between marks)
    python port_words.py --list     only prints them

An entry's id is P and the FNV-1a hash of the English words, so it stays the same wherever the words move to. A
translation of one with %d, %s ... must have the same ones in the same order (runtime/translate.c takes no other).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SOURCES = [('Field menu and shops', 'fieldmenu.c'), ('Battle menus', 'battleui.c'), ('What techniques, skills and items do', 'battledesc.c'),
           ('Side panels', 'panels.c')]
# strings in those files that are no words on the screen
NOT = set("""text snprintf MACRO HUMAN PSIV_FIELDMENU_LOG PSIV_BUI_LOG party enemy full read size record display plain
record. in into""".split()) | {' (heal)'}
SKIP = re.compile(r'#include|getenv|fprintf\(|(?<!sn)printf\(|aside\(|fopen|strn?cmp\(|strstr|SDL_Log|MSG_LOG')


def without_comments(t):
    out, i = [], 0
    while i < len(t):
        if t.startswith('/*', i):
            j = t.index('*/', i) + 2
            out.append('\n' * t.count('\n', i, j))
            i = j
        elif t[i] == "'" and i + 2 < len(t) and (t[i + 2] == "'" or t[i + 1] == '\\'):
            j = t.index("'", i + (3 if t[i + 1] == '\\' else 1)) + 1
            out.append(t[i:j])
            i = j
        elif t[i] == '"':
            j = i + 1
            while t[j] != '"':
                j += 2 if t[j] == '\\' else 1
            out.append(t[i:j + 1])
            i = j + 1
        else:
            out.append(t[i])
            i += 1
    return ''.join(out)


def unescape(s):
    raw = re.sub(r'\\x([0-9A-Fa-f]{2})', lambda m: chr(int(m[1], 16)), s)
    raw = raw.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')
    try:
        return raw.encode('latin-1').decode('utf-8')
    except UnicodeError:
        return raw


def fnv(s):
    h = 0x811C9DC5
    for b in s.encode('utf-8'):
        h = (h ^ b) * 0x01000193 & 0xFFFFFFFF
    return h


def words():
    """[(heading, id, text)]"""
    out, seen = [], set()
    for heading, name in SOURCES:
        t = without_comments(open(os.path.join(ROOT, 'runtime', name), encoding='utf-8').read())
        t = re.sub(r'"\s*\n\s*"', '', t)                       # (a string over two lines is one)
        for line in t.split('\n'):
            if SKIP.search(line):
                if 'snprintf(' not in line or re.search('getenv|MSG_LOG', line):
                    continue
                line = line[line.rindex('snprintf('):]
            for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', line):
                s = unescape(m[1])
                bare = re.sub(r'%[-+ 0#]*\d*(?:\.\d+)?l*[a-zA-Z%]', '', s)
                if not re.search(r'[A-Za-z]{2}', bare) or s in NOT or s in seen:
                    continue
                seen.add(s)
                out.append((heading, 'P%08X' % fnv(s), s))
    return out


def main():
    w = words()
    if len({i for _, i, _ in w}) != len(w):
        sys.exit('two of them have the same hash')
    if '--list' in sys.argv:
        sys.stdout.reconfigure(encoding='utf-8')
        for h, i, s in w:
            print(h, i, repr(s))
        print(len(w))
        return
    body = '\n'.join('%s|%s|%s' % (h, i, s.replace('\n', '\\n')) for h, i, s in w)
    for name, a, b in (('index.html', '<script id="portwords" type="text/plain">\n', '</script>'),
                       ('psiv_translate.py', 'PORT_WORDS = """\\\n', '"""')):
        p = os.path.join(HERE, name)
        t = open(p, encoding='utf-8', newline='').read()
        crlf = '\r\n' in t
        t = t.replace('\r\n', '\n')
        i = t.index(a) + len(a)
        j = t.index(b, i)
        t = t[:i] + body + '\n' + t[j:]
        open(p, 'w', encoding='utf-8', newline='').write(t.replace('\n', '\r\n') if crlf else t)
    print(len(w), 'of the port\'s words')


if __name__ == '__main__':
    main()
