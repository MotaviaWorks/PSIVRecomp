#!/usr/bin/env python3
"""Recompile Phantasy Star IV's 68000 code to C.

Reads the function map and indirect-jump targets produced by tools/resolve.py
(analysis/functions.json, analysis/indirect_targets.json) and writes:

  build/gen/psiv_funcs.h      prototypes
  build/gen/psiv_NN.c         the functions, split for parallel compiling
  build/gen/psiv_table.c      sorted address -> function table for indirect calls

Every 68000 function becomes `void F_xxxxxx(void)` operating on the CPU state
in runtime/m68k.h. Branches inside a function become gotos; jump tables become
switches; calls push the real return address (see CALL in m68k.h).

Condition codes are only computed where they can be read: a backward liveness
pass per function, with each function's flags-read-at-entry solved for the
whole program (a routine may return a result in Z for its caller's `beq`).
"""
import json
import sys
from collections import defaultdict
from pathlib import Path

from program import Program, DIRECT_EA

ROOT = Path(__file__).resolve().parent.parent
ANALYSIS = ROOT / "analysis"
GEN = ROOT / "build" / "gen"
FILES = 32

# Runtime hooks: (label, byte offset, instruction it must be, C function called
# before that instruction executes).
HOOKS = [
    # WaitForVInt: `st VInt_Flag` then spins on `tst.b (VInt_Flag).w / bne.s`.
    # The hook ends the frame: render, present, input, then the VInt handler
    # clears the flag and the spin exits.
    ("WaitForVInt", 4, "tst.b\t($FFFFEF12).w", "psiv_wait_vblank"),
    # Each run of the 68000 sound driver: numbers sound ticks so the chip-write
    # log can be compared with BizHawk tick by tick, independent of lag.
    ("UpdateSound", 0, "lea\t($FF5000).l, a6", "psiv_sound_tick"),
    # Diagonal steps on the field (an option; runtime/walkto.c). FieldObj_Move
    # begins a party member's step of one cell. The first hook comes before
    # the leader's collision check, with the pad's directions in d0: it may
    # choose another direction. The second comes as any step begins: it may
    # add the other axis. With the option off both return at once.
    ("FieldObj_Move", 0x26, "bsr.w\t$45B5A", "psiv_field_step_dir"),
    ("loc_4785C", 0, "move.b\t#$3, ($FFFFECE5).w", "psiv_field_step_begin"),
    # More save slots: pages of three (an option; runtime/savepages.c). The
    # title's menu as it is about to look for saves; each frame its Continue
    # window, its Erase window and the game's Save window wait for the pad;
    # and a save as it begins. With the option off each returns at once.
    ("loc_4335C", 0, "move.w\t#$11, ($FFFFEC28).w", "psiv_savepage_title"),
    ("loc_435B4", 0, "move.b\t($FFFFEF03).w, d0", "psiv_savepage_pick"),
    ("loc_43848", 0, "move.b\t($FFFFEF03).w, d0", "psiv_savepage_pick"),
    ("loc_593D4", 0, "move.w\t#$2, d0", "psiv_savepage_list"),
    ("TransferToSRAM", 0, "lea\t($FFFFF002).w, a0", "psiv_savepage_store"),
]

# Immediates the runtime supplies instead: (label, byte offset, instruction it
# must be, C expression used for its immediate operand). The expression must
# give the original value unless an enhancement is on, so the faithful port
# is unchanged.
#
# Widescreen: the sprite builders keep a sprite piece only if its x (VDP
# coordinates) minus $41 is below $17F, i.e. screen x -63 to 319. With
# widescreen on, psiv_sprite_xbase() and psiv_sprite_xlimit() let pieces
# through from x -96 (wide enough for the left margin) out to x 383 (the
# VDP's limit), while the sprite table has room (80 entries).
SPRITE_XBASE = "psiv_sprite_xbase()"
SPRITE_XLIMIT = "psiv_sprite_xlimit(D(6))"
IMM_OVERRIDES = [
    ("Battle_FillSpriteAttributes", 0x64, "subi.w\t#$41, d4", SPRITE_XBASE),
    ("Battle_FillSpriteAttributes", 0x68, "cmpi.w\t#$17F, d4", SPRITE_XLIMIT),
    ("Field_FillSpriteAttributes", 0x64, "subi.w\t#$41, d4", SPRITE_XBASE),
    ("Field_FillSpriteAttributes", 0x68, "cmpi.w\t#$17F, d4", SPRITE_XLIMIT),
    ("loc_44BE0", 0x62, "subi.w\t#$41, d4", SPRITE_XBASE),
    ("loc_44BE0", 0x66, "cmpi.w\t#$17F, d4", SPRITE_XLIMIT),
    ("loc_44D02", 0x08, "subi.w\t#$41, d4", SPRITE_XBASE),
    ("loc_44D02", 0x0C, "cmpi.w\t#$17F, d4", SPRITE_XLIMIT),
]
OVERRIDE_PROTOTYPES = ["uint32_t psiv_sprite_xbase(void);", "uint32_t psiv_sprite_xlimit(uint32_t d6);"]

X, N, Z, V, C = 16, 8, 4, 2, 1
NZVC, ALL = N | Z | V | C, 31
COND_USE = {"t": 0, "f": 0, "hi": C | Z, "ls": C | Z, "cc": C, "cs": C, "ne": Z, "eq": Z,
            "vc": V, "vs": V, "pl": N, "mi": N, "ge": N | V, "lt": N | V, "gt": N | V | Z,
            "le": N | V | Z}
BITS = {"b": 8, "w": 16, "l": 32}
MASK = {"b": "0xFFu", "w": "0xFFFFu", "l": "0xFFFFFFFFu"}
RD = {"b": "rd8", "w": "rd16", "l": "rd32"}
WR = {"b": "wr8", "w": "wr16", "l": "wr32"}


def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


def fname(a):
    return f"F_{a:06X}"


class EmitError(Exception):
    pass


# ---------------------------------------------------------------------- flags
def flag_effect(i):
    """(use, def) condition-code masks of a non-control-flow instruction."""
    m, ops = i.mnem, i.ops
    dst = ops[-1] if ops else None
    if m in ("move",):
        if ops[0].kind in ("sr", "ccr"):
            return ALL, 0
        if dst.kind in ("sr", "ccr"):
            return 0, ALL
        if "usp" in (ops[0].kind, dst.kind):
            return 0, 0
        return 0, NZVC
    if m in ("movea", "lea", "pea", "adda", "suba", "exg", "link", "unlk", "movem", "movep",
             "nop", "reset"):
        return 0, 0
    if m in ("moveq", "and", "or", "eor", "not", "clr", "tst", "ext", "swap", "mulu", "muls",
             "tas", "divu", "divs", "cmp", "cmpa", "cmpi", "cmpm"):
        return 0, NZVC
    if m in ("andi", "ori", "eori"):
        return (ALL, ALL) if dst.kind in ("sr", "ccr") else (0, NZVC)
    if m in ("add", "addi", "sub", "subi", "neg"):
        return 0, ALL
    if m in ("addq", "subq"):
        return (0, 0) if dst.kind == "an" else (0, ALL)
    if m in ("addx", "subx", "negx", "abcd", "sbcd", "nbcd"):
        return X | Z, ALL
    if m in ("asl", "asr", "lsl", "lsr"):
        return (X if ops[0].kind == "dn" and len(ops) == 2 else 0), ALL
    if m in ("rol", "ror"):
        return 0, NZVC
    if m in ("roxl", "roxr"):
        return X, ALL
    if m in ("btst", "bchg", "bclr", "bset"):
        return 0, Z
    if m == "chk":
        return 0, N
    if m in ("bcc", "scc", "dbcc"):
        return COND_USE[i.cond], 0
    if m == "stop":
        return 0, ALL
    raise EmitError(f"no flag rule for {i.text()}")


class Recompiler:
    def __init__(self):
        fm = json.loads((ANALYSIS / "functions.json").read_text())
        it = json.loads((ANALYSIS / "indirect_targets.json").read_text())
        p = lambda s: int(s[1:], 16)
        targets = {p(s["addr"]): [p(t["addr"]) for t in s["targets"]] for s in it["sites"]}
        targets.update({p(s["addr"]): [p(t["addr"]) for t in s["targets"]] for s in it["trap2_sites"]})
        self.P = Program(targets)
        self.entries = {p(f["entry"]) for f in fm["functions"]}
        self.funcs = self.P.functions(self.entries)
        self.live_in = {e: 0 for e in self.funcs}      # flags each function reads at entry
        self.rom_size = len(self.P.rom)
        self.hooks = {}
        for label, off, expect, cfn in HOOKS:
            a = self.P.labels[label.lower()] + off
            got = self.P.insn(a).text()
            if got != expect:
                raise EmitError(f"hook {cfn}: expected '{expect}' at {label}+{off} (${a:X}), found '{got}'")
            self.hooks[a] = cfn
        self.imm_overrides = {}
        for label, off, expect, expr in IMM_OVERRIDES:
            a = self.P.labels[label.lower()] + off
            got = self.P.insn(a).text()
            if got != expect:
                raise EmitError(f"override {expr}: expected '{expect}' at {label}+{off} (${a:X}), found '{got}'")
            self.imm_overrides[a] = expr

    # ------------------------------------------------------------ CFG helpers
    def flow(self, a):
        return self.P.flow(self.P.insn(a))

    def call_use(self, targets):
        if not targets:
            return ALL
        u = 0
        for t in targets:
            u |= self.live_in.get(t, ALL)
        return u

    def liveness(self, fn):
        """live_after[a] for every instruction of fn (flags that may be read later)."""
        P, body = self.P, fn.body
        succ, gen, kill, exit_use = {}, {}, {}, {}
        for a in body:
            i, f = P.insn(a), self.flow(a)
            s, ex = [], 0
            if f.kind in ("call", "icall"):
                use, d = self.call_use(f.targets), 0
                if i.mnem in ("trap", "exception", "illegal"):
                    use = ALL
                if f.fall is not None:
                    if f.fall in body and not (f.fall in self.entries and f.fall != fn.entry):
                        s.append(f.fall)
                    else:
                        ex |= self.live_in.get(f.fall, ALL)
            elif f.kind == "ret":
                use, d = (0, ALL) if i.mnem in ("rte", "rtr") else (0, 0)
                ex = ALL if i.mnem == "rts" else 0
            else:
                if f.kind in ("seq", "cond"):
                    use, d = flag_effect(i)
                else:
                    use, d = (COND_USE[i.cond], 0) if i.mnem in ("bcc", "dbcc") else (0, 0)
                outs = list(f.targets) + ([f.fall] if f.fall is not None else [])
                if f.kind == "ijump" and not f.targets:
                    ex |= ALL
                for t in outs:
                    if t in body and (t == fn.entry or t not in self.entries):
                        s.append(t)
                    else:
                        ex |= self.live_in.get(t, ALL)
            succ[a], gen[a], kill[a], exit_use[a] = s, use, d, ex
        live_after = {a: 0 for a in body}
        live_before = {a: 0 for a in body}
        order = sorted(body, reverse=True)
        changed = True
        while changed:
            changed = False
            for a in order:
                out = exit_use[a]
                for t in succ[a]:
                    out |= live_before[t]
                inn = gen[a] | (out & ~kill[a] & ALL)
                if out != live_after[a] or inn != live_before[a]:
                    live_after[a], live_before[a] = out, inn
                    changed = True
        return live_after, live_before

    def solve_live_in(self):
        for rnd in range(12):
            changed = False
            for e, fn in self.funcs.items():
                _, before = self.liveness(fn)
                v = before.get(e, 0)
                if v != self.live_in[e]:
                    self.live_in[e] = v
                    changed = True
            if not changed:
                return rnd + 1
        return 12

    # ------------------------------------------------------------ operands
    def emit_function(self, fn):
        g = FuncGen(self, fn)
        return g.run()


class FuncGen:
    def __init__(self, R, fn):
        self.R, self.P, self.fn = R, R.P, fn
        self.cur = -1                                    # address of the instruction being emitted
        self.body = fn.body
        self.lines = []
        self.labels = set()
        self.live_after, _ = R.liveness(fn)

    # ---------------------------------------------------------- targets
    def inside(self, t):
        """True if a transfer to t stays in this function (goto), else it is a tail call."""
        return t in self.body and (t == self.fn.entry or t not in self.R.entries)

    def goto(self, t):
        if self.inside(t):
            self.labels.add(t)
            return f"goto L_{t:06X};"
        if t in self.R.entries and t < self.R.rom_size:
            return f"{{ {fname(t)}(); return; }}"
        return f"{{ m68k_call({h(t)}); return; }}"

    # ---------------------------------------------------------- operands
    def addr(self, o, size, tmp, pre):
        """C expression for the address of memory operand o; side effects go to pre."""
        inc = {"b": 1, "w": 2, "l": 4}.get(size, 4)
        k = o.kind
        if k == "ind":
            return f"A({o.reg})"
        if k == "postinc":
            if o.reg == 7 and inc == 1:
                inc = 2
            pre.append(f"uint32_t {tmp} = A({o.reg}); A({o.reg}) += {inc};")
            return tmp
        if k == "predec":
            if o.reg == 7 and inc == 1:
                inc = 2
            pre.append(f"A({o.reg}) -= {inc}; uint32_t {tmp} = A({o.reg});")
            return tmp
        if k == "disp":
            return f"(A({o.reg}) + {h(o.val)})"
        if k == "idx":
            return f"(A({o.reg}) + {h(o.val)} + {self.index(o)})"
        if k in ("absw", "absl", "pcdisp"):
            return h(o.val)
        if k == "pcidx":
            return f"({h(o.val)} + {self.index(o)})"
        raise EmitError(f"not a memory operand: {k}")

    @staticmethod
    def index(o):
        r = f"{'D' if o.xreg[0] == 'd' else 'A'}({o.xreg[1]})"
        return r if o.xsize == "l" else f"SX16({r})"

    def read(self, o, size, tmp, pre):
        k = o.kind
        if k == "dn":
            return f"D({o.reg})" if size == "l" else f"(D({o.reg}) & {MASK[size]})"
        if k == "an":
            return f"A({o.reg})" if size == "l" else f"(A({o.reg}) & {MASK[size]})"
        if k == "imm":
            if self.cur in self.R.imm_overrides:             # supplied by the runtime
                return f"({self.R.imm_overrides[self.cur]} & {MASK[size]})"
            return h(o.val & int(MASK[size][:-1], 16))
        if k == "sr":
            return "GET_SR()"
        if k == "ccr":
            return "GET_CCR()"
        a = self.addr(o, size, tmp, pre)
        if a != tmp and ("A(" in a or "D(" in a):
            pre.append(f"uint32_t {tmp} = {a};")
            a = tmp
        return f"{RD[size]}({a})"

    def mem_addr(self, o, size, tmp, pre):
        """Address of a memory operand, computed once into tmp (for read-modify-write)."""
        a = self.addr(o, size, tmp, pre)
        if a != tmp:
            pre.append(f"uint32_t {tmp} = {a};")
        return tmp

    def write(self, o, size, val, addr_tmp=None):
        k = o.kind
        if k == "dn":
            if size == "l":
                return f"D({o.reg}) = {val};"
            return f"SET{size.upper()}(D({o.reg}), {val});"
        if k == "an":
            return f"A({o.reg}) = {val};"
        if k == "ccr":
            return f"SET_CCR({val});"
        if k == "sr":
            return f"SET_SR({val});"
        return f"{WR[size]}({addr_tmp}, {val});"

    # ---------------------------------------------------------- instructions
    def need(self, i, mask):
        return bool(self.live_after.get(i.addr, ALL) & mask)

    def emit(self, i):
        """C statements for instruction i (control flow included)."""
        self.cur = i.addr
        m, ops, sz = i.mnem, i.ops, i.size
        nxt = i.addr + i.length
        pre, out = [], []
        f = self.P.flow(i)

        # ---- control flow
        if m == "bra":
            return [self.goto(ops[0].val)]
        if m == "bcc":
            return [f"if (CC_{i.cond.upper()}) {self.goto(ops[0].val)}"]
        if m == "dbcc":
            r = ops[0].reg
            cond = "" if i.cond == "f" else f"if (!(CC_{i.cond.upper()})) "
            return [f"{cond}{{ uint32_t w_ = (D({r}) - 1) & 0xFFFFu; SETW(D({r}), w_); "
                    f"if (w_ != 0xFFFFu) {self.goto(ops[1].val)} }}"]
        if m == "bsr":
            return self.call_direct(ops[0].val, nxt)
        if m in ("jsr", "jmp"):
            o = ops[0]
            if o.kind in DIRECT_EA:
                t = o.val & 0xFFFFFF
                return self.call_direct(t, nxt) if m == "jsr" else [self.goto(t)]
            a = self.addr(o, "l", "t_", pre)
            if m == "jsr":
                return pre + [f"CALL_IND({a}, {h(nxt)});"]
            return pre + self.jump_indirect(a, f.targets)
        if m == "rts":
            return ["RTS();"]
        if m == "rtr":
            return ["RTR();"]
        if m == "rte":
            return ["RTE();"]
        if m == "trap":
            return [f"TRAP({32 + ops[0].val}, {h(nxt)});"]
        if m in ("illegal", "exception"):
            vec = 4 if m == "illegal" else ops[0].val
            self.labels.add(i.addr)
            return [f"m68k_exception({vec}, {h(i.addr)}); goto L_{i.addr:06X};"]
        if m == "stop":
            return [f"m68k_stop({h(ops[0].val)});"]
        if m == "reset":
            return ["m68k_reset_insn();"]
        if m == "nop":
            return []

        return self.alu(i, pre)

    def call_direct(self, t, ret):
        if t in self.R.entries and t < self.R.rom_size:
            return [f"CALL({fname(t)}, {h(ret)});"]
        return [f"CALL_IND({h(t)}, {h(ret)});"]

    def jump_indirect(self, a, targets):
        inner = [t for t in targets if self.inside(t)]
        lines = [f"{{ uint32_t j_ = ({a}) & 0xFFFFFFu;"]
        if inner:
            lines.append("  switch (j_) {")
            for t in sorted(set(inner)):
                self.labels.add(t)
                lines.append(f"  case {h(t)}: goto L_{t:06X};")
            lines.append("  default: break;")
            lines.append("  }")
        lines.append("  m68k_call(j_); return; }")
        return lines

    def alu(self, i, pre):
        m, ops, sz = i.mnem, i.ops, i.size
        need = lambda mask: self.need(i, mask)
        bits = BITS.get(sz, 32)
        o = []

        if m in ("move", "movea") and ops[0].kind not in ("usp",) and ops[1].kind != "usp":
            src, dst = ops
            if dst.kind == "an" or m == "movea":
                v = self.read(src, sz, "s_", pre)
                v = f"SX16({v})" if sz == "w" else v
                return pre + [f"A({dst.reg}) = {v};"]
            if src.kind == "sr":
                v = "GET_SR()"
            elif src.kind == "ccr":
                v = "GET_CCR()"
            else:
                v = self.read(src, sz if dst.kind not in ("sr", "ccr") else "w", "s_", pre)
            if dst.kind in ("sr", "ccr"):
                return pre + [self.write(dst, "w", v)]
            o.append(f"uint32_t v_ = {v};")
            if dst.kind not in ("dn", "an"):
                da = self.mem_addr(dst, sz, "d_", o)
                o.append(self.write(dst, sz, "v_", da))
            else:
                o.append(self.write(dst, sz, "v_"))
            if need(NZVC):
                o.append(f"LOGIC{bits}(v_);")
            return pre + o
        if m == "move" and ops[0].kind == "usp":
            return [f"A({ops[1].reg}) = m68k.usp;"]
        if m == "move" and ops[1].kind == "usp":
            return [f"m68k.usp = A({ops[0].reg});"]
        if m == "moveq":
            v = ops[0].val & 0xFFFFFFFF
            o = [f"D({ops[1].reg}) = {h(v)};"]
            if need(NZVC):
                o.append(f"LOGIC32({h(v)});")
            return o
        if m == "lea":
            a = self.addr(ops[0], "l", "s_", pre)
            return pre + [f"A({ops[1].reg}) = {a};"]
        if m == "pea":
            a = self.addr(ops[0], "l", "s_", pre)
            return pre + [f"{{ uint32_t p_ = {a}; PUSH32(p_); }}"]
        if m == "clr":
            return self.rmw(i, pre, lambda d: "0u", logic=True, read_dst=False)
        if m in ("not",):
            return self.rmw(i, pre, lambda d: f"(~{d} & {MASK[sz]})", logic=True)
        if m in ("neg", "negx"):
            fn = "NEG" if m == "neg" else "NEGX"
            if not need(ALL) and m == "neg":
                return self.rmw(i, pre, lambda d: f"((0u - {d}) & {MASK[sz]})", logic=False)
            return self.rmw(i, pre, lambda d: f"{fn}{bits}({d})", logic=False)
        if m == "tst":
            v = self.read(ops[0], sz, "s_", pre)
            return pre + [f"LOGIC{bits}({v});"] if need(NZVC) else pre + [f"(void){v};"]
        if m == "ext":
            r = ops[0].reg
            if sz == "w":
                o = [f"SETW(D({r}), SX8(D({r})));"]
                if need(NZVC):
                    o.append(f"LOGIC16(D({r}));")
            else:
                o = [f"D({r}) = SX16(D({r}));"]
                if need(NZVC):
                    o.append(f"LOGIC32(D({r}));")
            return o
        if m == "swap":
            r = ops[0].reg
            o = [f"D({r}) = D({r}) >> 16 | D({r}) << 16;"]
            if need(NZVC):
                o.append(f"LOGIC32(D({r}));")
            return o
        if m == "exg":
            a, b = (f"{'D' if x.kind == 'dn' else 'A'}({x.reg})" for x in ops)
            return [f"{{ uint32_t t_ = {a}; {a} = {b}; {b} = t_; }}"]
        if m in ("and", "or", "eor", "andi", "ori", "eori"):
            cop = {"and": "&", "or": "|", "eor": "^"}[m.rstrip("i")]
            src, dst = ops
            if dst.kind in ("ccr", "sr"):
                get = "GET_CCR()" if dst.kind == "ccr" else "GET_SR()"
                return [self.write(dst, "w", f"({get} {cop} {self.read(src, 'w', 's_', pre)})")]
            s = self.read(src, sz, "s_", pre)
            pre.append(f"uint32_t s2_ = {s};")
            return self.rmw(i, pre, lambda d: f"({d} {cop} s2_)", logic=True, dst=dst)
        if m in ("add", "addi", "sub", "subi", "addq", "subq", "addx", "subx"):
            src, dst = ops
            base = "ADD" if m.startswith("add") else "SUB"
            if dst.kind == "an":                       # addq/subq to An: 32-bit, no flags
                s = self.read(src, sz, "s_", pre)
                cop = "+" if base == "ADD" else "-"
                return pre + [f"A({dst.reg}) {cop}= {s};"]
            s = self.read(src, sz, "s_", pre)
            pre.append(f"uint32_t s2_ = {s};")
            if m in ("addx", "subx"):
                return self.rmw(i, pre, lambda d: f"{base}X{bits}({d}, s2_)", logic=False, dst=dst)
            if not need(ALL):
                cop = "+" if base == "ADD" else "-"
                return self.rmw(i, pre, lambda d: f"(({d} {cop} s2_) & {MASK[sz]})", logic=False, dst=dst)
            return self.rmw(i, pre, lambda d: f"{base}{bits}({d}, s2_)", logic=False, dst=dst)
        if m in ("adda", "suba"):
            src, dst = ops
            s = self.read(src, sz, "s_", pre)
            s = f"SX16({s})" if sz == "w" else s
            cop = "+" if m == "adda" else "-"
            return pre + [f"A({dst.reg}) {cop}= {s};"]
        if m in ("cmp", "cmpi", "cmpa", "cmpm"):
            src, dst = ops
            s = self.read(src, sz, "s_", pre)
            if m == "cmpa":
                s = f"SX16({s})" if sz == "w" else s
                pre.append(f"uint32_t s2_ = {s};")
                d = f"A({dst.reg})"
                return pre + ([f"CMP32({d}, s2_);"] if need(NZVC) else [])
            pre.append(f"uint32_t s2_ = {s};")
            d = self.read(dst, sz, "d_", pre)
            return pre + ([f"CMP{bits}({d}, s2_);"] if need(NZVC) else [f"(void)({d});"])
        if m in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"):
            if len(ops) == 1:                          # memory, word, by one
                return self.rmw(i, pre, lambda d: f"m68k_{m}({d}, 1, 16)", logic=False, dst=ops[0],
                                size="w")
            cnt = h(ops[0].val) if ops[0].kind == "imm" else f"D({ops[0].reg})"
            return self.rmw(i, pre, lambda d: f"m68k_{m}({d}, {cnt}, {bits})", logic=False, dst=ops[1])
        if m in ("mulu", "muls"):
            s = self.read(ops[0], "w", "s_", pre)
            r = ops[1].reg
            return pre + [f"D({r}) = m68k_{m}(D({r}), {s});"]
        if m in ("divu", "divs"):
            s = self.read(ops[0], "w", "s_", pre)
            r = ops[1].reg
            return pre + [f"if (!m68k_{m}(&D({r}), {s})) m68k_exception(5, {h(i.addr + i.length)});"]
        if m in ("abcd", "sbcd"):
            src, dst = ops
            s = self.read(src, "b", "s_", pre)
            pre.append(f"uint32_t s2_ = {s};")
            return self.rmw(i, pre, lambda d: f"m68k_{m}({d}, s2_)", logic=False, dst=dst, size="b")
        if m == "nbcd":
            return self.rmw(i, pre, lambda d: f"m68k_nbcd({d})", logic=False, dst=ops[0], size="b")
        if m in ("btst", "bchg", "bclr", "bset"):
            bit, dst = ops
            width = 32 if dst.kind == "dn" else 8
            bsz = "l" if dst.kind == "dn" else "b"
            b = h(bit.val % width) if bit.kind == "imm" else f"(D({bit.reg}) & {width - 1}u)"
            pre.append(f"uint32_t b_ = {b};")
            if m == "btst":
                d = self.read(dst, bsz, "d_", pre)
                return pre + [f"m68k.z = !(({d} >> b_) & 1u);"]
            op = {"bchg": "^", "bclr": "& ~", "bset": "|"}[m]
            return self.rmw(i, pre, lambda d: f"({d} {op} (1u << b_))", logic=False, dst=dst, size=bsz,
                            before=lambda d: f"m68k.z = !(({d} >> b_) & 1u);")
        if m == "scc":
            cond = f"CC_{i.cond.upper()}"
            return self.rmw(i, pre, lambda d: f"(({cond}) ? 0xFFu : 0u)", logic=False, dst=ops[0],
                            size="b", read_dst=False)
        if m == "tas":
            return self.rmw(i, pre, lambda d: f"({d} | 0x80u)", logic=False, dst=ops[0], size="b",
                            before=lambda d: f"LOGIC8({d});")
        if m == "movem":
            return self.movem(i, pre)
        if m == "movep":
            src, dst = ops
            n = 2 if sz == "w" else 4
            if src.kind == "dn":                       # register -> alternate bytes
                a = self.addr(dst, "l", "d_", pre)
                pre.append(f"uint32_t ma_ = {a};")
                for k in range(n):
                    pre.append(f"wr8(ma_ + {2 * k}u, D({src.reg}) >> {8 * (n - 1 - k)});")
                return pre
            a = self.addr(src, "l", "s_", pre)
            pre.append(f"uint32_t ma_ = {a}, v_ = 0;")
            for k in range(n):
                pre.append(f"v_ = v_ << 8 | rd8(ma_ + {2 * k}u);")
            pre.append(self.write(dst, sz, "v_"))
            return pre
        if m == "link":
            r, d = ops[0].reg, ops[1].val
            return [f"PUSH32(A({r})); A({r}) = A(7); A(7) += {h(d)};"]
        if m == "unlk":
            r = ops[0].reg
            return [f"A(7) = A({r}); A({r}) = POP32();"]
        if m == "chk":
            s = self.read(ops[0], "w", "s_", pre)
            r = ops[1].reg
            return pre + [f"{{ int16_t v_ = (int16_t)D({r}), b_ = (int16_t){s}; "
                          f"if (v_ < 0 || v_ > b_) {{ m68k.n = v_ < 0; m68k_exception(6, {h(i.addr + i.length)}); }} }}"]
        raise EmitError(f"unhandled instruction {i.text()} at ${i.addr:X}")

    def rmw(self, i, pre, expr, logic, dst=None, size=None, read_dst=True, before=None):
        """dst = expr(dst) with optional flags. For memory, the address is computed once."""
        dst = dst if dst is not None else i.ops[-1]
        sz = size or i.size
        bits = BITS[sz]
        o = list(pre)
        if dst.kind == "dn":
            d = f"D({dst.reg})" if sz == "l" else f"(D({dst.reg}) & {MASK[sz]})"
            if before:
                o.append(before(d))
            o.append(f"uint32_t r_ = {expr(d)};")
            o.append(self.write(dst, sz, "r_"))
        else:
            a = self.mem_addr(dst, sz, "d_", o)
            d = f"{RD[sz]}({a})" if read_dst else "0u"
            if read_dst:
                o.append(f"uint32_t dv_ = {d};")
                d = "dv_"
            if before:
                o.append(before(d))
            o.append(f"uint32_t r_ = {expr(d)};")
            o.append(self.write(dst, sz, "r_", a))
        if logic and self.need(i, NZVC):
            o.append(f"LOGIC{bits}(r_);")
        elif i.mnem == "clr" and self.need(i, NZVC):
            o.append("LOGIC32(0u);")
        return o

    def movem(self, i, pre):
        sz = i.size
        n = 2 if sz == "w" else 4
        if i.ops[-1].kind == "reglist":                # memory -> registers
            src, mask = i.ops[0], i.ops[1].val
            o = list(pre)
            if src.kind == "postinc":
                o.append(f"uint32_t ma_ = A({src.reg});")
            else:
                a = self.addr(src, sz, "s_", o)
                o.append(f"uint32_t ma_ = {a};")
            for b in range(16):
                if mask >> b & 1:
                    reg = f"D({b})" if b < 8 else f"A({b - 8})"
                    v = f"SX16(rd16(ma_))" if sz == "w" else "rd32(ma_)"
                    o.append(f"{reg} = {v}; ma_ += {n};")
            if src.kind == "postinc":
                o.append(f"A({src.reg}) = ma_;")
            return o
        mask, dst = i.ops[0].val, i.ops[1]                # registers -> memory
        o = list(pre)
        regs = [b for b in range(16) if mask >> b & 1]
        if dst.kind == "predec":
            # stored from the highest register down; An itself is stored before the decrement
            o.append(f"uint32_t ma_ = A({dst.reg}), v_[16];")
            for b in regs:
                o.append(f"v_[{b}] = {'D' if b < 8 else 'A'}({b % 8});")
            for b in reversed(regs):
                o.append(f"ma_ -= {n}; {WR[sz]}(ma_, v_[{b}]);")
            o.append(f"A({dst.reg}) = ma_;")
            return o
        a = self.addr(dst, sz, "d_", o)
        o.append(f"uint32_t ma_ = {a};")
        for b in regs:
            o.append(f"{WR[sz]}(ma_, {'D' if b < 8 else 'A'}({b % 8})); ma_ += {n};")
        return o

    # ---------------------------------------------------------- function
    def run(self):
        P, fn = self.P, self.fn
        order = sorted(self.body)
        blocks = []
        for k, a in enumerate(order):
            i = P.insn(a)
            f = P.flow(i)
            code = self.emit(i)
            # fall-through to whatever comes next in emission order
            fall = f.fall if f.kind in ("seq", "cond", "call", "icall") else None
            if f.kind == "seq" and i.mnem in ("illegal", "exception"):
                fall = None
            if fall is not None:
                nxt = order[k + 1] if k + 1 < len(order) else None
                if fall != nxt or not self.inside(fall):
                    code.append(self.goto(fall))
            blocks.append((a, i, code))
        out = [f"void {fname(fn.entry)}(void) /* {fn.name} */ {{"]
        if order[0] != fn.entry:
            # the body includes code below the entry (a shared tail or a loop head)
            self.labels.add(fn.entry)
            out.append(f"    goto L_{fn.entry:06X};")
        for a, i, code in blocks:
            lab = f"L_{a:06X}: " if a in self.labels else ""
            out.append(f"{lab}{{ /* {a:06X}: {i.text().replace(chr(9), ' ')} */ BLOCK({h(a)});")
            if a in self.R.hooks:
                out.append(f"    {self.R.hooks[a]}();")
            out.extend("    " + c for c in code)
            out.append("}")
        out.append("}")
        return out


def main():
    R = Recompiler()
    rounds = R.solve_live_in()
    GEN.mkdir(parents=True, exist_ok=True)
    funcs = sorted(R.funcs.values(), key=lambda f: f.entry)
    errors = []
    bodies = []
    for fn in funcs:
        try:
            bodies.append((fn, FuncGen(R, fn).run()))
        except EmitError as e:
            errors.append(f"{fn.name}: {e}")
    if errors:
        print(f"{len(errors)} functions failed:")
        for e in errors[:20]:
            print("  ", e)
        return 1

    hdr = ["/* generated by tools/emit.py - do not edit */", "#pragma once", '#include "m68k.h"', "",
           "/* runtime hooks (see HOOKS in tools/emit.py) */"]
    hdr += [f"void {cfn}(void);" for cfn in sorted(set(R.hooks.values()))] + [""]
    hdr += ["/* immediates supplied by the runtime (see IMM_OVERRIDES in tools/emit.py) */"]
    hdr += OVERRIDE_PROTOTYPES + [""]
    hdr += [f"void {fname(fn.entry)}(void); /* {fn.name} */" for fn in funcs]
    (GEN / "psiv_funcs.h").write_text("\n".join(hdr) + "\n")

    per = (len(bodies) + FILES - 1) // FILES
    total_lines = 0
    for k in range(FILES):
        chunk = bodies[k * per:(k + 1) * per]
        text = ["/* generated by tools/emit.py - do not edit */", '#include "psiv_funcs.h"', ""]
        for fn, lines in chunk:
            text += lines + [""]
        total_lines += len(text)
        (GEN / f"psiv_{k:02d}.c").write_text("\n".join(text) + "\n")

    # names come from the disassembly's labels; they are only used in crash reports
    tab = ["/* generated by tools/emit.py - do not edit */", '#include "psiv_funcs.h"', "",
           "const PsivFunction psiv_functions[] = {"]
    tab += [f'    {{ 0x{fn.entry:06X}u, {fname(fn.entry)}, "{fn.name}" }},' for fn in funcs]
    tab += ["};", f"const int psiv_function_count = {len(funcs)};"]
    (GEN / "psiv_table.c").write_text("\n".join(tab) + "\n")
    insns = sum(len(f.body) for f in funcs)
    print(f"{len(funcs)} functions, {insns} instructions -> {FILES} files, {total_lines} lines "
          f"(flag liveness solved in {rounds} rounds)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
