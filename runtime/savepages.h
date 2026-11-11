/*
 * More save slots: pages of three.
 *
 * The cartridge's 8 KB hold a header and three slots, and the game's windows
 * list three. So the port keeps several such saves -- pages, each a file in
 * the cartridge's own layout -- and puts another in the game's hands when the
 * page is turned: Left or Right in the game's Save window; in its Continue
 * and Erase windows (their slots stand side by side), Right past the third
 * or Left past the first. The game's own code lists, loads and saves what
 * is in the cartridge, as ever; only which 8 KB that is changes.
 *
 * The hooks are placed by tools/emit.py. With no pages set (Classic, the
 * headless tools) each returns at once and the game is untouched.
 */
#ifndef PSIV_SAVEPAGES_H
#define PSIV_SAVEPAGES_H

#include <stddef.h>

#define SAVEPAGES_MAX 4               /* twelve slots */

/* pages: how many (1 = off: the first page only); go(page): the front end
 * writes the page in memory to its file and reads that one into bus_sram
 * (zeros if it has no file yet); returns 0 if it could not. */
void savepages_set(int pages, int (*go)(int page));
void savepages_set_page(int page);    /* the page the front end read at the start */
int  savepages_page(void);
int  savepages_count(void);
/* While one of those windows is up: a line saying which slots it shows and
 * how to see the others. Returns 0 when there is nothing to say. */
int  savepages_note(char *out, size_t n);

/* A slot of a page (8 KB, the cartridge's layout) holds a save. */
int  savepages_saved(const unsigned char *cartridge, int slot);
/* told of every save as the game makes it: the page and the slot */
void savepages_on_save(void (*fn)(int page, int slot));

/* Continue from the launcher: the game is started and taken through its
 * own title to a save, unseen. savepages_continue(slot) before the game
 * starts (the page already in memory); while savepages_continuing() is
 * true the front end shows no picture, plays no sound, does not wait
 * between frames, and gives the game savepages_pad() for its pad -- Start,
 * every other frame, which passes the logo and the title and chooses
 * "Continue"; the slot is then chosen for it (psiv_savepage_pick). It
 * ends when the game leaves its title for the map, or gives up if the
 * slot turns out to hold no save (the title is shown instead). */
void savepages_continue(int slot);
int  savepages_continuing(void);
unsigned char savepages_pad(void);

/* called from the recompiled code */
void psiv_savepage_title(void);       /* the title's Continue / Start / Erase menu is about to check for saves */
void psiv_savepage_pick(void);        /* each frame the Continue or Erase window waits */
void psiv_savepage_list(void);        /* each frame the Save window waits */
void psiv_savepage_store(void);       /* the game is about to write a slot and the header */

#endif
