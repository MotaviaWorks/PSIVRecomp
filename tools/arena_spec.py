"""Boss Arena: each boss fight's story party, as levels and gear (runtime/arena_data.h).

The party for each of the game's 27 boss fights (Event_Battle_Index 0..$1A) is the one a
player has just before it. This script reads a folder of collected saves (keep them out of
git: `imported saves/` is ignored), finds for each boss the last save before its fight (by
the event flag the boss's script sets just before it), and writes down only what a strategy
guide would: who is in the party, each one's level and gear, extra skills the androids were
given in the story, the items carried, and where the party stood. The stats themselves are
not copied: the arena rebuilds every character from the ROM's own level tables (arena.c),
and this script checks that the rebuild gives each saved character's stats exactly.

    python tools/arena_spec.py ["imported saves/port"]
"""
import glob
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "dist" / "sprites" / "_src"))
from psivart import L, ROM  # noqa: E402

NAMES = "Chaz Alys Hahn Rune Gryz Rika Demi Wren Raja Kyra Seth".split()

# story order: (Event_Battle_Index, the arena's name for it, the event flag its script sets just before the fight)
BOSSES = [
    (0x00, "Igglanova", 11), (0x01, "Igglanova at Zema", 51), (0x02, "Sand Worm", 27), (0x03, "Juza", 65),
    (0x04, "Zio", 66), (0x05, "Gy-Laguiah", 105), (0x06, "Zio at Nurvus", 101), (0x07, "Fract Ooze", 47),
    (0x08, "Chaos Sorcerer", 113), (0x09, "Dark Force", 131), (0x0D, "Zombies of Reshel", 139),
    (0x0A, "Carnivorous Trees", 149), (0x0B, "Gy-Laguiah on Dezolis", 146), (0x0C, "D-Elm-Lars", 147),
    (0x0E, "Xe-A-Thoul", 154), (0x0F, "Spector", 166), (0x10, "Lashiec", 155), (0x11, "Dark Force 2", 158),
    (0x12, "Dark Force 3", 197), (0x13, "King Rappy", 175), (0x14, "Servants", 178), (0x15, "Dominators", 182),
    (0x16, "De-Vars", 212), (0x17, "Sa-Lews", 210), (0x18, "Alys in the Tower of Anger", 228),
    (0x19, "Re-Faze", 225), (0x1A, "Profound Darkness", 232),
]
# not in the arena: the first Zio (Event_Battle_Index 4) is scripted to lose, no real boss fight. Each
# boss keeps its place in BOSSES as its key (ArenaBoss.key): what the player keeps per boss goes by it
OMIT = {0x04}
# Igglanova in the Academy's basement comes before any save: Chaz and Alys as the game starts them,
# Chaz a few fights on
OPENING = dict(meseta=0, items=[0] * 40, party=[(0, 3, None, []), (1, 7, None, [])])
# Where every arena save loads: Motavia's world map outside Piata, where nothing happens on arrival
# (a save's own place may be in a town, or a world whose arrival plays a scene first)
SPOT = dict(world=0, map2=0, map=0, x=1584, y=1312)

R16 = lambda a: struct.unpack(">H", ROM[a:a + 2])[0]
R32 = lambda a: struct.unpack(">I", ROM[a:a + 4])[0]
INITIAL = L("InitialCharStats")        # 66 bytes a character
LEVELS = L("CharLevelTablePtrs")       # start level (word), table (long) a character; 22 bytes a level


def rebuild(cid, level):
    """what the level tables give: (max hp, max tp, 4 stats, techniques, skills, skill uses)"""
    a = INITIAL + cid * 66
    r = ROM[a:a + 66]
    hp, tp, stats = R16(a + 8), R16(a + 10), list(r[12:16])
    techs = [t for t in r[34:50] if t]
    skills = [s for s in r[50:58] if s]
    uses = list(r[58:66])
    start, tab = R16(LEVELS + cid * 6), R32(LEVELS + cid * 6 + 2)
    for lv in range(start, level):
        e = tab + (lv - start) * 22
        hp, tp, stats = R16(e + 4), R16(e + 6), list(ROM[e + 8:e + 12])
        if ROM[e + 12]:
            techs.append(ROM[e + 12])
        if ROM[e + 13]:
            skills.append(ROM[e + 13])
        uses = list(ROM[e + 14:e + 22])
    return hp, tp, stats, techs, skills, uses


def read_save(path):
    blk = open(path, "rb").read()[0x100:0xB00]
    g16 = lambda b, k: struct.unpack(">H", b[k:k + 2])[0]
    flags = {f for f in range(256) if blk[f >> 3] >> (7 - (f & 7)) & 1}
    party = []
    for cid in [x for x in blk[0x30A:0x30F] if x != 0xFF]:
        b = blk[0x400 + cid * 0x80:0x480 + cid * 0x80]
        level = g16(b, 8)
        hp, tp, stats, techs, skills, uses = rebuild(cid, level)
        s_skills = [s for s in b[0x62:0x6A] if s]
        got = (g16(b, 0x10), g16(b, 0x14), [b[0x18], b[0x1B], b[0x1E], b[0x21]], [t for t in b[0x52:0x62] if t])
        if got != (hp, tp, stats, techs) or s_skills[:len(skills)] != skills:
            raise SystemExit(f"{path}: {NAMES[cid]} level {level} does not rebuild from the level tables")
        party.append((cid, level, list(b[0x4C:0x50]), s_skills[len(skills):]))
    return dict(flags=flags, world=blk[0x300], map2=g16(blk, 0x302), map=g16(blk, 0x304), x=g16(blk, 0x306),
                y=g16(blk, 0x308), meseta=struct.unpack(">I", blk[0x338:0x33C])[0], items=list(blk[0x310:0x338]),
                party=party)


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "imported saves" / "port")
    saves = {}
    for p in sorted(glob.glob(os.path.join(folder, "save*.srm"))):
        saves[int(os.path.basename(p)[4:6])] = read_save(p)
    if not saves:
        raise SystemExit(f"no saves in {folder}")
    out = ["/* generated by tools/arena_spec.py from a folder of collected saves: do not edit.",
           " * Each boss fight's story party: levels, gear and items only (arena.c rebuilds the",
           " * stats from the ROM's level tables). Story order. */",
           "static const ArenaBoss ARENA_BOSSES[] = {"]
    for key, (event, name, flag) in enumerate(BOSSES):
        if event in OMIT:
            continue
        before = [n for n, s in saves.items() if flag not in s["flags"]]
        if before:
            n = max(before)
            s, src = saves[n], f"save {n}"
        else:
            s, src = OPENING, "the game's start"
        members = []
        for cid, level, equip, extra in s["party"]:
            eq = "{0xFF, 0xFF, 0xFF, 0xFF}" if equip is None else "{" + ", ".join(f"0x{e:02X}" for e in equip) + "}"
            ex = "{" + ", ".join(f"0x{e:02X}" for e in (extra + [0] * 4)[:4]) + "}"
            members.append(f"{{{cid}, {level}, {eq}, {ex}}}")
        party = " ".join(f"{NAMES[c]} {lv}" for c, lv, _, _ in s["party"])
        items = ", ".join(f"0x{i:02X}" for i in s["items"])
        out.append(f"    /* {name}: {party} ({src}) */")
        p = SPOT
        out.append(f"    {{\"{name}\", 0x{event:02X}, {key}, {p['world']}, 0x{p['map2']:X}, 0x{p['map']:X}, {p['x']}, {p['y']}, "
                   f"{s['meseta']}u, {len(members)},")
        out.append(f"     {{{', '.join(members)}}},")
        out.append(f"     {{{items}}}}},")
    out.append("};")
    dest = ROOT / "runtime" / "arena_data.h"
    dest.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"{dest}: {len(BOSSES) - len(OMIT)} bosses from {len(saves)} saves")


if __name__ == "__main__":
    main()
