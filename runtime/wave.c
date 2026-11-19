/* Wave Mode: see wave.h */
#include "wave.h"
#include "m68k.h"
#include "music.h"
#include "walkto.h"

#include <string.h>

#include "wave_data.h"

/* RAM ($FFxxxx); see the disassembly's ps4.constants.asm */
#define GAME_MODE      0xEF00u           /* Game_Mode_Index: $C the field, $14 a battle, below 8 the title */
#define GAME_ROUTINE   0xEC20u           /* Game_Mode_Routine: 0 the party walks free; $14 to a random battle */
#define FIELD_MAP      0xEC28u           /* Field_Map_Index */
#define WIN_END        0xEC98u           /* the end of the plane mappings a window saved: $6040 = none */
#define RANDOM_BATTLES 0xECE7u           /* Random_Battles_Flag (the map's: off while the run walks) */
#define BATTLE_TYPE    0xECEEu           /* 2: a random battle */
#define EVENT_BATTLE   0xECFCu           /* Event_Battle_Index ($FF: a random battle) */
#define BATTLE_ROUTINE 0x4108u           /* Battle_Routine_2 (& $7FFF): $1E the victory's window */
#define LAST_MESSAGE   0x802Fu           /* ... $2F Battle_LastMessage, bit 15 set: out to the field */
#define SOUND_RESTORE  0xEE64u           /* $FF: Battle_LastMessage puts the field's music back */
#define MACRO_DATA     0xF444u
#define CHAR_STATS     0xF500u           /* $80 a character: curr_hp $E, max_hp $10, curr_tp $12, max_tp $14, status $16 */
#define DEAD_BITS      0x44              /* status: bit 2 dead, bit 6 dead (androids) */

#define SETTLE         20                /* frames standing on the field before the walk */

/* Battle_FormationData1..4: Kosinski-packed, $80 formations each, $FF after each */
static const uint32_t FORMATION_TABLES[4] = {0x283E6C, 0x2842BC, 0x28471C, 0x284B8C};

static int forced = -1;
void wave_force_formation(int idx) { forced = idx >= 0 && idx < 4 * 0x80 ? idx : -1; }

/* At loc_7EF0, with d0 the formation's number in the table at a0 (unpacked next): ours instead.
 * Only for a random battle -- a boss's (Event_Battle_Index set) is never touched. */
void psiv_battle_formation(void) {
    if (forced < 0 || m68k_ram[EVENT_BATTLE] != 0xFF) return;
    D(0) = (D(0) & 0xFFFF0000u) | (uint32_t)(forced & 0x7F);
    A(0) = FORMATION_TABLES[forced >> 7];
}

int wave_count(void) { return (int)(sizeof WAVE_RUNS / sizeof WAVE_RUNS[0]); }
const WaveRun *wave_run(int i) { return i >= 0 && i < wave_count() ? &WAVE_RUNS[i] : NULL; }

/* ------------------------------------------------------------------ the run */
static int run = -1, state = WAVE_OFF, node, settle, fresh, victory, in_battle;
static ArenaMember party[5];
static int nparty;
static uint8_t locked[ARENA_MACROS];
static uint32_t frames;
static uint8_t fell;
static unsigned serial;
static int dots_x[WAVE_NODES + 1], dots_y[WAVE_NODES + 1], ndots;

static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }
static void w16(unsigned a, unsigned v) { m68k_ram[a & 0xFFFF] = (uint8_t)(v >> 8); m68k_ram[(a + 1) & 0xFFFF] = (uint8_t)v; }

int wave_state(void) { return state; }
int wave_current(void) { return run; }
unsigned wave_serial(void) { return serial; }
int wave_node(void) { return node; }
uint32_t wave_time(void) { return frames; }
uint8_t wave_fell(void) { return fell; }
int wave_party(uint8_t *ids) {
    for (int k = 0; k < nparty; k++) ids[k] = party[k].id;
    return nparty;
}
int wave_player_input(void) { return state == WAVE_FIGHTING && in_battle && !victory; }
int wave_staging(void) { return state == WAVE_LOADING || state == WAVE_RELOAD; }

void wave_setup(int r, const uint8_t *picks, int n, const uint8_t *macros) {
    const WaveRun *w = wave_run(r);
    run = w ? r : -1;
    nparty = 0;
    for (int k = 0; w && k < n && k < 5; k++)
        if (picks[k] < w->nroster) party[nparty++] = w->roster[picks[k]];
    if (macros) memcpy(locked, macros, ARENA_MACROS);
    else memset(locked, 0, ARENA_MACROS);
    node = 0;
    frames = 0;
    fell = 0;
    ndots = 0;
    state = WAVE_OFF;
    serial++;
    music_wave_stop();                                   /* (a run before this one: its file) */
}

void wave_make_cart(uint8_t *cart) {
    const WaveRun *w = wave_run(run);
    if (!w) { memset(cart, 0, ARENA_CART_SIZE); return; }
    const WaveNode *nd = &w->node[0];
    ArenaBoss b;
    memset(&b, 0, sizeof b);
    b.name = w->name;
    b.world = nd->world;
    b.map2 = nd->from;
    b.map = nd->map;
    b.x = nd->x;
    b.y = nd->y;
    b.n = (uint8_t)nparty;
    memcpy(b.party, party, sizeof party);
    memcpy(b.items, w->items, 40);
    arena_make_cart_for(&b, locked, cart);
    fresh = 1;                                           /* the party to be finished on arrival */
}

void wave_make_reload_cart(uint8_t *cart) {
    const WaveRun *w = wave_run(run);
    if (!w || node >= WAVE_NODES) { memset(cart, 0, ARENA_CART_SIZE); return; }
    const WaveNode *nd = &w->node[node];
    arena_make_cart_here(nd->world, nd->from, nd->map, nd->x, nd->y, cart);
    fresh = 0;
}

void wave_begin(void) {
    if (!wave_run(run)) return;
    state = WAVE_LOADING;
    settle = 0;
    victory = 0;
    in_battle = 0;
    ndots = 0;                                           /* (a new map: keep away from its door) */
}

void wave_stop(void) {
    state = WAVE_OFF;
    music_wave_stop();
    forced = -1;
    walkto_force(0);
    walkto_cancel();
}

/* the party standing on the field, nothing on: no window, event or step under way */
static int field_ready(void) {
    int cx, cy;
    return r16(GAME_MODE) == 0x0C && r16(GAME_ROUTINE) == 0 && m68k_ram[EVENT_BATTLE] == 0xFF
        && r16(WIN_END) <= 0x6040 && walkto_leader_cell(&cx, &cy);
}

/* the node's dot: a few steps on from here, away from the door and the dots before; walk there */
static void walk_on(void) {
    const WaveNode *nd = &wave_run(run)->node[node];
    int cx, cy, tx, ty;
    walkto_leader_cell(&cx, &cy);
    if (ndots < WAVE_NODES + 1) { dots_x[ndots] = cx; dots_y[ndots] = cy; ndots++; }
    walkto_force(1);
    state = WAVE_WALKING;
    if (walkto_pick(nd->steps, dots_x, dots_y, ndots, &tx, &ty) && walkto_go(tx, ty, -1)) return;
    walkto_force(0);                                     /* (nowhere to go: the battle here) */
}

static void start_battle(void) {
    const WaveNode *nd = &wave_run(run)->node[node];
    walkto_force(0);
    wave_force_formation(nd->formation);
    m68k_ram[EVENT_BATTLE] = 0xFF;
    m68k_ram[BATTLE_TYPE] = 2;
    w16(GAME_ROUTINE, 0x14);                             /* FieldRoutine_Battle, as RunRandomBattles sets it */
    state = WAVE_FIGHTING;
    victory = 0;
    in_battle = 0;
}

/* after a won battle: 40% of HP and TP back to those standing (to their max), the fallen up
 * with 25% */
static void recover(void) {
    for (int k = 0; k < nparty; k++) {
        unsigned c = CHAR_STATS + party[k].id * 0x80u;
        unsigned mh = r16(c + 0x10), mt = r16(c + 0x14), h = r16(c + 0x0E), t = r16(c + 0x12);
        if ((m68k_ram[c + 0x16] & DEAD_BITS) || h == 0) {
            m68k_ram[c + 0x16] &= (uint8_t)~DEAD_BITS;
            h = mh / 4 ? mh / 4 : 1;
            t = mt / 4;
        } else {
            h += mh * 2 / 5;
            t += mt * 2 / 5;
        }
        w16(c + 0x0E, h < mh ? h : mh);
        w16(c + 0x12, t < mt ? t : mt);
    }
}

int wave_frame(void) {
    if (state == WAVE_OFF || state == WAVE_RELOAD || state == WAVE_CLEARED || state == WAVE_LOST) return state;
    const WaveRun *w = wave_run(run);
    unsigned mode = r16(GAME_MODE);
    if (state != WAVE_FIGHTING && mode == 0x0C) m68k_ram[RANDOM_BATTLES] = 0;   /* only the run's battles */
    if (state == WAVE_LOADING) {
        if (!field_ready()) { settle = 0; return state; }
        if (++settle < SETTLE) return state;
        if (fresh) {                                     /* the run's start: gear's bonuses, full HP and TP */
            arena_finish_party(party, nparty);
            fresh = 0;
        }
        walk_on();
        return state;
    }
    if (state == WAVE_WALKING) {
        if (walkto_active()) return state;
        if (!field_ready()) return state;
        start_battle();
        return state;
    }
    /* WAVE_FIGHTING */
    if (mode == 0x14) {
        in_battle = 1;
        memcpy(&m68k_ram[MACRO_DATA], locked, ARENA_MACROS);   /* the run's macros, whatever is set in battle */
        if (!victory) {
            frames++;
            for (int k = 0; k < nparty; k++)
                if (m68k_ram[CHAR_STATS + party[k].id * 0x80u + 0x16] & DEAD_BITS) fell |= (uint8_t)(1 << k);
            if ((r16(BATTLE_ROUTINE) & 0x7FFF) == 0x1E) {   /* the last enemy down: no victory, out */
                victory = 1;
                w16(BATTLE_ROUTINE, LAST_MESSAGE);
                m68k_ram[SOUND_RESTORE] = 0xFF;
            }
        }
        return state;
    }
    if (in_battle && mode < 0x08) {                      /* Game Over: the title */
        state = WAVE_LOST;
        forced = -1;
        music_wave_stop();
        return state;
    }
    if (!victory || !field_ready()) return state;
    forced = -1;
    recover();
    if (++node >= WAVE_NODES) {
        state = WAVE_CLEARED;
        music_wave_stop();
        return state;
    }
    if (r16(FIELD_MAP) == w->node[node].map) walk_on();  /* the next dot on this map */
    else state = WAVE_RELOAD;                            /* another map: the front end loads it */
    return state;
}
