/*
 * First-person dungeons (an option, M7): in the game's dungeons the field is
 * drawn as the leader sees it -- a corridor in perspective, like the mazes of
 * Phantasy Star I -- built live from the game's own map and tiles: the walls
 * are the map's 16x16 solid cells, faced with the wall fronts the game draws
 * in its 3/4 view, and the floor is the map itself, laid out in perspective
 * (rugs, patterns, stairs and all). Palettes and animated tiles come straight
 * from the VDP, so the game's fades and animations carry over. People, chests
 * and the other objects stand in it as pictures turned to the camera (from
 * the game's object list and art, near or far); people show the side that
 * faces you; one set into a wall is painted onto it. What the 3/4 view draws
 * over the party (a pillar's shaft, an arch over a passage) stands up from
 * the floor line beneath it. Doors open as the game opens them (in the map).
 * The game's windows and text stay on top. The game runs as it always does; only the
 * picture and (in the app) the meaning of the d-pad change: up walks forward,
 * left / right turn that way, down turns back.
 *
 * Inspired by Gen1Recomp's 3D mode. Nothing is drawn from outside the game.
 */
#ifndef PSIV_FPVIEW_H
#define PSIV_FPVIEW_H

#include <stdint.h>
#include "vdp.h"

void fpview_set_enabled(int on);
int  fpview_enabled(void);
int  fpview_active(void);                 /* enabled, and the game shows a dungeon */
int  fpview_in_battle(void);              /* a battle fought in the first-person view */

/* Draw the view at `scale` x the game's w x h into out (stride in pixels),
 * the game's windows (from its own frame, `game`, VDP_MAX_W wide) on top.
 * Returns 0 (nothing drawn) when not active. */
int  fpview_compose(const VdpLine *lines, const uint32_t *game, int w, int h, uint32_t *out, int stride, int scale);

/* A click at (fx, fy) on the view as last drawn (the game's w x h frame):
 * the map cell of the floor there (or of the cell before the wall the click
 * is on), or the object shown there (*slot >= 0, its cell). 0: not shown. */
int  fpview_pick(float fx, float fy, int w, int h, int *cx, int *cy, int *slot);
/* where world pixel (wx, wy) on the floor shows on that frame; 0: it doesn't */
int  fpview_project(float wx, float wy, int w, int h, float *fx, float *fy);

/* The pad as the player holds it -> as the game should see it (pad bits as
 * bus.h): directions turn relative to where the camera looks, each one kept
 * until it is let go, so holding "left" turns once and walks on. Menus and
 * dialogue get the pad unchanged. */
uint8_t fpview_remap_pad(uint8_t pad);

#endif
