#!/usr/bin/env python3
"""Find and resolve every indirect jmp/jsr in Phantasy Star IV (ps4disasm).

Input : build/disasm/ps4.lst (from tools/build_disasm.py) + rom/psiv.bin.
Output: analysis/indirect_sites.json and analysis/indirect_sites.md

Each site gets a kind, its candidate targets (label + ROM address) and a
confidence. Self-check: every resolved target must be the start address of an
instruction in the listing, otherwise the site is flagged BAD.
"""
import json, re, struct, sys
from collections import defaultdict, Counter
from pathlib import Path

import listing

ROOT = Path(__file__).resolve().parent.parent
LST = ROOT / "build" / "disasm" / "ps4.lst"
ROM = ROOT / "rom" / "psiv.bin"
OUT = ROOT / "analysis"

rom = ROM.read_bytes()
def w(a): return struct.unpack(">H", rom[a:a+2])[0]
def sw(a): return struct.unpack(">h", rom[a:a+2])[0]
def l(a): return struct.unpack(">I", rom[a:a+4])[0]

# ---------------------------------------------------------------- parse listing
_insns, labels, label_name = listing.parse(LST)
insns = [vars(i) for i in _insns]   # this script indexes fields by name

# instruction lengths / code-start set
code_starts = {i["addr"] for i in insns if not i["mnem"].startswith("dc")}
by_addr = {i["addr"]: k for k, i in enumerate(insns)}

def lbl(a):
    return label_name.get(a, f"${a:X}")

def sym(name):
    name = name.strip().strip("()")
    name = re.sub(r"\)?\.[wl]$", "", name).strip("()")
    return labels.get(name.lower())

# ---------------------------------------------------------------- table readers
UNCOND = {"bra", "bra.w", "bra.s", "jmp", "rts", "rte", "rtr"}

RTS = -1                               # table entry that is just "rts"

def read_branch_table(t, stop_at=None):
    """bra.w / bra.s / jmp abs / jmp pc-rel entries starting at t."""
    k = by_addr.get(t)
    if k is None:
        return None, "table address not an instruction"
    first = insns[k]["mnem"]
    if first not in ("bra.w", "bra.s", "bra", "jmp", "rts"):
        return None, f"table starts with {first}"
    short = first in ("bra.s", "rts")   # 2-byte entries: bra.s and rts may mix
    LONG = ("bra.w", "bra", "jmp", "rts") # 4/6-byte entries mix; an rts entry can end a table
    targets, a, seen = [], t, set()
    while k < len(insns):
        ins = insns[k]
        if ins["addr"] != a or (ins["mnem"] != first and not (
                (short and ins["mnem"] in ("bra.s", "rts")) or
                (not short and ins["mnem"] in LONG))):
            break
        if a != t and ins["labels"]:
            break                     # tables sit back to back: a label starts the next one
        if a in seen or (stop_at and a >= stop_at):
            break
        if targets and a in {x for x, _ in targets}:
            break                     # walked into one of our own targets
        op = w(a)
        if op == 0x4E75:
            tgt = RTS
        elif op == 0x6000:
            tgt = a + 2 + sw(a + 2)
        elif op >> 8 == 0x60:
            d = op & 0xFF
            tgt = a + 2 + (d - 256 if d > 127 else d)
        elif op == 0x4EF9:
            tgt = l(a + 2)
        elif op == 0x4EFA:
            tgt = a + 2 + sw(a + 2)
        else:
            break
        targets.append((tgt, a))
        seen.add(a)
        a = ins["next"]; k += 1
    return [t_ for t_, _ in targets], None

def read_data_table(t, kind, base=None):
    """dc.w offsets (relative to base) or dc.l absolute pointers."""
    k = by_addr.get(t)
    if k is None:
        return None, "table address not listed"
    want = "dc.w" if kind == "w" else "dc.l"
    targets, a = [], t
    while k < len(insns):
        ins = insns[k]
        if ins["mnem"] != want or (a != t and ins["labels"]):
            break                     # a label starts the next table
        if a != t and targets and a >= min(targets) and min(targets) > t:
            break                     # ran into the code the table points at
        # a dc line may hold several values
        end = ins["next"]
        while a < end:
            if kind == "w":
                tgt = (base if base is not None else t) + sw(a); a += 2
            else:
                tgt = l(a) & 0xFFFFFF; a += 4
            targets.append(tgt)
        k += 1
        if k < len(insns) and insns[k]["labels"] and any(
                lab.lower() in refd for lab in insns[k]["labels"]):
            break                     # next label is referenced on its own
    return targets, None

# labels referenced anywhere as operands (to know where a table legitimately ends)
refd = set()
for ins in insns:
    for tok in re.findall(r"[A-Za-z_][\w.]*", ins["ops"]):
        refd.add(tok.lower())

# ---------------------------------------------------------------- field stores
# move.l #Label, disp(aN)   /  move.l #Label, (RAM).w  -> candidate targets for
# pointers later loaded from that field / variable.
field_stores = defaultdict(set)
ram_stores = defaultdict(set)
for ins in insns:
    if ins["mnem"] not in ("move.l", "movea.l"):
        continue
    m = re.match(r"#([A-Za-z_][\w.]*)\s*,\s*(.+)$", ins["ops"])
    if not m:
        continue
    tgt = sym(m.group(1))
    if tgt is None or tgt not in code_starts:
        continue
    dst = m.group(2).replace(" ", "")
    fm = re.match(r"(\$?[\w]+)?\(a[0-7]\)$", dst)
    if fm:
        field_stores[(fm.group(1) or "0").lower()].add(tgt)
        continue
    rm = re.match(r"\(([^)]+)\)\.[wl]$", dst)
    if rm:
        ram_stores[rm.group(1).lower()].add(tgt)

# ---------------------------------------------------------------- resolve sites
IND_RE = re.compile(r"^(?:(?P<tbl>[\w.$-]+)\(pc,(?P<pidx>[ad][0-7])(?:\.[wl])?\)"
                    r"|(?P<disp>-?\$?[\w]+)?\((?P<areg>a[0-7])(?:,(?P<aidx>[ad][0-7])(?:\.[wl])?)?\))$")

def back(k, limit=14):
    """Walk backwards within straight-line code; yields (ins, crossed_label)."""
    crossed = False
    for j in range(k - 1, max(-1, k - 1 - limit), -1):
        ins = insns[j]
        if ins["mnem"] in UNCOND:
            return
        yield ins, crossed
        if ins["labels"]:
            crossed = True

def imm_for(k, reg):
    for ins, _ in back(k):
        m = re.match(r"#(\$?[0-9A-Fa-f]+)\s*,\s*" + reg + r"$", ins["ops"].replace(" ", ""))
        if m and ins["mnem"] in ("move.w", "moveq", "move.l", "move.b"):
            v = m.group(1)
            return int(v[1:], 16) if v.startswith("$") else int(v)
        if re.search(r",\s*" + reg + r"$", ins["ops"]):
            return None
    return None

sites = []
for k, ins in enumerate(insns):
    if ins["mnem"] not in ("jmp", "jsr"):
        continue
    m = IND_RE.match(ins["ops"].replace(" ", ""))
    if not m:
        continue
    s = dict(addr=f"${ins['addr']:X}", insn=f"{ins['mnem']} {ins['ops']}",
             lst_line=ins["lst"], func=None, kind=None, table=None,
             targets=[], confidence="none", notes=[])
    for j in range(k, -1, -1):          # nearest preceding label = enclosing code
        if insns[j]["labels"]:
            s["func"] = insns[j]["labels"][-1]; break

    if m.group("tbl"):                   # ---- jmp T(pc,dN)
        t = sym(m.group("tbl"))
        s["table"] = m.group("tbl")
        # offset table: move.w T(pc,dN),dN ; jmp T(pc,dN)
        off = None
        for p, _ in back(k, 4):
            if p["mnem"] == "move.w" and re.match(re.escape(m.group("tbl")) + r"\(pc,", p["ops"].replace(" ", "")):
                off = p; break
        if off:
            tg, err = read_data_table(t, "w", base=t)
            s["kind"] = "pc_offset_table"
        else:
            tg, err = read_branch_table(t)
            s["kind"] = "pc_branch_table"
        if err: s["notes"].append(err)
        s["targets"] = tg or []
        s["confidence"] = "high" if tg else "none"
    else:                                # ---- register indirect
        areg, aidx, disp = m.group("areg"), m.group("aidx"), m.group("disp")
        src, crossed = None, False
        for p, c in back(k):
            if re.search(r",\s*" + areg + r"$", p["ops"].replace(" ", "")) or \
               p["mnem"].startswith(("adda", "addq")) and p["ops"].replace(" ", "").endswith(areg):
                src, crossed = p, c; break
        if src is None:
            s["kind"] = "register_param"
            s["notes"].append(f"{areg} not set in this block: passed in by caller")
            if s["func"] == "Trap02Exception":
                s["kind"] = "trap2_dispatcher"
                s["notes"] = ["resolved per 'trap #2' call site (see trap2_sites)"]
                s["confidence"] = "n/a"
        else:
            ops = src["ops"].replace(" ", "")
            s["notes"].append(f"{areg} from: {src['mnem']} {src['ops']}")
            if crossed:
                s["notes"].append("crossed a label while tracing: another path may set it")
            lm_ = re.match(r"(?:\()?([A-Za-z_][\w.]*)\)?(?:\.[wl])?(?:\(pc\))?," + areg + "$", ops)
            pm_ = re.match(r"([A-Za-z_][\w.]*)\(pc,([ad][0-7])(?:\.[wl])?\)," + areg + "$", ops)
            fm_ = re.match(r"(-?\$?[\w]+)?\((a[0-7])\)," + areg + "$", ops)
            rm_ = re.match(r"\(([^)]+)\)\.[wl]," + areg + "$", ops)
            if src["mnem"] == "lea" and lm_ and sym(lm_.group(1)) is not None:
                t = sym(lm_.group(1)); s["table"] = lm_.group(1)
                if aidx:
                    v = imm_for(k, aidx)
                    tg, err = read_branch_table(t)
                    if tg is None:
                        tg, err = read_data_table(t, "w", base=t)
                    if v is not None and tg:
                        ent = t + v
                        one, _ = read_branch_table(ent)
                        s["kind"] = "lea_table_const_index"
                        s["targets"] = one[:1] if one else []
                        s["notes"].append(f"index {aidx} = {v:#x} (constant)")
                    else:
                        s["kind"] = "lea_table"
                        s["targets"] = tg or []
                    if err: s["notes"].append(err)
                    s["confidence"] = "high" if s["targets"] and not crossed else "medium"
                else:
                    s["kind"] = "lea_direct"
                    s["targets"] = [t]
                    s["confidence"] = "high" if not crossed else "medium"
            elif src["mnem"] in ("movea.l", "move.l") and pm_ and sym(pm_.group(1)) is not None:
                t = sym(pm_.group(1)); s["table"] = pm_.group(1)
                tg, err = read_data_table(t, "l")
                s["kind"] = "pc_pointer_table"
                s["targets"] = tg or []
                if err: s["notes"].append(err)
                s["confidence"] = "high" if tg else "none"
            elif src["mnem"] in ("movea.l", "move.l") and re.match(
                    r"\(" + areg + r",[ad][0-7](?:\.[wl])?\)," + areg + "$", ops):
                base = None
                for p, c in back(by_addr[src["addr"]]):
                    mm = re.match(r"(?:\()?([A-Za-z_][\w.]*)\)?(?:\.[wl])?(?:\(pc\))?," + areg + "$",
                                  p["ops"].replace(" ", ""))
                    if p["mnem"] == "lea" and mm:
                        base = mm.group(1); crossed = crossed or c; break
                    if p["ops"].replace(" ", "").endswith("," + areg):
                        break
                if base and sym(base) is not None:
                    t = sym(base); s["table"] = base
                    tg, err = read_data_table(t, "l")
                    s["kind"] = "lea_pointer_table"
                    s["targets"] = tg or []
                    if err: s["notes"].append(err)
                    s["confidence"] = ("high" if not crossed else "medium") if tg else "none"
                else:
                    s["kind"] = "pointer_table_param"
                    s["notes"].append("pointer-table base passed in by caller")
            elif src["mnem"].startswith("adda") and ops.startswith("("):
                # adda.w (aN,dK),aN after lea T,aN -> offset table relative to T
                base = None
                for p, _ in back(by_addr[src["addr"]] if src["addr"] in by_addr else k):
                    mm = re.match(r"(?:\()?([A-Za-z_][\w.]*)\)?(?:\.[wl])?(?:\(pc\))?," + areg + "$",
                                  p["ops"].replace(" ", ""))
                    if p["mnem"] == "lea" and mm:
                        base = mm.group(1); break
                if base and sym(base) is not None:
                    t = sym(base); s["table"] = base
                    s["targets"], _ = read_data_table(t, "w", base=t)
                    s["kind"] = "offset_table"
                    s["confidence"] = "high"
                else:
                    s["kind"] = "offset_table_param"
                    s["notes"].append("table base passed in by caller")
            elif fm_ and src["mnem"] in ("movea.l", "move.l"):
                key = (fm_.group(1) or "0").lower()
                s["kind"] = "object_field_pointer"
                s["table"] = f"{fm_.group(1) or '0'}({fm_.group(2)})"
                s["targets"] = sorted(field_stores.get(key, ()))
                s["confidence"] = "low"
                s["notes"].append(f"candidates = every 'move.l #code,{key}(aN)' in the game "
                                  "(over-approximation; needs runtime confirmation)")
            elif rm_ and src["mnem"] in ("movea.l", "move.l"):
                key = rm_.group(1).lower()
                s["kind"] = "ram_pointer"
                s["table"] = rm_.group(1)
                s["targets"] = sorted(ram_stores.get(key, ()))
                s["confidence"] = "medium" if s["targets"] else "none"
            else:
                s["kind"] = "unresolved"
    # self-check
    bad = [t for t in s["targets"] if t != RTS and t not in code_starts]
    if bad:
        s["notes"].append("BAD targets (not an instruction start): " +
                          ", ".join(f"${b:X}" for b in bad))
        s["confidence"] = "BAD"
    s["targets"] = [dict(addr="rts", label="(return)") if t == RTS else
                    dict(addr=f"${t:X}", label=lbl(t)) for t in s["targets"]]
    sites.append(s)

# ---------------------------------------------------------------- trap #2 callers
trap2 = []
for k, ins in enumerate(insns):
    if ins["mnem"] == "trap" and ins["ops"].replace(" ", "") == "#2":
        base = None
        for p, _ in back(k):
            mm = re.match(r"(?:\()?([A-Za-z_][\w.]*)\)?(?:\.[wl])?(?:\(pc\))?,a0$", p["ops"].replace(" ", ""))
            if p["mnem"] == "lea" and mm:
                base = mm.group(1); break
            if p["ops"].replace(" ", "").endswith(",a0"):
                break
        e = dict(addr=f"${ins['addr']:X}", lst_line=ins["lst"], table=base, targets=[],
                 confidence="none")
        if base and sym(base) is not None:
            t = sym(base)
            tg, _ = read_data_table(t, "w", base=t)
            bad = [x for x in tg if x not in code_starts]
            e["targets"] = [dict(addr=f"${x:X}", label=lbl(x)) for x in tg]
            e["confidence"] = "BAD" if bad else "high"
        trap2.append(e)

# ---------------------------------------------------------------- report
OUT.mkdir(exist_ok=True)
(OUT / "indirect_sites.json").write_text(json.dumps(dict(sites=sites, trap2_sites=trap2), indent=1))

kinds = Counter(s["kind"] for s in sites)
conf = Counter(s["confidence"] for s in sites)
ntg = sum(len(s["targets"]) for s in sites)
md = [f"# PSIV indirect jumps\n",
      f"{len(sites)} indirect jmp/jsr sites, {ntg} resolved target edges; "
      f"{len(trap2)} `trap #2` call sites.\n",
      "## By kind\n", "| kind | sites |", "|---|---|"]
md += [f"| {k} | {v} |" for k, v in kinds.most_common()]
md += ["\n## By confidence\n", "| confidence | sites |", "|---|---|"]
md += [f"| {k} | {v} |" for k, v in conf.most_common()]
t2c = Counter(e["confidence"] for e in trap2)
md += [f"\n`trap #2` callers: " + ", ".join(f"{k} {v}" for k, v in t2c.most_common())]
md += ["\n## Needs attention (none / low / BAD / unresolved / param)\n",
       "| addr | func | insn | kind | notes |", "|---|---|---|---|---|"]
for s in sites:
    if s["confidence"] in ("none", "low", "BAD") or s["kind"] in ("unresolved", "register_param", "offset_table_param"):
        md.append(f"| {s['addr']} | {s['func']} | `{s['insn']}` | {s['kind']} | {'; '.join(s['notes'])} |")
(OUT / "indirect_sites.md").write_text("\n".join(md) + "\n")
print("\n".join(md[:30]))
