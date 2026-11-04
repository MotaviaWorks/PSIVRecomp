#ifndef PSIV_FRAME_H
#define PSIV_FRAME_H

#include <stdint.h>
#include "vdp.h"

/* The finished frame, 0xAARRGGBB, VDP_MAX_W pixels per row. */
extern uint32_t frame_pixels[VDP_MAX_W * VDP_MAX_H];
extern uint64_t frame_count;
/* The same frame as separate layers, line by line (for enhancements that
 * recompose it, and for layer dumps). Valid inside platform_frame. */
extern VdpLine frame_lines[VDP_MAX_H];

/* Called once per frame after rendering, before the game's VInt runs: show
 * the frame, read input into bus_pad[], pace to 60 Hz. Implemented by the
 * front end (main_sdl.c, headless.c). */
void platform_frame(const uint32_t *pixels, int width, int height);

/* One frame of audio: interleaved stereo int16 at SOUND_RATE (sound.h). */
void platform_audio(const int16_t *samples, int count);

/* Render a frame and deliver HInt/VInt. Called from the WaitForVInt hook. */
void frame_run(void);

/* True if this frame ends with the game's VInt actually running (a logical
 * frame, in reference-trace terms). Valid inside platform_frame. */
int frame_vint_will_fire(void);

/* Run one VInt as the original does while the game is still busy: VInt_Flag
 * clear, so only the sound driver runs. Used to replay recorded lag. */
void frame_lag_vint(void);

/* Lag VInts to run right after this frame's VInt (reference replay only). */
extern int frame_pending_lag;

#endif
