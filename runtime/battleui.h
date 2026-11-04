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
 * While commands are chosen the game shows only the member choosing; here
 * the whole party stays in the picture (their name-table cells, kept from
 * frames where they stand in full, drawn back into plane A; their art never
 * leaves VRAM), the member choosing has a gold aura (an ally being targeted
 * a cyan one), and the commands sit in a strip the dock opens under the
 * party, so no menu covers anyone.
 */
#ifndef PSIV_BATTLEUI_H
#define PSIV_BATTLEUI_H

#include <stdint.h>

void battleui_set_enabled(int on);
int  battleui_enabled(void);

/* once per frame, before its lines are drawn (frame.c): decides whether the
 * modern menus are up, and if so hides the game's windows from the picture */
void battleui_frame(void);

/* the modern menus are up this frame: the front end draws them */
int  battleui_active(void);

/* draw over a picture of the frame at `scale` times the game's resolution:
 * out holds the view as frame_run drew it, view_w x h game pixels */
void battleui_draw(uint32_t *out, int out_stride, int scale, int view_w, int h);

#endif
