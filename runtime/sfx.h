/*
 * Sound effects (M7): the port's own instead of the game's, or your samples.
 *
 * Two sets of replacements, by the game's sound IDs (SFXID_* in
 * ps4.constants.asm):
 *   - modern: the port's own effects, synthesized at startup (sfxsynth.c), so
 *     a public build ships no sound files
 *   - custom: your samples, DIR/sfx.ini mapping an ID to a WAV file or a
 *     folder of them (one picked at random each time), or to a synth recipe
 *     ("synth:NAME"); IDs it doesn't map use the modern set
 * When the game asks for a replaced sound, the frame loop calls
 * sfx_request(); the replacement starts and the request is dropped before the
 * sound driver sees it, so the original doesn't play. Only the sound driver's
 * own RAM changes; the game's logic never reads it, so nothing else differs.
 */
#ifndef PSIV_SFX_H
#define PSIV_SFX_H

#include <stdint.h>

enum { SFX_ORIGINAL, SFX_MODERN, SFX_CUSTOM };

int  sfx_synth_load(void);               /* render the modern set; number of sound IDs it replaces */
int  sfx_load(const char *dir);          /* your samples: number of IDs mapped (0 if no sfx.ini) */
void sfx_set_mode(int mode);             /* SFX_ORIGINAL, SFX_MODERN or SFX_CUSTOM */
int  sfx_mode(void);
int  sfx_enabled(void);                  /* some sound is replaced in the current mode */

/* write every synthesized effect (each variant) to DIR as NAME_vN.wav; count written */
int  sfx_synth_dump(const char *dir);

/* the game asked for this sound: 1 = a replacement started, drop the request */
int  sfx_request(uint8_t id);

/* add the playing samples to a frame of audio: interleaved stereo float at
 * 16-bit scale, SOUND_RATE (sound.h), before the mix bus (mixbus.h) */
void sfx_mix(float *samples, int frames);

#endif
