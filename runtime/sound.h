#ifndef PSIV_SOUND_H
#define PSIV_SOUND_H

#include <stdint.h>
#include <stdio.h>

/* Output: interleaved stereo int16 at SOUND_RATE (the YM2612's native rate). */
#define SOUND_RATE 53267

void sound_init(void);

/* 68000 side of the Z80 area ($A00000-$A0FFFF), bus request and reset */
uint8_t  sound_68k_read(uint32_t addr);
void     sound_68k_write(uint32_t addr, uint8_t v);
uint16_t sound_busreq_read(void);
void     sound_busreq_write(uint16_t v);
void     sound_reset_write(uint16_t v);
void     sound_psg_write(uint8_t v);          /* $C00011 */

/* Run the Z80, YM2612 and PSG to the end of this frame (called once per
 * frame after VInt). Returns the number of stereo samples now in *out. */
int sound_end_frame(const int16_t **out);

/* VGM-style write log for checking against the VGM rips: one line per chip
 * write, "sample chip reg value". NULL turns it off. */
void sound_set_log(FILE *f);

#endif
