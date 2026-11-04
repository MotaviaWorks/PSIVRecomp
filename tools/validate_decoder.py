#!/usr/bin/env python3
"""Prove tools/m68k.py against every 68000 instruction in the game.

For each instruction the listing shows, decode it from the ROM bytes and check:
  1. length   - matches the listing (next listed address minus this one)
  2. mnemonic - matches the source, allowing assembler synonyms (dbra = dbf, ...)
  3. bytes    - the decoder's own text, re-assembled with the same assembler,
                reproduces the ROM bytes exactly. This is the strict check: a
                wrong register, size, mode, displacement or immediate fails it.

Exit status 0 only if all three pass for every instruction.
"""
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

import listing
from m68k import Decoder, DecodeError

ROOT = Path(__file__).resolve().parent.parent
DIS = ROOT / "disasm" / "ps4disasm"
LST = ROOT / "build" / "disasm" / "ps4.lst"
ROM = ROOT / "rom" / "psiv.bin"
WORK = ROOT / "build" / "decoder"

DATA = re.compile(r"^(dc|ds|dcb)(\.|$)|^(binclude|align|even|cnop)$")

# Spellings the source may use for the same encoding the decoder names.
SYNONYMS = {
    "dbra": "dbf", "bhs": "bcc", "blo": "bcs", "dbhs": "dbcc", "dblo": "dbcs",
    "shs": "scc", "slo": "scs", "move": "move", "illegal": "illegal",
}
# Source mnemonics the assembler turns into a specific form (e.g. "add #1" -> addi).
FAMILY = {
    "movea": "move", "adda": "add", "addi": "add", "addq": "add",
    "suba": "sub", "subi": "sub", "subq": "sub",
    "cmpa": "cmp", "cmpi": "cmp", "cmpm": "cmp",
    "andi": "and", "ori": "or", "eori": "eor",
}


def base(m: str) -> str:
    m = m.split(".")[0]
    return SYNONYMS.get(m, m)


def main() -> int:
    rom = ROM.read_bytes()
    dec = Decoder(rom)
    insns, _, _ = listing.parse(LST)
    code = [i for i in insns if i.cpu == "68000" and not DATA.match(i.mnem)]
    z80 = sum(1 for i in insns if i.cpu == "z80")
    print(f"{len(insns)} listed lines with bytes, {len(code)} 68000 instructions "
          f"({z80} Z80 lines skipped)")

    decoded, fails = [], Counter()
    examples = {}

    def fail(kind, ins, msg):
        fails[kind] += 1
        examples.setdefault(kind, []).append(f"  ${ins.addr:06X} lst:{ins.lst}  {ins.mnem} {ins.ops}  ->  {msg}")

    for k, ins in enumerate(code):
        try:
            d = dec.decode(ins.addr)
        except DecodeError as e:
            fail("decode", ins, str(e))
            continue
        decoded.append(d)
        # 1. length: only meaningful when the next listed line is contiguous
        gap = ins.next - ins.addr
        if 0 < gap <= 10 and gap != d.length:
            fail("length", ins, f"decoded {d.length}, listing {gap}  [{d.text()}]")
        # 2. mnemonic
        src, got = base(ins.mnem), base(d.name)
        if src != got and FAMILY.get(got, got) != FAMILY.get(src, src):
            fail("mnemonic", ins, d.text())

    # 3. bytes: re-assemble the decoder's text at the original addresses
    WORK.mkdir(parents=True, exist_ok=True)
    # asl rewrites a zero displacement "0(a4)" as "(a4)", a different encoding.
    # Assemble those as 1(a4) and patch the word back after checking it.
    asm = ["\tcpu 68000", "\tsupmode on", "\tpadding off"]
    zero_disp = []
    for d in decoded:
        zeros = [o for o in d.ops if o.kind == "disp" and o.val == 0]
        for o in zeros:
            o.val = 1
        asm.append(f"\torg ${d.addr:X}")
        asm.append("\t" + d.text())
        for o in zeros:
            o.val = 0
        zero_disp += zeros
    (WORK / "roundtrip.asm").write_text("\n".join(asm) + "\n")
    for f in ("roundtrip.p", "roundtrip.log", "roundtrip.bin"):
        (WORK / f).unlink(missing_ok=True)
    if os.name == "nt":
        env = dict(os.environ, AS_MSGPATH=str(DIS / "AS/win32"), USEANSI="n")
        asm_cmd = [str(DIS / "AS/win32/asw.exe"), "-xx", "-c", "-E", "-q", "roundtrip.asm"]
        p2bin = [str(DIS / "AS/win32/p2bin.exe")]
    else:
        env = None
        asm_cmd = [shutil.which("asl") or str(Path.home() / ".local/bin/asl"),
                   "-xx", "-c", "-E", "-q", "roundtrip.asm"]
        p2bin = [shutil.which("p2bin") or str(Path.home() / ".local/bin/p2bin")]
    subprocess.run(asm_cmd, cwd=WORK, env=env, capture_output=True, text=True)
    log = WORK / "roundtrip.log"
    if log.exists() and log.read_text(errors="replace").strip():
        errs = log.read_text(errors="replace")
        lines = [l for l in errs.splitlines() if "error" in l.lower()]
        print(f"assembler errors: {len(lines)}")
        for l in lines[:15]:
            print("  ", l)
    if not (WORK / "roundtrip.p").exists():
        print("roundtrip assembly produced no output")
        return 1
    r = subprocess.run(p2bin + ["-r", "$0-$2FFFFF", "-l", "0", "roundtrip.p", "roundtrip.bin"],
                       cwd=WORK, env=env, capture_output=True, text=True)
    out = (WORK / "roundtrip.bin").read_bytes() if (WORK / "roundtrip.bin").exists() else b""
    if not out:
        print("p2bin failed:", r.stdout[-500:], r.stderr[-500:])
        return 1
    out = bytearray(out)
    for o in zero_disp:
        if out[o.extpos:o.extpos + 2] == b"\x00\x01":
            out[o.extpos:o.extpos + 2] = b"\x00\x00"
    for d in decoded:
        a, b = rom[d.addr:d.addr + d.length], bytes(out[d.addr:d.addr + d.length])
        if a != b:
            fails["bytes"] += 1
            examples.setdefault("bytes", []).append(
                f"  ${d.addr:06X}  {d.text():40s} rom {a.hex()} asm {b.hex()}")

    total = len(code)
    for kind in ("decode", "length", "mnemonic", "bytes"):
        print(f"{kind:9s}: {total - fails[kind] if kind != 'bytes' else len(decoded) - fails[kind]}"
              f"/{total if kind != 'bytes' else len(decoded)} ok, {fails[kind]} failed")
        for e in examples.get(kind, [])[:12]:
            print(e)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
