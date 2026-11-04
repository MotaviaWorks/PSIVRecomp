#!/usr/bin/env python3
"""Compare the port against BizHawk (Genesis Plus GX) frame by frame.

    python tools/bizhawk/check.py                    record the newgame scenario, then replay it
    python tools/bizhawk/check.py --play             play the game yourself in BizHawk; close the
                                                     window when done and the port replays it
    python tools/bizhawk/check.py --trace FILE       replay an existing trace (e.g. your own play)
    python tools/bizhawk/check.py --movie FILE       play an input movie (by logical frame, see
                                                     psiv_headless --movie) in BizHawk, then replay
    python tools/bizhawk/check.py --dump-at N        also dump raw memory at logical frame N
                                                     on both sides and diff them

Recording runs EmuHawk with tools/bizhawk/newgame.lua (unthrottled, it closes
itself). Replay runs build/native-Release/psiv_headless --replay, which stops
at the first logical frame whose memory differs and dumps the port's state;
re-run with --dump-at at that frame to see which bytes differ.

Both sides start from the same battery save: the trace's .start.sram (which
the recorder always writes), else --sram, else the movie's "# from-save:",
else an empty one. Automated runs leave BizHawk's own save file alone; --play
uses and keeps it, like normal play.

EmuHawk is found via the PSIV_BIZHAWK environment variable, or the default
install path below. BizHawk is given a copy of rom/psiv.bin named .md so it
picks the Genesis core without asking.
"""
import argparse
import os
from datetime import datetime
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OUT = ROOT / "build" / "bizhawk"
BIZHAWK = Path(os.environ.get("PSIV_BIZHAWK", r"H:\Emulation\08 - Tools\BizHawk\EmuHawk.exe"))
HEADLESS = ROOT / "build" / ("native-Release/psiv_headless.exe" if os.name == "nt" else "native/psiv_headless")


SRAM_SIZE = 0x2000          # the cartridge's battery RAM: odd bytes of $200001-$203FFF


def to_bizhawk_sram(port: bytes) -> bytes:
    """Port save (8 KB of odd bytes) -> BizHawk's "SRAM" domain image (16 KB,
    every byte of $200000-$203FFF; the even bytes are unused)."""
    image = bytearray(b"\xFF" * (2 * SRAM_SIZE))
    image[1::2] = port[:SRAM_SIZE]
    return bytes(image)


def from_bizhawk_sram(image: bytes) -> bytes:
    return bytes(image[1::2][:SRAM_SIZE])


def record(scenario: str, dump_at: int | None, movie: Path | None = None,
           sram_in: Path | None = None, play: bool = False) -> Path:
    OUT.mkdir(parents=True, exist_ok=True)
    rom = OUT / "psiv.md"
    if not rom.exists():
        shutil.copyfile(ROOT / "rom" / "psiv.bin", rom)
    env = dict(os.environ, PSIV_ROOT=str(ROOT))
    for k in ("PSIV_SRAM_IN", "PSIV_SRAM_OUT", "PSIV_TRACE_OUT", "PSIV_MOVIE_FILE", "PSIV_DUMP_AT"):
        env.pop(k, None)
    if dump_at:
        env["PSIV_DUMP_AT"] = str(dump_at)
    if play:
        # live play: record_play.lua at normal speed, from BizHawk's own save
        name = "play_" + datetime.now().strftime("%Y%m%d_%H%M%S")
        trace = OUT / f"{name}.trace"
        env["PSIV_TRACE_OUT"] = str(trace)
        lua = ROOT / "tools" / "bizhawk" / "record_play.lua"
        timeout = None
    else:
        if movie:
            # a movie: tools/bizhawk/movie.lua plays it by logical frame
            name = movie.stem
            env["PSIV_MOVIE_FILE"] = str(movie.resolve())
            lua = ROOT / "tools" / "bizhawk" / "movie.lua"
            timeout = 1800
        else:
            name = scenario
            lua = ROOT / "tools" / "bizhawk" / f"{scenario}.lua"
            timeout = 600
        trace = OUT / f"{name}.trace"
        env["PSIV_TRACE_OUT"] = str(trace)
        env["PSIV_SRAM_OUT"] = str(trace.with_suffix(".bizhawk.sram"))
        # start from the given save, or an empty one -- never from whatever
        # BizHawk's own save file happens to hold
        image = OUT / f"{name}.bizhawk-in.sram"
        image.write_bytes(to_bizhawk_sram(sram_in.read_bytes() if sram_in else bytes(SRAM_SIZE)))
        env["PSIV_SRAM_IN"] = str(image)
    err = OUT / "error.txt"
    err.unlink(missing_ok=True)
    for suffix in (".trace", ".start.sram", ".bizhawk.sram"):
        trace.with_suffix(suffix).unlink(missing_ok=True)
    if play:
        print("Play in the EmuHawk window (it restarts the game itself). Close it when you're "
              "done; the port then replays your session.", flush=True)
    else:
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
    ap.add_argument("--play", action="store_true",
                    help="play live in BizHawk; when you close it, the port replays your session")
    ap.add_argument("--trace", help="replay this trace instead of recording one")
    ap.add_argument("--scenario", default="newgame", help="tools/bizhawk/<scenario>.lua to record")
    ap.add_argument("--movie", help="record this input movie (by logical frame) instead of a scenario")
    ap.add_argument("--sram", help="battery save to start from (port format, 8 KB); default: the "
                                   "movie's '# from-save: NAME' line, if any")
    ap.add_argument("--dump-at", type=int, help="dump raw memory at this logical frame on both sides")
    args = ap.parse_args()

    movie = Path(args.movie) if args.movie else None
    sram_in = Path(args.sram) if args.sram else None
    if movie and not sram_in:
        sys.path.insert(0, str(ROOT / "tools" / "movies"))
        from saves import save_for              # "# from-save: NAME" in the movie
        sram_in = save_for(movie)
    if args.trace:
        trace = Path(args.trace)
    else:
        trace = record(args.scenario, args.dump_at, movie, sram_in, play=args.play)
    # the save BizHawk started from (the recorder keeps it next to the trace)
    start = trace.with_suffix(".start.sram")
    if start.exists() and not args.sram:
        sram_in = trace.with_suffix(".start.srm")
        sram_in.write_bytes(from_bizhawk_sram(start.read_bytes()))
    port_sram = trace.with_suffix(".port.srm")
    cmd = [str(HEADLESS), "--replay", str(trace), "--sram-out", str(port_sram)]
    if sram_in:
        cmd += ["--sram", str(sram_in)]
    if args.dump_at:
        cmd += ["--dump-at", str(args.dump_at), "--keep-going"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(r.stdout + r.stderr)
    sys.stdout.flush()

    # the battery save must come out the same too
    bizhawk_sram = trace.with_suffix(".bizhawk.sram")
    if bizhawk_sram.exists() and port_sram.exists():          # (recording clears old ones)
        ref, got = from_bizhawk_sram(bizhawk_sram.read_bytes()), port_sram.read_bytes()
        if ref == got:
            used = sum(1 for b in got if b not in (0x00, 0xFF))
            print(f"battery save identical ({used} bytes in use)")
        else:
            diff = [i for i in range(min(len(ref), len(got))) if ref[i] != got[i]]
            print(f"battery save DIFFERS in {len(diff)} bytes, first at offset ${diff[0]:04X} "
                  f"(BizHawk {ref[diff[0]]:02X}, port {got[diff[0]]:02X})" if diff else
                  f"battery save sizes differ: BizHawk {len(ref)}, port {len(got)}")
            r = subprocess.CompletedProcess(r.args, 1)

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
