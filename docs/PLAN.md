# PSIV Recomp — plan

A native port of **Phantasy Star IV (USA)** for modern platforms, built by
statically recompiling the original 68000 code into C and running it on a
re-implemented Mega Drive hardware layer.

## Ground rules

- **The repo never contains the ROM or anything generated from it.** You supply
  your own dump; the build verifies it (SHA1 `bc7ff6d6…`, 3 MB, USA) and
  generates the C code locally. Same model as N64Recomp-style projects.
- **Faithful first, enhancements second.** The base port must behave exactly
  like the original (proven against an emulator, frame by frame). Enhancements
  ([IDEAS.md](IDEAS.md)) are layered on top and can be switched off.
- **Cross-platform C + CMake + SDL3.** Developed on Windows (MSVC), also builds
  on Linux (Hermes).

## Pipeline

```
your dump (.smd/.bin)
  └─ tools/prepare_rom.py   → rom/psiv.bin (verified)
       └─ tools/build_disasm.py → build/disasm/ps4.lst (byte-identical rebuild of the disassembly)
            └─ analysis: functions, control flow, indirect-jump targets
                 └─ recompiler → build/gen/*.c (one C function per 68k function)
                      └─ + runtime (CPU state, bus, VDP, sound, SDL3) → psiv.exe
```

The disassembly (`disasm/ps4disasm`, pinned submodule) supplies labels and
structure. The recompiler decodes the **ROM bytes** itself and cross-checks
every instruction against the listing, so the two can never silently disagree.

## Architecture

### Recompiler (Python, `tools/recomp/`)
- **68000 decoder** from ROM bytes; validated against the listing for every
  instruction (mnemonic, size, operands, length).
- **Function discovery**: entry points from the vector table, every `jsr`/`bsr`
  target, and the resolved indirect-jump target sets
  (`tools/indirect_jumps.py`: 374/401 sites resolved, 180/180 `trap #2` callers).
- **Emitter**: each 68k function becomes a C function; branches inside it become
  `goto`s; `jmp table(pc,dN)` becomes a `switch`; indirect calls go through a
  generated address→function table that aborts loudly on an unknown target.
- **Flag liveness**: condition codes are only computed where something reads
  them, which removes most of the cost of emulating the 68000's flags.

### CPU model (generated code + `runtime/cpu`)
- Registers in a struct (`d[8]`, `a[8]`, SR, split X/N/Z/V/C flags).
- Memory through inline accessors: fast paths for work RAM (`$FF0000`) and ROM,
  slow path for I/O, VDP, Z80 area and battery SRAM (banked via `$A130F1`).
- `jsr`/`rts` become real C calls and returns. The return address is still
  pushed to the emulated stack, so code that inspects the stack sees the right
  values. The sound driver's four `addq.w #4,sp` "return from my caller too"
  tricks are recognised as a pattern and emitted as an early-return signal.

### Frame model
The game's main loop sets `VInt_Flag` and spins in `WaitForVInt`. The port
replaces that spin with a runtime call: render the frame (scanline by
scanline, calling the horizontal-interrupt handler where the game programmed
it), run the recompiled `VInt` handler, present, read input, sync audio. Other
hardware spin-loops (Z80 bus grant, DMA busy) complete immediately. No threads
or cycle counting are needed.

### Hardware runtime (C, `runtime/`)
- **VDP**: registers, VRAM/CRAM/VSRAM, all three DMA modes, status/HV counter.
- **Renderer**: written from scratch, *layer-separated* (plane B, plane A,
  window, sprites, priority kept apart until compositing). That separation is
  what makes widescreen and battle depth possible later.
- **Sound**: the Z80 runs the game's own 1,314-byte DAC driver under emulation;
  YM2612 FM and PSG via small open-source chip cores (each download asked for
  first). Candidates: ymfm (BSD-3) for the YM2612, a zlib-licensed Z80 core.
  References: `vgm/` holds VGM rips of all 48 tracks, which are logs of every
  sound-chip write, so the port's own writes can be compared track by track;
  `mp3/` is there for listening comparisons.
- **Saves**: battery SRAM persisted to a file next to the executable.
- **Overrides**: any recompiled function can be replaced or wrapped by
  hand-written C, keyed by ROM address. `WaitForVInt` is the first; the text
  UI, widescreen and cheats all use the same mechanism.

## Verification

1. **Decoder vs listing**: 100% of instructions must match, run on every build.
2. **Generated code compiles cleanly** (warnings as errors) on MSVC and gcc/clang.
3. **BizHawk reference** (Desk): a Lua script records, at every `WaitForVInt`
   (one *logical* frame, so lag frames don't misalign anything), the joypad
   state plus hashes of RAM, VRAM, CRAM and VSRAM. The port replays the same
   inputs and must produce the same hashes; on the first mismatch it dumps both
   states so the diverging RAM points straight at the bug. Your own playthrough
   recorded in BizHawk becomes the regression test for the whole game.

## Milestones

| | Milestone | Done when |
|---|---|---|
| M0 | Project setup | ROM prep, byte-identical rebuild and jump analysis run on Desk — **done 2026-11-03** |
| M1 | Decoder + function map | Decoder matches the listing 100%; function list + CFG; cross-branch tracing closes the remaining indirect sites — **done 2026-11-03** (see below) |
| M2 | Emitter | All code recompiles to C and compiles cleanly — **done 2026-11-03** (see below) |
| M3 | First boot | SDL3 window shows the SEGA logo and the title screen (no sound) — **done 2026-11-03**: SEGA logo, intro, title, and a new game through to the first field scene |
| M4 | Reference harness | BizHawk recordings replay in the port with matching state through the intro — **done 2026-11-03**: 2,348/2,348 frames identical through the first field scene |
| M5 | Sound | Z80 + YM2612 + PSG; music and effects match — **done 2026-11-03**: all 52 music IDs identical to BizHawk on every 68000 sound write |
| M6 | Playable | Saves, controller support, scaling/fullscreen; full playthrough matches the reference |
| M7 | Enhancements | Features from [IDEAS.md](IDEAS.md) |

### M1 results

- **Decoder**: all 102,494 68000 instructions pass length, mnemonic and byte-exact
  round-trip checks (`tools/validate_decoder.py`).
- **Reach**: 101,423 of 102,494 instructions are reachable from the vectors. The
  other 1,071 (128 runs, nearly all unlabelled code right after an `rts`) look
  dead; they are compiled anyway.
- **Functions**: 3,925 entry points (`analysis/functions.json`), from direct
  calls, jump tables, code addresses stored in data tables (`dc.l EnemyAI_…`,
  2,103 of them: the window system, enemy AI and techniques are only reachable
  this way), the vectors, and the RAM HInt trampoline (`jmp` written at
  `$FFECB0`; targets `HInt`/`HInt2`).
- **Indirect jumps**: 396 of 401 sites resolved with high confidence, plus all 180
  `trap #2` callers. Remaining: 4 pointers read from object fields
  (`Window_Update` and three in battle code) and the Nemesis decompressor's
  saved pixel-writer pointer. Every code address the game uses as a value is a
  function entry, so these go through the generated address→function table,
  which aborts loudly on anything else; the BizHawk harness confirms them.
- **Quirks found**: two branches into the middle of an instruction
  (`bcc.w loc_586C2-2`, and one at `$58514`) that would end in a line-F
  exception on hardware; the model reproduces that.

### M2 results

- **Generator** (`tools/emit.py`, 3.6 s): 3,925 functions, 113,047 instructions,
  ~491k lines of C in 32 files. Condition codes are computed only where they can
  be read; each function's flags-read-at-entry is solved for the whole program
  (5 rounds), because routines return results in Z for their callers.
- **Calls**: `jsr`/`bsr` push the real return address and make a C call; every
  call site checks the stack came back balanced (the sound driver's
  `addq.w #4,sp` "return from my caller too" makes the caller return as well).
  Debug builds also check each popped return address matches the one pushed.
- **Builds clean** with MSVC `/W3 /WX` (15 s for both variants) and gcc `-O3
  -Wall -Werror`.
- **Smoke test** (`psiv_smoke`): with stub hardware and the vertical-blank
  interrupt delivered at `WaitForVInt`, the game boots, uploads the Z80 driver,
  reaches the title at frame 195 and field mode at frame 612, and runs 36,000
  frames (85.6M blocks, 0.3 s) with no unknown indirect target and no return
  mismatch. MSVC and gcc builds finish with the identical RAM checksum.
- Not yet proven: that every instruction's semantics match a real 68000. That
  is what the BizHawk comparison (M4) checks.

### M3 results

- **VDP** (`runtime/vdp.c`): registers, VRAM/CRAM/VSRAM, all three DMA modes
  (instant), status, HV counter. Scanline renderer with plane A/B, window and
  sprites in separate line buffers, mixed by priority at the end; per-line and
  per-cell horizontal scroll, per-column vertical scroll, sprite line limits and
  masking.
- **Frame loop** (`runtime/frame.c`): a generator hook (`HOOKS` in
  `tools/emit.py`, checked against the expected instruction) replaces the
  `WaitForVInt` spin with: render each line, firing HInt when the line counter
  runs out (`HInt2` rewrites vertical scroll mid-frame), present + read input,
  then run the game's VInt handler.
- **Bus**: battery SRAM at the odd bytes of `$200001-$203FFF` while `$A130F1`
  bit 0 is set (the inline ROM fast path stops at `$200000` then), 3-button pads.
- **Front ends**: `psiv` (SDL3: integer scaling, keyboard/gamepad, 59.92 Hz
  pacing, saves, screenshots, fast-forward) and `psiv_headless` (scripted
  input, PNG screenshots).
- **Verified by screenshots**: SEGA logo, intro story with the Motavia panel,
  title with START, and a new game through Alys's first dialogue in the
  opening scene.
- **Approximations to revisit with the BizHawk reference (M4)**:
  - VInt only fires when the game waits for it, so frames where the original
    would lag (and still run the sound driver from `VInt_Skip`) don't lag here.
  - The status register's vblank bit alternates for code that polls it
    outside VInt; the HV counter is coarse.
  - Shadow/highlight mode and interlace are not implemented.
  - Colours use a linear 3-bit to 8-bit ramp.

### M4 results

- **BizHawk execute hooks work** on the Genesis Plus GX core, so the reference
  samples at exactly the port's frame boundary: entry to `VInt` ($524) with
  `VInt_Flag` set (a *logical frame*). VInts with the flag clear are counted as
  lag.
- **Trace** (`tools/bizhawk/record.lua`, v3): per logical frame the pad byte,
  lag count, main-code stack pointer, and 16-hex-digit SHA-256s of RAM (16 ×
  4 KB, live stack only, sound driver split out), VRAM (16 × 4 KB), CRAM,
  VSRAM, and the sound driver's RAM. `psiv_headless --replay` mirrors it;
  `tools/bizhawk/check.py` runs both and `diffdump.py` names differing bytes.
- **Result**: the scripted new-game run (2,600 emulated frames: SEGA screen,
  intro, title, new game, opening dialogue, walking) matches on all 2,348
  logical frames: every byte of RAM outside the sound driver, all of VRAM,
  CRAM and VSRAM, and the stack pointer.
- **Bugs it found and fixed**:
  - VSRAM keeps the full 16-bit word as written; I had masked it to 11 bits.
  - VDP status: the VInt-pending bit is already clear inside the handler; bits
    15-10 are open bus ($EC00, the prefetched address word at the one read the
    game stores).
  - Sprite collision (bit 5) and overflow (bit 6) flags, set while drawing
    and cleared when read.
- **Known, accepted difference**: the sound driver's RAM drifts (from logical
  frame 397 in this run). On the original, lag VInts land somewhere inside the
  main code's work; the replay delivers them at the start of it, so a sound
  request can be consumed one interrupt earlier or later. It does not feed
  back into game state here; the sound driver is verified against the VGM
  rips in M5 instead.

### M5 results

- **Sound** (`runtime/sound.c`): the game's own Z80 DAC driver runs on floooh's
  `z80.h` with the Mega Drive Z80 map (RAM, YM2612, bank register, 32 KB window
  into cartridge ROM for the drum samples, PSG). YM2612 by ymfm
  (`runtime/ym2612.cpp`); PSG written here. Everything advances in Z80 cycles:
  after each VInt the rest of the frame (~59,737 cycles) is played out, with
  YM samples at 53,267 Hz interleaved so DAC writes land at the right moments.
  SDL resamples to the output device. Master gain ×2.25 (loudest song peaks
  around −2 dBFS).
- **Z80 handshake**: the 68000 driver's `StopZ80` retries while the Z80 flags
  it is busy (`$A01FFD`); every bus release lets the Z80 run 32 cycles (taken
  from the frame's budget), as it would during those retries on hardware.
- **Verified against BizHawk**: `tools/bizhawk/jukebox.lua` plays every music
  ID from the same boot state and logs the 68000's YM2612/PSG writes per
  sound-driver tick (`UpdateSound`, counted by a generator hook on the port's
  side, so lag doesn't misalign anything). **52/52 IDs identical: 788,683
  writes**, 30 seconds of each song.
- **Against the VGM rips**: chip state compared frame by frame
  (`tools/vgmstate.py`). "The End of the Millennium" and "Motavia Town" are
  100% identical, but rips carry artifacts (register dumps at the start and at
  loop points, leftover values on silent channels, a 60.00 Hz tick instead of
  59.92), so they are for listening rather than proof.
- **Not machine-checked**: the Z80's DAC stream (BizHawk can't see Z80
  writes), ymfm's and the PSG's synthesis, and the mix. Check by ear against
  `mp3/`.

### M6 progress

- **Player** (`psiv.exe`): `psiv.ini` settings (scale, fullscreen, integer
  scaling, 4:3 aspect, filter, volume, keyboard and gamepad bindings), ROM
  picker that accepts SMD dumps, battery saves in `saves/psiv.srm`, crash and
  hang reports (`psiv-crash.txt`, with the 68000 call chain by function name).
- **Input movies** by logical frame drive the port and BizHawk identically;
  `tools/movies/try.py` branches new moves off a movie with screenshots.
- **Battery saves verified**: the port's save is the cartridge's odd bytes
  (8 KB); BizHawk's "SRAM" domain holds them at odd offsets. Saving in slot 1
  after the opening: 7,091 frames identical and the save identical byte for
  byte. Movies can start from a save (`# from-save:`); Continue loads the
  field at frame 685.
- **Live play as a test**: `check.py --play` records your own session in
  BizHawk (from BizHawk's own save, so Continue works) and the port replays
  it; `trace2movie.py` turns a session into a committable input movie.
- **Battles and the beam counter**: the first recorded battle diverged 19
  frames in (the port rolled an ambush, BizHawk a normal start). The battle
  random numbers (`UpdateRNGSeed2`, the game's only read of it) mix in the
  VDP's HV counter through `move.w $8(a5),d0` with a5 = the VDP data port, and
  its value depends on cycle timing the port doesn't model. Traces (v5) now
  log every value read (`hv:` column, from an exec hook right after the read)
  and the port's replay feeds them back, like pad input. Normal play still
  approximates the counter (beam line, H = 0), which is less random than
  hardware; deriving it from executed work is a polish item.
- **Return-address trick in battle windows**: `Battle_CreateWindow` and
  `Battle_DestroyWindow` do `addq.l #2,(sp)` once the window is fully
  open/closed, so the caller skips the `rts` after the call and runs the next
  instruction (61 call sites, e.g. `move.w #2,(Battle_Routine_2)` to hand over
  to the menu). Calls now check the address the callee returned to and, if it
  changed, continue at the recompiled function there (`m68k_resume`); every
  one of the 61 continuations is one. The trace build still stops on a changed
  return address with no function behind it.
- **Comparison blind spot fixed**: the stack's 4 KB block was hashed only from
  sp up, which skipped $4000-$47FF, i.e. all battle state (`Battle_Routine`,
  enemy stats, fighter objects, vehicle stats). Trace v5 hashes those
  variables and the live stack separately, leaving out only the dead stack
  ($4800 to sp).
- **Verified so far**: 2,348 frames (scripted new game), 6,830 (the whole
  opening), 7,091 (opening + save), and **PsiLok's first live session: 24,635
  frames, 7 battles, all identical, and his in-game save identical byte for
  byte** (`session1.movie`, from his own save).
- **To do**: further into the game (more recorded sessions: towns, dungeons,
  vehicles, techniques), packaging; normal-play beam-counter realism.

## Open risks

- **BizHawk execute hooks on the Genesis core**: the reference harness assumes
  `event.onmemoryexecute` works with BizHawk's GPGX core. Checked first in M4;
  fallback is comparing at a RAM frame counter instead of at `WaitForVInt`.
- **Object-field function pointers** (4 sites): only confirmable at runtime;
  the dispatcher's loud failure on unknown targets plus the reference harness
  will catch any target the static analysis missed.
- **Raster effects**: anything the game does mid-frame through the horizontal
  interrupt must be reproduced by the scanline renderer.
