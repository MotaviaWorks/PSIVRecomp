#!/usr/bin/env python3
"""Scenario authoring helper: base movie + extra moves -> screenshots.

    python tools/movies/try.py BASE.movie "+0 L 60" "+60 D 60" --shots 6900,7000,7100

Writes build/shots/try.movie (base followed by the extra lines), runs
psiv_headless with screenshots at the given logical frames (default: the end),
and prints where they went.
"""
import argparse
import subprocess
import sys
from pathlib import Path

from saves import save_for

ROOT = Path(__file__).resolve().parent.parent.parent
HEADLESS = ROOT / "build" / "native-Release" / "psiv_headless.exe"
OUT = ROOT / "build" / "shots" / "try"


def last_frame(lines):
    prev = last = 0
    for line in lines:
        s = line.split("#")[0].split()
        if len(s) < 2:
            continue
        start = prev + int(s[0][1:]) if s[0].startswith("+") else int(s[0])
        n = int(s[2]) if len(s) > 2 else 1
        prev, last = start, max(last, start + n)
    return last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("moves", nargs="*")
    ap.add_argument("--shots", default="")
    ap.add_argument("--where", action="store_true", help="also print the 68000 call chain at each shot")
    args = ap.parse_args()
    lines = Path(args.base).read_text().splitlines() + args.moves
    OUT.mkdir(parents=True, exist_ok=True)
    movie = OUT / "try.movie"
    movie.write_text("\n".join(lines) + "\n")
    end = last_frame(lines)
    shots = [int(x) for x in args.shots.split(",") if x] or [end]
    cmd = [str(HEADLESS), "--movie", str(movie), "--frames", str(end + 60)]
    sram = save_for(movie)
    if sram:
        cmd += ["--sram", str(sram)]
    for s in shots:
        cmd += ["--shot", f"{s}:{OUT / f'{s:06d}.png'}"]
    if args.where:
        cmd += ["--where", ",".join(str(s) for s in shots)]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = [l for l in r.stdout.splitlines() if "saved" in l or "FATAL" in l or "mode" in l or "waiting" in l]
    print("\n".join(out[-12:]))
    if r.returncode not in (0,):
        print(r.stdout[-2000:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
