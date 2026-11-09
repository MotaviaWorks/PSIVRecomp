/*
 * The field's living water (an Enhanced option): the sea, rivers and pools
 * the party walks beside, drawn over the game's own picture at the display's
 * resolution -- the water bends with slow ripples, light plays over it in
 * moving caustics, glints come and go, foam runs along the shores, and deep
 * water darkens a little.
 *
 * What is water comes from the game itself: its collision map marks the
 * cells (code 9, the same map click to walk uses), and the colours drawn in
 * the cells that are water all round tell which pixels of the cells along a
 * shore are water too. Only the background planes count: the party, people,
 * windows and text stay as they are. Nothing of the game's state is touched.
 */
#ifndef PSIV_FIELDFX_H
#define PSIV_FIELDFX_H

#include <stdint.h>
#include "vdp.h"

void fieldfx_set_water(int on);
int  fieldfx_water(void);
void fieldfx_set_trees(int on);                  /* trees moving in the breeze */
int  fieldfx_trees(void);

/* After a frame is drawn: find the water and the trees in it (w x h game
 * pixels, lines as the VDP drew them). 1: there is something to draw. */
int  fieldfx_prepare(const VdpLine *lines, int w, int h);
int  fieldfx_pixels(void);
int  fieldfx_tree_count(void);                   /* trees it found */                       /* how many game pixels of water it found */

/* Draw it over that frame, already scaled up ds times into out (stride in
 * pixels); pixels is the game's own picture (src_stride in pixels). */
void fieldfx_draw(const uint32_t *pixels, int src_stride, int w, int h, uint32_t *out, int stride, int ds);

#endif
