/*
 * Battle depth (M7): battles redrawn as layers at different depths under a
 * slowly moving camera, composited at the output resolution.
 *
 * PSIV draws a battle on two planes: the background on plane B, everything
 * else as plane A tiles -- enemies at low priority, the party and the windows
 * at high priority -- plus sprites for cursors and for a character in the
 * middle of an action. depth.c sorts each pixel into a layer by plane,
 * priority and tile number, then draws the layers back to front, each moved
 * by the camera according to its depth: background least, party most,
 * windows not at all. Purely presentational: game state is never touched, so
 * the BizHawk comparisons are unaffected.
 *
 * Levels: 0 off, 1 subtle, 2 strong. Changes ease in and out.
 */
#ifndef PSIV_DEPTH_H
#define PSIV_DEPTH_H

#include <stdint.h>
#include "vdp.h"

#define DEPTH_LEVELS 3

void        depth_set_level(int level);
int         depth_get_level(void);
const char *depth_level_name(int level);

/* Call once per displayed frame, with that frame's layers. In a battle with
 * the effect on (or still easing out), composes the frame at `scale` times
 * the game's resolution into out (out_stride pixels per row, 0xAARRGGBB) and
 * returns 1. Otherwise returns 0: show the normal frame. */
int depth_compose(const VdpLine *lines, int w, int h, uint32_t *out, int out_stride, int scale);

#endif
