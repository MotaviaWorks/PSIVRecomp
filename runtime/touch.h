/*
 * Touch controls: an on-screen Mega Drive pad -- d-pad, A B C, Start -- and a
 * menu button, drawn over the game while touch is in use. A gamepad or a key
 * hides it; the next touch brings it back.
 *
 * A tap away from the controls is a click (the system menu, the battle
 * menus, click to walk). The controls slide: a thumb can roll from B to C,
 * and the d-pad follows the thumb in eight directions.
 */
#ifndef PSIV_TOUCH_H
#define PSIV_TOUCH_H

#include <SDL3/SDL.h>
#include <stdint.h>

void    touch_init(int visible);           /* shown at the start (a phone without a gamepad) */

/* a finger event, with the render output's size in pixels; returns 1 for a
 * tap away from the controls, at (*tap_x, *tap_y) in render pixels */
int     touch_event(const SDL_Event *e, int ow, int oh, float *tap_x, float *tap_y);

uint8_t touch_pad(void);                   /* PAD_* bits held on the on-screen pad */
int     touch_take_menu(void);             /* 1 once after the menu button was pressed */
/* two fingers put down together away from the controls are the rewind button: 1 once after they tapped (let go
 * within half a second, without sliding), 2 once when they have been held that long; 0 otherwise */
int     touch_take_rewind(void);
int     touch_visible(void);
void    touch_hide(void);                  /* a gamepad or the keyboard took over */
void    touch_suspend(int on);             /* on: no pad, every touch a tap (the launcher) */
void    touch_draw(SDL_Renderer *r, int ow, int oh);

/* Settings > Controls: how large the controls are and how strongly they are drawn, each a percentage of the
 * usual (opacity 0: unseen, still working); and where each stands, as touch_layout_get gave it ("" the usual
 * places) */
void    touch_set_look(int size_pct, int opacity_pct);
void    touch_set_layout(const char *s);
void    touch_layout_get(char *out, size_t n);

/* arranging them: while on, the controls are shown alone (whatever touch_visible and touch_suspend say), a
 * finger or the mouse drags the d-pad, A B C (together), Start or the menu button, and two buttons at the top
 * put them back and end it. touch_edit_pointer is the mouse: phase 0 down, 1 moved, 2 up, in render pixels. */
void    touch_edit(int on);
int     touch_editing(void);
void    touch_edit_pointer(int phase, float x, float y, int ow, int oh);
int     touch_edit_take_done(void);        /* 1 once after Done was pressed */

#endif
