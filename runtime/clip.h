/*
 * Short clips of the picture, for the settings' previews (Settings > Display
 * and Battles: a setting off beside it on). Made once from the player's own
 * ROM by `psiv --headless --preview-scene ...` (headless.c), in the
 * background, and kept in the player's folder (previews/); nothing of the
 * game ships with the port.
 *
 * A clip is a run of frames, w x h, RGB565, each stored against the one
 * before it (XOR) and deflated (RFC 1951, fixed codes): a few hundred KB to
 * about 2 MB for four seconds, most of it rain.
 *
 *   "PSIVCLIP1"  w, h, frames, fps (16 bits each, big-endian)
 *   per frame:   its deflated size (32 bits), the deflated bytes
 */
#ifndef PSIV_CLIP_H
#define PSIV_CLIP_H

#include <stddef.h>
#include <stdint.h>

/* recording: frames given as 0xAARRGGBB, the top-left w x h of each */
int  clip_rec_begin(int w, int h, int fps);
void clip_rec_frame(const uint32_t *argb, int stride);
int  clip_rec_frames(void);
int  clip_rec_save(const char *path);          /* 1 saved (then the recording is gone) */

typedef struct {
    int w, h, n, fps;
    uint16_t *px;                              /* n frames of w * h, RGB565 */
} Clip;

/* a clip file, read whole into c: 1, or 0 (c left empty) */
int  clip_load(const char *path, Clip *c);
int  clip_unpack(const uint8_t *d, size_t len, Clip *c);
void clip_free(Clip *c);

/* raw deflate of n bytes into out (cap bytes): its size, 0 if it didn't fit */
size_t clip_deflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

#endif
