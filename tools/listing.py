"""Parser for the assembler listing (build/disasm/ps4.lst).

Every analysis/recompiler tool reads the program through this module, so the
column quirks live in one place:

  "     646/     64E : 4EFB 0002           \tjmp\tloc_652(pc,d0.w)"
   ^line no ^address  ^byte field (20 cols) ^source text

The prefix widens by one column once line numbers reach 6 digits. The two
assemblers handle that differently: asl 1.42 (Linux) shrinks the byte field so
source text stays at column 40; the older asw.exe bundled for Windows keeps the
field at 20 columns, pushing the source to column 41. Getting this wrong
silently truncates every source line past line 99,999, so detect it.
"""
import re
from dataclasses import dataclass, field
from pathlib import Path

LINE_RE = re.compile(r"^(?:\(\d+\))?\s*(\d+)/\s*([0-9A-F]+) : ")
LABEL_RE = re.compile(r"^([A-Za-z_.@][\w.@]*):")
INSN_RE = re.compile(r"^\s+([a-zA-Z][\w.]*)\s*(.*?)\s*(?:;.*)?$")
FIELD = 20
MIN_SRC_COL = 40


@dataclass
class Insn:
    addr: int
    mnem: str            # lower-case, with size suffix ("move.w")
    ops: str             # operand text as written in the source
    lst: int             # 1-based line in the listing
    labels: list = field(default_factory=list)   # labels defined right before
    hexbytes: str = ""   # bytes the listing shows on this line (may be partial)
    next: int = 0        # address of the following listed instruction


def _detect_padded(lines) -> bool:
    """True if the byte field stays 20 wide on 6-digit lines (asw.exe style)."""
    votes = 0
    for raw in lines:
        m = LINE_RE.match(raw)
        if not m or m.end() <= MIN_SRC_COL - FIELD or len(raw) <= m.end() + FIELD:
            continue
        votes += 1 if raw[MIN_SRC_COL] == " " else -1
        if abs(votes) >= 50:
            break
    return votes > 0


def parse(path: Path):
    """Returns (insns, labels, label_name).

    insns      : list[Insn] for every line that emitted bytes, in address order
    labels     : lower-case label name -> address
    label_name : address -> first label name seen at that address
    """
    lines = path.read_text(encoding="latin-1").splitlines()
    padded = _detect_padded(lines)
    insns, labels, label_name, pending = [], {}, {}, []
    for n, raw in enumerate(lines, 1):
        if raw.startswith("  Symbol Table"):
            break
        m = LINE_RE.match(raw)
        if not m:
            continue
        addr = int(m.group(2), 16)
        col = m.end() + FIELD if padded else max(MIN_SRC_COL, m.end())
        fld, src = raw[m.end():col], raw[col:]
        lm = LABEL_RE.match(src)
        if lm and not fld.strip():
            name = lm.group(1)
            labels[name.lower()] = addr
            label_name.setdefault(addr, name)
            pending.append(name)
            src = src[lm.end():]
            if not src.strip():
                continue
        if not fld.strip() or fld.lstrip().startswith("="):
            continue                      # no bytes emitted (comment, equ, false IF)
        im = INSN_RE.match(src)
        if not im:
            continue
        insns.append(Insn(addr=addr, mnem=im.group(1).lower(), ops=im.group(2).strip(),
                          lst=n, labels=pending, hexbytes=fld.strip()))
        pending = []
    for i, ins in enumerate(insns):
        ins.next = insns[i + 1].addr if i + 1 < len(insns) else ins.addr
    return insns, labels, label_name
