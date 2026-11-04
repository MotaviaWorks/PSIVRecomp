#!/usr/bin/env python3
"""Reassemble the disassembly and check it is byte-identical to rom/psiv.bin.

Produces build/disasm/ps4.lst (address-annotated listing) and
build/disasm/ps4built.bin. The listing is the input to every analysis and
recompiler tool, so they all see exactly the code that is in the ROM.

Windows uses the assembler bundled in the disassembly (AS/win32/asw.exe);
elsewhere it uses `asl` from PATH or ~/.local/bin.
"""
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIS = ROOT / "disasm" / "ps4disasm"
OUT = ROOT / "build" / "disasm"
ROM = ROOT / "rom" / "psiv.bin"


def run(cmd, **kw):
    r = subprocess.run(cmd, cwd=DIS, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-3000:] + r.stderr[-3000:])
        raise SystemExit(f"failed: {' '.join(map(str, cmd))}")
    return r


def main() -> int:
    if not ROM.exists():
        raise SystemExit("rom/psiv.bin missing: run tools/prepare_rom.py on your dump first")
    OUT.mkdir(parents=True, exist_ok=True)
    for f in ("ps4.p", "ps4.h", "ps4.log", "ps4.lst"):
        (DIS / f).unlink(missing_ok=True)

    if os.name == "nt":
        env = dict(os.environ, AS_MSGPATH="AS/win32", USEANSI="n")
        run([str(DIS / "AS/win32/asw.exe"), "-xx", "-c", "-E", "-A", "-L", "ps4.asm"], env=env)
        p2bin = DIS / "AS/win32/ps4p2bin.exe"
    else:
        asl = shutil.which("asl") or str(Path.home() / ".local/bin/asl")
        # case-sensitive filesystems: the source spells this file with a capital N
        font = DIS / "graphics/font/Font Nemesis.bin"
        if not font.exists():
            font.symlink_to("Font nemesis.bin")
        # newer asl resolves includes relative to the including file; the source expects the root
        run([asl, "-xx", "-c", "-E", "-A", "-L", "-q", "-i", ".", "ps4.asm"])
        p2bin = OUT / "ps4p2bin"
        if not p2bin.exists():
            subprocess.run(["cc", "-O2", "-w", "-o", str(p2bin), str(DIS / "AS/ps4p2bin.c")], check=True)

    if (DIS / "ps4.log").exists():
        sys.stdout.write((DIS / "ps4.log").read_text(errors="replace")[:3000])
        raise SystemExit("assembler reported errors/warnings")
    built = OUT / "ps4built.bin"
    run([str(p2bin), "ps4.p", str(built), "ps4.h"])
    shutil.move(str(DIS / "ps4.lst"), OUT / "ps4.lst")
    for f in ("ps4.p", "ps4.h"):
        (DIS / f).unlink(missing_ok=True)

    got = hashlib.sha1(built.read_bytes()).hexdigest()
    want = hashlib.sha1(ROM.read_bytes()).hexdigest()
    if got != want:
        print(f"MISMATCH: built {got}, rom {want}")
        return 1
    print(f"OK: byte-identical ({got}); listing at {(OUT / 'ps4.lst').relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
