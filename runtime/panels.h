/*
 * Widescreen side panels (M7, PsiLok's idea): in exploration the game keeps its
 * original 320-pixel view and the 16:9 margins show the party instead of more
 * map. Left: a card per member (portrait, name, level, HP / TP / EXP bars,
 * status). Right: meseta and the inventory. Everything is read from the
 * game's RAM (party slots $F40A, inventory $F410, money $F438, character
 * stats $F500 + id * $80) and ROM (item names, level tables); nothing is
 * written. Drawn at the output resolution with the modern-text font.
 */
#ifndef PSIV_PANELS_H
#define PSIV_PANELS_H

#include <stdint.h>

/* take the party's portraits from the ROM (call once it is loaded); returns
 * how many (cards without one show the character's initial) */
int  panels_init(void);

/* is the game in exploration (where the panels replace the extra map)? */
int  panels_field(void);

/* draw both panels into an image (2 * margin + view_w) * scale wide and
 * h * scale tall: the left panel fills x < margin * scale, the right one
 * x >= (margin + view_w) * scale */
void panels_draw(uint32_t *out, int stride, int scale, int margin, int view_w, int h);

#endif
