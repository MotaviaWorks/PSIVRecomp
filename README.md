# PSIV Recomp

<img src="res/psiv.png" alt="The port's icon: a four-point star" width="96" align="right">

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

The PSG is implemented in `runtime/sound.c`. Android and the portable Linux
build also need the SDL3 3.4.20 source (`SDL3-3.4.20.tar.gz`, same releases
page), kept outside the repo. The modern text's font, Noto Sans (SIL OFL 1.1),
is committed in `res/fonts/` with its licence.

### Platforms and packages

| | Build | Package for testers |
|---|---|---|
| Windows | `build.bat` (static C runtime: no redistributable needed) | `python tools/package.py windows build\native-Release dist` |
| Linux | `cmake -S . -B build/linux -G Ninja && ninja -C build/linux` (system SDL3) | below |
| Linux, portable | SDL3 built static from source, then `-DPSIV_PORTABLE=ON -DPSIV_TOOLS=OFF -DCMAKE_PREFIX_PATH=<its prefix>` | `python3 tools/package.py linux <build> dist` |
| Android | `tools/build_android.sh <SDL3 source>` on Linux (SDK, NDK r30, build-tools 36, JDK 17; no Gradle) | the `.apk` it makes |

- **The launcher:** the app opens on it -- Start game, the mode (Enhanced /
  Classic, a switch), Settings (the same five pages as in the game), Quit --
  over a drifting field of stars under the port's emblem, the icon's
  four-point star drawn at full size from the same shape (`launcher()` in
  `runtime/main_sdl.c`). The game boots only when Start game is chosen.
  `psiv --play` skips it (for frontends and shortcuts); the game-frame
  testing hooks (`--menu`, `--test-keys`, `--test-click`, `--test-rumble`,
  `--capture`) skip it too, unless a `--menu-script` drives the launcher.
- **First run:** with no `rom/psiv.bin`, a dump in the home folder or `rom/`
  installs itself at startup; otherwise Start game asks for it in a file
  dialog (Android: the system picker), and cancelling returns to the launcher.
  `.smd`, raw, or either inside a `.zip` (`runtime/unzip.c`); checked by
  SHA-256 and kept as `rom/psiv.bin`. Testing hooks: `--test-pick FILE`
  answers the dialog, `--no-dialogs` sends message boxes to the log.
- **Home folder:** next to the program when writable (the zip, the tarball),
  else `SDL_GetPrefPath` (`%APPDATA%\PSIV Recomp`, `~/.local/share/psiv-recomp`);
  Android: `Android/data/com.psilok.psiv/files`; a dev build: the project root.
- **Portable Linux:** libstdc++ and SDL3 are linked in, so it needs only glibc;
  `runtime/glibc_compat.c` binds the functions newer glibcs re-versioned to
  their old versions, so it runs on glibc 2.35 (Ubuntu 22.04) while being built
  on a current distro. Check: `objdump -T psiv | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1`.
- **Android:** `libmain.so` (the port) under SDL3's activity
  (`android/java`), landscape, an on-screen pad (`runtime/touch.c`) that hides
  when a gamepad or key is used; Back opens the menu. The APK is signed with
  `~/.psiv-keys/psiv-alpha.jks` (made on first build, password beside it):
  keep that key, since Android only installs updates signed with the same one.
- **macOS:** not packaged yet. The code is ready for it (an app bundle's
  home is the pref path); it needs a Mac to build and sign.
- `docs/TESTERS.md` is the README that goes into every package.

### Controls

| Mega Drive | Keyboard | Gamepad |
|---|---|---|
| D-pad | arrows | D-pad / left stick |
| A / B / C | Z / X / C | West / South / East |
| Start | Enter | Start |

The app opens on the launcher: Start game, Enhanced or Classic, Settings,
Quit (arrows or the stick, Enter or C; Left / Right flips the mode).
Hold Tab to fast-forward, F10 pause, F11 fullscreen, F12 screenshot
(`screenshots/`). **Esc** (or the pad's Back/Select) opens the system menu:
resume, settings, quit. Settings are on five pages -- Game, Display, Sound,
Battles, Controls -- turned with Tab / Page Up / Page Down, the shoulder
buttons, or a click or tap on the page's name; the line under the list says
what the highlighted setting does. Battery saves go to `saves/psiv.srm`;
`psiv --save FILE` plays another, an emulator's `.srm` too (BizHawk, Gens,
Kega Fusion), written back in its own layout.

**Rumble** (Settings > Controls > Rumble: off / light / normal / strong; `pad.rumble`):
the Mega Drive had none, so the port reads the moments from the game's
memory (`runtime/rumble.c`, reads only, so the game is unchanged): a blow
landing, felt with the target's hit reaction (heavier when it's the party's
hurt, stronger the more HP it takes when the game has worked that out yet),
a critical hit with its white flash, an enemy destroyed, a party member
falling, the final bosses' deaths, the big techniques, and the field's
doors, stone slabs, elevators, breaking ice and barriers, explosions and a
ship's engines, each shaped to the length of its sound. It plays on a
gamepad's two motors; on a phone, or a handheld whose controls can't
rumble, on the device's vibration motor, a short pulse per effect. Mode:
Classic leaves it as set (nothing on screen or in the sound changes).
`psiv_headless --rumble-log` lists the effects as they come
(`PSIV_RUMBLE_SOUNDS=1`: every sound request too); `psiv --test-rumble F`
attaches a virtual gamepad that logs what it's asked to play.

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

- **Music** (original chip / Algol): *Algol* is the port's own modern sound
  chip (`runtime/algol.c`). The game's music driver still runs and still
  writes the YM2612 and the PSG; Algol decodes those writes into notes
  (pitch, instrument, loudness, pan) and plays them on new instruments --
  unison saw leads, brass, strings, a bass with a sub, a driven guitar,
  plucks, FM bells and keys, an organ, soft square leads -- each of the
  game's FM patches voiced by the instrument closest to its sound, with a
  synthesized drum kit in place of the drum samples (voices and effects
  sampled in the game are played cleanly from your ROM), reverb, chorus and
  a stereo delay. Parts the game doubles on several channels are merged.
  Sound effects keep the original chips, on the channels they borrow.
  The tense songs -- the title, Meet Them Head-On and the battle and boss
  themes -- get hard rock drums: a kick with a beater knock, a snare with a
  crack, deeper toms, brighter cymbals, a room and a punchy bus compressor,
  and a hi-hat on the eighths locked to the song's own backbeat. In the
  louder sections a ride takes over from the hat, with a crash on the way
  in: Meet Them Head-On's melody, the title's break, the second half of
  Defeat at a Blow, the middle of The King of Terrors (`RIDE_MAP` in
  `algol.c`, positions in driver ticks from the song's start, found with
  `python tools/algol_sections.py 8F`, which lists a song bar by bar).
  `algol.ini` in the working folder overrides the instrument of any patch
  (`PSIV_ALGOL_LOG=1` lists them) and the drums of any song (`song 8F =
  rock` or `= standard`); `psiv_headless --algol` renders with it.

  **Instruments from a SNES game you own** (`enhance.music_bank` in
  `psiv.ini`: the path to its ROM): Algol plays the music with that game's
  own instruments and drums instead of its synthesizers, decoded from your
  copy at startup (`runtime/snesbank.c`: the S-DSP's BRR decoder, each
  sample with its loop, envelope and the game's tuning). Nothing from the
  game ships. Known so far: **Lufia II** (37 instruments). Which sample plays
  what can be changed in `algol.ini` (`bank lead = 35`, optionally a
  transposition in semitones: `bank bass = 26 -12`; `PSIV_ALGOL_LOG=1` lists
  them); `psiv_headless --algol-bank ROM` renders with it. Adding a game
  means finding how it stores its samples: `tools/snes/sound_ram.py`.

- **First-person dungeons -- paused** (2026-11-05): not in the menu or
  `psiv.ini` until it plays well; `PSIV_FIRST_PERSON=1` turns it on for
  development, and `psiv_headless --fpview S` still renders it. What it is:
  the dungeons seen from the
  leader's eyes, like the mazes of Phantasy Star I, built live from the
  game's own map and tiles (`runtime/fpview.c`): the walls are its solid
  cells faced with the wall fronts it draws in its 3/4 view, the floor is the
  map itself in perspective, and its palettes and animations carry over.
  Up walks forward, left / right turn that way, down turns back (each held
  key keeps its direction); menus and dialogue are drawn on top and take the
  pad as usual. A battle that starts there is fought in that view: the
  corridor behind the enemies, the party seen through the battle menus.
  People, chests and the game's other objects stand in the corridor as
  pictures turned to you, drawn from the game's own object list and art,
  near or far (the game itself only draws what is on its screen); people
  show the side of them that faces you, and a thing set into a wall (the
  Machine Center's elevator doors) is painted onto it. What the game draws
  over the party in its 3/4 view -- a pillar's shaft, an arch over a
  passage, a wall's top edge -- stands up from the floor line beneath it at
  the art's own scale: the Soldier's Temple's pillars rise at full height
  beside its walls, its arch hangs over the doorway. Doors open as in the
  game (each door changes the map, and the view follows it: the Machine
  Center's elevator slides open). Towns and the planets' surfaces keep the
  game's view. `psiv_headless --fpview S` takes shots of it
  (`PSIV_FPVIEW_ANYWHERE=1`: in towns too, for testing). Inspired by
  Gen1Recomp's 3D mode.

- **Click to walk** (Controls page: on / off): a click on the field sends the party
  there, by the shortest way the game's own collision allows
  (`runtime/walkto.c`): the d-pad is held a step at a time, as you would.
  Click a person, a chest or a door to walk up to it, face it and press C
  (talk, open, examine); click a wall, water or a mountain to get as near as
  you can; click a cave or a town's entrance to go in. It works in first
  person too (the floor or the person under the cursor). A marker shows
  where you're going; your own keys take over at once, a right click stops.
  While text is up, a click is C (next) and a right click B. In battles
  (the modern menus) a click picks what it is on -- Command / Macro / Run, a
  command, a technique, skill or item (on any page), an enemy or a member of
  the party as the target -- by walking the game's own cursor there a press
  at a time and pressing C; a right click is B (back).
  `psiv_headless --click F:X,Y[,R]` clicks frame pixel (X, Y) at logical
  frame F (R: the right button).

- **Your music** (on / off): audio files of your own -- a remix, say -- play
  in place of the game's songs. Put WAVs (16/24-bit or float, any rate) in
  `music/` (git-ignored), named by the song's ID: `music/8F.wav` for Meet
  Them Head-On (`PSIV_MUSIC_LOG=1` prints each song's ID and name as it
  starts: 8F battles, 95 bosses, AA Zio, A0 Dark Force, 96 vehicle battles,
  AC the title, 8C the Motavia field). Or name them in `music/music.ini`,
  which also sets the loop (the WAV's own loop markers count too), where a
  file starts (by default at its first sound) and its level:

  ```
  8F = Meet Them Head-On (my remix).wav
  8F loop 19.76 212.08      ; at 212.08 s, back to 19.76 s
  8F gain -2                ; dB
  ```

  The game's sound effects keep playing on the chips; the file starts with
  its song, follows the game's fade-outs and pauses, and stops when the music
  does. Over Algol or the original chip alike. `psiv_headless --custom-music
  DIR` renders with it.

- **Sound effects** (modern / your samples / original): *modern* is the port's
  own set, synthesized at startup (`runtime/sfxsynth.c`), so nothing is
  shipped: blade hits (a short swing, a slice, a ringing edge, a thump; hits
  in quick succession overlap), the slasher's throw (the spin flying off and
  coming back, timed to its hits, with Doppler), a menu-cursor tick and a
  two-note chime for choices -- several variants each, picked at random.
  *Your samples* maps sound IDs to your own WAVs in `sfx/sfx.ini` (a file, or
  a folder played at random); IDs it doesn't map stay modern. Samples stay out
  of git. `psiv_headless --sfx-dump DIR` writes every synthesized sound to
  WAV files; `--sfx-synth` / `--sfx DIR` play them in headless runs.

- **16:9 widescreen** (on / off; 400 pixels wide, 424 with the 4:3 TV
  shape): the field and battles show more to the left and right. The game
  only keeps what a 320-pixel screen needs in video memory, so the field's
  margins are drawn from the map it keeps in RAM instead (`runtime/widemap.c`:
  checked against the screen every frame, the planes' own content when they
  disagree, the edge chunks repeated past a map's edge); objects are no
  longer dropped near the old edges; battle backgrounds are mirrored outward.
  The title, intro and other screens built for 320 pixels keep their shape.
  `psiv_headless --wide-plain` shows the margins the old way, for comparison;
  `PSIV_WIDEMAP_LOG=1` logs how the screen and the map agree.

- **Text** (modern / original): dialogue, menus and battle windows redrawn
  with a TrueType font (`enhance.text_font`, Segoe UI by default) at full
  resolution, in the game's own positions, colours and timing. Text is
  recognised from what the game draws (its 8x8 window font and the 8x16
  dialogue font, read from the ROM), so the game itself is untouched.
  Needs `third_party/stb/stb_truetype.h` (public domain).

- **Side panels** (Widescreen: Side panels): in exploration the game keeps its
  original view and the 16:9 margins show the party -- a card per member
  (portrait, level, HP / TP / EXP bars, status) and meseta with the
  inventory. Portraits, item names and level tables come from your ROM at
  startup; nothing from the game ships with the port.

- **Battle menus** (modern / original): the battle windows replaced by menus
  drawn at full resolution -- a party bar with portraits and HP / TP bars
  (and each member's chosen action), the commands spelled out (Attack,
  Technique, Skill, Item, Defend) with a line on what each does, technique /
  skill / item lists with costs or uses left and a description of the one
  highlighted, a marker and name over the enemy being targeted. The whole
  party stays on screen in their places all battle long (the original shows
  only the member choosing, the one acting, or the one an enemy attacks,
  moved in front of it): an attacker's place empties while they strike, an
  enemy's attack lands on its target where they stand, a caster casts and
  an archer throws from their own place, and the game's messages move to the
  top of the screen. The cards run in the order the party stands.
  Whoever is choosing has a gold aura, an ally being targeted a cyan one;
  the commands sit in a strip under the party and the lists beside or above
  it, so no menu covers anyone. The game
  still runs its own menus: the port mirrors their state from RAM, so the
  controls are the original ones. Names come from your ROM; the descriptions
  are the port's own (`runtime/battledesc.c`). Vehicle battles keep the
  original windows. Needs the modern-text font.

Planned (see `docs/IDEAS.md`): more of the sound set synthesized (techniques,
enemy attacks, the field).

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

`check.py --movie M --sram FILE` starts both sides from a battery save of
your own, in any layout the port reads.

### The test bot: battles and saves from anywhere in the game

`psiv_headless --bot SEED:WALK:N[:V]` plays by itself (`runtime/testbot.c`).
It Continues from the battery save, walks the world map back and forth
(`WALK` is `LR`, `UD`, `RL` or `DU`) until a random battle starts, and fights
N battles to the end through the game's own menus, which it reads from RAM.
Each battle has a plan: attacks, techniques, skills, items, a macro, running,
a mix, with a random usable entry and target each time. Then it saves in
slot 1. `V` boards a vehicle first (1 Land Rover, 2 Ice Digger, 3 Hydrofoil,
used as items from the field menu) for vehicle battles. `--bot-movie FILE`
keeps its input as a movie. `tools/bizhawk/bot.lua` is the same bot for
BizHawk (`check.py --bot 8:LR:7 --sram SAVE`): there the reference fights its
own battles, and the port replays them frame by frame.

`tools/battle_test.py FOLDER` runs it from every save in a folder:

- finds a walk that meets a battle without entering a town;
- lets the bot play, then replays its input with the port's additions on (two
  sets): the game must end in the same state, checked by its RAM (less the
  sound driver, the stack and the sprite table) and by the battery save it
  writes;
- screenshots every battle with the additions on;
- checks the save the bot wrote: its checksum, and that Continue brings it
  back;
- with `--bizhawk`, BizHawk plays too, one save at a time (EmuHawk starts
  minimized, and waits while yours is open), and its screenshots of the
  gravity techniques' raster effect are held against the port's.

Results go to `build/bot/` (`results.json`, `bizhawk.json`, the bot's logs and
the screenshots). `tools/battle_probe.py FOLDER` takes
a folder of saves (from anywhere in the game: keep collected saves out of
git, `imported saves/` is ignored), Continues each, walks until a random
battle and screenshots the battle menus: the commands, the technique and
skill lists, and a round of attacks. Its movies replay in `check.py`
(`--list` shows each save's world, place and party).

### Checking the sound

```
python tools/soundcheck.py --render --bizhawk   # every song: all 68000 sound writes vs BizHawk, tick by tick
python tools/soundcheck.py                      # chip state vs the VGM rips in vgm/ (rips have artifacts)
build\native-Release\psiv_headless.exe --frames 1500 --music 8F:30 --wav song.wav   # jukebox: render one song
```

`--music ID:F` plays any sound ID (`MusicID_*` in `ps4.constants.asm`) the
way the game requests it; `--ymlog` logs every chip write with the source CPU
and the sound-driver tick.

**The mix and the output.** The chips, the port's sound effects and your
music are summed in float; `runtime/mixbus.c` makes the 16-bit samples. With
only the game's own sound (Classic, and every reference check) that is the
same conversion as ever, bit for bit. With the port's sound in the mix, a
look-ahead peak limiter (1.2 ms, ceiling -0.8 dBFS, 80 ms release) turns the
whole mix down for a moment on a loud hit over loud music, instead of the
per-sample soft clipping it replaced (heard as distortion); `psiv_headless`
prints how much it worked. The app keeps ~50 ms queued for the output device
(twice its buffer and a frame) and holds it there by nudging the playback
speed (at most 0.5%, typically a few hundredths of a percent: the PC's timer
and the device's clock drift apart). `psiv.log` gets an `audio:` line for any
30 s with a gap (underrun) or a dropped frame; `PSIV_AUDIO_LOG=1` logs every
5 s, `=2` each refill too; `psiv --test-ff F:N` holds fast forward.

`prepare_rom.py` accepts raw and interleaved SMD dumps and refuses anything
that isn't the USA ROM (SHA1 `bc7ff6d6a8408f38562bc610f24645cad6c42629`).
`build_disasm.py` reassembles the disassembly with the bundled assembler on
Windows (`asl` on Linux) and fails unless the result is byte-identical.

## Layout

| Path | What |
|---|---|
| `disasm/ps4disasm` | Phantasy Star IV disassembly (submodule, [squidfeatures/ps4disasm](https://github.com/squidfeatures/ps4disasm), originally by lory90) |
| `tools/` | ROM prep, rebuild, listing parser, 68000 decoder, program model, resolver, C generator |
| `runtime/` | CPU (`m68k.*`), bus, VDP + renderer (`vdp.*`), frame loop, SDL3 and headless front ends; touch controls, rumble, zip reader, glibc compatibility |
| `third_party/` | SDL3 for Windows builds, ymfm, z80.h, stb (fetched, never committed) |
| `res/` | The app's icon: an original four-point star in the game's window colours, drawn by `tools/make_icon.py` (which also writes `runtime/icon.h` and Android's launcher icons); `fonts/`: Noto Sans (OFL); no game art |
| `android/` | Android manifest, resources and activity (`tools/build_android.sh`) |
| `analysis/` | Generated analysis: `functions.json` (every function entry and why), `indirect_targets.{json,md}` (addresses and labels only) |
| `docs/` | Plan, ideas, and the testers' README (`TESTERS.md`) |
| `rom/`, `build/`, `dist/` | Your ROM, build output and packages (never committed) |
| `vgm/`, `mp3/` | Your soundtrack references for the sound milestone (never committed) |

The disassembly is for informational and educational use; commercial use is
prohibited by its author.
