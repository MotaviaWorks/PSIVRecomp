#!/usr/bin/env python3
"""Turn a recorded session (a BizHawk trace) into an input movie.

    python tools/movies/trace2movie.py build/bizhawk/play_20261103_191455.trace tools/movies/NAME.movie

A trace logs the pad buttons the game read at every logical frame, so the
movie replays exactly the same inputs: check.py --movie can then re-record it
in BizHawk any time, and try.py can branch new scenarios off it. Only inputs
go into the movie (nothing from the game itself), so it can be committed.

If the session started from a battery save, the movie needs that save too:
when it matches one the movies already produce (build/saves/*.srm) the movie
says "# from-save: NAME"; otherwise the save is copied to
build/saves/NAME-start.srm (not committed) and the movie refers to that.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SAVES = ROOT / "build" / "saves"                  # saves the movies make
FIXED = ROOT / "tools" / "movies" / "saves"       # saves no movie can make (git-ignored)
KEYS = "UDLRABCS"            # pad bits 0-7: up down left right A B C start
SRAM_SIZE = 0x2000


def pad_runs(trace: Path):
    """[(first logical frame, pad byte, length)] for every run of held buttons."""
    runs, cur = [], None
    for line in trace.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        f = line.split()
        n, pad = int(f[0]), int(f[2])
        if cur and cur[1] == pad and cur[0] + cur[2] == n:
            cur[2] += 1
            continue
        if cur and cur[1]:
            runs.append(tuple(cur))
        cur = [n, pad, 1]
    if cur and cur[1]:
        runs.append(tuple(cur))
    return runs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("movie")
    ap.add_argument("--until", type=int, help="stop the movie at this logical frame")
    args = ap.parse_args()
    trace, movie = Path(args.trace), Path(args.movie)
    lines = ["# psiv movie v1", f"# from {trace.name}"]

    start = trace.with_suffix(".start.sram")
    if start.exists():
        port = start.read_bytes()[1::2][:SRAM_SIZE]          # BizHawk image -> port save
        if any(b not in (0x00, 0xFF) for b in port):
            known = [p for p in sorted(SAVES.glob("*.srm")) + sorted(FIXED.glob("*.srm"))
                     if p.read_bytes()[:SRAM_SIZE] == port]
            if known:
                name = known[0].stem
            else:
                name = f"{movie.stem}-start"
                FIXED.mkdir(parents=True, exist_ok=True)
                (FIXED / f"{name}.srm").write_bytes(port)
                print(f"saved the starting battery save as tools/movies/saves/{name}.srm (keep it: "
                      f"the movie needs it; *.srm stays out of git)")
                lines.append(f"# starts from the save the session started with: "
                             f"tools/movies/saves/{name}.srm (game data, not in git)")
            lines.append(f"# from-save: {name}")

    end = int([l for l in trace.read_text().splitlines() if l and not l.startswith("#")][-1].split()[0])
    if args.until:
        end = min(end, args.until)
    runs = [(n, pad, min(length, end + 1 - n)) for n, pad, length in pad_runs(trace) if n <= end]
    for n, pad, length in runs:
        keys = "".join(k for i, k in enumerate(KEYS) if pad >> i & 1)
        lines.append(f"{n} {keys} {length}")
    lines.append(f"{end} - 1")                                # keep the full length
    movie.write_text("\n".join(lines) + "\n")
    print(f"wrote {movie}: {len(runs)} inputs over {end} logical frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
