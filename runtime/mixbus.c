/*
 * The mix bus: see mixbus.h.
 *
 * The limiter is the usual look-ahead design. Each sample needs a gain of
 * ceiling / peak (1 under the ceiling). The smallest need in a window of
 * LOOKAHEAD + 1 samples is held; the gain falls to it at once and recovers
 * exponentially (RELEASE); that is averaged over LOOKAHEAD samples, so the
 * gain slopes down over the look-ahead instead of stepping; and the audio is
 * delayed by LOOKAHEAD. A peak leaves the delay exactly when the averaged
 * gain has reached what it needs, so nothing goes over the ceiling, and
 * under it the gain is exactly 1.
 */
#include "mixbus.h"
#include "sound.h"

#include <math.h>
#include <string.h>

#define LOOKAHEAD 64                      /* samples: 1.2 ms */
#define CEILING   30000.0f                /* -0.8 dBFS: room for resampling's overshoot */
#define RELEASE   0.080                   /* seconds */

static int limiter = -1;                  /* -1: not set yet */
static float delay[LOOKAHEAD][2];         /* the audio, LOOKAHEAD samples late */
static float need[LOOKAHEAD + 1];         /* the gain each sample in the window needs */
static float smooth[LOOKAHEAD];           /* the held, released gains averaged */
static int dpos, npos;
static double held = 1.0, release_k;   /* double: in float the last steps to 1 round away */
static uint64_t n_limited, n_total;
static float lowest = 1.0f;

static void reset(void) {
    memset(delay, 0, sizeof delay);
    for (int i = 0; i <= LOOKAHEAD; i++) need[i] = 1.0f;
    for (int i = 0; i < LOOKAHEAD; i++) smooth[i] = 1.0f;
    dpos = npos = 0;
    held = 1.0;
    release_k = 1.0 - exp(-1.0 / (RELEASE * SOUND_RATE));
}

void mixbus_set_limiter(int on) {
    on = on != 0;
    if (on == limiter) return;
    limiter = on;
    reset();
}

static int16_t to16(float s) {
    long v = lrintf(s);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void mixbus_finish(const float *in, int16_t *out, int frames) {
    if (limiter < 0) mixbus_set_limiter(0);
    n_total += (uint64_t)frames;
    if (!limiter) {
        /* the game's own sound: whole numbers, converted as they always were */
        for (int i = 0; i < frames * 2; i++) out[i] = to16(in[i]);
        return;
    }
    for (int n = 0; n < frames; n++) {
        float l = in[n * 2], r = in[n * 2 + 1];
        float peak = fmaxf(fabsf(l), fabsf(r));
        need[npos] = peak > CEILING ? CEILING / peak : 1.0f;
        if (++npos > LOOKAHEAD) npos = 0;
        float hold = 1.0f;
        for (int i = 0; i <= LOOKAHEAD; i++) hold = fminf(hold, need[i]);
        held = hold < held ? hold : held + (hold - held) * release_k;
        if (hold == 1.0f && held > 0.99999) held = 1.0;               /* back to exactly 1: untouched again */
        smooth[dpos] = (float)held;
        float sum = 0.0f;
        for (int i = 0; i < LOOKAHEAD; i++) sum += smooth[i];
        float g = sum / (float)LOOKAHEAD;
        float dl = delay[dpos][0], dr = delay[dpos][1];
        delay[dpos][0] = l;
        delay[dpos][1] = r;
        if (++dpos == LOOKAHEAD) dpos = 0;
        if (g < 1.0f) {
            n_limited++;
            if (g < lowest) lowest = g;
            dl *= g;
            dr *= g;
        }
        out[n * 2] = to16(dl);
        out[n * 2 + 1] = to16(dr);
    }
}

void mixbus_stats(uint64_t *limited, uint64_t *total, double *deepest_db) {
    *limited = n_limited;
    *total = n_total;
    *deepest_db = 20.0 * log10((double)lowest);
}

static float level_music = 1.0f, level_effects = 1.0f, level_voices = 1.0f;

static float level(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

void mixbus_set_levels(float music, float effects, float voices) {
    level_music = level(music);
    level_effects = level(effects);
    level_voices = level(voices);
}

float mixbus_music(void) { return level_music; }
float mixbus_effects(void) { return level_effects; }
float mixbus_voices(void) { return level_voices; }

