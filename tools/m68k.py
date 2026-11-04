"""Motorola 68000 instruction decoder.

Decodes one instruction from ROM bytes into a structured form the recompiler
emits C from, and formats it back as assembly in the syntax the disassembly
uses (asl/asw). tools/validate_decoder.py proves the decoder by re-assembling
that text for every instruction in the game and comparing the bytes.

Only the plain 68000 instruction set is decoded (no 68010+ forms).
"""
from dataclasses import dataclass, field
import struct

COND = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
        "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]
SIZES = {0: "b", 1: "w", 2: "l"}


class DecodeError(Exception):
    pass


@dataclass
class Op:
    """One operand.

    kind: dn an ind postinc predec disp idx absw absl pcdisp pcidx imm
          sr ccr usp reglist target
    """
    kind: str
    reg: int = 0          # register number (dn/an/ind/postinc/predec/disp/idx)
    val: int = 0          # displacement, address, immediate, target or reglist mask
    xreg: str = ""        # index register for idx/pcidx, e.g. "d0"
    xsize: str = "w"      # index register size
    extpos: int = -1      # ROM address of this operand's displacement word (disp mode)

    def fmt(self) -> str:
        k = self.kind
        if k == "dn":
            return f"d{self.reg}"
        if k == "an":
            return areg(self.reg)
        if k == "ind":
            return f"({areg(self.reg)})"
        if k == "postinc":
            return f"({areg(self.reg)})+"
        if k == "predec":
            return f"-({areg(self.reg)})"
        if k == "disp":
            return f"{shex(self.val)}({areg(self.reg)})"
        if k == "idx":
            d = shex(self.val) if self.val else ""
            return f"{d}({areg(self.reg)},{self.xreg}.{self.xsize})"
        if k == "absw":
            return f"(${self.val & 0xFFFFFFFF:X}).w"
        if k == "absl":
            return f"(${self.val:X}).l"
        if k == "pcdisp":
            return f"${self.val:X}(pc)"
        if k == "pcidx":
            return f"${self.val:X}(pc,{self.xreg}.{self.xsize})"
        if k == "imm":
            return f"#{shex(self.val)}"
        if k in ("sr", "ccr", "usp"):
            return k
        if k == "reglist":
            return fmt_reglist(self.val)
        if k == "target":
            return f"${self.val:X}"
        raise ValueError(k)


@dataclass
class Insn:
    addr: int
    length: int
    mnem: str                 # base mnemonic, e.g. "move", "addq", "bcc", "dbcc", "scc"
    size: str = ""            # "b", "w", "l", "s" (short branch) or ""
    ops: list = field(default_factory=list)
    cond: str = ""            # condition for bcc/dbcc/scc

    @property
    def name(self) -> str:
        """Mnemonic as written in assembly (condition filled in)."""
        if self.mnem in ("bcc", "dbcc", "scc"):
            return self.mnem[:-2] + self.cond
        return self.mnem

    def text(self) -> str:
        n = self.name + (f".{self.size}" if self.size else "")
        return n + ("\t" + ", ".join(o.fmt() for o in self.ops) if self.ops else "")


def areg(n: int) -> str:
    return "sp" if n == 7 else f"a{n}"


def shex(v: int) -> str:
    return f"-${-v:X}" if v < 0 else f"${v:X}"


def fmt_reglist(mask: int) -> str:
    """mask bit 0 = d0 ... bit 15 = a7 (normal order, already un-reversed)."""
    names = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]
    parts, i = [], 0
    while i < 16:
        if mask >> i & 1:
            j = i
            # ranges never cross the d/a boundary
            while j + 1 < 16 and mask >> (j + 1) & 1 and (j + 1) // 8 == i // 8:
                j += 1
            parts.append(names[i] if i == j else f"{names[i]}-{names[j]}")
            i = j + 1
        else:
            i += 1
    return "/".join(parts)


def sx8(v):  return v - 0x100 if v & 0x80 else v
def sx16(v): return v - 0x10000 if v & 0x8000 else v
def sx32(v): return v - 0x100000000 if v & 0x80000000 else v


class Decoder:
    def __init__(self, rom: bytes):
        self.rom = rom

    def w(self, a):
        if a + 2 > len(self.rom):
            raise DecodeError(f"read past end at ${a:X}")
        return struct.unpack_from(">H", self.rom, a)[0]

    def l(self, a):
        return (self.w(a) << 16) | self.w(a + 2)

    # -------------------------------------------------------------- operands
    def ea(self, mode, reg, size, pos):
        """Decode an effective address whose extension words start at pos."""
        if mode == 0:
            return Op("dn", reg), pos
        if mode == 1:
            return Op("an", reg), pos
        if mode == 2:
            return Op("ind", reg), pos
        if mode == 3:
            return Op("postinc", reg), pos
        if mode == 4:
            return Op("predec", reg), pos
        if mode == 5:
            return Op("disp", reg, sx16(self.w(pos)), extpos=pos), pos + 2
        if mode == 6:
            x = self.w(pos)
            return Op("idx", reg, sx8(x & 0xFF), *self.xreg(x)), pos + 2
        if reg == 0:
            return Op("absw", val=sx16(self.w(pos)) & 0xFFFFFFFF), pos + 2
        if reg == 1:
            return Op("absl", val=self.l(pos)), pos + 4
        if reg == 2:
            return Op("pcdisp", val=(pos + sx16(self.w(pos))) & 0xFFFFFF), pos + 2
        if reg == 3:
            x = self.w(pos)
            return Op("pcidx", val=(pos + sx8(x & 0xFF)) & 0xFFFFFF, xreg=self.xreg(x)[0],
                      xsize=self.xreg(x)[1]), pos + 2
        if reg == 4:
            if size == "b":
                return Op("imm", val=self.w(pos) & 0xFF), pos + 2
            if size == "w":
                return Op("imm", val=self.w(pos)), pos + 2
            if size == "l":
                return Op("imm", val=self.l(pos)), pos + 4
            raise DecodeError("immediate without size")
        raise DecodeError(f"bad EA mode 7/{reg}")

    @staticmethod
    def xreg(x):
        if x & 0x0700:
            raise DecodeError("68020 full-format index word")
        return (("a" if x & 0x8000 else "d") + str(x >> 12 & 7)), ("l" if x & 0x0800 else "w")

    # -------------------------------------------------------------- decode
    def decode(self, addr: int) -> Insn:
        op = self.w(addr)
        line = op >> 12
        mode, reg = op >> 3 & 7, op & 7
        hi = op >> 9 & 7
        pos = addr + 2

        def mk(mnem, size="", ops=(), end=None, cond=""):
            return Insn(addr, (end if end is not None else pos) - addr, mnem, size, list(ops), cond)

        if line == 0:
            if op & 0x0100:
                dn = Op("dn", hi)
                if mode == 1:            # movep
                    opm = op >> 6 & 7
                    size = "w" if opm in (4, 6) else "l"
                    mem = Op("disp", reg, sx16(self.w(pos)), extpos=pos)
                    return mk("movep", size, [mem, dn] if opm < 6 else [dn, mem], pos + 2)
                kind = ["btst", "bchg", "bclr", "bset"][op >> 6 & 3]
                dst, end = self.ea(mode, reg, "b", pos)
                return mk(kind, "", [dn, dst], end)
            if hi == 4:                  # static bit ops
                kind = ["btst", "bchg", "bclr", "bset"][op >> 6 & 3]
                bit = self.w(pos) & 0xFF
                dst, end = self.ea(mode, reg, "b", pos + 2)
                return mk(kind, "", [Op("imm", val=bit), dst], end)
            names = {0: "ori", 1: "andi", 2: "subi", 3: "addi", 5: "eori", 6: "cmpi"}
            if hi not in names:
                raise DecodeError(f"line 0 op {op:04X}")
            sz = op >> 6 & 3
            if sz == 3:
                raise DecodeError(f"line 0 size 3 {op:04X}")
            size = SIZES[sz]
            if mode == 7 and reg == 4 and hi in (0, 1, 5):   # to ccr / sr
                tgt = "ccr" if size == "b" else "sr"
                imm = self.w(pos) & (0xFF if size == "b" else 0xFFFF)
                return mk(names[hi], "", [Op("imm", val=imm), Op(tgt)], pos + 2)
            imm, p2 = self.ea(7, 4, size, pos)
            dst, end = self.ea(mode, reg, size, p2)
            return mk(names[hi], size, [imm, dst], end)

        if line in (1, 2, 3):
            size = {1: "b", 3: "w", 2: "l"}[line]
            src, p2 = self.ea(mode, reg, size, pos)
            dmode = op >> 6 & 7
            dst, end = self.ea(dmode, hi, size, p2)
            if dmode == 1:
                if size == "b":
                    raise DecodeError("movea.b")
                return mk("movea", size, [src, dst], end)
            return mk("move", size, [src, dst], end)

        if line == 4:
            return self.line4(addr, op, mode, reg, hi, pos, mk)

        if line == 5:
            if op >> 6 & 3 == 3:
                cond = COND[op >> 8 & 15]
                if mode == 1:
                    tgt = (pos + sx16(self.w(pos))) & 0xFFFFFF
                    return mk("dbcc", "", [Op("dn", reg), Op("target", val=tgt)], pos + 2, cond)
                dst, end = self.ea(mode, reg, "b", pos)
                return mk("scc", "", [dst], end, cond)
            size = SIZES[op >> 6 & 3]
            data = hi or 8
            dst, end = self.ea(mode, reg, size, pos)
            return mk("subq" if op & 0x0100 else "addq", size, [Op("imm", val=data), dst], end)

        if line == 6:
            c = op >> 8 & 15
            d = op & 0xFF
            if d == 0:
                tgt, end, size = (pos + sx16(self.w(pos))) & 0xFFFFFF, pos + 2, "w"
            elif d == 0xFF:
                raise DecodeError("68020 long branch")
            else:
                tgt, end, size = (pos + sx8(d)) & 0xFFFFFF, pos, "s"
            mnem = "bra" if c == 0 else "bsr" if c == 1 else "bcc"
            return mk(mnem, size, [Op("target", val=tgt)], end, "" if c < 2 else COND[c])

        if line == 7:
            if op & 0x0100:
                raise DecodeError(f"line 7 {op:04X}")
            return mk("moveq", "", [Op("imm", val=sx8(op & 0xFF)), Op("dn", hi)])

        if line in (8, 0xC):
            opm = op >> 6 & 7
            if opm in (3, 7):
                name = ("divu" if opm == 3 else "divs") if line == 8 else ("mulu" if opm == 3 else "muls")
                src, end = self.ea(mode, reg, "w", pos)
                return mk(name, "w", [src, Op("dn", hi)], end)
            if op & 0x1F0 == 0x100:          # sbcd / abcd
                name = "sbcd" if line == 8 else "abcd"
                if op & 8:
                    return mk(name, "", [Op("predec", reg), Op("predec", hi)])
                return mk(name, "", [Op("dn", reg), Op("dn", hi)])
            if line == 0xC and op & 0x1F8 in (0x140, 0x148, 0x188):
                f = op & 0x1F8
                if f == 0x140:
                    return mk("exg", "", [Op("dn", hi), Op("dn", reg)])
                if f == 0x148:
                    return mk("exg", "", [Op("an", hi), Op("an", reg)])
                return mk("exg", "", [Op("dn", hi), Op("an", reg)])
            name = "or" if line == 8 else "and"
            size = SIZES[opm & 3]
            ea, end = self.ea(mode, reg, size, pos)
            ops = [ea, Op("dn", hi)] if opm < 4 else [Op("dn", hi), ea]
            return mk(name, size, ops, end)

        if line in (9, 0xD):
            base = "sub" if line == 9 else "add"
            opm = op >> 6 & 7
            if opm in (3, 7):
                size = "w" if opm == 3 else "l"
                src, end = self.ea(mode, reg, size, pos)
                return mk(base + "a", size, [src, Op("an", hi)], end)
            size = SIZES[opm & 3]
            if opm >= 4 and mode in (0, 1):  # addx / subx
                if mode:
                    return mk(base + "x", size, [Op("predec", reg), Op("predec", hi)])
                return mk(base + "x", size, [Op("dn", reg), Op("dn", hi)])
            ea, end = self.ea(mode, reg, size, pos)
            ops = [ea, Op("dn", hi)] if opm < 4 else [Op("dn", hi), ea]
            return mk(base, size, ops, end)

        if line == 0xB:
            opm = op >> 6 & 7
            if opm in (3, 7):
                size = "w" if opm == 3 else "l"
                src, end = self.ea(mode, reg, size, pos)
                return mk("cmpa", size, [src, Op("an", hi)], end)
            size = SIZES[opm & 3]
            if opm < 4:
                src, end = self.ea(mode, reg, size, pos)
                return mk("cmp", size, [src, Op("dn", hi)], end)
            if mode == 1:
                return mk("cmpm", size, [Op("postinc", reg), Op("postinc", hi)])
            dst, end = self.ea(mode, reg, size, pos)
            return mk("eor", size, [Op("dn", hi), dst], end)

        if line == 0xE:
            kinds = ["as", "ls", "rox", "ro"]
            dirn = "l" if op & 0x0100 else "r"
            if op >> 6 & 3 == 3:             # memory shift, word, by one
                kind = kinds[hi & 3]
                if hi & 4:
                    raise DecodeError(f"68020 bitfield {op:04X}")
                dst, end = self.ea(mode, reg, "w", pos)
                return mk(kind + dirn, "w", [dst], end)
            size = SIZES[op >> 6 & 3]
            kind = kinds[op >> 3 & 3]
            cnt = Op("dn", hi) if op & 0x20 else Op("imm", val=hi or 8)
            return mk(kind + dirn, size, [cnt, Op("dn", reg)])

        raise DecodeError(f"line {line:X} op {op:04X} at ${addr:X}")

    def line4(self, addr, op, mode, reg, hi, pos, mk):
        if op & 0x1C0 == 0x1C0:
            src, end = self.ea(mode, reg, "l", pos)
            return mk("lea", "", [src, Op("an", hi)], end)
        if op & 0x1C0 == 0x180:
            src, end = self.ea(mode, reg, "w", pos)
            return mk("chk", "w", [src, Op("dn", hi)], end)
        sz = op >> 6 & 3
        top = op & 0x0F00
        if top in (0x000, 0x200, 0x400, 0x600):
            if sz == 3:
                if top == 0x000:
                    dst, end = self.ea(mode, reg, "w", pos)
                    return mk("move", "w", [Op("sr"), dst], end)
                if top == 0x400:
                    src, end = self.ea(mode, reg, "w", pos)
                    return mk("move", "w", [src, Op("ccr")], end)
                if top == 0x600:
                    src, end = self.ea(mode, reg, "w", pos)
                    return mk("move", "w", [src, Op("sr")], end)
                raise DecodeError(f"move from ccr (68010) {op:04X}")
            name = {0x000: "negx", 0x200: "clr", 0x400: "neg", 0x600: "not"}[top]
            dst, end = self.ea(mode, reg, SIZES[sz], pos)
            return mk(name, SIZES[sz], [dst], end)
        if top == 0x800:
            if sz == 0:
                dst, end = self.ea(mode, reg, "b", pos)
                return mk("nbcd", "", [dst], end)
            if sz == 1:
                if mode == 0:
                    return mk("swap", "", [Op("dn", reg)])
                src, end = self.ea(mode, reg, "l", pos)
                return mk("pea", "", [src], end)
            size = "w" if sz == 2 else "l"
            if mode == 0:
                return mk("ext", size, [Op("dn", reg)])
            mask = self.w(pos)
            dst, end = self.ea(mode, reg, size, pos + 2)
            if mode == 4:                    # -(An): mask is bit-reversed
                mask = int(f"{mask:016b}"[::-1], 2)
            return mk("movem", size, [Op("reglist", val=mask), dst], end)
        if top == 0xA00:
            if op == 0x4AFC:
                return mk("illegal")
            if sz == 3:
                dst, end = self.ea(mode, reg, "b", pos)
                return mk("tas", "", [dst], end)
            dst, end = self.ea(mode, reg, SIZES[sz], pos)
            return mk("tst", SIZES[sz], [dst], end)
        if top == 0xC00 and sz >= 2:
            size = "w" if sz == 2 else "l"
            mask = self.w(pos)
            src, end = self.ea(mode, reg, size, pos + 2)
            return mk("movem", size, [src, Op("reglist", val=mask)], end)
        if top == 0xE00:
            if op & 0xFFF0 == 0x4E40:
                return mk("trap", "", [Op("imm", val=op & 15)])
            if op & 0xFFF8 == 0x4E50:
                return mk("link", "", [Op("an", reg), Op("imm", val=sx16(self.w(pos)))], pos + 2)
            if op & 0xFFF8 == 0x4E58:
                return mk("unlk", "", [Op("an", reg)])
            if op & 0xFFF8 == 0x4E60:
                return mk("move", "l", [Op("an", reg), Op("usp")])
            if op & 0xFFF8 == 0x4E68:
                return mk("move", "l", [Op("usp"), Op("an", reg)])
            simple = {0x4E70: "reset", 0x4E71: "nop", 0x4E73: "rte", 0x4E75: "rts",
                      0x4E76: "trapv", 0x4E77: "rtr"}
            if op in simple:
                return mk(simple[op])
            if op == 0x4E72:
                return mk("stop", "", [Op("imm", val=self.w(pos))], pos + 2)
            if op & 0xFFC0 in (0x4E80, 0x4EC0):
                tgt, end = self.ea(mode, reg, "l", pos)
                return mk("jsr" if op & 0x40 == 0 else "jmp", "", [tgt], end)
        raise DecodeError(f"line 4 op {op:04X} at ${addr:X}")
