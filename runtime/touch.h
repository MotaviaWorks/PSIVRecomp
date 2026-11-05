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
int     touch_visible(void);
void    touch_hide(void);                  /* a gamepad or the keyboard took over */
void    touch_suspend(int on);             /* on: no pad, every touch a tap (the launcher) */
void    touch_draw(SDL_Renderer *r, int ow, int oh);

#endif
