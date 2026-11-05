/*
 * Widescreen in the field: the margins drawn from the map in RAM.
 *
 * The game only puts into the VDP's planes what a 320-pixel screen needs, so
 * beyond it the planes hold stale strips (on the left after most moves) and,
 * at a map's edges, the map's other side. With widescreen on, the frame loop
 * calls widemap_frame() before drawing a field frame; while the planes agree
 * with the map on screen, the margins are drawn from the map itself
 * (vdp_margin_entry), the way the game would have drawn them.
 */
#ifndef PSIV_WIDEMAP_H
#define PSIV_WIDEMAP_H

/* Before a frame's lines: set up (or switch off) the margins for this frame.
 * `wide`: the frame is drawn wider than the mode. */
void widemap_frame(int wide);

/* Beyond a map's edge (towns, dungeons; the world maps go round): 0 = the
 * backdrop, 1 = the edge chunks repeated (the default). */
void widemap_set_edge(int mode);

/* 0: the margins straight from the planes, as before (for comparisons). */
void widemap_set_enabled(int on);

#endif
