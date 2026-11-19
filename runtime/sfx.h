/*
 * Sound effects (M7): modern sounds instead of the game's, where there is one.
 *
 * Replacements, by the game's sound IDs (SFXID_* in ps4.constants.asm):
 *   - the library: the modern sounds that ship with the game (res/sounds in
 *     the repo, "sounds" beside the program): XX_1.wav .. XX_4.wav for sound
 *     ID XX, one picked at random each time, and sounds.ini for their levels
 *     (made from CC0 recordings: res/sounds/CREDITS.txt)
 *   - custom: your samples, DIR/sfx.ini mapping an ID to a WAV file or a
 *     folder of them (one picked at random each time), or to a synth recipe
 *     ("synth:NAME"); they come before the library's
 *   - synth: the first modern set, synthesized (sfxsynth.c). Retired from the
 *     game 2026-11-16; headless --sfx-synth still plays it (SFX_MODERN)
 * An effect with no replacement is the game's own (on the chip, or on Algol).
 * When the game asks for a replaced sound, the frame loop calls
 * sfx_request(); the replacement starts and the request is dropped before the
 * sound driver sees it, so the original doesn't play. Only the sound driver's
 * own RAM changes; the game's logic never reads it, so nothing else differs.
 */
#ifndef PSIV_SFX_H
#define PSIV_SFX_H

#include <stdint.h>

/* SFX_ORIGINAL: nothing replaced; SFX_CUSTOM: yours, then the library ("Modern
 * sounds: On"); SFX_MODERN: the synthesized set alone (headless, for study) */
enum { SFX_ORIGINAL, SFX_MODERN, SFX_CUSTOM };

int  sfx_synth_load(void);               /* render the synthesized set; number of sound IDs it replaces */
int  sfx_load(const char *dir);          /* your samples: number of IDs mapped (0 if no sfx.ini) */
/* The library in DIR (XX_1.wav .. XX_4.wav, sounds.ini): number of IDs it has.
 * read gives a file's contents (malloc'd, freed with done), or NULL: so the
 * app can read the APK's assets; both NULL = plain files */
int  sfx_library_load(const char *dir, void *(*read)(const char *path, long *size), void (*done)(void *));
int  sfx_library_count(void);
void sfx_set_mode(int mode);             /* SFX_ORIGINAL, SFX_MODERN or SFX_CUSTOM */
/* The effects that are not replaced are the game's own, on the chips -- or, with
 * this on, on Algol's clean chip (sound.c, algol2.h). Apart from the mode: a
 * replaced effect never reaches the driver, the rest play on whichever this says */
void sfx_set_algol(int on);
int  sfx_algol(void);
int  sfx_mode(void);
int  sfx_enabled(void);                  /* some sound is replaced in the current mode */

/* write every synthesized effect (each variant) to DIR as NAME_vN.wav; count written */
int  sfx_synth_dump(const char *dir);

/* the game asked for this sound: 1 = a replacement started, drop the request */
int  sfx_request(uint8_t id);

/* add the playing samples to a frame of audio: interleaved stereo float at
 * 16-bit scale, SOUND_RATE (sound.h), before the mix bus (mixbus.h) */
void sfx_mix(float *samples, int frames);
void sfx_stop_all(void);
/* a WAV file's contents as stereo int16 at SOUND_RATE (malloc'd), or NULL */
int16_t *sfx_decode_wav(const uint8_t *buf, long size, int *frames_out);                 /* silence what is playing (a rewind) */

#endif
