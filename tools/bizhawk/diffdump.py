#!/usr/bin/env python3
"""Diff a BizHawk reference dump against a port dump, naming RAM addresses.

    python tools/bizhawk/diffdump.py build/bizhawk/newgame build/bizhawk/newgame.port386

Each prefix has .ram.bin/.vram.bin/.cram.bin/.vsram.bin (from record.lua's
PSIV_DUMP_AT and psiv_headless --replay / --dump-at). RAM differences are
grouped into runs and labelled with the nearest name from ps4.constants.asm.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CONSTS = ROOT / "disasm" / "ps4disasm" / "ps4.constants.asm"


def ram_names():
    names = {}
    for line in CONSTS.read_text(encoding="latin-1").splitlines():
        m = re.match(r"^(\w+)\s*=\s*ramaddr\(\$FFFF([0-9A-F]{4})\)", line)
        if m:
            names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def main():
    ref, port = sys.argv[1], sys.argv[2]
    sp = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    names = ram_names()
    keys = sorted(names)

    def nm(a):
        best = None
        for k in keys:
            if k <= a:
                best = k
            else:
                break
        return f"{names[best]}+${a - best:X}" if best is not None and a - best < 0x100 else "?"

    for region, size in (("ram", 0x10000), ("vram", 0x10000), ("cram", 128), ("vsram", 80)):
        a = Path(f"{ref}.{region}.bin").read_bytes()[:size]
        b = Path(f"{port}.{region}.bin").read_bytes()[:size]
        # dead stack: $4800 up to sp ($4000-$47FF are game variables)
        diff = [i for i in range(size) if a[i] != b[i] and not (region == "ram" and sp and 0x4800 <= i < sp)]
        print(f"{region}: {len(diff)} differing bytes")
        runs = []
        for i in diff:
            if runs and i - runs[-1][1] <= 3:
                runs[-1][1] = i
            else:
                runs.append([i, i])
        for s, e in runs[:40]:
            label = nm(s) if region == "ram" else ""
            print(f"  ${s:04X}-${e:04X} ({e - s + 1:4d})  {label:36s} ref {a[s:e + 1][:12].hex()}  port {b[s:e + 1][:12].hex()}")


if __name__ == "__main__":
    main()
