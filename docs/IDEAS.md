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

## First-person dungeons (PsiLok, 2026-11-04) -- v1 (`runtime/fpview.c`)
From Gen1Recomp's 3D mode and Phantasy Star I's mazes: the dungeons in
first person, from the game's own tiles, and their battles fought there.
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

### Instrument banks from SNES games you own (PsiLok, 2026-11-04) -- v1: Lufia II (`runtime/snesbank.c`)
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
