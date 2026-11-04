#!/usr/bin/env python3
"""Compare the port against BizHawk (Genesis Plus GX) frame by frame.

    python tools/bizhawk/check.py                    record the newgame scenario, then replay it
    python tools/bizhawk/check.py --trace FILE       replay an existing trace (e.g. your own play)
    python tools/bizhawk/check.py --movie FILE       play an input movie (by logical frame, see
                                                     psiv_headless --movie) in BizHawk, then replay
    python tools/bizhawk/check.py --dump-at N        also dump raw memory at logical frame N
                                                     on both sides and diff them

Recording runs EmuHawk with tools/bizhawk/newgame.lua (unthrottled, it closes
itself). Replay runs build/native-Release/psiv_headless --replay, which stops
at the first logical frame whose memory differs and dumps the port's state;
re-run with --dump-at at that frame to see which bytes differ.

EmuHawk is found via the PSIV_BIZHAWK environment variable, or the default
install path below. BizHawk is given a copy of rom/psiv.bin named .md so it
picks the Genesis core without asking.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OUT = ROOT / "build" / "bizhawk"
BIZHAWK = Path(os.environ.get("PSIV_BIZHAWK", r"H:\Emulation\08 - Tools\BizHawk\EmuHawk.exe"))
HEADLESS = ROOT / "build" / ("native-Release/psiv_headless.exe" if os.name == "nt" else "native/psiv_headless")


def record(scenario: str, dump_at: int | None, movie: Path | None = None) -> Path:
    OUT.mkdir(parents=True, exist_ok=True)
    rom = OUT / "psiv.md"
    if not rom.exists():
        shutil.copyfile(ROOT / "rom" / "psiv.bin", rom)
    env = dict(os.environ, PSIV_ROOT=str(ROOT))
    if dump_at:
        env["PSIV_DUMP_AT"] = str(dump_at)
    if movie:
        # a movie: tools/bizhawk/movie.lua plays it by logical frame
        name = movie.stem
        trace = OUT / f"{name}.trace"
        env.update(PSIV_MOVIE_FILE=str(movie.resolve()), PSIV_TRACE_OUT=str(trace))
        lua = ROOT / "tools" / "bizhawk" / "movie.lua"
        timeout = 1800
    else:
        name = scenario
        trace = OUT / f"{scenario}.trace"
        lua = ROOT / "tools" / "bizhawk" / f"{scenario}.lua"
        timeout = 120
    err = OUT / "error.txt"
    err.unlink(missing_ok=True)
    trace.unlink(missing_ok=True)
    print(f"recording {name} in BizHawk (the window closes itself)...", flush=True)
    p = subprocess.Popen([str(BIZHAWK), f"--lua={lua}", str(rom)], env=env)
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        raise SystemExit(f"BizHawk did not finish within {timeout // 60} minutes; closed it. "
                         + (err.read_text() if err.exists() else "No script error was logged."))
    if err.exists():
        raise SystemExit("BizHawk script error: " + err.read_text())
    if not trace.exists():
        raise SystemExit(f"BizHawk exited without writing {trace}")
    return trace


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", help="replay this trace instead of recording one")
    ap.add_argument("--scenario", default="newgame", help="tools/bizhawk/<scenario>.lua to record")
    ap.add_argument("--movie", help="record this input movie (by logical frame) instead of a scenario")
    ap.add_argument("--dump-at", type=int, help="dump raw memory at this logical frame on both sides")
    args = ap.parse_args()

    if args.trace:
        trace = Path(args.trace)
    else:
        trace = record(args.scenario, args.dump_at, Path(args.movie) if args.movie else None)
    cmd = [str(HEADLESS), "--replay", str(trace)]
    if args.dump_at:
        cmd += ["--dump-at", str(args.dump_at), "--keep-going"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(r.stdout + r.stderr)
    sys.stdout.flush()

    if args.dump_at:
        ref = trace.with_suffix("")
        port = Path(str(ref) + ".port")
        lines = [l for l in trace.read_text().splitlines() if not l.startswith("#")]
        sp = lines[args.dump_at - 1].split()[3] if len(lines) >= args.dump_at else None
        print(f"\nbyte diff at logical frame {args.dump_at}:", flush=True)
        diff = [sys.executable, str(ROOT / "tools" / "bizhawk" / "diffdump.py"), str(ref), str(port)]
        subprocess.run(diff + ([sp] if sp else []), check=False)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
