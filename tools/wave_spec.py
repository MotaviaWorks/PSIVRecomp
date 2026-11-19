"""Wave Mode: the runs (runtime/wave_data.h).

A run is five battles in a row on the game's own maps, each one of the game's own
random-battle formations (Battle_FormationData1..4). This script writes down, for each
run: its level and item kit; the characters who can be picked (everyone who could be at
that level: their level tables start at or below it), each with the gear they had in the
collected save nearest that level (as tools/arena_spec.py, only the gear: the stats are
rebuilt from the level tables); and each node: the map, where the party comes into it (a
door's arrival cell, from the disassembly's map data), how far it walks to its dot, the
formation, and the enemies' names for the menus.

    python tools/wave_spec.py ["imported saves/port"]
"""
import collections
import glob
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import arena_spec  # noqa: E402
from arena_spec import NAMES, L, ROM, R16  # noqa: E402

ASM = ROOT / "disasm" / "ps4disasm" / "ps4.asm"
CONSTS = ROOT / "disasm" / "ps4disasm" / "ps4.constants.asm"

ITEM = dict(Monomate=0x7D, Dimate=0x7E, Trimate=0x7F, Antidote=0x80, MoonDew=0x82, StarDew=0x83, SolDew=0x86)

# name, level, item kit, nodes: (map, places's name, steps to its dot, formation)
RUNS = [
    ("Motavian Depths", 15, [("Monomate", 6), ("Dimate", 2), ("Antidote", 2), ("MoonDew", 1)], [
        ("BirthValley_B1", "Birth Valley", 12, 145),
        ("TonoeBasement_B3", "Tonoe's Basement", 12, 166),
        ("BioPlant", "Bio-Plant", 12, 185),
        ("Wreckage_F2", "Wreckage", 12, 206),
        ("BioPlant_B2", "Bio-Plant, Lower Level", 12, 195),
    ]),
    ("Zio's Reach", 26, [("Dimate", 6), ("Monomate", 4), ("Antidote", 2), ("MoonDew", 2), ("StarDew", 1)], [
        ("ZioFort_F3", "Zio's Fort", 12, 226),
        ("PlateSystem_F3", "Plate System", 12, 239),
        ("LadeaTower_F3", "Tower of Ladea", 12, 224),
        ("ClimCenter_F2", "Climate Center", 12, 326),
        ("Nurvus_B4", "Nurvus", 12, 263),
    ]),
    ("The Edge of Algo", 42, [("Trimate", 4), ("Dimate", 6), ("MoonDew", 3), ("StarDew", 2), ("SolDew", 1)], [
        ("VahalFort_F2", "Vahal Fort", 12, 436),
        ("GaruberkTower_Part3", "Garuberk Tower", 12, 402),
        ("StrengthTower_F2", "Tower of Strength", 12, 453),
        ("GaruberkTower_Part5", "Garuberk Tower, Upper Floors", 12, 395),
        ("TheEdge_Part6", "The Edge", 12, 493),
    ]),
]

FORM_TABLES = ["Battle_FormationData1", "Battle_FormationData2", "Battle_FormationData3", "Battle_FormationData4"]


def kos(pos):
    """Kosinski, as KosDecomp unpacks it"""
    out = bytearray()
    st = dict(pos=pos)

    def byte():
        v = ROM[st["pos"]]
        st["pos"] += 1
        return v

    def word():
        lo = byte()
        return lo | byte() << 8
    st["desc"], st["bits"] = word(), 16

    def bit():
        b = st["desc"] & 1
        st["desc"] >>= 1
        st["bits"] -= 1
        if st["bits"] == 0:
            st["desc"], st["bits"] = word(), 16
        return b
    while True:
        if bit():
            out.append(byte())
            continue
        if bit():
            lo, hi = byte(), byte()
            off = (((hi & 0xF8) << 5) | lo) - 0x2000
            cnt = hi & 7
            if cnt:
                cnt += 2
            else:
                c = byte()
                if c == 0:
                    break
                if c == 1:
                    continue
                cnt = c + 1
        else:
            cnt = (bit() << 1 | bit()) + 2
            off = byte() - 0x100
        for _ in range(cnt):
            out.append(out[len(out) + off])
    return bytes(out)


def formation(idx):
    """the enemy IDs of formation idx (7 bytes, then ID and position pairs, then $FF)"""
    raw = kos(L(FORM_TABLES[idx >> 7]))
    entries, cur = [], []
    for b in raw:
        cur.append(b)
        if b == 0xFF:
            entries.append(cur)
            cur = []
    e = entries[idx & 0x7F]
    return [e[i] for i in range(7, len(e) - 1, 2)]


def enemy_name(i):
    p = L("EnemyNames")
    for _ in range(i):
        while ROM[p] < 0xFE:
            p += 1
        p += 1
    s = ""
    while ROM[p] < 0xFE:
        c = ROM[p]
        s += " " if c == 0 else chr(64 + c) if c <= 26 else chr(48 + c - 0x1B) if 0x1B <= c <= 0x24 else \
            "-" if c == 0x31 else "." if c == 0x53 else "?"
        p += 1
    return re.sub(r"[A-Z]+", lambda m: m.group(0)[0] + m.group(0)[1:].lower(), s)


def enemies(idx):
    n = collections.Counter(enemy_name(e) for e in formation(idx))
    return ", ".join(f"{k} x{v}" if v > 1 else k for k, v in n.items())


def maps_and_doors():
    ids = {}
    for line in open(CONSTS, encoding="utf-8", errors="replace"):
        m = re.match(r"MapID_(\w+) = id\(\w+\)\s*;\s*(\$?[0-9A-F]+)", line)
        if m:
            v = m.group(2)
            ids[m.group(1)] = int(v[1:], 16) if v.startswith("$") else int(v)
    num = lambda s: int(s[1:], 16) if s.startswith("$") else int(s)
    lines = open(ASM, encoding="utf-8", errors="replace").read().split("\n")
    doors, cur = [], None
    for i, l in enumerate(lines):
        m = re.match(r"^Map_(\w+):", l)
        if m:
            cur = m.group(1)
        m1 = re.match(r"\s+dc\.b\s+(\$?[0-9A-F]+),\s*(\$?[0-9A-F]+)$", l)
        if not (cur and m1 and i + 3 < len(lines)):
            continue
        m2 = re.match(r"\s+dc\.w\s+(\$?[0-9A-F]+)$", lines[i + 1])
        m3 = re.match(r"\s+dc\.w\s+MapID_(\w+)", lines[i + 2])
        m4 = re.match(r"\s+dc\.b\s+(\$?[0-9A-F]+),\s*(\$?[0-9A-F]+),\s*(\$?[0-9A-F]+),\s*(\$?[0-9A-F]+)", lines[i + 3])
        if m2 and m3 and m4:
            doors.append((cur, m3.group(1), num(m4.group(1)), num(m4.group(2))))
    return ids, doors


def world_of(name, doors):
    adj = collections.defaultdict(set)
    for a, b, _, _ in doors:
        adj[a].add(b)
        adj[b].add(a)
    seen, q = {name}, [name]
    while q:
        x = q.pop(0)
        if x in ("Motavia", "Dezolis", "Rykros"):
            return ("Motavia", "Dezolis", "Rykros").index(x)
        for y in sorted(adj[x]):
            if y not in seen:
                seen.add(y)
                q.append(y)
    return 0


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "imported saves" / "port")
    seen = collections.defaultdict(list)                 # character: (level, save, gear, extra skills)
    for p in sorted(glob.glob(os.path.join(folder, "save*.srm"))):
        n = int(os.path.basename(p)[4:6])
        for cid, level, equip, extra in arena_spec.read_save(p)["party"]:
            seen[cid].append((level, n, equip, extra))
    if not seen:
        raise SystemExit(f"no saves in {folder}")
    ids, doors = maps_and_doors()
    starts = {c: R16(arena_spec.LEVELS + c * 6) for c in range(10)}
    out = ["/* generated by tools/wave_spec.py from the ROM's tables, the disassembly's map data and a",
           " * folder of collected saves (only gear): do not edit. Wave Mode's runs (wave.h). */",
           "static const WaveRun WAVE_RUNS[] = {"]
    for name, level, kit, nodes in RUNS:
        items = []
        for it, k in kit:
            items += [ITEM[it]] * k
        items = (items + [0] * 40)[:40]
        roster = []
        for c in range(10):
            if starts[c] > level or not seen[c]:
                continue
            lv, src, equip, extra = min(seen[c], key=lambda s: (abs(s[0] - level), -s[1]))
            eq = "{" + ", ".join(f"0x{e:02X}" for e in equip) + "}"
            ex = "{" + ", ".join(f"0x{e:02X}" for e in (extra + [0] * 4)[:4]) + "}"
            roster.append(f"{{{c}, {max(level, starts[c])}, {eq}, {ex}}} /* {NAMES[c]}: gear of save {src} (level {lv}) */")
        out.append(f"    {{\"{name}\", {level},")
        out.append("     {" + ", ".join(f"0x{i:02X}" for i in items) + "},")
        out.append(f"     {len(roster)}, {{")
        out += [f"        {r}," for r in roster]
        out.append("     }, {")
        for mp, place, steps, form in nodes:
            into = [(a, x, y) for a, b, x, y in doors if b == mp]
            if not into:
                raise SystemExit(f"{mp}: no door into it")
            src, cx, cy = into[0]
            out.append(f"        {{\"{place}\", {world_of(mp, doors)}, 0x{ids[mp]:X}, 0x{ids.get(src, 0):X}, {cx * 16}, {cy * 16}, "
                       f"{steps}, {form}, \"{enemies(form)}\"}},   /* {mp}, from {src} */")
        out.append("     }},")
    out.append("};")
    dest = ROOT / "runtime" / "wave_data.h"
    dest.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"{dest}: {len(RUNS)} runs")


if __name__ == "__main__":
    main()
