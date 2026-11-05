/*
 * The last stage of the sound: the chips, the port's sound effects and your
 * music, summed in float at 16-bit scale, made into the 16-bit samples the
 * front ends play.
 *
 * With the game's own sound only (Classic, and the reference checks), the
 * samples are converted exactly as they always were. With the port's
 * additions, a look-ahead peak limiter keeps the sum under the ceiling: it
 * sees a peak 1.2 ms before it plays and turns the whole mix down smoothly
 * just for it, coming back over ~80 ms, instead of bending each sample that
 * goes over (heard as distortion).
 */
#ifndef PSIV_MIXBUS_H
#define PSIV_MIXBUS_H

#include <stdint.h>

/* 1 = the limiter on (the port's sound is in the mix); 0 = straight
 * conversion. Switching it costs a 1.2 ms jump; it's meant for settings
 * changes, not every frame. */
void mixbus_set_limiter(int on);

/* frames stereo samples from in (float, 16-bit scale) to out */
void mixbus_finish(const float *in, int16_t *out, int frames);

/* since the start: samples turned down by the limiter, all samples, and the
 * deepest it went (dB, <= 0) */
void mixbus_stats(uint64_t *limited, uint64_t *total, double *deepest_db);

#endif
