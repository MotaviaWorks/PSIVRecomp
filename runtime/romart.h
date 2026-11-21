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

/* a party member's field sprite (0 Chaz .. 10 Seth) as 16x32 RGBA, colour 0
 * transparent: frame 0-8 as the game keeps them (0 standing facing down, 1-2
 * its steps; 3-5 facing left; 6-8 facing up), flip: mirrored (facing right,
 * as the game draws it); 1 on success */
#define ROMART_FIELD_W 16
#define ROMART_FIELD_H 32
int romart_field_sprite(int character, int frame, int flip, uint8_t *rgba);

#endif
