/*
 * Click to walk (an option): a mouse click on the field sends the party
 * there. The click becomes a cell of the map -- in the game's own view the
 * camera plus the pixel; in first person the floor under the click, or the
 * person or chest clicked (fpview_pick) -- a path is found over the game's own
 * collision, and the d-pad is held a step at a time, as a player would: the
 * game moves as it always does. Clicking a person, a chest or anything else
 * the game keeps as an object walks up to it, faces it and presses C (talk,
 * open, examine); clicking a wall or water goes as near as it can. While a
 * window or an event's text is up, a click is C (next) and a right click B.
 * The player's own keys or buttons take over at once; a window, a battle, a
 * vehicle or another map ends the walk too.
 */
#ifndef PSIV_WALKTO_H
#define PSIV_WALKTO_H

#include <stdint.h>

void walkto_set_enabled(int on);
int  walkto_enabled(void);

/* A click on the game's own view at screen pixel (sx, sy) -- (0, 0) the top
 * left of the 320-wide picture (negative or past 320 in widescreen margins).
 * Returns 1 if a walk started. */
int  walkto_click_screen(int sx, int sy);

/* A click while the field shows a window or an event's text: C (next, yes)
 * for the left button, B (back) for the right. 1 if it was taken so. */
int  walkto_click_button(int right);

/* Walk to map cell (cx, cy) (16x16, the leader's feet), or to object slot
 * `slot` of Field_Objects_Memory (>= 0: to its side, then face it and press C). */
int  walkto_go(int cx, int cy, int slot);
void walkto_cancel(void);
int  walkto_active(void);

/* The pad the game sees this frame: `user` is what the player holds (any of
 * it cancels the walk), `pad` what would go to the game without a walk. */
uint8_t walkto_pad(uint8_t user, uint8_t pad);

/* Where the walk is going, for a marker: world pixels (the cell's centre) */
int  walkto_target(int *wx, int *wy);
/* ... and that point on the game's own view (screen pixels), if it's a field map */
int  walkto_target_screen(int *sx, int *sy);

#endif
