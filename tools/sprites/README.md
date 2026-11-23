# PSIV Recomp: the sprite editor

A page you open in your browser to redraw sprites for PSIV Recomp and share them as a mod. Nothing to
install, and it needs no internet.

**Where this stands:** it covers everyone who walks on the field (the eleven of the party, four of the
story's people: Zio, Saya, Professor Holt and the Principal, and forty kinds of townspeople) and the
party's portraits. Battle sprites are not in it yet: the port does not replace those so far.

## How to use it

1. Download `index.html` (or the whole repository) and open it in a browser.
2. Press **Open my ROM** and pick your own Phantasy Star IV (USA) ROM: the same file PSIV Recomp plays
   (`.bin`, `.md`, `.gen` or `.smd`). It is read in your browser and goes nowhere.
3. Pick a character at the left: the party first, then the story's people, then the townspeople (they have
   no names in the game, so they are numbered; the picture tells you who is who). Each group folds away by
   the caret on its heading. A townsperson you redraw changes
   everywhere that kind of person appears. Their frame is in the middle to draw on; at the right the game's walk and
   yours play side by side, at the game's pace.
4. Write your mod's name, your name and a line about it, and press **Export mod**. Unzip the file into
   the `mods` folder beside your saves, and switch the mod on from the launcher's **Mods** page. Mods work
   in Enhanced only.

## Frames

The game's walk is a cycle of four frames a facing (standing, a step, standing, the other step), each held
for nine ticks (eight for the townspeople). **Pictures for each of the game's frames** lets you keep that, or draw two or four pictures
for each frame: the port shows them one after another inside the time the game holds its own, so the walk
takes exactly as long as before and only looks smoother. In the row of frames, a mark under a picture
shows where each of the game's frames begins.

- **Copy the frame before** starts a picture from the one before it: the usual way to draw an in-between.
- **Draft the in-betweens** fills every picture between the game's frames for you, for all four facings: what
  is the same in a frame and the next stays, and what differs slides across a little at a time. No colours
  are mixed. It is a rough start to tidy by hand, not a finished walk: the game has only three drawings a
  facing to go on. A mod made this way holds shifted pieces of the game's own frames, so think before
  sharing one that you have not redrawn.
- **Make it the other side, mirrored** draws Right from Left, or Left from Right. (The game itself draws
  Right as Left mirrored.)
- **Save the sheet / Load a sheet** pass a character's frames to another drawing program and back as one
  PNG: a row a facing (down, up, right, left), a column a picture. Colours are matched to the nearest of
  the sprite's fifteen.

## Drawing

- **Pencil, Fill, Pick colour** work with the chosen colour; **Game's own** paints the game's pixels back.
- **Undo** and **Redo** (Ctrl + Z, Ctrl + Y) cover the picture you are on.
- **Onion skin** shows the picture before this one in blue (and, if you choose, the one after in orange)
  wherever this one is empty, so you can see where a leg or an arm was. The walk is a loop, so the first
  picture's "before" is the last.
- **Copy**, **Paste** and **Flip** (Ctrl + C, Ctrl + V) carry a whole picture to another frame, another
  facing or another character, or turn it left to right.
- **Move the picture** shifts it a pixel at a time (Shift + the arrow keys). What leaves one edge comes in
  at the other, so nothing is lost.
- The Left and Right arrow keys step through the pictures.
- **Canvas: Large** doubles the canvas for fine work.

## More pixels

**Pixels for each of the game's pixels** gives the canvas two or four times the pixels along each side:
32 x 64 or 64 x 128 in place of the game's 16 x 32. Going finer keeps what you drew, each pixel becoming a
block; going coarser makes each block the colour most of it has, and asks first.

The port draws the finer detail where it enlarges the game's picture for the screen. How much of it shows
depends on the screen: the port makes its picture at up to four times the game's size, so a 64 x 128
sprite shows in full on a 1080p screen or larger, and a smaller window shows it reduced. The game itself
still works with the sprite at 16 x 32: where it puts a roof or a tree in front of a character, it hides
them by its own pixels.

## Colours

Above the swatches, choose **the character's own colours** (only those the game's sprite of that character
uses, so a redraw stays in the character's look) or **all fifteen** of their palette. Pick colour
takes a colour from the picture. Loading a sheet matches its colours to whichever set is showing.

## What the game sets

- A sprite stands in the game's 16 x 32 pixels, in fifteen colours and see-through. The party share one
  palette and the townspeople another, the same in every town, so a colour means the same for everyone
  in a group. Saya, Zio and townspeople 36 and 37 the game draws with the party's colours, and the editor
  shows them so.
- The picture shows where the game's own sprite would be.

## Portraits

Under **Portraits** each of the party has two pictures. Each starts as a single still:

- **large**, 80 x 80: the game's Status page picture. In Enhanced the port shows this one wherever it
  draws a face large enough: the dialogue's portrait with modern text, the battle's party bar, the menus.
  This is the one to redraw first.
- **small**, 48 x 48: the game's dialogue portrait. The port shows it where it draws a face small.

They take two or four pixels for each of the game's as the sprites do (up to 320 x 320 for a large one),
and the party's fifteen colours. **Save the picture / Load a picture** pass one to another drawing
program and back as a PNG; its colours are matched to the nearest of those showing above the swatches.

**A portrait that moves.** Under **Frames**, give a portrait 2 to 8 frames and it plays in the game: a
blink, the eyes, the hair. A new frame starts as a copy of the last, so you change only what moves. Two
times set the pace: how long each frame shows, and how long the first one rests before the others play.
For a blink, let the first rest a few seconds and the others pass quickly; for hair that keeps moving,
no rest. The side-by-side plays yours as the game will, the onion skin shows what differs from the frame
before, and **Save / Load the frames** pass them as one PNG, side by side. In the game each of the party
plays a little out of step with the others, so they do not all blink at once.

Limits for now: where the port leaves the dialogue's portrait to the game (the text set to the game's
own font, and the Talk command chosen from the menu), the game's own portrait shows, not a mod's; and
Alys's wounded portrait and the story's illustrations are not in the editor.

## What a mod holds

A folder with a `mod.txt` (its name, author and a line about it) and a `.spr` file for each character you
changed. A pixel you left as the game has it is stored as "the game's own": the port shows the game's
pixel there, read from the player's ROM. So a touched-up sprite holds only your changes, and none of the
game's art is in the file. **Open sprite files** loads a mod's `.spr` files back in, yours or someone
else's.

The file's format, for the curious: `runtime/spritemod.h` in the port.
