# Enhancement ideas

Milestone M7. Each one is noted with the part of the architecture it depends
on. Rule for all of them: presentation only unless stated, so the game's own
state (and the BizHawk comparisons) never change; Classic mode turns them all
off.

## Depth in battles -- v1 done (`runtime/depth.c`, F9: off / subtle / strong)
Battles redrawn as layers at different depths under a slowly moving camera:
background, enemies (+ damage numbers), action sprites, party, windows. Idle
drift, a lean-in and zoom while someone acts, a shake when damage appears;
composited at the window's resolution so motion is sub-pixel smooth.
- **How PSIV draws a battle** (found with `psiv_headless --layers`): background
  on plane B; everything else as plane A tiles -- enemies at low priority,
  party and windows at high priority, told apart by tile number (party
  $180-$27F, windows $680-$7FF and icons below $180, damage numbers $5E0-$5EF);
  sprites only for cursors and a character mid-attack (low priority).
- Next: battle **fog**, hit **flashes / flares / particles** (below), vehicle
  battles (different layout, not checked yet).

Game context (enemies, techniques and their elements, locations):
https://phantasystar.fandom.com/wiki/Phantasy_Star_IV:_The_End_of_the_Millennium

## Battle atmosphere and effects (PsiLok, 2026-11-03) -- fog, light, sparks, embers, flares done
- **Fog between the party and the enemies**: an animated, semi-transparent
  layer in the depth stack, tinted from the battle background.
- **Attack effects**: hit flashes, optical/lens flares, glow on spells.
- **Particles**: sparks on hits, dust, embers for fire techniques.
- **Needs:** events -- an attack starting (action sprites appear), a hit
  landing (damage numbers appear), techniques (the command data in
  `Battle_Command_Data`), all readable without touching game state.

## System menu (Esc) (PsiLok, 2026-11-03)
An overlay menu on Esc (quitting moves into it): resume, settings, quit.
- **Classic / Enhanced** master switch (Classic = the faithful port).
- Per feature: battle depth level, fog, effects, original or modern text,
  4:3 or 16:9, filters, volume, controls.
- **Needs:** an overlay renderer with its own font (SDL3's built-in debug
  font to start, a proper font later) and saving to `psiv.ini`.

## Widescreen side panels (PsiLok, 2026-11-03) -- v1 done
In exploration, use the 16:9 margins as frames with party info instead of
more map: each member's HP/TP, status, EXP, the inventory. Read from the
party's RAM (stats, items) and drawn with the modern-text renderer; an
option next to plain widescreen.

## Modern battle UI (PsiLok, 2026-11-03) -- v1 in progress (`runtime/battleui.c`)
An Enhanced option replacing the game's battle windows: full action names
(Attack, Technique, Skill, Item, Defend), technique and skill names with
short descriptions (our own wording), ROM portraits, HP/TP bars. It mirrors
the game's own menu state from RAM (the game still handles input and the
battle). The windows are hidden in the renderer: wherever plane A holds a
window tile, the cell is drawn from the game's own window-free copy of the
battle map ($FF2540, which it uses to restore what a window covered), and
the cursor sprites are left out; the remaining windows (enemy names,
messages) are read back from their tiles and redrawn in the same style.
- **The whole party while commands are chosen** (PsiLok, 2026-11-04; done): the
  game clears everyone but the member choosing from plane A *and* from the
  window-free map, and its command window covers part of that member (the
  vertical cut PsiLok saw). Their art stays in VRAM (fighter +$E = its VRAM
  address, +$C = x + 128; blocks of 6x6 cells, rows 15-20), so the port keeps
  each member's name-table cells from frames where they stand in full and
  draws them back; it holds them after the last command until the game draws
  someone itself. Gold aura on the member choosing (their silhouette from the
  frame, blurred, never over the party's own pixels; motes rising), cyan on an
  ally target. Commands moved to a strip the dock opens under the party; lists
  beside the party where they fit, else above everyone's heads.
- **The whole party for the whole battle** (PsiLok, 2026-11-04; done): while
  the battle plays out the game also shows only one member -- the one acting
  (Current_Actor_Index $4142 = fighter + 1; attackers appear as sprites at
  the enemy and reuse their own VRAM for the attack frames) or the one an
  enemy attacks, *moved* (fighter +$C) in front of that enemy; casters move
  to the centre. The port keeps a copy of each member's tiles from the main
  menu and draws absent members from it, behind low-priority sprites
  (vdp_plane_a_flags, vdp_tile_shadow); the one acting stays out while
  low-priority sprites are on screen; a member the game moved goes back to
  their place with the game's picture of them (white hit flash = their
  cells in palette 2, damage box = tiles $5E4+ inside their block), and the
  sprites on them follow, weighted by height so a bolt from the enemy bends
  to them (vdp_sprite_dx, display only). Messages that would cover the party
  go to the top; party cards are ordered by fighter +$13 (place in the line,
  set from the first frame, even in an ambush). An ambush still shows the
  original view until the first main menu (nobody to copy yet).

## Public release (PsiLok, 2026-11-03)
Players bring their own ROM; nothing from the game is in the repo or the
release. All art and text the enhancements show is read from the ROM at
runtime (romart.c). Not shippable: the character art in characters/ and the
Splice samples (sfx/; their licence forbids redistributing the samples);
Segoe UI (Microsoft) -- bundle an open-licence font instead. The executable
contains code translated from the game: safest is publishing the source and
build scripts so players build the port from their own ROM.

## Ideas from Gen1Recomp
Its options and modules map onto this port: `Tilt` (an HD-2D diorama
perspective for the field, eased between 0/15/35/50 degrees), survey `Zoom`,
`WideBattle` + fill past the map edges (widescreen), `Font`/`TextBox` (modern
text), `PaletteFX`/`Pipelines` (colour modes, filters), a mod system with
profiles (hooks, asset swaps -- music replacement fits there), rulesets
(faithful vs. quality-of-life), a save converter and save editor.

## Widescreen -- v1 done (2026-11-03)
Show more of the world to the left and right instead of a 320-pixel window.
- **Needs:** a renderer that can draw past 320 px. The VDP's planes are 512 px
  wide or more, so field maps often already have the tiles; where they don't,
  the map loader has to fill the extra columns.
- **Hooks:** sprite culling and camera clamping (the game skips objects outside
  320 px), and window/menu positions so the UI stays centred.

## Full text UI -- v1 done (2026-11-03): modern font in place; re-layout still open
Dialogue, menus and battle text drawn with a modern font at native resolution,
using the whole width instead of the original small windows.
- **Needs:** overrides on the text and window routines (control-code tables
  such as `TextCtrlCodesJmpTbl` and `Win_CtrlCodesJmpTbl` are already resolved)
  so the port receives the *text*, not the 8×8 tiles.
- **Bonus:** the same hook makes a retranslation or a font swap easy.

## Cheats
Toggleable cheats from an in-game menu: stat and money edits, encounter
on/off, experience multiplier, walk through walls, and classic Game Genie / Pro
Action Replay codes.
- **Needs:** the runtime owns all memory, so RAM cheats are a frame hook and
  ROM patches are a byte overlay. Game Genie/PAR codes decode to these.
- **UI:** an overlay menu (settings and cheats together).

## Generated sound effects for the public build (PsiLok, 2026-11-03) -- v1 started: blade hits, slasher, cursor, choice (`runtime/sfxsynth.c`)
The Splice samples stay local (`sfx/` + `sfx.ini` is a personal override);
a release needs effects of its own. Plan, best fit first:
- **Synthesized in the port** (the default pack): each effect a short recipe
  rendered at startup -- filtered-noise whooshes with a swept band-pass
  (swords), a whoosh with a ~14 Hz wobble and a doppler arc panned across the
  stereo field (boomerang), modal "ring" partials (metal), pitch-dropping sine
  thumps plus a noise crack (hits), crackle through resonant filters (fire),
  shimmering arpeggios (healing), short blips (menus), a synthetic reverb.
  Nothing to ship but code; deterministic, so the replays stay checkable.
- **Done so far** (Esc > Settings > Sound effects: Modern): $F5 every blade
  hit (the request comes as the blow lands; also each enemy the slasher
  passes through), $B7 Alys's throw (the first hit follows 38-41 frames
  later, so the return is timed to it), $F2 cursor, $F3 choice; levels within
  ~2 dB of the originals in the mix, a soft knee where they meet loud music.
  Next candidates by frequency in a session: BA/D8/D7 enemy attacks, B9 enemy
  killed, B8 miss, the technique sounds (BB, BE Foi, C6 Zan...).
- **Remastered originals** (an option): the game's own FM effect, with a
  modern layer added at runtime (sub-bass on hits, a transient, stereo room).
- **AI-generated** (later, only for what synthesis does badly, e.g. monster
  voices): a text-to-audio model on Hermes, best takes curated and shipped as
  our own files -- the model download needs PsiLok's approval and its licence
  checked for the outputs.

## Modern music and samples
Swap the sound bank for something more modern, as an option next to the
original chip sound.
- **Levels of ambition**, cheapest first:
  - Replace the drum/voice samples the Z80 streams (the DAC bank in ROM) with
    higher-quality ones.
  - Keep the game's own note data but play it on new instruments: the music
    driver's notes, instrument changes and volumes are visible to the port
    (the same state `tools/vgmstate.py` compares), so each FM instrument can
    be mapped to a sampled or synthesized one, like driving a modern synth
    from MIDI.
  - Full replacement tracks: per music ID (`MusicID_*`, e.g. `$8F` Meet Them
    Head-On), play a rendered audio file instead of the chip music, with loop
    points. That makes remixes possible as a selectable soundtrack.
- **Needs:** a hook on the sound driver's song request (`PlaySoundID` /
  `Sound_Index`), so the port knows which song is playing, and a mixer that
  can mute chip channels and play the replacement in sync.

### Algol, a modern sound chip (PsiLok, 2026-11-04) -- v1 done (`runtime/algol.c`)
The second level above. The chips still run for the game (it reads their
status); every write also goes to Algol, which keeps its own copy of the
registers and decodes notes: key on/off from YM $28, pitch from F-number and
block times the main carrier's multiple (the loudest carrier), loudness from
the carriers' total levels, pan from $B4, the patch as a hash of its
operator registers (carrier TLs left out: those are the volume). PSG: period
and attenuation per channel (the driver's software envelope becomes the
voice's amplitude), noise as filtered noise. Drums: the 68000 writes the DAC
driver's mailbox in Z80 RAM -- $1F02 pitch, then $1F00 the sample (1-17;
table at Z80 $0500 -> banks $E0000-$F8000, 8-bit PCM at 2,000-10,300 Hz).
Samples 1/11/12 snare, 2/13/14 kick, 3 ride, 4 hat, 9 open hat, 5 tom, 6/10
crash are synthesized; 7 (a pitched instrument, Ryucross Field), 8 (a voice),
15-17 (effects) are played from the ROM, interpolated and smoothed.
- **Effects keep the chips:** the driver's sound-effect tracks ($FF5220-$53A0,
  $30 each: +0 bit 7 playing, +1 channel) say which channels an effect holds;
  ym_generate_mask() plays only those on the YM (a subclass of ymfm's YM2612,
  clocked as usual), the PSG likewise, Algol the rest.
- **Instruments** come from the patch's sound (alg 7 -> organ; low notes ->
  bass, or guitar if they die fast; slow attack -> strings / brass; decaying
  -> pluck / keys, or bell if a modulator is off the harmonic series;
  sustained -> lead / brass), a small table of picks by ear (`CURATED`), and
  `algol.ini` ("hash = instrument", hashes from `PSIV_ALGOL_LOG=1`). Exact
  doubles across channels (the driver's thickening trick) are merged.
- **Tuned against the original** per channel with `PSIV_CHANNELS=mask` (solo
  in both chips) and loudness / band balance measurements: each part within
  ~2 dB of the original, the mix at its loudness; matching the FM's fizz is
  not the aim (a modern tilt instead, with a little air on the master).
- **Hard rock drums for the tense songs** (PsiLok, 2026-11-04; done): the song
  playing is known from $FF5020, where PlayMusic leaves its voice table's
  address (unique for all 52; table MusicPtrs at ROM $D1C40). Rock style
  for $AC (the title), $8F, $95, $A0, $98, $A9 (algol.ini: `song XX = rock /
  standard`): rock variants of each drum, a four-line room and a parallel
  bus compressor, and a closed hat on the eighths -- the backbeat is the
  commonest gap between snares (rolls under 0.4 s ignored, ties to the one
  nearest ~0.85 s), re-locked on every snare on the grid (8F: 0.200 s
  eighths; the title: 0.213 s). The game's two drum tracks can write the
  same sample in one tick: deduplicated (the console plays one sample at a
  time; Algol's kit rings together).
- **Next:** per-song picks by ear with PsiLok (he produces: his choice of
  sounds is the point); the PSG echo copies as one voice through a real
  delay; a MIDI export of the decoded notes (for remixing); a "your bank"
  mode (an SF2 or samples you provide, like the sound effects).

## Smaller ones worth keeping in mind
- Fast-forward, and turbo for battles and text.
- Save anywhere. Harder than in an emulator: the recompiled game's position
  lives in the C call stack, not in a PC register, so a snapshot has to be
  taken where that stack is known. The game waiting for vertical blank in the
  field is such a point (the 68000 stack names every caller, as the crash
  reports already show); resuming means re-entering each function at its call
  site, which the generator could emit. Snapshots at that point only, not
  mid-frame.
- Integer scaling, CRT-style filters, fullscreen.
- Remappable controls and modern gamepad support.
