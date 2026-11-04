"""Program model: decoded instructions, control flow, functions.

Builds on tools/m68k.py (decoding) and the listing (labels, which bytes are
code). Indirect jump/call targets come from a target map
{site address: [target addresses]}; tools/resolve.py produces it.

Control flow vocabulary (Flow.kind):
  seq     falls through to the next instruction
  jump    unconditional transfer (bra, direct jmp)
  cond    conditional branch (bcc, dbcc): target or fall through
  ijump   indirect jmp: targets from the target map
  call    direct call (bsr, direct jsr, trap): callee + fall through on return
  icall   indirect jsr: callees from the target map + fall through
  ret     rts / rte / rtr
Illegal instructions and undecodable words are a "call" to their exception
handler with no fall-through.
"""
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path
import re
import struct

import listing
from m68k import Decoder, DecodeError, Insn, Op

ROOT = Path(__file__).resolve().parent.parent
LST = ROOT / "build" / "disasm" / "ps4.lst"
ROM = ROOT / "rom" / "psiv.bin"

DIRECT_EA = ("absw", "absl", "pcdisp")


@dataclass
class Flow:
    kind: str
    targets: list = field(default_factory=list)
    fall: int | None = None


@dataclass
class Function:
    entry: int
    name: str
    body: set = field(default_factory=set)        # instruction addresses
    calls: set = field(default_factory=set)       # callee entries (direct + indirect)
    tails: set = field(default_factory=set)       # entries reached by jump/fall-through (tail calls)


class Program:
    def __init__(self, targets=None):
        self.rom = ROM.read_bytes()
        self.dec = Decoder(self.rom)
        insns, self.labels, self.label_name = listing.parse(LST)
        self.listed = {i.addr: i for i in insns}
        self.listed_code = {i.addr for i in insns if i.cpu == "68000"
                            and not i.mnem.startswith(("dc", "ds"))}
        self.targets = targets or {}               # site -> [targets]
        self._cache = {}

    # ------------------------------------------------------------ basics
    def name(self, addr):
        return self.label_name.get(addr, f"sub_{addr:X}")

    def long(self, addr):
        return struct.unpack_from(">I", self.rom, addr)[0]

    def insn(self, addr):
        """Decoded instruction at addr.

        Words the 68000 cannot decode raise an exception on real hardware, so
        they decode to a pseudo-instruction "exception" naming the vector:
        line A -> 10, line F -> 11, anything else -> 4 (illegal instruction).
        The game has at least one branch into the middle of an instruction
        (bcc.w loc_586C2-2), which ends in exactly this.
        """
        i = self._cache.get(addr)
        if i is None:
            try:
                i = self.dec.decode(addr)
            except DecodeError:
                line = self.rom[addr] >> 4
                vec = 10 if line == 0xA else 11 if line == 0xF else 4
                i = Insn(addr, 2, "exception", "", [Op("imm", val=vec)])
            self._cache[addr] = i
        return i

    def code_refs(self):
        """Code labels the program uses as values: function-pointer candidates.

        Returns {address: set of reasons} where a reason is "data" (dc.l Label,
        dc.w Label-Base) or "imm" (move.l #Label / lea Label / pea Label).
        """
        refs = defaultdict(set)
        ident = re.compile(r"[A-Za-z_.@][\w.@]*")
        for i in self.listed.values():
            if i.cpu != "68000":
                continue
            if i.mnem.startswith("dc."):
                for item in i.ops.split(","):
                    m = re.fullmatch(r"\s*([A-Za-z_.@][\w.@]*)\s*(?:-\s*[A-Za-z_.@][\w.@]*)?\s*", item)
                    if m:
                        a = self.labels.get(m.group(1).lower())
                        if a in self.listed_code:
                            refs[a].add("data")
                continue
            if i.mnem in ("lea", "pea") or (i.mnem.startswith("move") and "#" in i.ops):
                for name in ident.findall(i.ops.split(",")[0]):
                    a = self.labels.get(name.lower())
                    if a in self.listed_code:
                        refs[a].add("imm")
        return refs

    def vectors(self):
        """Handler addresses in ROM from the 68000 vector table (vector 0 is the stack)."""
        return [a for a in self.all_vectors() if a < len(self.rom)]

    def all_vectors(self):
        return [self.long(4 * v) & 0xFFFFFF for v in range(1, 64)]

    def ram_vectors(self):
        """Vectors pointing into RAM. PSIV's HInt vector is $FFECB0 (HInt_Jump): the
        game writes `jmp xxx` there, with the target at vector+2 (HInt_Addr)."""
        return sorted({self.long(4 * v) for v in range(1, 64) if self.long(4 * v) & 0xFFFFFF >= len(self.rom)})

    def flow(self, i):
        m, nxt = i.mnem, i.addr + i.length
        if m == "bra":
            return Flow("jump", [i.ops[0].val])
        if m in ("bcc", "dbcc"):
            return Flow("cond", [i.ops[-1].val], nxt)
        if m == "bsr":
            return Flow("call", [i.ops[0].val], nxt)
        if m in ("jsr", "jmp"):
            o = i.ops[0]
            if o.kind in DIRECT_EA:
                t = o.val & 0xFFFFFF
                return Flow("call", [t], nxt) if m == "jsr" else Flow("jump", [t])
            ts = list(self.targets.get(i.addr, []))
            return Flow("icall", ts, nxt) if m == "jsr" else Flow("ijump", ts)
        if m in ("rts", "rte", "rtr"):
            return Flow("ret")
        if m == "trap":
            handler = self.long(4 * (32 + i.ops[0].val)) & 0xFFFFFF
            ts = [handler] + list(self.targets.get(i.addr, []))   # trap #2: per-caller targets
            return Flow("call", ts, nxt)
        if m in ("illegal", "exception"):
            vec = 4 if m == "illegal" else i.ops[0].val
            return Flow("call", [self.long(4 * vec) & 0xFFFFFF])   # handler; no return
        return Flow("seq", [], nxt)

    # ------------------------------------------------------------ discovery
    def discover(self, extra_entries=()):
        """Walk everything reachable from the vectors (and extra entries).

        Returns (reached instruction addresses, function entry set, errors).
        """
        entries = set(self.vectors()) | set(extra_entries)
        reached, errors = set(), []
        work = deque(entries)
        while work:
            a = work.popleft()
            if a in reached or a >= len(self.rom):
                continue
            try:
                i = self.insn(a)
            except DecodeError as e:
                errors.append((a, str(e)))
                continue
            reached.add(a)
            f = self.flow(i)
            if f.kind in ("call", "icall"):
                for t in f.targets:
                    if t not in entries:
                        entries.add(t)
                    work.append(t)
            else:
                work.extend(f.targets)
            if f.fall is not None:
                work.append(f.fall)
        return reached, entries, errors

    def functions(self, entries):
        """Body of each entry: code reachable without calls, stopping at other entries."""
        funcs = {}
        for e in sorted(x for x in entries if x < len(self.rom)):
            fn = Function(e, self.name(e))
            work = deque([e])
            while work:
                a = work.popleft()
                if a in fn.body:
                    continue
                if a != e and a in entries:
                    fn.tails.add(a)          # jump or fall-through into another function
                    continue
                if a >= len(self.rom):
                    continue
                try:
                    i = self.insn(a)
                except DecodeError:
                    continue
                fn.body.add(a)
                f = self.flow(i)
                if f.kind in ("call", "icall"):
                    fn.calls.update(f.targets)
                else:
                    work.extend(f.targets)
                if f.fall is not None:
                    work.append(f.fall)
            funcs[e] = fn
        return funcs

    def predecessors(self, reached):
        """Instruction-level predecessor edges for code in `reached`.

        preds[a] = [(p, kind)] with kind "fall" (p falls through to a),
        "branch" (p jumps/branches to a) or "return" (a follows the call at p).
        """
        preds = defaultdict(list)
        for p in reached:
            f = self.flow(self.insn(p))
            if f.kind in ("call", "icall"):
                preds[f.fall].append((p, "return"))
                continue
            for t in f.targets:
                preds[t].append((p, "branch"))
            if f.fall is not None:
                preds[f.fall].append((p, "fall"))
        return preds
