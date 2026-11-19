/*
 * The headless front end (headless.c), built into the game: `psiv --headless
 * [options]` runs it in place of the window (main_sdl.c hands over before
 * anything of its own starts). The game uses it to make its settings
 * previews (clip.h) in the background, from the player's ROM.
 */
#define PSIV_IN_APP 1
#include "headless.c"
