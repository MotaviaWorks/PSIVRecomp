/*
 * Modern text (M7): the game's text redrawn with a TrueType font at the
 * output resolution.
 *
 * PSIV draws text two ways, both recognised here without touching the game:
 *   - windows, menus and battle windows: one 8x8 font tile per character,
 *     tile = $680 + code (a second copy at $7BF + code holds the battle
 *     numbers); a tile counts as text only if it uses just the font's two
 *     colours ($E background, $F letter), so map tiles are never mistaken
 *   - dialogue: a canvas of tiles $580-$5FF, 32 x 2 cells of 8x16, into which
 *     ParseText draws each character from Art_DialogueFont (an 8x16 1-bit
 *     bitmap per code, $F on $E); a cell is decoded back to its bitmap and
 *     looked up in that font, read from the ROM at startup
 * The text keeps the game's positions, colours and timing (the typewriter
 * effect, line breaks); words are laid out proportionally from where the
 * game put them, numbers stay one per cell so columns still line up.
 */
#ifndef PSIV_HDTEXT_H
#define PSIV_HDTEXT_H

#include <stdint.h>
#include "vdp.h"

int  hdtext_init(const char *font_path);    /* 1 if the font loaded */
void hdtext_set_enabled(int on);
int  hdtext_enabled(void);

/* does this frame show any text? */
int  hdtext_has_text(const VdpLine *lines, int w, int h);

/* draw the frame's text over an image of the frame at `scale` times the
 * game's resolution (out, out_stride pixels per row) */
void hdtext_draw(const VdpLine *lines, int w, int h, uint32_t *out, int out_stride, int scale);

/* nearest-neighbour upscale of a frame (src_stride pixels per row) */
void hdtext_upscale(const uint32_t *src, int src_stride, int w, int h, uint32_t *out, int out_stride, int scale);

#endif
