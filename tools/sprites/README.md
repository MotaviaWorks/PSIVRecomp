# PSIV Recomp: the sprite editor

A page you open in your browser to redraw sprites for PSIV Recomp and share them as a mod. Nothing to
install, and it needs no internet.

**Where this stands:** a first version. It covers the eleven of the party walking on the field. The
townspeople, battle sprites and anything larger than the game's own sprites are not in it yet.

## How to use it

1. Download `index.html` (or the whole repository) and open it in a browser.
2. Press **Open my ROM** and pick your own Phantasy Star IV (USA) ROM: the same file PSIV Recomp plays
   (`.bin`, `.md`, `.gen` or `.smd`). It is read in your browser and goes nowhere.
3. Pick a character at the left. Their frame is in the middle to draw on; at the right the game's walk and
   yours play side by side, at the game's pace.
4. Write your mod's name, your name and a line about it, and press **Export mod**. Unzip the file into
   the `mods` folder beside your saves, and switch the mod on from the launcher's **Mods** page. Mods work
   in Enhanced only.

## Frames

The game's walk is a cycle of four frames a facing (standing, a step, standing, the other step), each held
for nine ticks. **Pictures for each of the game's frames** lets you keep that, or draw two or four pictures
for each frame: the port shows them one after another inside the time the game holds its own, so the walk
takes exactly as long as before and only looks smoother. In the row of frames, a mark under a picture
shows where each of the game's frames begins.

- **Copy the frame before** starts a picture from the one before it: the usual way to draw an in-between.
- **Make it the other side, mirrored** draws Right from Left, or Left from Right. (The game itself draws
  Right as Left mirrored.)
- **Save the sheet / Load a sheet** pass a character's frames to another drawing program and back as one
  PNG: a row a facing (down, up, right, left), a column a picture. Colours are matched to the nearest of
  the sprite's fifteen.

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
uses, so a redraw stays in the character's look) or **all fifteen** of the party's palette. Pick colour
takes a colour from the picture. Loading a sheet matches its colours to whichever set is showing.

## What the game sets

- A sprite stands in the game's 16 x 32 pixels, in fifteen colours and see-through: the party's own
  palette on the field. All the party share it, so a colour means the same for every character.
- The picture shows where the game's own sprite would be.

## What a mod holds

A folder with a `mod.txt` (its name, author and a line about it) and a `.spr` file for each character you
changed. A pixel you left as the game has it is stored as "the game's own": the port shows the game's
pixel there, read from the player's ROM. So a touched-up sprite holds only your changes, and none of the
game's art is in the file. **Open sprite files** loads a mod's `.spr` files back in, yours or someone
else's.

The file's format, for the curious: `runtime/spritemod.h` in the port.
