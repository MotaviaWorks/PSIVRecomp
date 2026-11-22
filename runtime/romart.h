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

/* the picture of a party member on the game's Status page (0 Chaz .. 10 Seth) as 80x80 RGBA, colour 0
 * transparent: the same faces as the dialogue's, drawn larger and finer; 1 on success */
#define ROMART_PROFILE 80
int romart_profile(int character, uint8_t *rgba);
/* the same as the palette's entries (0-15 of line 2; 0: nothing there), for drawing in the colours the
 * game has loaded now (a fade, a flash); 1 on success */
int romart_profile_index(int character, uint8_t *index);
/* whose dialogue portrait these 36 tiles are (as they stand in VRAM at tile $55C): 0 Chaz .. 10 Seth;
 * -1: someone else's, or no portrait at all */
int romart_portrait_who(const uint8_t *tiles);

/* One of the game's story panels (the manga illustrations of its cutscenes), by the number the game knows it
 * by (Panel_Create's), as it draws it: its two layers, the front over the back. A new buffer of *w x *h RGBA
 * (the caller frees it), nothing drawn black; NULL if there is no such panel. */
uint8_t *romart_panel(int index, int *w, int *h);

/* a party member's field sprite (0 Chaz .. 10 Seth) as 16x32 RGBA, colour 0
 * transparent: frame 0-8 as the game keeps them (0 standing facing down, 1-2
 * its steps; 3-5 facing left; 6-8 facing up), flip: mirrored (facing right,
 * as the game draws it); 1 on success */
#define ROMART_FIELD_W 16
#define ROMART_FIELD_H 32
int romart_field_sprite(int character, int frame, int flip, uint8_t *rgba);

#endif
