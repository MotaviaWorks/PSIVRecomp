/*
 * Rumble (an option): what happens in the game, felt on a gamepad's two motors
 * or a phone's vibration motor. The Mega Drive had none, so the port reads
 * the moments from the game's own memory -- reads only: the game's state is
 * never touched, so the replays and the BizHawk comparisons don't change.
 *
 *   - battles: a blow landing, with the target's hit reaction (stronger the
 *     more of its HP it takes, when the game has worked that out already;
 *     the party's own hurt felt heavier than the enemies'), a critical hit
 *     (one big blow, with the game's white flash), a party member falling,
 *     an enemy destroyed, the final bosses' deaths, the big techniques
 *   - the field: doors, slabs, elevators, breaking ice and barriers,
 *     explosions, a ship's engines
 *
 * The frame loop reports each frame (rumble_frame, rumble_after_vint); the
 * front end asks what to play (rumble_motors, rumble_take_pulse) on its own
 * clock, so effects keep their length whatever the frame rate.
 */
#ifndef PSIV_RUMBLE_H
#define PSIV_RUMBLE_H

#include <stdint.h>

#define RUMBLE_LEVELS 4                       /* off, light, normal, strong */
void        rumble_set_level(int level);
int         rumble_get_level(void);
const char *rumble_level_name(int level);

/* The frame loop: once a frame when its work is done, before the vertical-
 * blank interrupt (HP and the sound requests are read), and once after it
 * (what the sound driver left in its queue). */
void rumble_frame(void);
void rumble_after_vint(void);

/* Both motors at `now_ms` (any steady clock in milliseconds): 0..1 each, the
 * level applied. low = the heavy motor, high = the light one. Call once per
 * displayed frame or so; effects reported since the last call start now. */
void rumble_motors(uint64_t now_ms, float *low, float *high);

/* For a one-motor vibrator (a phone), after rumble_motors: 1 if a pulse
 * should start now -- its strength (0..1, the level applied) and length (at
 * most half a second). A weaker effect doesn't cut a stronger one short. */
int  rumble_take_pulse(uint64_t now_ms, float *strength, int *ms);

void rumble_stop(void);                       /* forget what's playing (paused, the menu) */
void rumble_test(void);                       /* a sample of the current level (the setting changed) */

/* testing: one line per effect as it's detected (NULL: none) */
void rumble_set_log(void (*log)(const char *line));

#endif
