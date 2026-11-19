/*
 * Boss Arena: any of the game's boss fights, again, with the party the story
 * has at that point.
 *
 * Every boss fight in the game is one routine (RunEventBattle) driven by one
 * number, Event_Battle_Index: the enemies, the backdrop and the music all
 * come from it. The arena builds a save in memory for the boss -- the story
 * party rebuilt from the ROM's own level tables at the levels a player has
 * there, with their gear and items (arena_data.h) -- which the front end
 * puts in the cartridge's place for one Continue, never on disk. Once the
 * party stands on the field, arena_frame() finishes the characters with the
 * game's own routines (their gear's bonuses), and starts the fight the way
 * the bosses' own scripts do. It then reads the outcome from RAM: the game
 * back on the field means a win; back at the title (Game Over) a loss.
 *
 * Nothing of the game's code changes; with no fight chosen, arena_frame()
 * returns at once.
 */
#ifndef PSIV_ARENA_H
#define PSIV_ARENA_H

#include <stdint.h>

#define ARENA_CART_SIZE 0x2000        /* a cartridge in the port's layout: the header and slot 1 */

typedef struct {
    uint8_t id, level;                /* character (0 Chaz .. 10 Seth); level */
    uint8_t equip[4];                 /* right hand, left hand, head, body ($FF: as the game starts them) */
    uint8_t extra[4];                 /* skills the story gave besides the level tables' (the androids') */
} ArenaMember;

typedef struct {
    const char *name;
    uint8_t event;                    /* Event_Battle_Index */
    uint8_t key;                      /* its place among the game's 27 fights (the first Zio's left out): what
                                       * the player keeps per boss -- times, pictures, macros -- goes by it */
    uint8_t world;
    uint16_t map2, map, x, y;         /* where the party stands as the save loads */
    uint32_t meseta;
    uint8_t n;
    ArenaMember party[5];
    uint8_t items[40];
} ArenaBoss;

enum { ARENA_OFF, ARENA_LOADING, ARENA_FIGHTING, ARENA_WON, ARENA_LOST, ARENA_CAPTURING, ARENA_CAPTURED };

int arena_count(void);
const ArenaBoss *arena_boss(int i);   /* story order */

/* The save for boss i: the cartridge (ARENA_CART_SIZE bytes, the port's
 * layout) the game Continues from. Needs the ROM loaded (bus_init). */
void arena_make_cart(int i, uint8_t *cart);

/* The same for any party and place (`b`'s event unused), with these macros (NULL: none);
 * and a save of the game as it is now, at another place (the save block from RAM). The
 * sum is made last (arena_cart_seal: again after any change to the block). */
void arena_make_cart_for(const ArenaBoss *b, const uint8_t *macros, uint8_t *cart);
void arena_make_cart_here(uint8_t world, uint16_t map2, uint16_t map, uint16_t x, uint16_t y, uint8_t *cart);
void arena_cart_seal(uint8_t *cart);
/* Once such a save has loaded and the party stands on the field: their gear's bonuses, by
 * the game's own routines, and full HP and TP */
void arena_finish_party(const ArenaMember *party, int n);

/* Macros (Macro_Data, 160 bytes: 8 macros of 5 turns, each turn the character's ID, the
 * command -- 1 attack, 2 technique, 3 skill, 4 item, 5 defend -- its technique, skill or
 * item, and 0): those the next save made will hold (NULL: everyone attacks in A) */
#define ARENA_MACROS 160
void arena_set_macros(const uint8_t *m);
void arena_default_macros(int i, uint8_t *m);
/* the macros as they were in the fight last followed (the player may set them in battle) */
const uint8_t *arena_fight_macros(void);
/* party member k of boss i: the techniques and skills the level tables give (and the story),
 * as the save will have them; returns the counts through nt, ns */
void arena_member_arts(int i, int k, uint8_t techs[16], int *nt, uint8_t skills[8], int *ns);
void arena_member_arts_of(const ArenaMember *m, uint8_t techs[16], int *nt, uint8_t skills[8], int *ns);
void arena_member_char(const ArenaMember *m, uint8_t *c);   /* the $80 bytes a save holds for them */

/* Fight boss i once its save has loaded (-1: no fight). */
void arena_begin(int i);
int  arena_state(void);
int  arena_current(void);
int  arena_fight_frames(void);        /* how long the fight has gone on (or went on) */
int  arena_staging(void);             /* the save loading, the party waiting on the field: the fight not asked for yet */
const char *arena_music(int i);       /* the music the game plays for it */

/* Scanning (arenapic.h): the fight is only staged -- once its command menu waits, the
 * boss is recorded as it idles (ARENA_CAPTURING), then ARENA_CAPTURED: the front end
 * packs the picture (arenapic_capture_pack) and moves on. */
void arena_set_scan(int on);

/* Each frame, on the game's side (frame_run): sets the fight up, then
 * follows it. Returns arena_state(). */
int  arena_frame(void);

#endif
