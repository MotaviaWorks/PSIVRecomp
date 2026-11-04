/*
 * Sound effect replacement (M7): play your own samples instead of chosen
 * sound effects.
 *
 * DIR/sfx.ini maps the game's sound IDs (SFXID_* in ps4.constants.asm) to a
 * WAV file, or a folder of them (one is picked at random each time). When the
 * game asks for a mapped sound, the frame loop calls sfx_request(); the
 * sample starts and the request is dropped before the sound driver sees it,
 * so the original sound doesn't play. Only the sound driver's own RAM
 * changes; the game's logic never reads it, so nothing else differs.
 */
#ifndef PSIV_SFX_H
#define PSIV_SFX_H

#include <stdint.h>

int  sfx_load(const char *dir);          /* number of sound IDs mapped (0 if no sfx.ini) */
void sfx_set_enabled(int on);
int  sfx_enabled(void);

/* the game asked for this sound: 1 = a replacement started, drop the request */
int  sfx_request(uint8_t id);

/* add the playing samples to a frame of audio: interleaved stereo int16 at
 * SOUND_RATE (sound.h) */
void sfx_mix(int16_t *samples, int frames);

#endif
