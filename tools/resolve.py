#!/usr/bin/env python3
"""Resolve every indirect jump/call in Phantasy Star IV by tracing register values.

Works on decoded instructions (tools/m68k.py) and the control-flow model
(tools/program.py). For each `jmp`/`jsr` through a register or a table it
finds every definition of the base/index registers that can reach the site:
backwards across branches, into called routines that may have changed the
register, and out to callers when the value is passed in. Each definition is
evaluated to an abstract value (constant, pointer table, offset table, value
loaded from an object field or RAM variable) and turned into targets.

Targets are the addresses actually jumped to: for `jmp T(pc,d0.w)` that is the
table entry (e.g. a `bra.w`), not where that entry branches. Discovery and
resolution alternate until nothing changes, since newly reached code can hold
more indirect jumps.

Outputs analysis/indirect_targets.json and analysis/indirect_targets.md.
"""
import json
import struct
import sys
from bisect import bisect_right
from collections import Counter, defaultdict, deque
from pathlib import Path

from program import Program, DIRECT_EA

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "analysis"

REGS = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]
DEST_WRITERS = {
    "move", "movea", "moveq", "lea", "add", "adda", "addi", "addq", "addx",
    "sub", "suba", "subi", "subq", "subx", "and", "andi", "or", "ori", "eor", "eori",
    "not", "neg", "negx", "clr", "ext", "swap", "mulu", "muls", "divu", "divs",
    "scc", "abcd", "sbcd", "asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr",
    "nbcd", "bchg", "bclr", "bset", "tas",
}
MAX_STEPS = 4000
MAX_DEPTH = 4


def reg_of(o):
    if o.kind == "dn":
        return f"d{o.reg}"
    if o.kind == "an":
        return f"a{o.reg}"
    return None


def writes(i):
    """Registers instruction i writes (a7 ignored: stack depth is not tracked)."""
    w = set()
    m = i.mnem
    if m in DEST_WRITERS and i.ops:
        r = reg_of(i.ops[-1])
        if r:
            w.add(r)
    elif m == "exg":
        w.update(filter(None, (reg_of(o) for o in i.ops)))
    elif m == "movem" and i.ops[-1].kind == "reglist":
        w.update(REGS[b] for b in range(16) if i.ops[-1].val >> b & 1)
    elif m == "dbcc":
        w.add(f"d{i.ops[0].reg}")
    elif m in ("link", "unlk"):
        w.add(f"a{i.ops[0].reg}")
    elif m == "move" and i.ops and i.ops[0].kind == "usp":
        w.add(f"a{i.ops[1].reg}")
    for o in i.ops:
        if o.kind in ("postinc", "predec"):
            w.add(f"a{o.reg}")
    w.discard("a7")
    return w


class Resolver:
    def __init__(self, P: Program):
        self.P = P
        self.label_addrs = sorted(set(P.labels.values()))

    # ---------------------------------------------------------------- setup
    def build(self, extra):
        P = self.P
        self._vcache, self._wcache, self._fcache = {}, {}, {}
        self.reached, self.entries, _ = P.discover(extra)
        self.preds = P.predecessors(self.reached)
        self.funcs = P.functions(self.entries)
        self.callers = defaultdict(list)            # entry -> call instructions
        for a in self.reached:
            f = P.flow(P.insn(a))
            if f.kind in ("call", "icall"):
                for t in f.targets:
                    self.callers[t].append(a)
        self._summaries()
        self._stores()
        # vectors into RAM are `jmp` trampolines: targets are what the game stores at vector+2
        self.ram_vector_targets = {v: sorted(self.ram_stores.get((v + 2) | 0xFF000000, ()) |
                                             self.ram_stores.get((v + 2) & 0xFFFFFFFF, ()))
                                   for v in P.ram_vectors()}

    def _summaries(self):
        """may_write[entry]: registers a call to entry may change; exits[entry]: its returns."""
        P = self.P
        direct, preserved, rets = {}, {}, {}
        for e, fn in self.funcs.items():
            w = set()
            for a in fn.body:
                w |= writes(P.insn(a))
            rets[e] = {a for a in fn.body if P.insn(a).mnem in ("rts", "rtr", "rte")}
            direct[e] = w
            preserved[e] = self._preserved(e, fn, rets[e])
        may = {e: set(direct[e]) for e in self.funcs}
        changed = True
        while changed:
            changed = False
            for e, fn in self.funcs.items():
                acc = set(direct[e])
                for c in fn.calls | fn.tails:
                    acc |= may.get(c, set(REGS) - {"a7"})
                acc -= preserved[e]
                if acc != may[e]:
                    may[e], changed = acc, True
        self.may_write = may
        exits = {e: set(r) for e, r in rets.items()}
        changed = True
        while changed:
            changed = False
            for e, fn in self.funcs.items():
                for t in fn.tails:
                    if not exits[t] <= exits[e]:
                        exits[e] |= exits[t]
                        changed = True
        self.exits = exits

    def _preserved(self, e, fn, rets):
        """Registers saved by `movem.l list,-(sp)` at entry and restored before every return."""
        P = self.P
        first = P.insn(e)
        if first.mnem != "movem" or first.ops[-1].kind != "predec" or not rets:
            return set()
        mask = first.ops[0].val
        for r in rets:
            ok = False
            for p, kind in self.preds.get(r, []):
                pi = P.insn(p)
                if kind == "fall" and pi.mnem == "movem" and pi.ops[0].kind == "postinc" \
                        and pi.ops[-1].val == mask:
                    ok = True
            if not ok:
                return set()
        return {REGS[b] for b in range(16) if mask >> b & 1} - {"a7"}

    def _stores(self):
        """Code addresses written to object fields / RAM variables (pointer candidates)."""
        P = self.P
        self.field_stores = defaultdict(set)
        self.ram_stores = defaultdict(set)
        for a in self.reached:
            i = P.insn(a)
            if i.mnem not in ("move", "movea") or i.size != "l" or len(i.ops) != 2:
                continue
            src, dst = i.ops
            vals = []
            if src.kind == "imm":
                vals = [src.val & 0xFFFFFF]
            elif src.kind == "an":
                # lea Code,aY / move.l aY,field : the immediately preceding instruction
                for p, kind in self.preds.get(a, []):
                    pi = P.insn(p)
                    if kind == "fall" and pi.mnem == "lea" and pi.ops[-1].kind == "an"                             and pi.ops[-1].reg == src.reg and pi.ops[0].kind in DIRECT_EA:
                        vals.append(pi.ops[0].val & 0xFFFFFF)
            vals = [v for v in vals if v in P.listed_code]
            if not vals:
                continue
            if dst.kind in ("disp", "ind"):
                self.field_stores[dst.val if dst.kind == "disp" else 0].update(vals)
            elif dst.kind in ("absw", "absl"):
                self.ram_stores[dst.val & 0xFFFFFFFF].update(vals)

    # ---------------------------------------------------------------- values
    def writes(self, a):
        w = self._wcache.get(a)
        if w is None:
            w = self._wcache[a] = writes(self.P.insn(a))
        return w

    def flow(self, a):
        f = self._fcache.get(a)
        if f is None:
            f = self._fcache[a] = self.P.flow(self.P.insn(a))
        return f

    def value(self, reg, at, depth=0):
        """Abstract values `reg` can hold just before the instruction at `at`."""
        if depth > 3:
            return {("unknown", "nesting")}
        key = (reg, at)
        if key in self._vcache:
            return self._vcache[key]
        self._vcache[key] = {("unknown", "recursive definition")}   # cycle guard
        res = self._value(reg, at, depth)
        self._vcache[key] = res
        return res

    def _value(self, reg, at, depth):
        P = self.P
        out = set()
        seen = set()
        work = deque([(at, ())])
        steps = 0
        while work:
            a, stack = work.popleft()
            steps += 1
            if steps > MAX_STEPS:
                out.add(("unknown", "search limit"))
                break
            # predecessors of a: instructions that may execute right before it
            preds = list(self.preds.get(a, []))
            if a in self.entries:
                if stack:                   # back out to the call we descended from
                    preds.append((stack[-1], "callsite"))
                    stack = stack[:-1]
                else:
                    preds += [(c, "callsite") for c in self.callers.get(a, [])]
                    if not self.callers.get(a) and not self.preds.get(a):
                        out.add(("unknown", f"entry {P.name(a)} has no known callers"))
            for p, kind in preds:
                if kind == "return":
                    # a follows a call at p: did the callee change reg?
                    callees = self.flow(p).targets
                    clobbers = [c for c in callees if reg in self.may_write.get(c, set(REGS))]
                    if clobbers:
                        if len(stack) >= MAX_DEPTH:
                            out.add(("unknown", "call depth"))
                            continue
                        for c in clobbers:
                            for r in self.exits.get(c, ()):
                                st = (r, stack + (p,))
                                if st not in seen:
                                    seen.add(st)
                                    self._visit(r, reg, st, work, out, depth)
                        continue
                    kind = "fall"           # callee leaves reg alone: step over the call
                st = (p, stack)
                if st in seen:
                    continue
                seen.add(st)
                self._visit(p, reg, st, work, out, depth)
        return out or {("unknown", "no definition found")}

    def _visit(self, p, reg, st, work, out, depth):
        if reg in self.writes(p):
            out |= self.eval_def(self.P.insn(p), reg, depth)
        else:
            work.append(st)

    def eval_def(self, i, reg, depth):
        """Abstract value(s) instruction i leaves in reg."""
        P = self.P
        m, ops = i.mnem, i.ops
        dst = ops[-1] if ops else None
        if reg_of(dst) != reg:
            return {("unknown", f"{i.text()} side effect")}
        src = ops[0]
        sub = lambda r: self.value(r, i.addr, depth + 1)

        if m == "lea":
            if src.kind in DIRECT_EA:
                return {("const", src.val)}
            if src.kind in ("disp", "ind"):
                d = src.val if src.kind == "disp" else 0
                return {("const", v[1] + d) if v[0] == "const" else v for v in sub(f"a{src.reg}")}
            return {("unknown", i.text())}
        if m == "moveq":
            return {("const", src.val)}
        if m in ("move", "movea") and len(ops) == 2:
            if src.kind == "imm":
                v = src.val
                if m == "movea" and i.size == "w":
                    v = v - 0x10000 if v & 0x8000 else v
                return {("const", v)}
            if src.kind in ("dn", "an"):
                return sub(reg_of(src))
            if src.kind == "pcidx":
                return {("ptrtab" if i.size == "l" else "wordtab", src.val, src.xreg)}
            if src.kind == "idx":
                res = set()
                kind = "ptrtab" if i.size == "l" else "wordtab"
                for v in sub(f"a{src.reg}"):
                    if v[0] == "const":
                        res.add((kind, v[1] + src.val, src.xreg))
                    elif v[0] == "offtab":
                        # base = T + offset_table[T]: one table per offset-table entry
                        for b in self.raw_offsets(v[1]):
                            res.add((kind, b + src.val, src.xreg))
                    else:
                        res.add(("unknown", f"table base {v}"))
                return res
            if src.kind in ("absw", "absl"):
                addr = src.val & 0xFFFFFFFF
                if addr < len(P.rom) and i.size == "l":
                    return {("const", P.long(addr))}
                return {("ram", addr)}
            if src.kind in ("disp", "ind"):
                return {("field", src.val if src.kind == "disp" else 0)}
            return {("unknown", i.text())}
        if m == "adda" and len(ops) == 2:
            if src.kind == "idx" and f"a{src.reg}" == reg and i.size == "w":
                res = set()
                for v in sub(reg):
                    res.add(("offtab", v[1] + src.val, src.xreg) if v[0] == "const"
                            else ("unknown", f"offset table base {v}"))
                return res
            if src.kind == "imm":
                return {("const", v[1] + src.val) if v[0] == "const" else v for v in sub(reg)}
            if src.kind in ("dn", "an"):
                res = set()
                for b in sub(reg):
                    for x in sub(reg_of(src)):
                        if b[0] == "const" and x[0] == "const":
                            res.add(("const", b[1] + x[1]))
                        elif b[0] == "const" and x[0] == "wordtab" and x[1] == b[1]:
                            res.add(("offtab", b[1], x[2]))
                        elif b[0] == "const":
                            res.add(("based", b[1], reg_of(src)))
                        else:
                            res.add(("unknown", f"{i.text()}"))
                return res
        if m in ("addq", "subq"):
            s = src.val if m == "addq" else -src.val
            return {("const", v[1] + s) if v[0] == "const" else v for v in sub(reg)}
        if m == "add" and src.kind == "dn" and reg_of(src) == reg:      # add.w d0,d0
            return {("const", v[1] * 2) if v[0] == "const" else ("unknown", "scaled index")
                    for v in sub(reg)}
        if m in ("lsl", "asl") and src.kind == "imm":
            return {("const", v[1] << src.val) if v[0] == "const" else ("unknown", "scaled index")
                    for v in sub(reg)}
        return {("unknown", i.text())}

    # ---------------------------------------------------------------- tables
    def next_label(self, t):
        k = bisect_right(self.label_addrs, t)
        return self.label_addrs[k] if k < len(self.label_addrs) else len(self.P.rom)

    def branch_table(self, t):
        """Entry addresses of a table of bra/jmp/rts instructions starting at t."""
        P, end, out, a = self.P, self.next_label(t), [], t
        first = None
        while a < end:
            i = P.insn(a)
            short = i.mnem in ("rts",) or i.size == "s"
            if i.mnem not in ("bra", "jmp", "rts"):
                break
            if first is None:
                first = short
            elif first != short and not (i.mnem == "rts"):
                break
            out.append(a)
            a += i.length
        return out

    def raw_offsets(self, t):
        """Bases T+w for each word w of an offset table at T whose entries point at data."""
        P, end, out = self.P, self.next_label(t), []
        for a in range(t, end - 1, 2):
            b = t + struct.unpack_from(">h", P.rom, a)[0]
            if not 0 <= b < len(P.rom):
                break
            out.append(b)
        return sorted(set(out))

    def data_table(self, t, kind):
        """Targets of a dc.l pointer table ("ptrtab") or dc.w offset table ("offtab"/"wordtab")."""
        P, end, out = self.P, self.next_label(t), []
        a = t
        step = 4 if kind == "ptrtab" else 2
        while a + step <= end:
            if kind == "ptrtab":
                tgt = P.long(a) & 0xFFFFFF
            else:
                w = struct.unpack_from(">h", P.rom, a)[0]
                tgt = t + w
            if tgt not in P.listed_code:
                break
            out.append(tgt)
            a += step
        return out

    # ---------------------------------------------------------------- sites
    def resolve_site(self, a):
        P = self.P
        i = P.insn(a)
        o = i.ops[0]
        site = dict(addr=a, insn=i.text(), kind=None, targets=[], confidence="none", notes=[])
        if o.kind == "pcidx":
            t = o.val
            idx = self.value(o.xreg, a)
            if any(v[0] == "wordtab" and v[1] == t for v in idx):
                site["kind"] = "pc_offset_table"
                site["targets"] = self.data_table(t, "offtab")
            else:
                consts = {v[1] for v in idx if v[0] == "const"}
                if consts and all(v[0] == "const" for v in idx):
                    site["kind"] = "pc_table_const_index"
                    site["targets"] = sorted(t + c for c in consts)
                else:
                    site["kind"] = "pc_branch_table"
                    site["targets"] = self.branch_table(t)
            site["confidence"] = "high" if site["targets"] else "none"
            return site

        base = self.value(f"a{o.reg}", a)
        disp = o.val if o.kind in ("disp", "idx") else 0
        targets, kinds, conf = set(), set(), []
        for v in base:
            if v[0] == "const":
                if o.kind == "idx":
                    idx = self.value(o.xreg, a)
                    if idx and all(x[0] == "const" for x in idx):
                        targets |= {v[1] + disp + x[1] for x in idx}
                        kinds.add("table_const_index")
                    else:
                        targets |= set(self.branch_table(v[1] + disp) or self.data_table(v[1] + disp, "offtab"))
                        kinds.add("register_table")
                else:
                    targets.add((v[1] + disp) & 0xFFFFFF)
                    kinds.add("constant")
                conf.append("high")
            elif v[0] in ("ptrtab", "offtab"):
                targets |= set(self.data_table(v[1], v[0]))
                kinds.add("pointer_table" if v[0] == "ptrtab" else "offset_table")
                conf.append("high")
            elif v[0] == "field":
                targets |= self.field_stores.get(v[1], set())
                kinds.add("object_field")
                conf.append("low")
                site["notes"].append(f"pointer loaded from field ${v[1]:X}: candidates are every code "
                                     "address the game stores at that offset (needs runtime check)")
            elif v[0] == "ram":
                targets |= self.ram_stores.get(v[1], set())
                kinds.add("ram_pointer")
                conf.append("medium")
                site["notes"].append(f"pointer loaded from RAM ${v[1]:X}")
            elif v[0] == "based":
                targets |= set(self.branch_table(v[1]))
                kinds.add("computed_offset")
                conf.append("medium")
                site["notes"].append(f"base ${v[1]:X} plus computed offset in {v[2]}")
            else:
                conf.append("none")
                site["notes"].append(f"unresolved: {v[1]}")
        site["kind"] = "+".join(sorted(kinds)) or "unresolved"
        site["targets"] = sorted(t for t in targets if t < len(P.rom))
        order = ["none", "low", "medium", "high"]
        site["confidence"] = min(conf, key=order.index) if conf else "none"
        if site["confidence"] == "none" and site["targets"]:
            site["confidence"] = "low"
        return site

    def trap2_site(self, a):
        """trap #2 = jump through the offset table in a0 (Trap02Exception)."""
        targets = set()
        for v in self.value("a0", a):
            if v[0] == "const":
                targets |= set(self.data_table(v[1], "offtab"))
        return dict(addr=a, targets=sorted(targets), confidence="high" if targets else "none")

    def sites(self):
        P = self.P
        out = []
        for a in sorted(self.reached):
            i = P.insn(a)
            if i.mnem in ("jmp", "jsr") and i.ops[0].kind not in DIRECT_EA:
                out.append(a)
        return out


def follow(P, a):
    """Final destination of a table entry: through bra/jmp, or 'rts'."""
    for _ in range(4):
        i = P.insn(a)
        if i.mnem == "rts":
            return "rts"
        if i.mnem == "bra" or (i.mnem == "jmp" and i.ops[0].kind in DIRECT_EA):
            a = i.ops[0].val & 0xFFFFFF
            continue
        return a
    return a


def main():
    P = Program()
    R = Resolver(P)
    extra = set(P.code_refs())
    rounds = 0
    while True:
        rounds += 1
        R.build(extra)
        sites = {a: R.resolve_site(a) for a in R.sites()}
        traps = {a: R.trap2_site(a) for a in sorted(R.reached)
                 if P.insn(a).mnem == "trap" and P.insn(a).ops[0].val == 2}
        new = {a: s["targets"] for a, s in sites.items()}
        new.update({a: t["targets"] for a, t in traps.items()})
        # the dispatcher inside the trap #2 handler can reach every caller's table
        handler = P.long(4 * 34) & 0xFFFFFF
        dispatch = [a for a in sites if handler in R.funcs and a in R.funcs[handler].body]
        for a in dispatch:
            union = sorted({x for t in traps.values() for x in t["targets"]})
            sites[a].update(kind="trap2_dispatch", targets=union, confidence="high",
                            notes=["union of all trap #2 callers' tables"])
            new[a] = union
        tramp = {t for ts in R.ram_vector_targets.values() for t in ts}
        if new == P.targets and tramp <= extra:
            break
        P.targets = new
        extra |= tramp
        if rounds > 8:
            break

    lc = P.listed_code
    live = set(R.reached)
    live_listed = len(live & lc)

    # Code nothing refers to (dead, as far as static analysis can tell) is compiled
    # anyway, so a jump there at runtime finds a function instead of nothing.
    dead = []
    for a in sorted(lc - live):
        if dead and dead[-1][1] == a:
            dead[-1][1] = P.listed[a].next
        else:
            dead.append([a, P.listed[a].next])
    dead_starts = {a for a, _ in dead}
    R.build(extra | dead_starts)
    for a in R.sites():
        if a not in sites:
            sites[a] = R.resolve_site(a)
            sites[a]["notes"].append("in unreferenced code")

    # why each entry point exists
    why = defaultdict(set)
    for v in P.vectors():
        why[v].add("vector")
    for t in tramp:
        why[t].add("ram_vector")
    for a, rs in P.code_refs().items():
        why[a] |= rs
    for a in R.reached:
        f = R.flow(a)
        if f.kind in ("call", "icall"):
            for t in f.targets:
                why[t].add("call" if f.kind == "call" else "indirect")
    for a in dead_starts:
        why[a].add("unreferenced")
    funcs = [dict(entry=f"${e:X}", name=f.name, insns=len(f.body),
                  via=sorted(why.get(e, {"jump-target"})),
                  calls=[f"${c:X}" for c in sorted(f.calls)],
                  tails=[f"${t:X}" for t in sorted(f.tails)])
             for e, f in sorted(R.funcs.items())]
    OUT.mkdir(exist_ok=True)
    (OUT / "functions.json").write_text(json.dumps(dict(
        functions=funcs, unreferenced_runs=[[f"${a:X}", f"${b:X}"] for a, b in dead]), indent=1))

    func_of = {}
    for e, f in R.funcs.items():
        for a in f.body:
            func_of.setdefault(a, e)
    report = dict(
        rounds=rounds, reached=len(live), listed_code=len(lc),
        reached_listed=live_listed, unreferenced_runs=len(dead), functions=len(R.funcs),
        sites=[dict(addr=f"${s['addr']:X}", func=P.name(func_of.get(s["addr"], s["addr"])),
                    insn=s["insn"], kind=s["kind"], confidence=s["confidence"], notes=s["notes"],
                    targets=[dict(addr=f"${t:X}", label=P.name(t), final=(
                        "rts" if follow(P, t) == "rts" else f"${follow(P, t):X}")) for t in s["targets"]])
               for s in sites.values()],
        ram_vectors={f"${v & 0xFFFFFFFF:X}": [dict(addr=f"${t:X}", label=P.name(t)) for t in ts]
                     for v, ts in R.ram_vector_targets.items()},
        trap2_sites=[dict(addr=f"${t['addr']:X}", confidence=t["confidence"],
                          targets=[dict(addr=f"${x:X}", label=P.name(x)) for x in t["targets"]])
                     for t in traps.values()],
    )
    OUT.mkdir(exist_ok=True)
    (OUT / "indirect_targets.json").write_text(json.dumps(report, indent=1))

    conf = Counter(s["confidence"] for s in sites.values())
    kinds = Counter(s["kind"] for s in sites.values())
    vias = Counter(v for f in funcs for v in f["via"])
    md = ["# PSIV indirect jumps (resolve.py)\n",
          f"{len(sites)} indirect sites, {len(traps)} `trap #2` callers; "
          f"{live_listed}/{len(lc)} listed instructions reachable from the vectors; "
          f"{len(dead)} unreferenced code runs ({len(lc) - live_listed} instructions) compiled anyway; "
          f"{len(R.funcs)} functions; {rounds} discovery rounds.\n",
          "Function entry points by reason: "
          + ", ".join(f"{k} {v}" for k, v in vias.most_common()) + "\n",
          "| confidence | sites |", "|---|---|"]
    md += [f"| {k} | {v} |" for k, v in sorted(conf.items(), key=lambda kv: -kv[1])]
    md += ["", "| kind | sites |", "|---|---|"]
    md += [f"| {k} | {v} |" for k, v in kinds.most_common()]
    t2 = Counter(t["confidence"] for t in traps.values())
    md += ["", "`trap #2` callers: " + ", ".join(f"{k} {v}" for k, v in t2.items()), "",
           "## Not high confidence", "", "| addr | function | insn | kind | conf | targets | notes |",
           "|---|---|---|---|---|---|---|"]
    for s in report["sites"]:
        if s["confidence"] != "high":
            md.append(f"| {s['addr']} | {s['func']} | `{s['insn']}` | {s['kind']} | {s['confidence']} | "
                      f"{len(s['targets'])} | {'; '.join(s['notes'])} |")
    (OUT / "indirect_targets.md").write_text("\n".join(md) + "\n")
    print("\n".join(md[:22]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
