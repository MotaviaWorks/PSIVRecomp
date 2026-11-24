# PSIV Recomp: translating the game

Two tools for anyone who wants to translate Phantasy Star IV for PSIV Recomp, and share the result:

- **The editor** (`index.html`): a page you open in your browser. It shows each line of the game as
  the game shows it; you type your translation beside it, and export a pack to share. Nothing to
  install.
- **The command-line tool** (`psiv_translate.py`): the same thing through a spreadsheet, for those who
  prefer one.

Both write the same pack, and the editor opens packs, so work can pass between them and between people.

**Where this stands:** the port shows a pack's **dialogue** (in builds after 0.7.1-beta). The names and
messages can be translated here too and are kept in the pack, but the port still draws the game's own:
that part comes next, and the pack's format may still change with it. The port's own words (its
settings, its descriptions, the Journal) are not covered yet.

A translation made this way is not a ROM patch. The port draws the game's text itself, with its own
font, so a pack is only the translated words and a font that has your language's characters. It needs
the port's **Text: Modern**.

## The editor

1. Download `index.html` (or the whole repository) and open it in a browser. It needs no internet.
2. Press **Open my ROM** and pick your own Phantasy Star IV (USA) ROM: the same file PSIV Recomp plays
   (`.bin`, `.md`, `.gen` or `.smd`). It is read in your browser and goes nowhere.
3. Pick a place at the left, a line in the middle, and type your translation at the right. The game's
   line is shown in its boxes above, and yours below as it will read.
4. Write your language's name at the top, as its speakers write it, and press **Export pack**.

Things worth knowing:

- **Your work is kept in the browser as you type.** Export a pack now and then all the same: clearing
  the browser's data clears it, and a pack is also how you move to another computer.
- **Open a pack** carries on from a pack: your own, or someone else's to continue or correct.
- **A field for each box.** The game shows a line in boxes, one after another, and each has its own field. The
  game's codes are put in for you. **Show the codes** shows the line as the pack holds it, for moving one.
- A line is marked green when it is ready, amber when it may look wrong on screen (too long for the
  box, a box with no words yet), and red when it has a code the game's line has not. A pack with red
  lines can only be exported as a draft.
- **Theme**, at the top: light, dark, high contrast, or blue like the game's own windows.
- **Ctrl + Enter** goes to the next line to translate.

### The Windows app

The same page in a window of its own: `TranslationEditor.exe` (keep the `.dll` files beside it). It does three
things more. It remembers your ROM. It keeps your work in a file (`%LOCALAPPDATA%\PSIV Translation Editor`)
instead of the browser's storage. And **Save to the game** (Ctrl + S) writes the pack straight into the game's
`mods` folder, with the `mod.txt` the Mods page reads; it asks where that folder is the first time. Work you began
in the browser comes over with **Export pack** there and **Open a pack** here.

To build it: `app\build.bat WEBVIEW2_FOLDER OUT_FOLDER`, with the compiler that comes with Windows.
`WEBVIEW2_FOLDER` holds three files of Microsoft's, from the Microsoft.Web.WebView2 package (they are named in
`build.bat`).
## The command-line tool

### What you need

- Python 3.8 or later. Nothing else to install.
- Your own Phantasy Star IV (USA) ROM: the same one PSIV Recomp plays (`.bin`, `.md`, `.gen`, `.smd`
  or a `.zip` of one).

### The three steps

```
python psiv_translate.py extract "Phantasy Star IV (USA).md"
python psiv_translate.py check   psiv-dialogue.csv --rom "Phantasy Star IV (USA).md"
python psiv_translate.py pack    psiv-dialogue.csv --language "简体中文" --rom "Phantasy Star IV (USA).md"
```

1. **extract** writes the workbook, `psiv-dialogue.csv`: 1,565 lines of dialogue and about 1,260
   names and messages. Open it in any
   spreadsheet (LibreOffice Calc, Excel, Google Sheets) as UTF-8, and write into the `translation`
   column. Run it again later over the same file and your work is kept.
2. **check** reads your translation and lists what is wrong (a PROBLEM stops the pack) and what may
   look wrong on screen (a warning does not). It also says how far along you are.
3. **pack** writes the file to share: your translation alone, each line under its number.

**The workbook holds the game's own script. Keep it to yourself**, as you would the ROM. The pack does
not: it has your words only, and that is the file to give to others.

## The marks in a line

The same in the editor and in the workbook.

### The workbook's columns

| column | |
|---|---|
| `id` | the line's number in the game: `D07.01A` is dialogue (bank 7, line $1A); `T2AAFB4` is a name or a message (by where it sits in the ROM). Don't change it. |
| `where` | the part of the game the line is heard in |
| `speaker` | whose portrait the line puts up, if any |
| `english` | the line as the game has it |
| `translation` | yours. Empty means not translated yet: the game's own line is shown. |

**Names and messages** are plain words: no codes, and the same number of lines as the game's where
you can. The game has some of them twice (a menu word in two menus); the editor offers to fill the
others that read the same. Its ten-letter names are not a limit for you: the port draws the text
itself. Some entries begin with a space or two, the game's own padding: you can leave it out.

**Dialogue** can run over several boxes. In a cell:

- **A new line** in the cell is a new line in the box. The box shows two lines of about 32 letters;
  a full-width character (Chinese, Japanese, Korean) counts as two.
- **`{page}`** on a line of its own: the box is full, and the game waits for a button.
- **`{cut}`** on a line of its own: the box closes, or the scene does something, and the line goes on.
- **Everything else in braces is a code of the game's**. Leave it where it is:

| code | what it does |
|---|---|
| `{F4:01}` | puts up a portrait (here Chaz's): whoever speaks next |
| `{F5:05,06}` | a Yes / No choice |
| `{FA:DA,03}` | "if this has already happened, say another line instead" |
| `{F2:...}` | the scene shows a picture, plays a sound or remembers something |
| `{F9:1D}` | a pause |
| `{F3}` | a code the game carries in a few lines; keep it |

You may move a code within a line when your language's word order asks for it (a portrait belongs
before the words its speaker says). The game follows its own codes whatever the pack's are, so **check**
only warns of one that is missing or moved.

Keep the pages as they are where you can: the same number of `{page}` marks as the original. Where a
translation needs more or fewer, **check** warns; how the port will handle that is not settled yet.

## What check looks for

- a line missing from the game, or in the workbook twice;
- a code the original does not have (one missing or out of order is a warning);
- a `{` or `}` that is not part of a code;
- (warning) a different number of pages from the original;
- (warning) more than two lines on a page, or a line wider than the box.

Without `--rom`, it checks against the workbook's own `english` column instead of the game.

## The pack

A plain text file, UTF-8:

```
# PSIV Recomp translation pack
format = 1
language = 简体中文

[D01.001]
非常感谢！
现在我安心多了。
```

## Trying it in the game

1. In the folder that holds your saves and `psiv.ini`, make a folder `mods`, and in it one for your
   translation, with any name: `mods/spanish`.
2. Export your pack into it: `mods/spanish/psiv-spanish.txt`. If your language needs characters the
   game's font has not (Chinese, Japanese, Korean ...), put a font there too (`.ttf` or `.otf`).
3. On the launcher, **Mods**, switch it on. The port must be in Enhanced, with **Text: Modern**.

**While you play, export again over the same file:** the game reads it again by itself within a second
or so and says so at the top of the screen. The box on the screen changes too.

- A line you have not translated stays in English, so a pack can be tried from its first line on.
- A line too long for the box is set smaller, up to four lines.
- With fewer `{page}` marks than the game's line, the last page stays up; with more, they share the
  game's boxes.
- Right-to-left languages are not handled.

Not yet: names, menus, shops, inns, battles, and the opening's words.

## Questions, and telling us you are translating

The Discord (https://discord.gg/YmgWfQGJAr) or an issue on GitHub. If you start a translation, say so:
a real pack to try the port with is the best help there is.
