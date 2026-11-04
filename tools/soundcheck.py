#!/usr/bin/env python3
"""Check every song: against BizHawk (exact) or the VGM rips (by state).

    python tools/soundcheck.py --bizhawk [--render]    every write, frame by frame, vs BizHawk
    python tools/soundcheck.py [--render]              chip state vs the rips in vgm/

--render plays each music ID ($81-$B4) through psiv_headless's jukebox
(--music) and logs the chip writes to build/audio/ids/<ID>.ymlog.

--bizhawk runs tools/bizhawk/jukebox.lua in EmuHawk, which plays the same IDs
from the same boot state and logs the 68000's YM2612/PSG writes; the port must
make the same writes on the same frames, in the same order. This is the
definitive check: same machine state, no rip artifacts.

Without --bizhawk, every rip is matched to the ID whose notes it shares and
the chip state is compared frame by frame (tools/vgmstate.py). Rips carry
artifacts (register dumps at the start and at loop points, leftover values on
silent channels), so a low score there needs a look, not an alarm.
"""
import argparse
import glob
import os
import subprocess
import sys
from pathlib import Path

from vgmcompare import read_port, read_vgm
from vgmstate import FRAME, RIP_FRAME, align, dump_time, frames, same

ROOT = Path(__file__).resolve().parent.parent
IDS = ROOT / "build" / "audio" / "ids"
HEADLESS = ROOT / "build" / ("native-Release/psiv_headless.exe" if os.name == "nt" else "native/psiv_headless")
REQUEST_FRAME = 30


def render(seconds):
    IDS.mkdir(parents=True, exist_ok=True)
    n = int(seconds * 60) + REQUEST_FRAME + 60
    for i in range(0x81, 0xB5):
        log = IDS / f"{i:02X}.ymlog"
        subprocess.run([str(HEADLESS), "--frames", str(n), "--music", f"{i:02X}:{REQUEST_FRAME}",
                        "--ymlog", str(log)], cwd=ROOT, check=True, capture_output=True)
        print(f"\rrendered ${i:02X}", end="", flush=True)
    print()


def compare(vgm, port, skip):
    pair = align(vgm, port, skip)
    if pair is None:
        return None
    tv, tp = pair
    n = int(min((vgm[-1][0] - tv) / RIP_FRAME, (port[-1][0] - tp) / FRAME)) - 4
    if n < 30:
        return None
    dump = dump_time(vgm)
    start = tv - 3 * RIP_FRAME
    a = frames(vgm, start, n, RIP_FRAME, count_from=max(start, dump + 1e-9) if dump >= 0 else start)
    b = frames(port, tp - 3 * FRAME, n, FRAME)
    ok = sum(1 for k in range(n) if same(a[k], b[k]))
    return ok, n


BIZHAWK = Path(os.environ.get("PSIV_BIZHAWK", r"H:\Emulation\08 - Tools\BizHawk\EmuHawk.exe"))
JUKE_FRAMES = 1800
SAMPLES_PER_FRAME = 53267 / 59.922751


def ticks_bizhawk(path):
    """{tick: [(chip, reg, value)]} from jukebox.lua ("tick chip reg value")."""
    out = {}
    for line in Path(path).read_text().splitlines():
        p = line.split()
        w = ("psg", None, int(p[2], 16)) if p[1] == "psg" else (p[1], int(p[2], 16), int(p[3], 16))
        out.setdefault(int(p[0]), []).append(w)
    return out


def ticks_port(path, from_sample):
    """{tick: [(chip, reg, value)]} for the 68000's writes in a --ymlog log
    ("sample chip reg value tick"; Z80 writes are tagged z... and skipped)."""
    out = {}
    for line in Path(path).read_text().splitlines():
        p = line.split()
        if p[1].startswith("z") or int(p[0]) < from_sample:
            continue
        if p[1] == "psg":
            w, tick = ("psg", None, int(p[2], 16)), int(p[3])
        else:
            w, tick = (p[1], int(p[2], 16), int(p[3], 16)), int(p[4])
        out.setdefault(tick, []).append(w)
    return out


def song_ticks(ticks):
    """Ticks from the song's first instrument load on, renumbered from 0.
    The log opens with the stop-all tick (which also silences operators via
    $40-$4F) and silent ticks; the two harnesses deliver the request a tick
    apart, so align on the first algorithm/feedback write ($B0-$B6), which
    only loading an instrument does."""
    order = sorted(ticks)
    setup = lambda ws: any(c != "psg" and 0xB0 <= r <= 0xB6 for c, r, _ in ws)
    first = next((t for t in order if setup(ticks[t])), None)
    if first is None:
        # no FM instrument (e.g. $AD, Explosion): the first tick after the stop
        # that is not the driver's idle pattern (27=00 twice, then key-offs)
        idle = [("ym0", 0x27, 0), ("ym0", 0x27, 0)]
        first = next((t for t in order[1:] if ticks[t] and ticks[t][:2] != idle), None)
    if first is None:
        return []
    # both logs close between frames, so the last tick is complete
    return [ticks.get(t, []) for t in range(first, order[-1] + 1)]


def check_bizhawk():
    out = ROOT / "build" / "bizhawk"
    out.mkdir(parents=True, exist_ok=True)
    rom = out / "psiv.md"
    if not rom.exists():
        rom.write_bytes((ROOT / "rom" / "psiv.bin").read_bytes())
    err = out / "error.txt"
    err.unlink(missing_ok=True)
    env = dict(os.environ, PSIV_ROOT=str(ROOT), PSIV_JUKE_FRAMES=str(JUKE_FRAMES))
    print("BizHawk: playing all 52 music IDs (the window closes itself)...", flush=True)
    p = subprocess.Popen([str(BIZHAWK), f"--lua={ROOT / 'tools' / 'bizhawk' / 'jukebox.lua'}", str(rom)], env=env)
    try:
        p.wait(timeout=1800)
    except subprocess.TimeoutExpired:
        p.kill()
        raise SystemExit("BizHawk did not finish within 30 minutes; closed it")
    if err.exists():
        raise SystemExit("BizHawk script error: " + err.read_text())

    exact = 0
    fmt = lambda w: f"{w[0]} {'' if w[1] is None else f'{w[1]:02X}='}{w[2]:02X}"
    for i in range(0x81, 0xB5):
        sid = f"{i:02X}"
        ref = song_ticks(ticks_bizhawk(out / f"juke_{sid}.log"))
        port = song_ticks(ticks_port(IDS / f"{sid}.ymlog", int(REQUEST_FRAME * SAMPLES_PER_FRAME)))
        if not ref or not port:
            print(f"${sid}: no song found ({'BizHawk' if not ref else 'port'})")
            continue
        n = min(len(ref), len(port))
        writes = sum(len(ref[t]) for t in range(n))
        bad = next((t for t in range(n) if ref[t] != port[t]), None)
        if bad is None:
            exact += 1
            print(f"${sid}: identical, {writes} writes over {n} ticks")
        else:
            a, b = ref[bad], port[bad]
            k = next((j for j in range(min(len(a), len(b))) if a[j] != b[j]), min(len(a), len(b)))
            print(f"${sid}: differs at tick {bad}/{n}, write {k}: "
                  f"BizHawk {fmt(a[k]) if k < len(a) else '(end)'} | port {fmt(b[k]) if k < len(b) else '(end)'}")
    print(f"\n{exact}/52 music IDs: every 68000 sound write identical to BizHawk, tick by tick")
    return 0 if exact == 52 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--render", action="store_true")
    ap.add_argument("--bizhawk", action="store_true")
    ap.add_argument("--seconds", type=float, default=40)
    args = ap.parse_args()
    if args.render:
        render(args.seconds)
    if args.bizhawk:
        return check_bizhawk()

    skip = (REQUEST_FRAME + 2) * FRAME
    ports = {p.stem: read_port(p) for p in sorted(IDS.glob("*.ymlog"))}
    total_ok = total_n = 0
    unmatched = []
    for f in sorted(glob.glob(str(ROOT / "vgm" / "*.vgm"))):
        name = Path(f).stem
        vgm = read_vgm(f)
        best = None
        for sid, port in ports.items():
            r = compare(vgm, port, skip)
            if r and (best is None or r[0] / r[1] > best[1][0] / best[1][1]):
                best = (sid, r)
        if best is None:
            unmatched.append(name)
            print(f"{name:34s}  no ID plays these notes")
            continue
        sid, (ok, n) = best
        total_ok += ok
        total_n += n
        flag = "" if ok == n else "  <--"
        print(f"{name:34s}  ${sid}  {ok:5d}/{n:5d} frames identical ({100 * ok / n:5.1f}%){flag}")
    if total_n:
        print(f"\noverall: {total_ok}/{total_n} frames identical ({100 * total_ok / total_n:.2f}%)")
    if unmatched:
        print(f"unmatched rips: {', '.join(unmatched)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
