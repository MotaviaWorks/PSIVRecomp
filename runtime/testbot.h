/*
 * A player for testing (psiv_headless --bot): from a battery save it
 * Continues, walks the world map back and forth until a random battle
 * starts, fights each battle to its end with planned commands, and after a
 * number of battles saves the game in slot 1. It only reads RAM and presses
 * the pad, like a player.
 *
 * tools/bizhawk/bot.lua is the same player for BizHawk, so the reference can
 * fight its own battles and the port replays them (tools/battle_test.py).
 */
#pragma once
#include <stdint.h>
#include <stdio.h>

/* seed: varies the choices; walk: "LR", "UD", "RL" or "DU" (the first leg,
 * then back); battles: how many to fight before saving; vehicle: board it
 * first (1 Land Rover, 2 Ice Digger, 3 Hydrofoil; 0 on foot) */
void testbot_start(uint32_t seed, const char *walk, int battles, int vehicle);
/* the pad for logical frame `logical` (called once per logical frame) */
uint8_t testbot_keys(uint64_t logical);
int testbot_on(void);
int testbot_done(void);
/* the input it gave, as a movie by logical frame (psiv_headless --movie) */
void testbot_movie(FILE *f);
