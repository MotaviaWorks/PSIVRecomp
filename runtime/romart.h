/*
 * Art taken from the player's own ROM at runtime, for the port's own UI
 * (side panels, battle UI). Nothing from the game ships with the port.
 */
#ifndef PSIV_ROMART_H
#define PSIV_ROMART_H

#include <stdint.h>

/* Sega's Nemesis compression: decompress at most max bytes of 8x8 tiles
 * (4 bits per pixel, 32 bytes per tile); returns the bytes written */
int romart_nemesis(const uint8_t *src, uint32_t src_len, uint8_t *dst, int max);

/* the dialogue portrait of a party member (0 Chaz .. 10 Seth) as 48x48 RGBA,
 * colour 0 transparent; 1 on success */
#define ROMART_PORTRAIT 48
int romart_portrait(int character, uint8_t *rgba);

#endif
