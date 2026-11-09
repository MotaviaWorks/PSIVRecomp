PSIV Recomp: alpha
==================

A native port of Phantasy Star IV for Windows, macOS, Linux and Android. The game's
68000 code is translated into C and runs directly on your machine, with
optional extras: widescreen, modern text and battle menus, battle depth and
effects, a battle camera, living water and swaying trees, the party's voices
in battle (AI placeholders for now), the port's own sound effects, a Journal,
cheats, click/tap to walk.

The sound effects most heard in battle -- blades, claws, guns, the
techniques, the combination attacks, enemies' blows -- are the port's own,
synthesized as it starts. To hear the game's originals: Settings > Sound >
Sound effects: Original.

**It ships none of the game.** You supply your own ROM.


Your ROM
--------

- **Phantasy Star IV (USA)**, the Mega Drive / Genesis cartridge.
- Any of these work: a raw dump (`.bin`, `.md`, `.gen`), an `.smd` copier dump,
  or the `.zip` it came in.
- The app opens on a launcher. The first time, **Start game** asks for the
  file. You can also put the file into the game's folder before you start,
  and it gets picked up there.
- It's checked (SHA-256 `511f35cc11f88316...`) and kept as `rom/psiv.bin`
  next to your saves, so it's only asked for once.
- Other releases (Japanese, Brazilian), hacks and translations are refused:
  the port is built for that one cartridge's code.


Installing
----------

**Windows (64-bit, Windows 10 or 11)**
Unzip the folder somewhere you can write to, for example `Documents\PSIV Recomp`,
then run `psiv.exe`. The alpha isn't code-signed, so SmartScreen may say
"Windows protected your PC". Click **More info**, then **Run anyway**.

**macOS (Apple Silicon: M1 or newer, macOS 11 or newer)**
Unzip it and drag **PSIV Recomp.app**, from the folder it makes, into
Applications. The alpha isn't notarized
by Apple, so the first time you open it macOS says it can't check it. Click
**Done**, open System Settings > Privacy & Security, scroll down to
"PSIV Recomp was blocked" and click **Open Anyway**. (On older macOS:
right-click the app, Open, then Open.) After that it opens normally.
Intel Macs aren't supported in this alpha.

**Linux, including the Steam Deck (x86-64)**
Extract the archive and run `./psiv`. It needs glibc 2.35 or newer, so Ubuntu
22.04+, Debian 12+, Fedora 36+, Arch, or an up-to-date Steam Deck.
On a Steam Deck: in Desktop Mode, extract the archive and put your ROM in the
folder. Start `psiv` once, then add it to Steam as a Non-Steam Game.

**Android (64-bit ARM, Android 7 or newer)**
Install the `.apk`; your browser or file manager will ask you to allow installs
from it. The first time you choose Start game, pick your ROM in the file
picker (the `.zip` is fine).
Phones get an on-screen pad. Handhelds with built-in controls (Odin, Retroid and
the like) start without one; touch the screen to bring it up.


Controls
--------

| | |
|---|---|
| Keyboard | arrows = D-pad, Z / X / C = A / B / C, Enter = Start |
| | Esc = menu, Tab (hold) = fast forward, Backspace = go back 10 seconds, F11 = fullscreen, F12 = screenshot |
| Gamepad | D-pad or left stick; west / south / east buttons = A / B / C; Start |
| | Select / Back = menu, right shoulder (hold) = fast forward, left shoulder = go back 10 seconds |
| Mouse | click to walk there; click a person or chest to walk up and talk or open it |
| | click battle commands and targets; right click = back |
| Touch | the on-screen pad; tap like a mouse click; the ≡ button = menu |
| Android | Back = menu |

The launcher has **Start game**, the switch between **Enhanced** (the port's
additions) and **Classic** (the original game, unchanged), **Settings** and
**Quit**. In the game, Settings are in the menu too (Esc, Select, Back or ≡), on seven pages: Game,
Display, Sound, Battles, Controls, Cheats, and Buttons (what each button and key does, with your
gamepad's own names for them). Turn pages with the shoulder buttons,
Tab, or a tap on a page's name; the line at the bottom says what the
highlighted setting does. **Mode: Classic** (on the Game page) switches every
addition off and gives you the original game. The Sound page has a slider
each for the volume of everything, the music, the effects and the voices.

The menu's **Journal** says where the story stands and what to do next (with a
hint if you're stuck), and keeps the story so far. The **Cheats** page (all off
unless you choose them): full HP or TP, no random battles, maximum meseta,
double or quadruple experience, enemies at 1 HP.

Gamepads rumble -- and phones vibrate -- when blows land, on critical hits,
when enemies are destroyed, and at the field's big moments (doors, explosions,
breaking ice). Settings > Controls > Rumble turns it down or off. If your gamepad doesn't
rumble, `psiv.log` says whether the game found it able to.


Where things are kept
---------------------

- **macOS:** `~/Library/Application Support/PSIV Recomp` (in Finder: Go >
  Go to Folder, and paste that). Put a ROM there to skip the file picker.
- **Windows and Linux:** next to the program: `psiv.ini` (settings), `saves/`,
  `screenshots/`, `psiv.log`. If that folder can't be written to, they go to
  `%APPDATA%\PSIV Recomp` (Windows) or `~/.local/share/psiv-recomp` (Linux) instead.
- **Android:** `Android/data/com.psilok.psiv/files` on the device's storage,
  which you can reach over USB.
- Saves are the cartridge's own battery saves, made the way the game makes them.
  An emulator's `.srm` save of this game works too: put it in `saves/` as
  `psiv.srm`.


Reporting a problem
-------------------

Please send:
- `psiv.log`, and `psiv-crash.txt` if there is one (both are in the folder above);
  for crackles or gaps in the sound, the `audio:` lines in `psiv.log` say when
  your output device ran short;
- the version: the menu shows it bottom left;
- your device or PC, and what you were doing;
- your save (`saves/psiv.srm`) if the problem is somewhere in the game.


Not in this alpha
-----------------

- Intel Macs.
- Save states: the game's own saves only. (Rewind goes back up to two minutes, ten
  seconds a press: Backspace, the left shoulder, or "Go back 10 seconds" in the menu.)
- The Algol music chip is an experiment (Settings > Sound > Music).
- Voiced story scenes and remixed music: optional packs, coming separately.


Credits and legal
-----------------

Phantasy Star is a trademark of SEGA. This is an unofficial fan project and is
not affiliated with or endorsed by SEGA. You need your own copy of the game.

PSIV Recomp is Copyright (C) 2026 MotaviaWorks, free software under the GNU
General Public License v3.0 (`LICENSE.txt`), with no warranty. Its source:
https://github.com/MotaviaWorks/PSIVRecomp

Built with SDL3 (zlib), ymfm (BSD-3-Clause), floooh's z80.h (zlib),
stb_truetype (MIT / public domain) and the Noto Sans font (SIL OFL 1.1). Their
licences are in the `LICENSES` folder.
