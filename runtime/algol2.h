/*
 * Algol (an option): the game's own instruments, clean -- the chip, remastered.
 *
 * (Proto Algol, the first one, algol.c, gives each FM patch one of its own ten
 * instruments.) Algol keeps the composer's: it is the YM2612 again, in floating point -- every
 * patch's four operators, its algorithm and feedback, the chip's envelope
 * generator rate for rate (so notes swell and fade exactly as they do on the
 * chip), key scaling, detune, the LFO, channel 3's own frequencies -- without
 * the chip's 9-bit output and the step in its DAC. The PSG's three squares are
 * drawn band-limited, its noise from the same shift register.
 *
 * Then some studio polish, chosen for each note from its patch (a bass stays
 * in the middle; a lead, a pad or a bell is doubled a few cents apart and
 * spread across the stereo field), and a reverb. PSIV_ALGOL2_DRY=1 leaves the
 * polish out: the chip's own mix, clean (for measuring one against the other).
 *
 * The drums are the game's own samples from the ROM, played as the chip plays
 * them: each value held, at the driver's own rates, the track's rests stopping
 * them; with the polish, panned and a little smoothed.
 *
 * (The files keep the names algol2 and the switch algol_set_version(2) from
 * when this was "Algol 2", beside the first.)
 */
#ifndef PSIV_ALGOL2_H
#define PSIV_ALGOL2_H

#include <stdint.h>

void algol2_reset(void);                  /* the console reset: the chips start over */
void algol2_silence(void);                /* every voice quiet, the registers kept */

/* the chips' writes, all along (algol.c passes them on): the registers are
 * always current, whichever plays */
void algol2_ym_write(int part, uint8_t reg, uint8_t value);
void algol2_psg_write(uint8_t value);
void algol2_dac_hit(int sample, int pitch, int volume, int pan);   /* drum sample 1-17 */
void algol2_dac_stop(void);               /* a rest in the drum track: the ROM's sample stops, as the chip's does */

/* one output sample, the chips' scale; fx_fm / fx_psg: the channels a sound
 * effect holds (the original chips play those) */
void algol2_render(float *left, float *right, unsigned fx_fm, unsigned fx_psg);

/* The sound effects on Algol too (Settings > Sound > Sound effects > Algol): the
 * channels an effect holds are played here instead of on the chips, each voice
 * with a polish of its own by the effect's kind (a menu's tick stays small and
 * close; a blow gets a room; a spell or thunder a hall and more width), into a
 * mix apart from the song's. The effects are still the driver's own, note for
 * note: only what plays them changes.
 *   algol2_render_split   one sample: the song into ml / mr if music, the
 *                         effects' channels into el / er if effects (either
 *                         pair may be NULL)
 *   algol2_fx_tracks      which effect holds each FM channel (6) and PSG
 *                         channel (4): its sound ID, 0 for none, $FF unknown */
void algol2_render_split(float *ml, float *mr, float *el, float *er, unsigned fx_fm, unsigned fx_psg, int music, int effects);
void algol2_fx_tracks(const uint8_t *fm_id, const uint8_t *psg_id);

#endif
