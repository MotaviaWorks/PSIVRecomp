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
python tools/indirect_jumps.py                         # writes analysis/indirect_sites.{json,md}
```

`prepare_rom.py` accepts raw and interleaved SMD dumps and refuses anything
that isn't the USA ROM (SHA1 `bc7ff6d6a8408f38562bc610f24645cad6c42629`).
`build_disasm.py` reassembles the disassembly with the bundled assembler on
Windows (`asl` on Linux) and fails unless the result is byte-identical.

## Layout

| Path | What |
|---|---|
| `disasm/ps4disasm` | Phantasy Star IV disassembly (submodule, [squidfeatures/ps4disasm](https://github.com/squidfeatures/ps4disasm), originally by lory90) |
| `tools/` | ROM prep, rebuild, listing parser, analysis (recompiler to come) |
| `analysis/` | Generated analysis reports (addresses and labels only) |
| `docs/` | Plan and ideas |
| `rom/`, `build/` | Your ROM and build output (never committed) |

The disassembly is for informational and educational use; commercial use is
prohibited by its author.
