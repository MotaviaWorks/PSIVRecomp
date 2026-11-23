/*
 * Sprites from a mod (a proof: the characters walking on the field, the party and the townspeople).
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

#endif
