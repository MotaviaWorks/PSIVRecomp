/*
 * A translation's dialogue (a pack made with tools/translate), shown in place of the game's own.
 *
 * Nothing the game does changes: it types its own line into its box, and the port draws the pack's words
 * there instead (hdtext.c), as many of them as the game has typed. Which line that is comes from the game
 * itself: while RunText types, its place in the text is in the registers VInt_Prepare saves on the stack. It is
 * believed only when the letters from the box's start to that place are the ones on the screen, so a wrong
 * reading shows the game's own words and never another line's. A line the pack does not have stays the game's.
 *
 * The text: the map's dialogue at $FF3000 (the pack's D lines: bank and number in it), and the short texts the
 * game runs through the same routine straight from the ROM (some of its T lines: inns, shops, things looked at).
 * Names and menus drawn otherwise are not covered yet.
 */
#ifndef PSIV_TRANSLATE_H
#define PSIV_TRANSLATE_H

#include <stddef.h>

/* a pack's text (UTF-8), or NULL for none: how many lines it has */
int  translate_load(const char *pack, size_t len);
int  translate_lines(void);                  /* the lines of the pack in use; 0: none */
const char *translate_language(void);        /* its language, as it calls itself */

/* once a frame, once the picture's lines are made: `key` the letters and digits of the game's dialogue box in
 * lower case (hdtext_dialogue_key) */
void translate_frame(const char *key);

int  translate_showing(void);                /* a box the pack has is up */
const char *translate_text(void);            /* that box's words, its lines parted by \n */
int  translate_shown(void);                  /* how many of its characters (a line's end counted) are typed */

/* what a file of text is: 1 a pack. `language` (n bytes) and the lines it has, either may be NULL */
int  translate_is_pack(const char *text, size_t len, char *language, size_t n, int *lines);

#endif
