#!/usr/bin/env python3
"""Compare sound-chip *state* between the port and a VGM rip, frame by frame.

    python tools/vgmstate.py "vgm/01 - The End of the Millennium.vgm" build/audio/title.ymlog [skip_seconds]

Write streams differ legitimately: VGM tools strip writes that change nothing
(a PSG volume rewritten with the same value, a key-off for a channel already
off). Chip state does not: if every audible register holds the same value at
every frame, the chips produce the same sound. This replays both logs into a
model of the YM2612 registers (minus the DAC sample stream), the per-channel
key-on bits and the PSG's tone/noise/volume registers, aligns the two on the
first note-frequency write, and samples both every 1/60 s.
"""
import sys

from vgmcompare import music_only, read_port, read_vgm

FRAME = 1 / 59.922751       # the console's (and the port's) frame
RIP_FRAME = 1 / 60.0         # these rips tick at exactly 735 samples of 44.1 kHz


class Chips:
    """Register state as a dict; keys never written are absent."""

    def __init__(self):
        self.state = {}
        self.latch = None

    def apply(self, chip, reg, val):
        s = self.state
        if chip == "psg":
            if val & 0x80:
                self.latch = (val >> 4) & 7
            elif self.latch is None:
                return                # a data byte before any latch: target unknown in this log
            r = ("psg", self.latch)
            if self.latch & 1 or self.latch == 6:            # volume / noise: 4 bits
                s[r] = val & 0x0F
            elif val & 0x80:                                  # tone low 4 bits
                s[r] = (s.get(r, 0) & 0x3F0) | (val & 0x0F)
            else:                                             # tone high 6 bits
                s[r] = (s.get(r, 0) & 0x0F) | ((val & 0x3F) << 4)
            return
        if chip == "ym0" and reg in (0x2A, 0x2B, 0x24, 0x25, 0x26, 0x27):
            return                    # DAC stream (checked by ear) and timers (never read)
        if chip == "ym0" and reg == 0x28:
            s[("key", val & 7)] = val >> 4
            return
        s[(chip, reg)] = val

    def snapshot(self):
        return dict(self.state)


def same(rip, port):
    """Registers the rip has set must hold the same value in the port."""
    return all(port.get(k) == v for k, v in rip.items())


def frames(writes, t_first, n, frame, count_from=None):
    """State sampled each frame from t_first. Writes before count_from (default
    t_first) are not counted: rips may start with a dump of whatever the chips
    held, and the port log holds the previous song; only what this song writes
    is compared."""
    chips, out, i = Chips(), [], 0
    count_from = t_first if count_from is None else count_from
    while i < len(writes) and writes[i][0] < count_from:
        if writes[i][1] == "psg" and writes[i][3] & 0x80:
            chips.latch = (writes[i][3] >> 4) & 7     # keep the PSG latch: data bytes depend on it
        i += 1
    for k in range(n):
        t = t_first + (k + 0.5) * frame          # mid-frame: after that frame's writes
        while i < len(writes) and writes[i][0] <= t:
            chips.apply(writes[i][1], writes[i][2], writes[i][3])
            i += 1
        out.append(chips.snapshot())
    return out


NOTE_REGS = (0xA0, 0xA1, 0xA2, 0xA4, 0xA5, 0xA6)


def first_note(writes):
    return next(w[0] for w in writes if w[1] == "ym0" and w[2] in NOTE_REGS)


def align(vgm, port, skip=0.0, run=12):
    """(rip time, port time) of the same point in the song.

    Many rips open with a register dump at time 0 (the trim point's chip
    state, every register in order); that is not the song, so it is skipped.
    The rip's first `run` note-frequency writes after it are then looked up as
    a sequence in the port's log."""
    dump_end = dump_time(vgm)
    rn = [w for w in music_only(vgm) if w[0] > dump_end and w[1] in ("ym0", "ym1") and w[2] in NOTE_REGS]
    pn = [w for w in music_only(port) if w[0] >= skip and w[1] in ("ym0", "ym1") and w[2] in NOTE_REGS]
    want = [(w[1], w[2], w[3]) for w in rn[:run]]
    keys = [(w[1], w[2], w[3]) for w in pn]
    for j in range(len(keys) - run):
        if keys[j:j + run] == want:
            return rn[0][0], pn[j][0]
    return None


def dump_time(vgm):
    """0.0 if the rip opens with a register dump at time 0, else -1."""
    return 0.0 if sum(1 for w in vgm if w[0] == 0.0) > 100 else -1.0


def main():
    vgm = read_vgm(sys.argv[1])
    port = read_port(sys.argv[2])
    # optional third argument: look for the song's first note only from this
    # many seconds into the port log (to skip an earlier song); all writes
    # still count toward chip state
    skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    pair = align(vgm, port, skip)
    if pair is None:
        print("the rip's opening notes do not occur in the port log")
        return 1
    tv, tp = pair
    print(f"aligned: rip {tv:.3f}s = port {tp:.3f}s")
    n = int(min((vgm[-1][0] - tv) / RIP_FRAME, (port[-1][0] - tp) / FRAME)) - 4
    # frame k of the rip against frame k of the port, each at its own frame rate,
    # starting a few frames before the first note so setup writes are included
    dump = dump_time(vgm)
    a = frames(vgm, tv - 3 * RIP_FRAME, n, RIP_FRAME,
               count_from=max(tv - 3 * RIP_FRAME, dump + 1e-9 if dump >= 0 else tv - 3 * RIP_FRAME))
    b = frames(port, tp - 3 * FRAME, n, FRAME)
    bad = [k for k in range(n) if not same(a[k], b[k])]
    print(f"{n} frames compared ({n * FRAME:.1f}s from the first note)")
    print(f"identical chip state on {n - len(bad)}/{n} frames ({100 * (n - len(bad)) / n:.1f}%)")
    if bad:
        k = bad[0]
        print(f"first difference at frame {k} ({k * FRAME:.2f}s):")
        diffs = [(key, v, b[k].get(key)) for key, v in sorted(a[k].items(), key=str) if b[k].get(key) != v]
        for key, v, p in diffs[:12]:
            print(f"  {key}: rip {v}  port {p}")
        if len(diffs) > 12:
            print(f"  ... {len(diffs) - 12} more registers")
        runs, start = [], bad[0]
        for x, y in zip(bad, bad[1:] + [None]):
            if y != x + 1:
                runs.append((start, x))
                start = y
        print(f"differing frames form {len(runs)} runs; first few: {runs[:8]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
