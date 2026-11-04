# Enhancement ideas

Parked until the faithful port is playable (milestone M6). Each one is noted
with the part of the architecture it depends on, so the base port is built in
a way that keeps it possible.

## Depth in battles
Give battles a sense of space: parallax between the background layers, the
enemy layer and the party, possibly a gentle camera move or 3D-style layering.
- **Needs:** the layer-separated renderer (backgrounds, enemy tiles and sprites
  kept apart before compositing), plus knowing which battle routines draw what.
- **Hooks:** battle background setup and the enemy sprite routines
  (`Battle_EnemySpritesJmpTbl`, `Battle_AnimateSprite`) to tag what each layer is.

## Widescreen
Show more of the world to the left and right instead of a 320-pixel window.
- **Needs:** a renderer that can draw past 320 px. The VDP's planes are 512 px
  wide or more, so field maps often already have the tiles; where they don't,
  the map loader has to fill the extra columns.
- **Hooks:** sprite culling and camera clamping (the game skips objects outside
  320 px), and window/menu positions so the UI stays centred.

## Full text UI
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
- Save anywhere (snapshots of the whole machine state are easy in a port).
- Integer scaling, CRT-style filters, fullscreen.
- Remappable controls and modern gamepad support.
