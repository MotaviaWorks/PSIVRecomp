/*
 * The port's own sound effects (M7), synthesized at startup: nothing from the
 * game or from a sample library ships with the port.
 *
 * Each effect is a recipe -- noise and oscillators through moving filters,
 * envelopes shaped to the game's timing, a little stereo motion and a small
 * reverb -- rendered in float at SOUND_RATE and normalised. A variant number
 * nudges timing, pitch and direction so repeats don't sound identical.
 * Rendering is deterministic (seeded noise).
 */
#ifndef PSIV_SFXSYNTH_H
#define PSIV_SFXSYNTH_H

#include <stdint.h>

/* the recipes */
typedef struct {
    const char *name;      /* "slash", "slasher", "cursor", "select" */
    const char *what;      /* one line for the logs and the docs */
    int variants;          /* how many differ (for random picks) */
    int overlap;           /* how many of it may sound at once (1: a new one cuts the last) */
} SfxRecipe;

int  sfxsynth_count(void);
const SfxRecipe *sfxsynth_recipe(int i);
const SfxRecipe *sfxsynth_find(const char *name);

/* variant v of a recipe as interleaved stereo int16 at SOUND_RATE (malloc'd;
 * the caller frees it); NULL for an unknown name */
int16_t *sfxsynth_render(const char *name, int variant, int *frames);

/* a rendered effect as a 16-bit stereo WAV file, for listening; 1 on success */
int sfxsynth_write_wav(const char *path, const int16_t *pcm, int frames);

#endif
