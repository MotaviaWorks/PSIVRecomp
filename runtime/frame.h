#ifndef PSIV_FRAME_H
#define PSIV_FRAME_H

#include <stdint.h>
#include "vdp.h"

/* The finished frame, 0xAARRGGBB, VDP_MAX_W pixels per row. */
extern uint32_t frame_pixels[VDP_MAX_W * VDP_MAX_H];
extern uint64_t frame_count;

/* Called once per frame after rendering, before the game's VInt runs: show
 * the frame, read input into bus_pad[], pace to 60 Hz. Implemented by the
 * front end (main_sdl.c, headless.c). */
void platform_frame(const uint32_t *pixels, int width, int height);

/* Render a frame and deliver HInt/VInt. Called from the WaitForVInt hook. */
void frame_run(void);

#endif
