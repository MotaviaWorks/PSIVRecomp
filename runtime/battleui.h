/*
 * Modern battle menus (M7, Enhanced): the game's battle windows -- the
 * status bar, the command icons, the technique / skill / item lists with
 * their checkbox cursors -- replaced by menus drawn at the output
 * resolution: full names and costs, a line on what each choice does, and a
 * party bar with the dialogue portraits from the player's ROM.
 *
 * The battle itself is untouched. The game still runs its own menus and
 * reads the pad; this module mirrors their state from RAM (which routine is
 * running, each cursor, the lists the game built) and draws it. The game's
 * windows are kept off the picture by drawing plane A, wherever a window
 * tile sits, from the battle's window-free map (the game keeps one at
 * $FF2540 to put back what a window covered), and by leaving out the cursor
 * sprites. Other windows (enemy names, "AGILITY is UP!", the victory
 * messages) are read back from their tiles and redrawn in the same style.
 * Vehicle battles keep the original windows.
 *
 * The game shows the whole party only at the main menu: while commands are
 * chosen, only the member choosing; while the battle plays out, only the one
 * acting or the one an enemy attacks (moved in front of it). Here the whole
 * party stays in their places: their name-table cells and tiles, kept from
 * the main menu, are drawn back into plane A (behind the game's action
 * sprites), and a member the game moves goes back to their place with the
 * sprites on them. The member choosing has a gold aura (an ally being
 * targeted a cyan one); the commands sit in a strip the dock opens under the
 * party and the game's battle messages move to the top, so nothing covers
 * anyone.
 */
#ifndef PSIV_BATTLEUI_H
#define PSIV_BATTLEUI_H

#include <stddef.h>
#include <stdint.h>

void battleui_set_enabled(int on);
int  battleui_enabled(void);

/* once per frame, before its lines are drawn (frame.c): decides whether the
 * modern menus are up, and if so hides the game's windows from the picture */
void battleui_frame(void);
/* rewind: the party picture kept for the battle, as bytes (out NULL: the
 * size); load returns the bytes it took */
size_t battleui_state_save(uint8_t *out);
size_t battleui_state_load(const uint8_t *in);

/* the modern menus are up this frame: the front end draws them */
int  battleui_active(void);

/* The camera pan, drawn by depth.c: while the battle plays out, the member
 * the game stood away from their place (one attacked, or a caster or archer,
 * under an enemy) and how far (pixels, + = right); 0 if no one. The member
 * whose tile it is (-1 none); and the others' pictures as kept at their
 * places: the pixel at plane A (px, py) as palette index | 0x80, 0 = none. */
void battleui_set_pan(int on);
int  battleui_pan_enabled(void);
int  battleui_pan(int *member, int *dx);
int  battleui_tile_member(int tile);
uint8_t battleui_pan_pixel(int px, int py, int *member);

/* draw over a picture of the frame at `scale` times the game's resolution:
 * out holds the view as frame_run drew it, view_w x h game pixels */
void battleui_draw(uint32_t *out, int out_stride, int scale, int view_w, int h);

/* The mouse: a click at (fx, fy), the frame's pixels as last drawn, on a
 * menu entry, a command, a page arrow or a target picks it -- played as the
 * pad: the game's own cursor is walked there a press at a time, then C. A
 * right click is B (back). 1 if the click was taken. */
int  battleui_click(float fx, float fy, int right);
/* the pad the game sees this frame: `user` what the player holds (any of it
 * stops a click's presses), `pad` what would go to the game otherwise */
uint8_t battleui_pad(uint8_t user, uint8_t pad);

#endif
