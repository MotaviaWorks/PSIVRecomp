/*
 * The port's own title art: a Roman numeral IV beside the game's logo and a
 * RECOMP wordmark under it, on the title screen.
 *
 * Presentation only: drawn over the finished frame, never into the game's
 * memory. The art is the port's (the PNGs in res/title, tools/make_title_art.py);
 * its colours are the title screen's own palette entries, read as the game
 * has them loaded, and it comes and goes with the logo's own dissolve.
 */
#ifndef PSIV_TITLEART_H
#define PSIV_TITLEART_H

#include <stdint.h>

void titleart_set(int on);        /* off by default (Classic, tests) */
/* Draw over a finished frame (VDP_MAX_W pixels a row) where the logo is up. */
void titleart_frame(uint32_t *pixels, int w, int h);

#endif
