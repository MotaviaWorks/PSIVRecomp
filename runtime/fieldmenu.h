/*
 * The field menu, redrawn (Enhanced): the game's camp menu on the whole
 * screen, in framed windows -- the commands, the party, the meseta -- with
 * its screens laid out anew: the items (the pack a page to a column, what
 * to do with one, on whom), the techniques and the skills (whose, those
 * usable outside battle with their cost or uses left, on whom), the
 * equipment (the member's four slots, every stat with what it becomes, the
 * things they could put on), the status (whose; the profile with the
 * portrait from the ROM, every technique, every skill) and the macros (the
 * eight across the top, the turns of the one chosen, who acts next and what
 * they do), each with a line on what the cursor is at. A shop or an inn is
 * laid out anew the same way (the keeper, buy or sell, the goods or the
 * pack, who could use the thing and what it would do for them, the
 * keeper's words); what a chest holds comes up in the game's own layout
 * over the field as it is.
 *
 * The menu itself is the game's. It still runs its own windows and reads the
 * pad; this module reads their state from RAM (the stack of windows the game
 * has open, each window's cursor, the lists it built, the text it put in
 * the plane) and draws it. The game's windows are kept off the picture by
 * drawing plane A, wherever one sits, from what the game saved from under
 * it when it opened (it puts that back when the window closes), and by
 * leaving out its red cursor. The field behind is one picture, taken as the
 * first window opens and held until the last closes.
 *
 * A window not laid out anew is drawn as it stands: the game's own words,
 * read back from its tiles (the window font, or a line of dialogue), in
 * this menu's frame and font, and a picture in it (a portrait) from the
 * game's own tiles. Over the screens above, a message of a line or two
 * takes the foot's place and a question or a list comes up in the middle
 * (the Telepipe's places, the order the party stands in, yes or no); Talk
 * keeps the game's layout.
 *
 * Where a row is laid out across that the game has one under the other (the
 * members on the equipment, technique, skill and status screens, the
 * macros' letters), Left and Right steer it: fieldmenu_pad hands them to
 * the game as Up and Down.
 */
#ifndef PSIV_FIELDMENU_H
#define PSIV_FIELDMENU_H

#include <stdint.h>

void fieldmenu_set_enabled(int on);
int  fieldmenu_enabled(void);

/* once per frame, before its lines are drawn (frame.c): decides whether the
 * menu is up, and if so hides the game's windows from the picture */
void fieldmenu_frame(void);
/* the menu is up this frame: the front end draws it */
int  fieldmenu_active(void);
/* The pad the game sees. A row laid out across (the party on the equipment
 * screen, the macros' letters) is steered with Left and Right: while the
 * game's cursor is in it they are handed over as Up and Down, which its own
 * list (one under the other) takes, and Up and Down themselves are left
 * out. Everything else passes as it is; with nothing held, a click's or the
 * wheel's presses are played here. */
uint8_t fieldmenu_pad(uint8_t pad);
/* The mouse (and a finger): a click at (fx, fy), the whole picture's game
 * pixels as last drawn. The left button on a row that can be chosen picks
 * it -- the game's own cursor is walked there a press at a time, then C;
 * anywhere else it is C. The right button is B. 1 if the menu took it. */
int  fieldmenu_click(float fx, float fy, int right);
/* a turn of the wheel: the cursor a row on (down) or back */
void fieldmenu_wheel(int down);
/* draw over the whole picture, w x h game pixels at `scale` times the
 * game's resolution (the wide view, side panels included) */
void fieldmenu_draw(uint32_t *out, int out_stride, int scale, int w, int h);

#endif
