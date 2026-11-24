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
 *
 * Names and menus (translate_word and the rest below): the words the port draws, and the game's in its windows,
 * looked up as the game has them: a name or message of the ROM's (the pack's T lines: the port reads the English at
 * their addresses and knows them by their letters and digits) or words of the port's own (P lines: by their hash).
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

/* the pack's words for a whole piece of English -- a name, a label, a line of a message; NULL: none */
const char *translate_word(const char *s);
/* the same for a line of one of the game's windows, which may be put together of more than one ("Chaz" + "has
 * slept!"): what is known of it in the pack's words, the rest as it is. 1 if any was */
int  translate_phrase(const char *s, char *out, size_t n);
/* the pack's words for a printf format of the port's (the same conversions in the same order, or it is not taken);
 * the format itself without */
#if defined(__GNUC__)
__attribute__((format_arg(1)))
#endif
const char *translate_format(const char *fmt);
static inline const char *translate_or(const char *s) {
    const char *t = translate_word(s);
    return t ? t : s;
}
#define TR(s)  translate_or(s)
#define TRF(f) translate_format(f)

/* what a file of text is: 1 a pack. `language` (n bytes) and the lines it has, either may be NULL */
int  translate_is_pack(const char *text, size_t len, char *language, size_t n, int *lines);

#endif
