#!/usr/bin/env python3
"""Turn the user's Phantasy Star IV dump into a verified raw ROM.

Accepts a raw dump (.bin/.md/.gen) or an interleaved SMD copier dump (.smd)
and writes rom/psiv.bin. Refuses anything that is not the USA release, since
every address in the recompiler metadata is specific to that build.

    python tools/prepare_rom.py "path/to/your dump.smd"
"""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "rom" / "psiv.bin"

# Phantasy Star IV (USA), GM MK-1307-00, 24 Mbit
EXPECTED_SHA1 = "bc7ff6d6a8408f38562bc610f24645cad6c42629"
EXPECTED_SIZE = 3 * 1024 * 1024
SMD_HEADER = 512
SMD_BLOCK = 16384


def is_smd(data: bytes) -> bool:
    # SMD = 512-byte header + 16 KB blocks; header bytes 8/9 are $AA $BB
    return (len(data) % SMD_BLOCK == SMD_HEADER
            and data[8] == 0xAA and data[9] == 0xBB)


def deinterleave_smd(data: bytes) -> bytes:
    """Each 16 KB block holds the odd bytes in its first half, even bytes in its second."""
    body = data[SMD_HEADER:]
    out = bytearray(len(body))
    half = SMD_BLOCK // 2
    for base in range(0, len(body), SMD_BLOCK):
        block = body[base:base + SMD_BLOCK]
        out[base + 1:base + SMD_BLOCK:2] = block[:half]
        out[base:base + SMD_BLOCK:2] = block[half:]
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    src = Path(sys.argv[1])
    data = src.read_bytes()
    kind = "raw"
    if is_smd(data):
        data = deinterleave_smd(data)
        kind = "SMD (de-interleaved)"
    sha1 = hashlib.sha1(data).hexdigest()
    if len(data) != EXPECTED_SIZE or sha1 != EXPECTED_SHA1:
        print(f"Not the expected Phantasy Star IV (USA) ROM.\n"
              f"  format: {kind}, size {len(data)}, sha1 {sha1}\n"
              f"  wanted: size {EXPECTED_SIZE}, sha1 {EXPECTED_SHA1}")
        return 1
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_bytes(data)
    print(f"OK: {kind} dump verified, wrote {OUT.relative_to(ROOT)} (sha1 {sha1})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
