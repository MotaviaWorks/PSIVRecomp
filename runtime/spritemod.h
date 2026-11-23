/*
 * Sprites from a mod (a first proof: the leader walking on the field).
 *
 * The game goes on exactly as it does: it moves its party, steps its two walking frames and
 * builds its sprites. The port only draws other pictures where the leader's pieces are, and
 * may draw more of them: a mod with 2 or 4 times the frames has its in-between frames shown
 * inside the time the game holds each of its own, so the walk takes as long as before.
 * Nothing the game can read is changed (its sprite collision flag is still worked out from
 * its own pixels: vdp.c).
 *
 * Enhanced only. For now set by PSIV_SPRITEMOD=FILE (no setting, no mods folder yet); the
 * file is what tools/spritemod_proof.py writes.
 */
#ifndef PSIV_SPRITEMOD_H
#define PSIV_SPRITEMOD_H

/* path: the sprite file, or NULL / "" for none. 1 if it loaded. */
int  spritemod_set_file(const char *path);
/* once a frame, before the picture is drawn */
void spritemod_frame(void);

#endif
