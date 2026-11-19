/*
 * Smoothing for the port's own picture.
 *
 * With Smoothing on (video.filter = linear) the graphics card blends the
 * game's 320-wide picture as it scales it to the window. But a frame the
 * port composes itself (modern text or menus up, the field's water, a
 * battle's depth) reaches the card already at 2 to 4 times the game's size,
 * its pixels as sharp blocks, and the card's blend then hardly shows: the
 * picture went sharp whenever a text box came up and soft again when it
 * went (GitHub issue 3). smooth_soften gives the composed picture the same
 * softness: each of its pixels becomes the average over one game pixel's
 * width and height around it, which for blocks of `scale` is exactly the
 * blend the card makes of the small picture. The port's text and menus are
 * drawn after it, and stay crisp.
 */
#ifndef PSIV_SMOOTH_H
#define PSIV_SMOOTH_H

#include <stdint.h>

/* Soften the w x h pixels at buf (ARGB, `stride` pixels a row), composed at
 * `scale` times the game's size. scale 1: nothing to do (the card's blend
 * is the whole of it). Alpha comes out opaque. */
void smooth_soften(uint32_t *buf, int stride, int w, int h, int scale);

#endif
