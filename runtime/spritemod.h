/*
 * Sprites from a mod (a proof: the characters walking on the field, the party and the townspeople; and
 * stills: the party's portraits, see spritemod_still).
 *
 * The game goes on exactly as it does: it moves its party, steps its two walking frames and
 * builds its sprites. The port only draws other pictures where the leader's pieces are, and
 * may draw more of them: a mod with 2 or 4 times the frames has its in-between frames shown
 * inside the time the game holds each of its own, so the walk takes as long as before.
 * Nothing the game can read is changed (its sprite collision flag is still worked out from
 * its own pixels: vdp.c).
 *
 * A file is for one kind of character: the one whose table of walking animations (in the ROM)
 * it names. Every object on the field with that table is shown with the file's pictures: the
 * leader, the followers, or all the townspeople of a kind.
 *
 * Enhanced only. For now set by PSIV_SPRITEMOD=FILE[;FILE...] (no setting, no mods folder
 * yet); the files are what tools/spritemod_proof.py writes. PSIV_SPRITEMOD_LOG=1 lists who is
 * on the screen and each one's table.
 */
#ifndef PSIV_SPRITEMOD_H
#define PSIV_SPRITEMOD_H

#include <stdint.h>

/* paths: the sprite files with a ; between them, or NULL / "" for none. How many loaded. */
int  spritemod_set_file(const char *paths);
/* once a frame, before the picture is drawn */
void spritemod_frame(void);
/* A mod's finer sprites (more pixels than the game's own, 2 or 4 to each along a side). The game's sprite
 * drawing carries them at its own size; where the picture is enlarged for the screen, their detail is
 * drawn over it. spritemod_fine: this frame has some. spritemod_draw: `lines` the frame's w x h lines,
 * `out` the picture at ds times the game's size, `stride` its row length. */
struct VdpLine;
int  spritemod_fine(void);
void spritemod_draw(const void *lines, int w, int h, uint32_t *out, int stride, int ds);

/* A mod's stills: pictures with no walk. kind 'P': the Status page's picture of a party member (80 x 80, the
 * face the port shows large); 'D': the dialogue's portrait of them (48 x 48). who: 0 Chaz .. 10 Seth (the file's
 * key is the letter and that number, in hex: P0 .. PA). A still may have up to 8 frames that play (a blink, the
 * eyes, the hair): frame 0 rests, the others follow, as the file says (spritemod_still_now picks).
 * spritemod_still returns frame `frame` at the game's size (0 nothing, 1-15 a colour of the party's palette
 * line, 255 the game's own pixel there), or NULL when no mod has it. *res and *fine (either may be NULL): if the
 * file is finer, its pixels to each of the game's along a side and the frame as the file has it, (side res)
 * squared; otherwise 1 and NULL. */
const uint8_t *spritemod_still(char kind, int who, int frame, int *res, const uint8_t **fine);
int spritemod_still_frames(char kind, int who);      /* how many frames a mod's still has; 0: no mod has one */
int spritemod_still_now(char kind, int who);         /* the frame of it to show now (0 with none, or one frame) */
/* a number that changes whenever the mods' files are set again: what was made from them is then out of date */
unsigned spritemod_generation(void);

#endif
