/*
 * Rewind (PsiLok's design, 2026-11-06): the picture fades to white, and fades
 * back in on the game as it was about ten seconds before -- one state kept
 * every ten seconds, the last two minutes of them. Pressed again while it
 * fades back in (or soon after), it goes back ten seconds more.
 *
 * A state is the whole machine -- the 68000, its RAM, the VDP, the Z80 and
 * its RAM, the YM2612 and the PSG, the bus -- and the game's own C stack,
 * where the recompiled game keeps its place (coro.h): the game runs on a
 * stack of its own and comes back to the program's stack at each frame,
 * where its stack can be copied. The battery save is left as it is: a game
 * saved after the state is still saved.
 */
#ifndef PSIV_REWIND_H
#define PSIV_REWIND_H

#include <stdint.h>

/* Run the game (`entry`: the console reset) on its own stack. Each frame
 * the game reaches comes back here: platform_frame shows it, then a state is
 * kept or put back. Never returns. */
void game_run(void (*entry)(void));
/* frame.c: the frame is drawn, over to platform_frame (on the program's
 * stack; called directly when game_run isn't running the game) */
void game_show_frame(const uint32_t *pixels, int width, int height);

void rewind_set_enabled(int on);
int  rewind_enabled(void);
/* start a rewind: 1 if there is a state to go back to (the fade starts), 0
 * if not yet */
int  rewind_request(void);
/* the fade: how white the picture is (0-1), and how loud the sound */
float rewind_white(void);
float rewind_volume(void);
/* how many seconds back the last rewind went (for a note on screen; 0 = none
 * lately) */
int rewind_note_seconds(void);

/* tests (headless): keep a state now; put back the one kept at `game_frame`
 * at once, without the fade (1 if there was one) */
void rewind_keep_now(void);
int  rewind_restore_frame(uint64_t game_frame);
/* the game's own frame count (logical frames), which goes back with a rewind */
uint64_t rewind_game_frame(void);
/* the front end's own counters go back too: called after a state is put back */
extern void (*rewind_on_restore)(uint64_t game_frame);

#endif
