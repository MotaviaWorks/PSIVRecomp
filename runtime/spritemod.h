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

/* paths: the sprite files with a ; between them, or NULL / "" for none. How many loaded. */
int  spritemod_set_file(const char *paths);
/* once a frame, before the picture is drawn */
void spritemod_frame(void);

#endif
