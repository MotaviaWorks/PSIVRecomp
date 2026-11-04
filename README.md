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

### Third-party code (fetched into `third_party/`, not committed)

| Folder | What | Source |
|---|---|---|
| `SDL3-3.4.20/` | Window, input, audio (Windows only; Linux uses the system SDL3) | `SDL3-devel-3.4.20-VC.zip` from github.com/libsdl-org/SDL releases |
| `ymfm/src/` | YM2612 FM chip (BSD-3) | github.com/aaronsgiles/ymfm, commit in `ymfm/COMMIT`: `src/ymfm.h`, `ymfm_fm.h/.ipp`, `ymfm_opn.h/.cpp`, `ymfm_adpcm.h/.cpp`, `ymfm_ssg.h/.cpp`, `ymfm_misc.h/.cpp` |
| `chips/` | Z80 CPU (zlib) | `chips/z80.h` from github.com/floooh/chips, commit in `chips/COMMIT` |

The PSG is implemented in `runtime/sound.c`.

### Controls

| Mega Drive | Keyboard | Gamepad |
|---|---|---|
| D-pad | arrows | D-pad / left stick |
| A / B / C | Z / X / C | West / South / East |
| Start | Enter | Start |

Hold Tab to fast-forward, F10 pause, F11 fullscreen, F12 screenshot
(`screenshots/`). **Esc** (or the pad's Back/Select) opens the system menu:
resume, settings, quit. Battery saves go to `saves/psiv.srm`.

### Enhancements

Settings in the Esc menu (saved to `psiv.ini`). **Mode: Classic** is the
original game, unchanged; **Enhanced** turns on the additions:

- **Battle depth** (F9: off / subtle / strong): battles drawn as layers at
  different depths under a gently moving camera that leans in on attacks.
- **Battle fog** (F8: off / light / heavy): drifting mist between the enemies
  and the party, tinted from each battle's background.
- **Battle effects** (off / subtle / full): the game's own attack and
  technique sprites light the scene in their colours, sparks and a flash
  where hits land, embers rising from fire, a lens flare at bright moments.

- **Sound effects** (modern / original): your own samples instead of chosen
  sounds, mapped by sound ID in `sfx/sfx.ini` (a file, or a folder played at
  random). The samples themselves stay out of git.

- **16:9 widescreen** (on / off; 400 pixels wide, 424 with the 4:3 TV
  shape): the field and battles show more to the left and right. Field
  margins come straight from the game's planes, which hold more of the map
  than the original view; objects are no longer dropped at the old right
  edge; battle backgrounds are mirrored outward. The title, intro and other
  screens built for 320 pixels keep their shape.

- **Text** (modern / original): dialogue, menus and battle windows redrawn
  with a TrueType font (`enhance.text_font`, Segoe UI by default) at full
  resolution, in the game's own positions, colours and timing. Text is
  recognised from what the game draws (its 8x8 window font and the 8x16
  dialogue font, read from the ROM), so the game itself is untouched.
  Needs `third_party/stb/stb_truetype.h` (public domain).

Planned (see `docs/IDEAS.md`): side panels with party info in widescreen.

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
CRAM and VSRAM, plus the VDP beam-counter values the game read (its battle
random numbers mix them in, and they depend on cycle timing);
`psiv_headless --replay` plays the same input with the same counter values and
stops at the first difference.

```
python tools/bizhawk/check.py                  # record the scripted new-game run, then replay it
python tools/bizhawk/check.py --movie tools/movies/save_academy.movie   # an input movie (see below)
python tools/bizhawk/check.py --dump-at 858    # also dump both sides at that frame and diff the bytes
python tools/bizhawk/check.py --play           # play it yourself; close EmuHawk and the port replays it
```

`--play` opens EmuHawk with `tools/bizhawk/record_play.lua`, which restarts
the game from power-on (the port can only replay from reset) and records
every frame at normal speed. Continue works: the battery save BizHawk starts
with is kept next to the trace (`.start.sram`) and given to the port, and the
save it ends with is compared byte for byte. Keep the Lua Console window open
while you play; closing it stops the recording. A recorded session can be
replayed again with `check.py --trace FILE`, or turned into an input movie:
`python tools/movies/trace2movie.py FILE tools/movies/NAME.movie`.

Automated runs start from an empty or given save and put BizHawk's own save
file back before they exit. EmuHawk's location defaults to
`H:\Emulation\08 - Tools\BizHawk\EmuHawk.exe` (override with the
`PSIV_BIZHAWK` environment variable).

### Input movies

Scenarios are input movies by logical frame (`tools/movies/*.movie`), played
identically by the port and BizHawk: `L KEYS [N]` or `+D KEYS [N]`, KEYS from
`UDLRABCS` or `-`. A movie that starts from a battery save says
`# from-save: NAME`; the save is made by playing `NAME.movie` and cached in
`build/saves/`. `tools/movies/try.py BASE.movie "MOVES" --shots F,F` branches
new moves off a movie and takes screenshots, for authoring.

- `opening.movie`: power-on through the intro to free movement (6,830 frames)
- `save_academy.movie`: the opening, then save in slot 1 (7,091 frames)
- `academy.movie`: Continue from that save; Chaz is free in the Academy at 700
- `session1.movie`: a recorded 7-minute session with 7 battles (24,604
  frames); `battle1.movie` is its first 4,900 frames, up to the first battle.
  Both start from the save that session began with (`tools/movies/saves/battle1-start.srm`,
  kept locally, not in git)

### Checking the sound

```
python tools/soundcheck.py --render --bizhawk   # every song: all 68000 sound writes vs BizHawk, tick by tick
python tools/soundcheck.py                      # chip state vs the VGM rips in vgm/ (rips have artifacts)
build\native-Release\psiv_headless.exe --frames 1500 --music 8F:30 --wav song.wav   # jukebox: render one song
```

`--music ID:F` plays any sound ID (`MusicID_*` in `ps4.constants.asm`) the
way the game requests it; `--ymlog` logs every chip write with the source CPU
and the sound-driver tick.

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
