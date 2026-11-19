/*
 * Scale2x / Scale3x: the game's picture enlarged with its stair-steps
 * rounded off (GitHub issue 4, "graphic filters").
 *
 * The well-known pixel-art rule (EPX / AdvMAME2x and 3x), written here from
 * its description: each game pixel becomes a 2x2 or 3x3 block, and a corner
 * of the block takes a neighbour's colour where the two neighbours on that
 * corner's sides agree with each other and the pixel is not on a straight
 * edge. Only colours already in the picture are used, so nothing blurs: a
 * diagonal line comes out as a finer diagonal, a dither pattern stays as it
 * is. Four times is twice applied twice.
 */
#ifndef PSIV_SCALEX_H
#define PSIV_SCALEX_H

#include <stdint.h>

/* The w x h pixels at src (`src_stride` pixels a row) to out at `scale`
 * times the size (`out_stride` pixels a row): 2 and 4 by Scale2x, 3 by
 * Scale3x; any other scale as plain blocks. */
void scalex_upscale(const uint32_t *src, int src_stride, int w, int h, uint32_t *out, int out_stride, int scale);

/* The same for a picture of palette numbers, a byte a pixel (a battle's
 * layers, depth.c): 0, "nothing here", is one more value, so a sprite's
 * outline is rounded against what shows behind it. */
void scalex_upscale8(const uint8_t *src, int src_stride, int w, int h, uint8_t *out, int out_stride, int scale);

#endif
