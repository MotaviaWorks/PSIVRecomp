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

## Generated sound effects for the public build (PsiLok, 2026-11-03)
The Splice samples stay local (`sfx/` + `sfx.ini` is a personal override);
a release needs effects of its own. Plan, best fit first:
- **Synthesized in the port** (the default pack): each effect a short recipe
  rendered at startup -- filtered-noise whooshes with a swept band-pass
  (swords), a whoosh with a ~14 Hz wobble and a doppler arc panned across the
  stereo field (boomerang), modal "ring" partials (metal), pitch-dropping sine
  thumps plus a noise crack (hits), crackle through resonant filters (fire),
  shimmering arpeggios (healing), short blips (menus), a synthetic reverb.
  Nothing to ship but code; deterministic, so the replays stay checkable.
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
