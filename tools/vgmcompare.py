#!/usr/bin/env python3
"""Compare the port's sound-chip writes with a VGM rip.

    python tools/vgmcompare.py "vgm/01 - The End of the Millennium.vgm" build/audio/title.ymlog

The port log comes from `psiv_headless --ymlog` ("sample chip reg value",
sample = index at 53,267 Hz). Both sides are reduced to the music driver's
register writes: YM2612 part I/II writes except the DAC sample stream ($2A)
and PSG writes. The comparison finds where the rip's first writes occur in the
port's log (the port log also has the boot and SEGA screen before the song),
then reports how much of the rip's write sequence the port reproduces in
order, the first point they part ways, and the tempo ratio from matched
writes.
"""
import difflib
import gzip
import struct
import sys
from pathlib import Path

PORT_RATE = 53267
VGM_RATE = 44100


def read_vgm(path):
    data = Path(path).read_bytes()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    if data[:4] != b"Vgm ":
        raise SystemExit(f"{path}: not a VGM file")
    version = struct.unpack_from("<I", data, 8)[0]
    off = struct.unpack_from("<I", data, 0x34)[0] if version >= 0x150 else 0
    pos = 0x34 + off if off else 0x40
    t, out = 0, []
    while pos < len(data):
        c = data[pos]
        if c == 0x66:
            break
        if c in (0x52, 0x53):
            reg, val = data[pos + 1], data[pos + 2]
            out.append((t, f"ym{c - 0x52}", reg, val))
            pos += 3
        elif c == 0x50:
            out.append((t, "psg", None, data[pos + 1]))
            pos += 2
        elif c == 0x61:
            t += struct.unpack_from("<H", data, pos + 1)[0]
            pos += 3
        elif c == 0x62:
            t += 735; pos += 1
        elif c == 0x63:
            t += 882; pos += 1
        elif 0x70 <= c <= 0x7F:
            t += (c & 15) + 1; pos += 1
        elif 0x80 <= c <= 0x8F:                     # DAC write from data bank + wait
            out.append((t, "ym0", 0x2A, None))
            t += c & 15; pos += 1
        elif c == 0x67:                             # data block
            size = struct.unpack_from("<I", data, pos + 3)[0] & 0x7FFFFFFF
            pos += 7 + size
        elif c == 0xE0:
            pos += 5
        elif c == 0x4F:
            pos += 2
        elif 0x90 <= c <= 0x95:                     # DAC stream control
            pos += {0x90: 5, 0x91: 5, 0x92: 6, 0x93: 11, 0x94: 2, 0x95: 5}[c]
        elif 0x30 <= c <= 0x3F:
            pos += 2
        elif 0x40 <= c <= 0x4E or 0x54 <= c <= 0x5F or 0xA0 <= c <= 0xBF:
            pos += 3
        elif 0xC0 <= c <= 0xDF:
            pos += 4
        elif 0xE1 <= c <= 0xFF:
            pos += 5
        else:
            raise SystemExit(f"{path}: unknown VGM command ${c:02X} at {pos:#x}")
    return [(tt / VGM_RATE, chip, reg, val) for tt, chip, reg, val in out]


def read_port(path):
    out = []
    for line in Path(path).read_text().splitlines():
        p = line.split()
        if p[1] == "psg":
            out.append((int(p[0]) / PORT_RATE, "psg", None, int(p[2], 16)))
        else:
            out.append((int(p[0]) / PORT_RATE, p[1], int(p[2], 16), int(p[3], 16)))
    return out


def music_only(writes):
    """Drop the DAC sample stream, and writes that store the value a register
    already holds: VGM tools strip those, so both sides are reduced alike.
    The key on/off register ($28) is always kept: rewriting it re-triggers notes."""
    out, state = [], {}
    for w in writes:
        chip, reg, val = w[1], w[2], w[3]
        if chip == "ym0" and reg in (0x2A, 0x2B):
            continue
        if chip != "psg" and reg != 0x28:
            if state.get((chip, reg)) == val:
                continue
            state[(chip, reg)] = val
        out.append(w)
    return out


def key(w):
    return (w[1], w[2], w[3])


def main():
    vgm = music_only(read_vgm(sys.argv[1]))
    port = music_only(read_port(sys.argv[2]))
    print(f"rip: {len(vgm)} music writes over {vgm[-1][0]:.1f}s; port: {len(port)} over {port[-1][0]:.1f}s")

    # anchor on the longest common run near the start of the song
    keys = [key(w) for w in port]
    head = [key(w) for w in vgm[:600]]
    best = difflib.SequenceMatcher(None, head, keys[:6000], autojunk=False).find_longest_match()
    if best.size < 20:
        print(f"could not find the song in the port log (longest common run {best.size})")
        return 1
    start = max(0, best.b - best.a)
    t0 = port[start][0]
    print(f"song starts at port write #{start} ({t0:.2f}s into the run); "
          f"anchor run of {best.size} writes at rip #{best.a}")

    # compare only the stretch both sides cover
    span_s = port[-1][0] - t0
    a = [key(w) for w in vgm if w[0] - vgm[0][0] <= span_s]
    b = keys[start:]
    print(f"comparing {span_s:.1f}s: {len(a)} rip writes vs {len(b)} port writes")
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    matched = sum(bl.size for bl in sm.get_matching_blocks())
    span = min(len(a), len(b))
    print(f"in-order match: {matched}/{len(a)} rip writes ({100 * matched / len(a):.1f}%)")
    first_diff = None
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "equal":
            first_diff = (tag, i1, i2, j1, j2)
            break
    if first_diff:
        tag, i1, i2, j1, j2 = first_diff
        print(f"first difference at rip write #{i1} ({vgm[i1][0]:.2f}s): {tag}")
        print("  rip :", [f"{c} {r if r is None else f'{r:02X}'}={v if v is None else f'{v:02X}'}" for c, r, v in a[i1:i1 + 6]])
        print("  port:", [f"{c} {r if r is None else f'{r:02X}'}={v if v is None else f'{v:02X}'}" for c, r, v in b[j1:j1 + 6]])
    else:
        print(f"identical for the {span} writes compared")

    # tempo: time between first and last matched write on each side
    blocks = [bl for bl in sm.get_matching_blocks() if bl.size]
    if len(blocks) >= 2:
        i0, j0 = blocks[0].a, blocks[0].b
        last = blocks[-1]
        i1, j1 = last.a + last.size - 1, last.b + last.size - 1
        dv = vgm[i1][0] - vgm[i0][0]
        dp = port[start + j1][0] - port[start + j0][0]
        if dv > 0:
            print(f"timing: rip {dv:.3f}s vs port {dp:.3f}s between the first and last matched write "
                  f"(port/rip = {dp / dv:.4f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
