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

## Smaller ones worth keeping in mind
- Fast-forward, and turbo for battles and text.
- Save anywhere (snapshots of the whole machine state are easy in a port).
- Integer scaling, CRT-style filters, fullscreen.
- Remappable controls and modern gamepad support.
