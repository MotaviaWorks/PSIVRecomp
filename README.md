# PSIV Recomp

A native port of **Phantasy Star IV: The End of the Millennium** (Mega Drive /
Genesis, USA) built by static recompilation. Work in progress: see
[docs/PLAN.md](docs/PLAN.md) for the architecture and milestones and
[docs/IDEAS.md](docs/IDEAS.md) for planned enhancements.

This repository contains no game data. You need your own dump of the USA
cartridge (`GM MK-1307-00`).

## Setup

Requires Python 3.10+, and git with submodules.

```
git submodule update --init
python tools/prepare_rom.py "path/to/your/dump.smd"   # .smd, .bin, .md or .gen
python tools/build_disasm.py                           # rebuild + verify, writes build/disasm/ps4.lst
python tools/validate_decoder.py                       # decoder vs every instruction in the game
python tools/resolve.py                                # function map + indirect-jump targets -> analysis/
python tools/emit.py                                   # recompile to C -> build/gen/
build.bat                                              # Windows (VS Build Tools); elsewhere: cmake -S . -B build/native && cmake --build build/native
build\native-Release\psiv.exe                          # play (run from the project folder)
```

On Windows the build uses the SDL3 development package in
`third_party/SDL3-3.4.20` (from the official `SDL3-devel-3.4.20-VC.zip`
release); on Linux it uses the system SDL3.

### Controls

| Mega Drive | Keyboard | Gamepad |
|---|---|---|
| D-pad | arrows | D-pad / left stick |
| A / B / C | Z / X / C | West / South / East |
| Start | Enter | Start |

Hold Tab to fast-forward, F11 fullscreen, F12 screenshot (`screenshots/`),
Esc quits. Battery saves go to `saves/psiv.srm`. No sound yet (milestone M5).

### Headless runs

`psiv_headless` runs without a window, as fast as possible, with scripted
input and PNG screenshots, e.g. start a new game and capture frame 1500:

```
build\native-Release\psiv_headless.exe --frames 1600 --press 250:S:4 --press 420:S:4 --press 560:C:4 --shot 1500:shot.png
```

`psiv_headless_trace` is the same with block tracing and return-address checks.

### Checking against BizHawk

The port is verified frame by frame against BizHawk (Genesis Plus GX core).
`tools/bizhawk/record.lua` logs, at every frame where the game waits for
vertical blank, the pad state, lag, stack pointer and SHA-256s of RAM, VRAM,
CRAM and VSRAM; `psiv_headless --replay` plays the same input and stops at the
first difference.

```
python tools/bizhawk/check.py                  # record the scripted new-game run, then replay it
python tools/bizhawk/check.py --dump-at 858    # also dump both sides at that frame and diff the bytes
```

To use your own play as a test: in EmuHawk load the ROM, reboot the core,
open Tools > Lua Console, open `tools/bizhawk/record_play.lua` and play. Then
`python tools/bizhawk/check.py --trace build/bizhawk/play_<date>_<time>.trace`.
EmuHawk's location defaults to `H:\Emulation\08 - Tools\BizHawk\EmuHawk.exe`
(override with the `PSIV_BIZHAWK` environment variable).

`prepare_rom.py` accepts raw and interleaved SMD dumps and refuses anything
that isn't the USA ROM (SHA1 `bc7ff6d6a8408f38562bc610f24645cad6c42629`).
`build_disasm.py` reassembles the disassembly with the bundled assembler on
Windows (`asl` on Linux) and fails unless the result is byte-identical.

## Layout

| Path | What |
|---|---|
| `disasm/ps4disasm` | Phantasy Star IV disassembly (submodule, [squidfeatures/ps4disasm](https://github.com/squidfeatures/ps4disasm), originally by lory90) |
| `tools/` | ROM prep, rebuild, listing parser, 68000 decoder, program model, resolver, C generator |
| `runtime/` | CPU (`m68k.*`), bus, VDP + renderer (`vdp.*`), frame loop, SDL3 and headless front ends |
| `third_party/` | SDL3 for Windows builds (fetched, never committed) |
| `analysis/` | Generated analysis: `functions.json` (every function entry and why), `indirect_targets.{json,md}` (addresses and labels only) |
| `docs/` | Plan and ideas |
| `rom/`, `build/` | Your ROM and build output (never committed) |
| `vgm/`, `mp3/` | Your soundtrack references for the sound milestone (never committed) |

The disassembly is for informational and educational use; commercial use is
prohibited by its author.
