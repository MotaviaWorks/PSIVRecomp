/*
 * Algol (M7, an option): a modern sound chip for the game's music.
 *
 * The music driver (SMPS, on the 68000) writes the YM2612 and the PSG, and
 * the Z80 plays its drum samples. The port keeps those chips running for the
 * game, which reads their status; with Algol on, what they are told is also
 * decoded into notes -- pitch, instrument (the FM patch), loudness, pan, key
 * on and off, drum hits -- and played on a modern synthesizer instead: each
 * FM patch is voiced by an instrument chosen from its sound (bass, lead,
 * brass, strings, pluck, bell, organ, keys), the PSG by soft synth leads and
 * filtered-noise hats, the drum samples by a synthesized kit (or the ROM's
 * own sample, played cleanly, for the voices and effects). Reverb, chorus
 * and a stereo delay on top. Sound effects keep the original chips: the
 * driver's track RAM says which channels an effect has taken over.
 *
 * Nothing ships but code; the game's state is never touched.
 */
#ifndef PSIV_ALGOL_H
#define PSIV_ALGOL_H

#include <stdint.h>

void algol_set_enabled(int on);
int  algol_enabled(void);
void algol_reset(void);

/* the chips' writes, as sound.c makes them */
void algol_ym_write(int part, uint8_t reg, uint8_t value);    /* part 0/1 = the YM2612's two halves */
void algol_psg_write(uint8_t value);
void algol_dac_hit(int sample, int pitch, int volume, int pan); /* the Z80 told to play drum sample 1-17 */
void algol_driver_tick(void);                                  /* after each run of the music driver */

/* channels an effect holds right now, as ym_generate_mask() takes them (bit
 * n = FM channel n+1, bit 5 = channel 6 / the DAC), and the PSG's (bit n =
 * PSG channel n, bit 3 = noise): the original chips play those */
unsigned algol_fm_effects(void);
unsigned algol_psg_effects(void);

/* one output sample of the music, about the level of the original chips */
void algol_render(float *left, float *right);

/* An instrument bank from a SNES game you own (snesbank.h): its samples in
 * place of Algol's own instruments and drums, the rest as before. NULL or ""
 * goes back to the synth. Returns 1 if the ROM is a game the port knows. */
int algol_set_bank(const char *rom_path);
const char *algol_bank_name(void);           /* "" when none */

#endif
