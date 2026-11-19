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
- **v2, five pages (PsiLok, 2026-11-05: "clean up the settings screen. It's
  starting to get crowded"):** 18 settings in one list became Game (Mode,
  Text, Dungeons), Display, Sound, Battles and Controls, two to five each,
  with short labels and a line under the list saying what the highlighted
  one does (Classic mode greys the additions and says why). A page bar
  above the list (Up from the first setting reaches it; Left / Right there
  turn pages), Tab / Shift+Tab / Page Up / Page Down, the shoulder buttons,
  or a tap on a page; hints for the device in use, naming the gamepad's own
  buttons. Drawn with uikit like the battle menus -- rounded navy panel,
  gold highlight, the modern font -- into a transparent texture
  (`ui_set_straight_alpha`: uikit and hdtext composite with straight alpha
  while it's on), redrawn only when something on it changes, fading in;
  SDL's 8x8 font if no font loads (`PSIV_MENU_8X8=1` shows it). Taps and
  clicks use the rectangles the drawing recorded; the mouse highlights the
  line under it. Testing: `psiv --menu F:settings:PAGE:ROW` and
  `--menu-script` (keys, clicks, hover, gamepad buttons, shots).
- **The launcher (PsiLok, 2026-11-05: "when launching the app it presents as a
  launcher with start game, settings, a toggle for classic or enhanced"):**
  the app opens on Start game / Mode (a two-way switch) / Settings / Quit
  before the game boots, over a field of drifting, twinkling stars (the
  brightest with four points) under the emblem: the icon's star from
  make_icon.py's formula (|x/a|^0.46 + |y/b|^0.46 < 1, white heart to gold
  tips, two sparkles), supersampled at the window's size, with a breathing
  halo. A line under the choices says what the highlighted one means (the
  ROM's state for Start game). The ROM is looked for quietly at startup;
  Start game asks for it if need be, and a cancelled picker returns to the
  launcher. Settings from the launcher come back to it. Touch is all taps
  there (`touch_suspend`); the left stick steers every menu now; F11 works
  in menus. `--play` skips it.
- **Next:** a "Return to the launcher" in the pause menu needs the game
  restarted from scratch (its state lives in the recompiled code's C stack):
  a longjmp out of `m68k_call` and everything re-initialised -- possible,
  not done.

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
  low-priority sprites are on screen. A member the game moves (a caster or an
  archer, the one an enemy attacks) stays where the game stands them, and
  whoever's place that is shows behind them pixel by pixel
  (vdp_plane_a_back). Until 2026-11-06 they went back to their own place and
  the sprites on them followed, weighted by height (vdp_sprite_dx): that
  tore sprites apart (an enemy's lunge left its tail behind, a technique's
  ball split in two), and an effect that runs from a caster's hand up into
  the enemy can't be in both places, so it was removed. Messages that would cover the party
  go to the top; party cards follow the order the party stands in (each
  one's x at the menus). An ambush still shows the original view until the
  first main menu (nobody to copy yet).
- **Tried across the whole game** (PsiLok's fan saves, 2026-11-04): 44 saves
  from chapters 1-6 (Gens .srm: every byte of $200000-$201FFF, slot 1),
  loaded as they are (bus_sram_import; `psiv --save FILE`). All 44 Continue
  into the field; 43 reach a random battle by walking back and forth (the
  one atop Soldier's Cave has none), several after placing the party a few
  steps off the town tile it was saved on (slot checksum: 16-bit sum of the
  $A00 bytes). `tools/battle_probe.py` shot every party's commands, lists
  and a round of attacks, which found: (1) fighter +$13 is the character's
  ID + 1, not the place in line -- it matched only for Chaz, Alys, Hahn;
  the cards now sort by x. (2) Kyra's throw (and a caster's) is drawn in
  the middle of the line with her x unchanged, over Chaz, so she stood twice
  and he vanished; a member not in the columns of their x is now looked for
  by their tiles (matched against the copy from the menus) and moved back
  like one the game moved. (3) Long skill descriptions were cut at two
  lines; the list grows to the longest (up to four). Six of those battles
  (saves 8, 16, 21, 25, 41, 44: every playable character, Motavia,
  Dezolis, Rykros) match BizHawk frame for frame, battery saves too.

## Public release (PsiLok, 2026-11-03)
Players bring their own ROM; nothing from the game is in the repo or the
release. All art and text the enhancements show is read from the ROM at
runtime (romart.c). Not shippable: the character art in characters/ and the
Splice samples (sfx/; their licence forbids redistributing the samples);
Segoe UI (Microsoft) -- bundle an open-licence font instead. The executable
contains code translated from the game: safest is publishing the source and
build scripts so players build the port from their own ROM.

### Alpha packages (PsiLok, 2026-11-04: "playable on Mac, Android and Linux ... install supplying it the game")
- Done: Windows zip, portable Linux tarball (glibc 2.35+), Android APK
  (arm64, Android 7+), each with no game data and `docs/TESTERS.md`. The
  first run installs the ROM (dialog, dropped in the folder, or zipped) and
  checks it. Noto Sans (OFL) bundled; Segoe UI only where Windows has it.
- The binaries still contain the translated game code (see above): shipping
  them is how comparable recomp ports are released, but it's PsiLok's call.
- Next: macOS (an app bundle, universal, ad-hoc signed; needs the MacBook);
  32-bit ARM and x86-64 Android if testers ask; a CRC/SHA list of other
  dumps that are byte-identical (the Steam / collection releases?) to accept;
  Android performance on low-end phones (depth and first person compose on
  the CPU at up to 4x).

## Rumble (PsiLok, 2026-11-05: "Build in rumble") -- v1 done (`runtime/rumble.c`)
Hits, criticals, kills and the field's big moments on a gamepad's two motors,
or a phone's vibration motor. Read from RAM, never written: the session
replay still matches all 24,635 frames, and its rumble log is identical on
MSVC and gcc (42 blows on enemies, 12 on the party, 5 criticals, 13 enemies
destroyed, 3 chests).
- **When a blow lands:** not the HP. The game takes the HP off about a second
  after the blow, as the damage number finishes (FighterShowDamage_DecreaseHP).
  The impact is the target's hit reaction: its fighter routine (+2) becomes 5
  (Character_DamageAnimation / Enemy_DamageAnimation, the blink and a
  critical's white flash), with Fighters_Hit_Flags ($FF4150) saying hit /
  critical / missed. A sword's slash sound comes 8 frames before it (the
  swing), an enemy's blow's sound on the same frame. Battle_Heal_Damage_List
  ($FF415A) already holds a sword's damage then, not yet the boomerang's or
  an enemy's (0 / -1): those get a fixed strength.
- **Criticals:** one big blow. The game plays the hit sound 4 times, 4 frames
  apart -- for enemies' blows on the frame of the reaction, for swords from 8
  frames before it, so beats timed from the reaction would trail the sound.
- **Falls and kills:** a party member's HP reaching 0 (when their number has
  been shown); an enemy's $B9 (Enemy_Dead, as it dissolves); the final bosses
  (Dark Force, Profound Darkness, Zio's last form) skip $B9, so their HP
  reaching 0 (untested: no save that far).
- **Sounds:** the queue at $FF500A, read before VInt (the driver clears every
  slot each tick). Each rumbling sound is shaped to its own envelope, measured
  from the original (`psiv_headless --music ID:60 --wav`, DC removed: the
  chip mixer idles at ~0.035 FS). The conveyor belt loops for as long as the
  belt runs: left out.
- **Output:** taps with an envelope per motor, summed so they saturate
  (1 - prod(1 - a)); levels 0.45 / 0.75 / 1.0. Gamepads: SDL_RumbleGamepad,
  200 ms at a time, renewed every 100 ms while an effect lasts (a stall can't
  leave it on); on Android at most every 50 ms and 16 steps (each change
  restarts the vibrator through Java; gamepad rumble needs Android 12+). A
  phone, or controls that can't rumble on mobile: the device's vibrator
  (SDL haptic "VIBRATOR_SERVICE", VIBRATE permission), one pulse per effect,
  at most 0.5 s. The menu and pause stop it; the setting plays a sample.
- **Next:** PsiLok's verdict on the Odin 2; the swing as a light whoosh before
  the impact; the enemies' attack styles (bites, lasers, claws); trigger
  rumble on Xbox pads; walking with poison (the game's field HP ticks);
  vehicles; a heartbeat when the party's HP is low.

## Ideas from Gen1Recomp
Its options and modules map onto this port: `Tilt` (an HD-2D diorama
perspective for the field, eased between 0/15/35/50 degrees), survey `Zoom`,
`WideBattle` + fill past the map edges (widescreen), `Font`/`TextBox` (modern
text), `PaletteFX`/`Pipelines` (colour modes, filters), a mod system with
profiles (hooks, asset swaps -- music replacement fits there), rulesets
(faithful vs. quality-of-life), a save converter and save editor.

## Widescreen -- v1 done (2026-11-03); margins from the map (2026-11-05)
Show more of the world to the left and right instead of a 320-pixel window.
- **Needs:** a renderer that can draw past 320 px. The VDP's planes are 512 px
  wide or more, so field maps often already have the tiles; where they don't,
  the map loader has to fill the extra columns.
- **Hooks:** sprite culling and camera clamping (the game skips objects outside
  320 px), and window/menu positions so the UI stays centred.
- **v2 (PsiLok, 2026-11-05: "tiles, NPCs and other objects ... spawn from the
  right of the screen or produce inaccurate visuals"):** the planes did NOT
  hold the margins reliably. RefreshTilesAsYouMove draws a chunk column 14
  chunks ahead when moving right but just in time when moving left, and chunk
  rows 16 chunks across from the camera's chunk, so the left margin held stale
  strips (8-112 of its 196 cells in PsiLok's session) and at a map's edge the
  margins showed the map's other side (the plane wraps). The right margin was
  always right. `runtime/widemap.c` now draws the field's margins from the
  layout + chunk table in RAM, after checking the screen agrees (windows' tiles
  left out; the table is only taken from RAM when it agrees too: the game puts
  it back ~5 frames after a window's save pointer drops). Past a map's edge:
  its edge chunks repeated. The sprite builders keep pieces from x -96 (was
  -63). Objects the game creates just off the 4:3 screen (scripted walk-ins)
  still appear in the margins as they're created; none in PsiLok's session.
  **Next if wanted:** fade such objects in; side-panel mode unchanged.

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
- **The battle set (PsiLok, 2026-11-05: "now that we have more battle data
  from spells and techniques, could we generate more sound effects")**: the
  test bot's battles over the fan saves, with every request logged against
  the command in force (`tools/sfx_census.py`), ranked the rest: $E8 claw
  (664 in 43 runs), $B9 enemy killed (505), $DD grave opening (an enemy's
  attack in battle, 456), $C6 Zan (342), $BA and $D5 enemy blows and bites
  (269 each), $B5 rod (245), $BE Foi (200), $CC Res (177), $BB cast (163), $B6
  Wren's shot (158), $C4 Moonshade (153), $D7 swipe, $BD heal cast, $B8 miss,
  $C7 Gra, $CD recovery, $C2 fire breath, $BC buff, $CB Brose, $C5 laser (Tsu's
  thunder). All 21 synthesized from six shared layers (swept noise, a gliding
  FM tone, a thump, crackle, glass bells, amplitude pulses), each keeping its
  original's length and shape (`tools/sfx_study.py`: spectrograms side by
  side) -- Gra's is one deep pulse, since the game asks for it every 8 frames
  of the wave. Levels: 1 dB under each original's loudest quarter second.
  The originals take a music channel while they play and these don't, so
  the mix limiter works more: on 3-6% of a bot run's samples, at most 1.5-3
  dB (was ~1%). Next: the rarer techniques (Megid, Legeon, Tandle, Phonon,
  Rimit, Eliminat...), enemies' $D8/$DA/$D9, and the field (doors, chests,
  stairs, the elevators).
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

### Algol, a modern sound chip (PsiLok, 2026-11-04) -- v1 done (`runtime/algol.c`; now *Proto Algol*: the name went to v2, the chip remastered, `runtime/algol2.c`, 2026-11-14)
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
  for $AC (the title), $8F, $95, $A0, $98, $A9, $AA (algol.ini: `song XX = rock /
  standard`): rock variants of each drum, a four-line room and a parallel
  bus compressor, and a closed hat on the eighths -- the backbeat is the
  commonest gap between snares (rolls under 0.4 s ignored, ties to the one
  nearest ~0.85 s), re-locked on every snare on the grid (8F: 0.200 s
  eighths; the title: 0.213 s). The game's two drum tracks can write the
  same sample in one tick: deduplicated (the console plays one sample at a
  time; Algol's kit rings together).
- **The ride in the louder sections** (PsiLok, 2026-11-04; done): the battle
  and boss themes are sectioned (8F: riff, build, melody, bridge; loop 42
  bars) but evenly loud -- the melody is only ~1 dB (K-weighted) over the
  rest -- so a live loudness detector would switch late or flap, and
  battles mostly hear the first pass. Instead `RIDE_MAP` holds each song's
  louder sections in driver ticks from its start (the driver is
  deterministic: same tick, same bar), with its loop, measured by
  `tools/algol_sections.py` (band-only Algol render, per-bar BS.1770
  loudness, drum hits, the loop from the FM key-ons). Algol counts ticks
  from the song's start (a new voice table, or the music tracks starting
  again; unknown if Algol is switched on mid-song), switches two ticks ahead
  of the downbeat, and the eighth there is a crash. The ride is one cymbal
  re-struck (wash that builds, stick ping, a little bell on the beat), not
  a voice per stroke. Laughter ($AA, Zio) joined the rock songs (evenly
  loud: hat throughout). The follower now locks only when half the recent
  snare gaps agree, and holds that tempo: Red Alert's syncopated snare had
  drawn a hat at the wrong speed, and the K4 S4 bars of The King of Terrors
  had pulled it to a 0.113 s eighth.
- **Next:** per-song picks by ear with PsiLok (he produces: his choice of
  sounds is the point); the PSG echo copies as one voice through a real
  delay; a MIDI export of the decoded notes (for remixing); a "your bank"
  mode (an SF2 or samples you provide, like the sound effects).

## First-person dungeons (PsiLok, 2026-11-04) -- v1 (`runtime/fpview.c`), PAUSED 2026-11-05
From Gen1Recomp's 3D mode and Phantasy Star I's mazes: the dungeons in
first person, from the game's own tiles, and their battles fought there.
- **Paused (PsiLok, 2026-11-05):** "it has more flaws that get in the way of
  the experience" -- on hold until he works out how it would play well. The
  setting and its menu entry are gone (an old `enhance.first_person` line in
  psiv.ini is ignored); the code stays, dormant: `PSIV_FIRST_PERSON=1` turns
  it on in the app, `psiv_headless --fpview S` still renders it. To bring it
  back: the setting in `settings[]`, its row in GAME_ITEMS, and the
  `fpview_set_enabled` line in `apply_enhancements` (main_sdl.c). Before
  reviving it: write down the flaws PsiLok ran into (not recorded yet) and
  decide what a functional version needs.
- **The map, live from RAM:** Map_Layout_FG/BG ($FFA000/$FFB000, a byte per
  32x32 chunk, rows of Map_Row_Size+1), Chunk_Table ($FF6000, 4x4 name-table
  words per chunk, bit 14 = collision; up to 256 chunks, so a big map's run
  on past $7000 -- the disassembly's Text_Buffer -- and the Plate System
  lost its end walls while only 128 were kept); $FFEC24 picks the layer
  walked on.
  A 16x16 cell's collision is GetChunkAndCollision's (bit 14 of its four
  words; 8 solid). Every visible tile matched VRAM (1,120/1,120 per plane).
  The leader is Character_1 ($FFC000): x +$30, y +$34 (stands on y+16),
  facing +6. Colours: the tile patterns in VRAM and CRAM, so fades and
  animated tiles carry over; palette lines are voted from what the game drew.
- **The look:** a raycaster on the 16x16 grid (66 degrees, walls 2 cells,
  eye 0.9). A face the game draws (a wall over a floor, seen from the south)
  takes its own art: the stack of cells above where it meets the floor; the
  other faces take the front of their own wall (found below them), else the
  map's commonest 2-cell front. The floor is the map in perspective; the
  ceiling dark; distance shading. Drawn at up to 2x, ~6 ms a frame.
- **Gotchas:** a window saves the plane mappings it covers over the chunk
  table ($FFEC98, the end of what is saved, is $6040 when none is open): the
  table is kept from frames with no window. The game's windows are found as
  plane A cells that differ from the map they hold (the plane is a 64x32 ring
  of the map; the pixel -> cell mapping from the VDP's own scroll, as the
  camera variables lag a frame), keeping groups with window art (tile $55C
  up: text $580-$5FF, portraits, frames $6E9+); the temple's sparkles ($17F)
  and the Plate System's platform ($1CE-$1D4, drawn on plane A) are map art.
- **Battles:** the last field view is kept (the battle loads its own tiles);
  in the battle the backdrop (plane B) and the party's cells ($180-$27F) show
  it, enemies, sprites and effects stay in front; the battle menus' aura is
  off (the party isn't in the picture).
- **Controls (the app):** relative, each held key latched to the direction it
  meant when pressed (so a held "left" turns once and walks on); untouched
  while a window is open. The headless and replays get the game's own input.
- **Dungeon list:** by map name (FieldMapPtrs): caves, towers, forts, plants,
  mazes, the Air Castle, Kuran, Garuberk...; towns and houses excluded.
- **Objects (v2):** from Field_Objects_Memory ($FFC000, 64 x $40; the party
  is the first five, left out), not the sprite table -- the game only builds
  sprites for what is on its screen (Field_LoadSprites culls by y,
  Field_FillSpriteAttributes by x), while every object keeps its world
  position (+$30/+$34) and current frame (mappings +$C: count-1, a byte, then
  6-byte pieces y / size / tile word / x / mirrored x). Art: art_tile (+$16)
  in VRAM, or for objects that stream it (render flag bit 5: the party, some
  NPCs) straight from ROM at art_ptr (+$18) + the piece's word x 32 -- an
  object off screen hasn't had its art sent. Bit 1 = hidden, bit 0 = placed
  on the screen; a window's arrow and the like are also caught by checking
  the sprite position against FieldObj_CalcSpritePos (position + $80 - the
  walked layer's camera and step). Standing on an open cell: a billboard at
  1/24 cell per pixel (a person ~1.3 cells: the eye is 0.9, walls 2), its
  feet on the floor, drawn after the walls with a per-column depth test.
  On a solid cell: painted over the wall art it covers, at the game's own
  scale (the Machine Center's elevator: 1,008 of its 1,024 pixels match the
  wall under it while shut). People turn: FieldObj_Animate picks the frame
  from a script per facing (mappings_addr +8, frame mappings_idx +$10), so
  a person is shown with their facing turned by the angle from the camera
  (whoever faces you shows their front). Only routines named as people in
  FieldObjectsJmpTbl turn -- a chest's "facing" is open / shut, and its
  table has just those two scripts. Checked: the temple's chest, the Plate
  System's white chest ($1D4, the pedestal), Tonoe's Motavians from front
  and back (`PSIV_FPVIEW_ANYWHERE`), the 24,635-frame session still matches.
- **Props (v3):** what stands in front of the party in the 3/4 view is drawn
  with priority over the sprites (the temple walks plane B; its pillars,
  arch and wall bands are on plane A; the Machine Center's wall tops are
  priority tiles on the walked layer itself). On screen, plane A always
  matched the RAM map (`planediff.py`-style checks on visible cells): nothing
  is drawn "only on screen". A prop is priority art over a walkable cell
  (runs of at most 4 cells down a tile column: a longer one is the top of a
  wall running north, lying beside it), plus the priority art straight above
  it; 4-connected groups by 8x8 tile; each stands on the floor line under its
  lowest tile, past at most 4 solid cells (a wall's front; under more it is
  the edge of a wall's top beyond, and not a prop) -- the group's lowest, so
  an arch hangs over its passage at the height of its legs. Drawn 1:1 from
  that line: a wall's face there grows by it (its own solid cells opaque,
  the prop's art see-through around it), and over open floor it is a face
  of its own on that line. The floor no longer shows props; a face shows
  no prop standing on another line (a pillar's top over the wall behind
  it). The renderer walks each ray past the first wall while something
  taller could still show (the tallest face in the map bounds it), draws
  far to near, and keeps a per-pixel depth for the billboards. Temple:
  pillars 5.5-6.5 cells, the south wall 5 with its arch. Re-analysed when
  the layouts or chunks change (doors) and every 30 frames (tiles load in);
  ~7.5 ms a frame at 2x.
- **Doors:** every door event (Bio-plant, elevators, Tonoe's basement,
  Garuberk Tower, the Silence Temple) writes the map layout in RAM and
  refreshes the planes, so the view follows them: the Machine Center's
  elevator ($4F -> $53 under the leader on C) slides open in 3D.
- **Next:** objects lying flat (floor switches) painted onto the floor
  instead of standing; turning in place (the game can't: a turn is a step);
  a minimap; ceilings from map art; a cheaper render mode.

### A second screen: dual-screen Android handhelds (PsiLok, 2026-11-15) -- bookmarked, not started
"I do want to experiment a dual screen android model, specifically the AYN Thor ... I can see this project
benefit owners." He does not own one.
- **The Thor:** top 6" Full HD 120 Hz (Android's primary display: the game stays there), bottom 3.92" touch,
  1240 x 1080, 60 Hz, a separate display; Android 13.
- **How:** SDL gives an Android app one window, so the bottom screen is a small piece of the port's own
  Android code: a `Presentation` on the secondary display with a surface the port fills from the CPU (its
  menus and map are composed there already), taps coming back as clicks.
- **What it could show:** in the field the world map, always open, tap to walk; in battle the party's cards
  and the enemies' names; in towns and dungeons the Journal's next step and the party's status.
- **Testing without one:** (1) a second window on Hermes / the PC standing in for it; (2) the Odin on its
  dock: the monitor is a secondary display to Android, the same mechanism; (3) a Thor owner from the Discord.
  The AYANEO Pocket DS and the Retroid add-on treat the bottom screen as primary: a swap option, later.

### Smoothing and V-Sync (GitHub issues 3 and 5, 2026-11-15) -- done (`runtime/smooth.c`; `vsync_update` in `main_sdl.c`)
- **Issue 3** ("bilinear filtering turns off between some conversations"): with Smoothing on, a plain frame
  went to the graphics card at the game's size and was blended as it was scaled; a composed frame (modern
  text or menus, water, battle depth) went there already at 2 to 4 times the size, as sharp blocks, so the
  blend hardly showed. `smooth_soften` now averages the composed picture over one game pixel's width and
  height (odd scale: that many taps; even: one more, the ends at half weight, so nothing shifts) before the
  port's text, menus and panels are drawn. On pixels that hold still, a text-box frame and a plain frame
  now differ by at most one colour level at window scales 2, 3 and 4 (they differed by 35 on average).
  Cost at 4 times: about 0.8 ms a frame on eight cores, 3.8 ms on one.
- **Issue 5** (V-Sync): `video.vsync`, off by default. The game's own 59.92 Hz pacing stays; the wait for
  the display is kept out of the frame time `detail_watch` measures. Off while fast forwarding, while the
  title goes by unseen, and on a display under 59 Hz. Not tried on a real display yet (Hermes runs the app
  off-screen): only that the renderer accepts it and the log says so.
- **Issue 4** (pixel-art filters: 2xSaI, xBRZ, hq2x, hq3x, Scale2x): **Scale2x / Scale3x done**
  (`runtime/scalex.c`, from scratch; PsiLok: "Go with option 1 ... we'll follow up with xBRZ / hq2x after").
  A third Smoothing choice (`video.filter = scale2x`). The game's frame is enlarged through the filter to
  the composing scale (2: Scale2x, 3: Scale3x, 4: Scale2x twice) in place of plain blocks, on every frame,
  so the picture does not change when a text box comes up; the card's blend (linear) covers what is left
  to the window's size. Water and trees are drawn over it as before. The Display page's preview shows it
  (the clip's frame through Scale3x). Cost on Hermes at 4 times, a text-box frame: 2.6 ms of composing (2.2 as plain
  blocks). A plain frame, which went to the card as it was, is now composed too (not timed on its own).
  - **Battles with Depth (2026-11-15, PsiLok: "build the per-layer version"):** there is no game-size
    picture of such a battle to filter, so each of its six layers (and the sprites on the party, and the
    pan's picture of the others) goes through `scalex_upscale8` as palette numbers, 0 = nothing there, by
    the composing scale; `build_map_fine` gives the cameras maps into the enlarged layers and makes the
    game-size maps from them. One composer for both: with the filter off it reads the game's own layers
    through the same pointers. Checked on Hermes in a Boss Arena fight: Off and Soft are identical to the
    build before, file for file, on ten frames (mid-attack ones among them); with Scale2x the sprites are
    where they were (the best match of the game's pixels is at no shift). Cost at 4 times: 0.7 ms a frame
    more on eight cores; on one core 4 times no longer fits a frame and the picture detail steps to 3.
  - **The port's own portraits (2026-11-15, PsiLok: "can the portraits in the battle menu also get the same
    treatment"):** `ui_image` follows Smoothing (`ui_set_image_look`): Soft blends the four pixels round each
    point; Scale2x reads a copy enlarged through the filter (2, 3 or 4 times, by how much the portrait is
    being enlarged; kept per portrait and factor). Every `ui_image` caller gets it: battle windows, side
    panels, the menus' and the launcher's portraits. A portrait shown at less than twice its 48 pixels (the
    small ones while a member chooses, at a 3x window) has nothing to filter and is unchanged. Off is still
    identical to the build before, file for file.
  - **Not filtered yet:** first-person dungeons (paused anyway).
  - **For a filter that blends colours** (xBRZ, hq2x): it cannot work on palette numbers. `fine_make` is
    the one place a layer is enlarged: such a filter would get the layer as colours with an empty value,
    and the composer would read colours instead of numbers (the storm's tint is applied by palette today).
  - **Next:** xBRZ and hq2x / hq3x. Both exist as free code (the port is GPLv3, so the licences fit), but
    bringing it in is a download that needs PsiLok's word; xBRZ is C++. 2xSaI would go the same way.

### World map (PsiLok, 2026-11-15) -- v1 done (`runtime/worldmap.c`; the UI in `main_sdl.c`)
"Would it be possible to build a map feature for the world maps?" His picks: both a corner
minimap and the whole map on a button; names for towns and dungeons, click to walk there,
explored areas only, zoom and pan; off by default.
- **Where the map is:** a planet is 128 x 128 chunks, kept in the ROM as sixteen uncompressed
  slabs of eight rows a layer (pointer tables loc_107DC2 / loc_10BE02, Motavia FG / BG;
  loc_115584 / loc_1175C4, Dezolis). RAM holds four at a time (slab s at (s & 3) * $400, the
  camera's and the three below: loc_53A26, RefreshMapLayout), patched as they are copied for
  what the story changed (loc_53BB6: Zio's Fort, the spaceports, the Machine Center, the
  Edge ...). The map is the ROM's slabs with RAM's laid over whenever the party is there.
  Dezolis' slabs 8-15 are slab 7 again: its map is the upper 128 x 64. Rykros (map 2) is an
  ordinary map, 64 x 64 chunks, whole in RAM.
- **The picture:** each chunk from the chunk table and VRAM as palette indices with the
  priority bit, plane A over plane B as the VDP mixes them, colours from CRAM. Kept at 1/2 to
  1/16 (RGB565, a 16x16 block per pair of chunks met, so the sea costs nothing; ~10 MB);
  full size and larger drawn on demand. Made again a slab's rows a frame (under 1 ms) when
  the table or the palette changes and 90 frames after the field settles. The sea is
  animated by turning a few palette entries round, so "the palette is steady" compares the
  colours whatever their order.
- **Places:** the worlds' "map transition data 2" entries, by name, in `PLACES`. An entry's
  cell is the leader's object at the way in's top left; the cells that lead in (collision
  code 1) are a block from the cell below: 4x4 at most for a city (range 1), 2x2 a town (2),
  two a cave or tower (9), one (3). A place is named only while such a cell is there, so
  nothing the story hasn't built yet is given away; the hidden cave on Motavia is left out.
- **Walking there:** a search over the whole world's collision (65,536 cells), then walkto.h
  six cells at a time -- a waypoint the screen shows, since walkto reads the slabs in RAM --
  set again three cells before each. A battle holds it; the d-pad, A, B or Start end it.
  Explored: only over cells the map shows.
- **Explored:** a byte a 16x16 cell, an ellipse round the leader each frame the field shows
  a world; `saves/explored.bin`, one for the install. A new game clears it: the story's
  flags ($FFF100, 64 bytes) have exactly 11 bits set when the game opens in Piata.
- **Not paused** (PsiLok, 2026-11-15: "I'd like to have the music continue"): the whole map no
  longer holds the game. The frame loop runs on with the player's pad kept from the game (and
  until it is let go after the map closes); the map's own events are taken first. A battle, a
  window or an event closes it (worldmap_shown). The party's ring is a texture of its own, so
  a still map is never redrawn.
- **Next:** a record per save slot; the vehicles (their own collision tables); towns and
  dungeons; the map kept for a planet the party isn't on.

### Click to walk (PsiLok, 2026-11-04) -- v1 done (`runtime/walkto.c`)
"Mouse click to direct the player to the location." The click becomes a map
cell (the game's view: the walked layer's camera + the pixel, the frame's
column offset by `vdp_view_x0` in widescreen; first person: `fpview_pick`,
the floor under the click by inverting the floor projection, the cell before
the wall it meets, or the billboard drawn there -- a per-pixel object id),
and a BFS over the game's own collision finds the way:
- **Collision:** GetChunkAndCollision, mirrored exactly: the walked layer's
  layout with the *other* layer's row size as the stride (a quirk of the
  game's; the sizes match in practice), outside on the planets the chunk row
  & $1F (the world layout is a 32-row ring). On foot (TileCollNormalPtrs)
  8 solid, 9 water, $A sand, $B ice block stop the party, $C is a counter;
  1 changes the map (only taken when it was clicked: caves, towns, stairs).
- **Objects:** FieldObj_DoObjCollision checks Field_Obj_Secondary ($FFC300,
  52) with render flag bit 3, where each stands and where its step ends --
  not outside on Motavia / Dezolis. The search treats them alike.
- **Driving the pad:** the leader reads the pad only between steps
  (FieldObj_Move), so the walk holds the direction of the next step while
  one is under way, and lets go for the last. Off the path (a slide round a
  corner, someone in the way for 40 frames) it searches again from where the
  leader is. A clicked object: walk to its side, press towards it (bumping
  turns the leader, FieldObj_Collided), then C for 3 frames.
- **Ends:** any key or button of the player's, a window or an event, a
  battle, a vehicle, another map; right click. With text up a click is C,
  a right click B (clicking through a conversation).
- **Checked:** temple: a cell 7 west (stops exactly on it); the chest from
  behind the wall (35 cells round, faces it, "AERO-PRISM is procured!"); the
  same chest clicked in first person; Tonoe: a Motavian, talk, two clicks
  through the text; the overworld: a mountain (nearest cell), a cave entrance
  (goes in); a key mid-walk stops it. The app itself (window -> frame
  mapping, the marker) through `--test-keys` / `--test-click` with
  SDL_VIDEO_DRIVER=offscreen, both views. Movies and the 24,635-frame
  session are unchanged.
- **Battles (v2):** the modern menus record where each choice was drawn
  (frame pixels): the main options, the command strip, list entries and page
  arrows, and the targets (a band round each enemy present, through the depth
  camera; each party card, a band round where each member stands). A click
  is played as the pad, closed-loop on the game's own cursors: main options
  Up/Down (Battle_UpdateRedCursor2), commands and targets Left/Right
  (Battle_UpdateCursor), lists Up/Down within a page of four and Right/Left
  to the next page's top (the page turn ignores presses: pressed again);
  each press held 2 frames and let go 2 (the menus act on new presses), then
  C. Checked in the Plate System: Command -> Technique -> Tsu (page 2) ->
  the Seeker; Res -> Gryz's card; a right click backs out of the list.
- **Next:** vehicles (2x2 cells, their own collision tables); click to
  choose in the camp menu and shops; drag to keep walking.

### Your own music (PsiLok, 2026-11-04) -- v1 done (`runtime/music.c`)
PsiLok's Meet Them Head-On rearrangement as a bonus without shipping it (it is
a derivative of Sega's composition): players put their own files in
`music/`, by song ID or named in `music/music.ini`. The song is known from
$FF5020 (the voice table PlayMusic leaves; unique for all 52), a restart
from the music tracks coming back on. The driver's sound command $FB
(Sound_StopMusic in the disassembly) is a fade-out: $FF5004 counts 15 steps
down, one every other tick, each track +1 attenuation, then StopAllSound; the
file follows it (~1.5 dB a step). $FF5007 pauses. While a file plays, the
chips play only the channels the effect tracks hold, as under Algol. WAV
only (no decoder to fetch): PCM 8/16/24/32-bit or float, the "smpl" loop;
three files cached (battle, field, one more). A file starts at its first
sound unless music.ini says otherwise (an MP3's encoder delay held up the
battle by 1.7 s). Tested with PsiLok's render 2 (3:35, 146 BPM): battles from
fan save 8, all 7 battles of his session (24,635 frames still match), level
within ~1-2 dB of the original song, loops sample-exact with no click.
- **Next:** OGG / MP3 (stb_vorbis / dr_mp3, single-file public-domain
  decoders: ask before fetching); a fade-in to follow the driver's ($FF5028);
  the menu could list which songs have a file.

### Instrument banks from SNES games you own (PsiLok, 2026-11-04) -- v1: Lufia II (`runtime/snesbank.c`) -- PAUSED 2026-11-06
Taken out of `psiv.ini` and the README's feature list for now (PsiLok). The
code stays: `PSIV_MUSIC_BANK=path/to/rom` in the environment turns it on in
the app, `psiv_headless --algol-bank ROM` still renders it. To bring it back:
the `enhance.music_bank` row in main_sdl.c's settings table and its read in
apply_enhancements, and the README paragraph (git history, 2026-11-06).

Players with a copy of a SNES game can have PSIV's music played on that
game's instruments: decoded from their ROM at startup, nothing shipped (no
samples from the internet). PsiLok's picks are in `custom soundtrack ideas/SNES/`
(git-ignored): Battletoads & Double Dragon, Breath of Fire 1-2, Chrono
Trigger, Earthworm Jim 1-2, Final Fantasy III, Final Fight, Lufia 1-2,
Shadowrun, Star Fox, Super Turrican 1-2. Lufia II was the test.
- **How the samples were found:** SNES music is BRR samples (9-byte blocks:
  a shift, one of four prediction filters, 16 4-bit samples) in the sound
  chip's 64 KB, uploaded by each game's driver. A blind scan of the ROM for
  BRR is noise. Instead BizHawk (bsnes core: it has an "APURAM" domain;
  Snes9x has none) dumped the sound RAM while the game played; the sample
  directory (4-byte start / loop entries) was at $0100, and each sample's
  bytes were in the ROM, raw. Lufia II keeps every instrument in one chain
  of records from $0C3828: `[length][loop offset][ADSR1][ADSR2][tuning 8.8]`
  then the BRR data; `08 00 00 00 00 00 00 00` is an empty slot; a loop
  offset equal to the length means the sample plays once. 42 records, 37
  samples. The tuning is the waveform's period in BRR blocks (16 samples):
  the sample sounds 2000 / tuning Hz at 32 kHz -- confirmed by measuring
  every looped sample's period. The loader finds the longest such chain
  (so other releases of the game work too).
- **In Algol:** a voice whose instrument has a bank sample reads it (cubic
  interpolation, looped; two reads a few cents apart for the instruments
  Algol voices in unison) under Algol's envelope times the game's own (its
  sustain level and decay rate from ADSR2), skipping the synth's drive and
  filter; drums map the same way. The levels were matched to the synth's
  per channel (-10 dB at first; now within 0.5 dB on 8F and AC), and the
  pitch tracks of each channel checked against the synth's. Algol's synth
  output is byte-identical when no bank is set.
- **The voicing** (which sample plays what) was picked from the samples'
  waveforms, spectra and envelopes, not by ear: lead 35, brass 37, strings
  6, bass 26, pluck 25, guitar 13, bell 39, organ 10, keys 20, chip 21
  (23 first: its fundamental is weak, the 3rd harmonic led), kick 29, snare
  38, hat 17, crash 34, tom 28. PsiLok's ears decide; `algol.ini`: `bank lead = N`.
- **Other titles:** Lufia 1 uses the same records (48 at $0480C0): needs a
  voicing. The rest use other drivers and need their own recipes, found the
  same way (`tools/snes/sound_ram.py dump / find / chain`): Square's (Chrono
  Trigger, FF III), Capcom's (Breath of Fire, Final Fight), Factor 5's
  (Super Turrican), Nintendo's N-SPC family (Star Fox), and others. Some
  pack their samples; then the recipe has to unpack them too.
- **Next:** a menu entry (the ROM's path is a psiv.ini setting for now);
  auto-voicing from the samples' features for games without a curated one;
  the generated hat and ride from the bank's cymbals; SF2 export of a bank
  (for PsiLok's own production).

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

## The sound effects on Algol (PsiLok, 2026-11-16: "I want to attempt making new versions of the sound effects") -- built, awaiting his ears
His choices: the direction "chip remastered" (keep each sound the game's own: cleaner, fuller, in stereo, with
reverb -- as Algol did for the music); what bothered him in the Modern set: "thin or synthetic"; all 67 effects,
the field's too; a new choice in Settings beside the others.
- **How:** nothing is replaced. The driver plays every effect as before; the channels its effect tracks hold
  ($FF5220 on, already read for the music's sake) are played by Algol's FM / PSG instead of the chips, in a mix
  of their own. So it covers every effect at once, cut-offs, repeats and the conveyor belt's loop included, and
  the game cannot tell (the same Boss Arena fight ends on the same frame either way).
- **Which effect is on a channel:** PlaySFX leaves the effect's voice table at the track's +$20; `sound.c` reads
  the driver's table (SFXPtrs, by way of $D1A6C) once and matches. That picks the polish (five kinds).
- **Measured, not heard** (Hermes ~/psiv-lab/sfxr): dry against the chip, 65 effects, level within 0.6 dB,
  envelope 0.95+ (40 ms), spectrum 0.84+; polished, loudness -1.5 to +1.1 dB of the chip's. With the option off
  every render is byte for byte what it was.
- **Found on the way:** two copies a few cents apart only beat on a low voice (so a low voice gets one), are still
  in step at a short sound's start (so the two are 1.1 dB lower), and a held low tone builds up on a reverb's
  resonance (the sends lose their lows at 220 Hz).
- **Next:** his notes from the listening page (H:\PSIV Project\sfx-algol\index.html) -> the XPOLISH table; maybe a
  kind per effect where one sits wrong; whether Algol should be the default in place of Modern.
- **Two settings since the same day** (PsiLok: "this can be the 'second' option ... Modern sounds can be a check
  box with 'Use modern sounds that are available'"): Sound effects = Original / Algol (what plays the game's
  own), Modern sounds = On / Off (the port's in place of the game's where there is one; your own from
  sfx/sfx.ini first). An old psiv.ini's modern / custom becomes On, its original Off. In the code Algol is a
  switch (`sfx_set_algol`), not a mode: a replaced effect never reaches the driver, the rest play on the chip
  or on Algol. He also wants a **new sound library for the Modern sounds** (the synthesized set is "thin or
  synthetic"): how it is made, and whether it ships with the game or as a pack, is his to say.
- **The new Modern library, his choices (2026-11-16):** made from free CC0 samples, layered; bundled with the game
  (so every source must be CC0, with its page and author on record: `res/sounds/CREDITS.txt`); the new library
  only -- the synthesized set no longer loads in the game; all 65 effects. Built so far: the loader
  (`sfx_library_load`: `sounds/XX_1..4.wav` + `sounds.ini`, beside the program / in the APK's assets / a
  development tree's `res/sounds`), `--sfx-library DIR` in headless, package.py copies `res/sounds` when it is
  there (the APK's assets still to do). No sounds yet: each download waits for his OK.
- **Downloaded and shortlisted (2026-11-16, on his yes):** 49 CC0 packs from OpenGameArt (`tools/sfxlib/sources.json`;
  one left out: its page says CC0, its own licence file CC BY-SA), 1,638 sounds measured (`inventory.py`), and for
  63 effects up to eight candidates each, cut to the original's length and loudness (`briefs.py`,
  `candidates.py`). They were chosen by file name and measurement, not by ear: the audition page
  (`audition.py`) is for him to tick the ones to keep; `build.py` turns his picks into `res/sounds` with
  `sounds.ini` and `CREDITS.txt`.

## Bug: "Storm sounds persist after battle" (QueeQueg, Discord #bug-reports, 2026-11-17) -- fixed 2026-11-18, not released
The rain's loop ended only in `weather_battle_end()`, and only `depth_compose()` called it. With Widescreen at
"side panels" the field is composed on another path (as are the first-person dungeons and a paused picture), so
after a battle in the rain `depth_compose()` was never reached and the rain played on until the next battle. In
0.5.0-beta too. Now the frame loop tells the weather every frame (`weather_frame()`): out of a battle it ends,
whatever draws the screen; a thunderclap still due is dropped. Shown on Hermes in the real app (side panels, a
forced storm by Zema, music and voices at 0): without the fix the sound stays at -23 dB for the 35 s after the
battle; with it, silence within two seconds. `PSIV_WEATHER=storm` (rain, snow...) forces the weather in the
app, as `--weather` does in headless.

## GitHub issue 9 (pepodmc3): group items, buy several, a Vehicles entry -- grouping prototyped 2026-11-18
- **Grouping, built (uncommitted):** the modern camp menu's Items screen shows one row per kind of item with its
  count ("Dimate x11"); `enhance.group_items` (on by default; Settings > Game > Group items). Display only: the
  pack is the game's (40 slots, one item each); a row stands for the first slot holding that item, so Use / Look
  / Discard act on one of them. Up / Down (and the wheel) go a kind at a time, Left / Right eight kinds: the port
  walks the game's own cursor there (the mouse's drive, with no C at the end). Tested on Hermes from saves 21, 30
  and 40: navigation, a click, Use (x11 -> x10), and off showing the game's list as before.
- **The battle's item menu too (2026-11-18, on his word):** what the fighter wears stays a row each (using a worn
  weapon is not using one from the pack), the pack one row a kind with "xN"; Up / Down a row, Left / Right a page
  of four; the same setting. Tested in the Boss Arena (D-Elm-Lars): Chaz's list 9 pages -> 4, the arrows, and
  Dimate chosen from its row ("Dimate -- On whom?").
- **Columns are pages, as in the game (2026-11-18, his note "each column acts as a page in the battle item
  menu"):** the game's own lists, battle and camp, wrap Up / Down inside a column and send Right / Left to the
  top of the next / previous column, doing nothing past the ends; the grouped rows now do the same
  (`page_step`, columns of four in battle, eight in camp). Checked against the game's own lists with grouping
  off.
- **His notes, same day:** the battle list's heading says "column 2/4", not "page"; one cursor tick a step
  (the port's walks through a kind's slots, or to a clicked row, ticked at every press: the extra $F2 requests
  are taken out of the sound queue, frame.c, camp and battle; 18 ticks -> 6 for six presses past Dimate x11);
  the camp menu's wheel rolls from a column's bottom to the next column's top, grouped or not.
- **Vehicle shortcuts (2026-11-18, his "Would it be possible to make vehicle shortcuts in the camp menu?"):** a
  "Vehicles" box under the commands on the menu's first screen lists the vehicles in the pack; one that can be
  boarded where the party stands is white, the rest grey -- the game's own test (ItemAction_LandRover etc.: a
  planet's world map, Field_Map_Index < $10, and the tile's Vehicle_Boarding_Flags at $FFEC7F, set as the menu
  opens). Choosing one plays the game's own way there as the pad (Items, the vehicle's slot, Use; each step
  waits for its window to settle: a C pressed while Use / Look / Discard was still opening was lost), the first
  screen held on show meanwhile, no cursor ticks and one choice sound. Keys: Down from Macros into the box, Up
  from its top back out, C boards; the mouse: a click. Tested on Hermes: saves 10-44 scanned (white / grey as
  the game would allow), save 13 boarded the Land Rover by keys, save 40 the Ice Digger and the Hydrofoil by
  clicks (PSIV_FIELDMENU_LOG names the vehicle).
- **Not yet:** the macros' item list and the shop's sell list are still one row per item;
  buying several at once. Classic stays the game's own.
