/*
 * Wave Mode (built on the Boss Arena, arena.h): five battles in a row, timed.
 *
 * A run (wave_data.h, from tools/wave_spec.py) is five nodes: each a map of the game's,
 * where the party comes into it, a dot a few steps on, and one of the game's own
 * random-battle formations. The player picks a party from the run's roster and sets
 * their macros; both are fixed for the run.
 *
 * The run starts from a save made in memory (as the arena's) on the first node's map.
 * There the port walks the party to the dot -- the d-pad held a step at a time
 * (walkto.c), so the game's own walking, collision and camera run -- and starts a
 * random battle, the formation the node's: Battle_SetupEnemyData picks one from the
 * map's tables and the RNG, and a hook as it is about to unpack it (loc_7EF0, see
 * tools/emit.py) puts the node's in its place. When the enemies fall the victory is
 * skipped (no experience, meseta or drops: the battle's own last step, out to the
 * field), the party recovers some (40% of HP and TP standing, 25% revived if they
 * fell), and walks on: on the same map to the next dot, or to another map by a save of
 * the game as it is (the save block from RAM) at the next map's door, Continued unseen.
 *
 * The time counts only in battle (menus too), from the battle's start to its last
 * enemy falling. A loss (Game Over) ends the run; the fifth win clears it.
 */
#ifndef PSIV_WAVE_H
#define PSIV_WAVE_H

#include <stdint.h>

#include "arena.h"

#define WAVE_NODES 5
#define WAVE_ROSTER 10

typedef struct {
    const char *place;
    uint8_t world;
    uint16_t map, from;               /* the node's map, and the map its door is from */
    uint16_t x, y;                    /* where the party comes in: pixels, as a save keeps it */
    uint8_t steps;                    /* the dot: about this many steps' walk on */
    uint16_t formation;               /* 0..503, the game's own numbering ($80 a table) */
    const char *enemies;              /* for the menus */
} WaveNode;

typedef struct {
    const char *name;
    uint8_t level;
    uint8_t items[40];                /* the kit: the run's items, the same for everyone */
    uint8_t nroster;
    ArenaMember roster[WAVE_ROSTER];  /* who can be picked: levels, gear */
    WaveNode node[WAVE_NODES];
} WaveRun;

enum { WAVE_OFF, WAVE_LOADING, WAVE_WALKING, WAVE_FIGHTING, WAVE_RELOAD, WAVE_CLEARED, WAVE_LOST };

int wave_count(void);
const WaveRun *wave_run(int i);

/* A run: the party (roster indices, in order, 1 to 5) and its macros (ARENA_MACROS
 * bytes); then the save for its first node (ARENA_CART_SIZE bytes), Continued by the
 * front end, and wave_begin(). */
void wave_setup(int run, const uint8_t *picks, int n, const uint8_t *macros);
void wave_make_cart(uint8_t *cart);
void wave_begin(void);
/* WAVE_RELOAD: the next node is on another map -- the save of the game as it is, at its
 * door (made now, before the console starts over): Continued, then wave_begin() */
void wave_make_reload_cart(uint8_t *cart);
void wave_stop(void);

/* each frame, on the game's side (frame_run); returns wave_state() */
int wave_frame(void);
int wave_state(void);
int wave_current(void);               /* the run */
unsigned wave_serial(void);           /* counts the runs set up (music.c: a run's own file starts once) */
int wave_node(void);                  /* 0..4: the battle under way, or the next */
int wave_party(uint8_t *ids);         /* the party's character IDs; returns how many */
uint32_t wave_time(void);             /* frames in battle so far */
uint8_t wave_fell(void);              /* bit k: party member k has been knocked out this run */
int wave_player_input(void);          /* 1: the player's buttons go to the game (in battle) */
int wave_staging(void);               /* loading, or the party waiting to walk: nothing to see yet */

/* the formation the next random battle has (-1: the game's own); the hook (generated
 * code calls it) */
void wave_force_formation(int idx);
void psiv_battle_formation(void);

#endif
