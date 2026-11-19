/*
 * Boss Arena: see arena.h.
 */
#include "arena.h"
#include "arenapic.h"
#include "m68k.h"

#include <string.h>

#include "arena_data.h"

/* ROM: the characters as a new game starts them (66 bytes each), and each one's level
 * table (CharLevelTablePtrs: the level it starts at, then the table; 22 bytes a level:
 * the experience to reach the next, then that level's max HP, max TP, strength, mental,
 * agility, dexterity, a technique learned, a skill learned, the skills' uses) */
#define INITIAL_CHAR_STATS   0x2A8ACAu
#define CHAR_LEVEL_TABLES    0x004074u
#define UPDATE_CHAR_MOD      0x05F754u   /* UpdateCharModStats: a0 the character, a3 its equipment */
#define UPDATE_CHAR_ELEMS    0x05FD2Au   /* UpdateCharElems: a0 the character */

/* RAM ($FFxxxx); see the disassembly's ps4.constants.asm */
#define GAME_MODE      0xEF00u           /* Game_Mode_Index: $C the field, $14 a battle, 0 the title */
#define FIELD_ROUTINE  0xEC20u           /* 0: the party walks free; $0C: an event */
#define EVENT_PTR      0xECA8u           /* the event to run (0: none) */
#define EXIT_FLAGS     0xEC27u           /* Routine_Exit_Flags: bit 3 = to RunEventBattle */
#define EVENT_BATTLE   0xECFCu           /* Event_Battle_Index ($FF: none) */
#define BATTLE_ROUTINE 0x4108u           /* Battle_Routine_2: & $7FFF = 2, the command menu waits */
#define CHAR_STATS     0xF500u           /* $80 a character */

/* in the save block (RAM $F100-$FAFF) */
#define B_TOWN_FLAGS   0x060
#define B_WORLD        0x300
#define B_MAP2         0x302
#define B_MAP          0x304
#define B_X            0x306
#define B_Y            0x308
#define B_PARTY        0x30A
#define B_ITEMS        0x310
#define B_MONEY        0x338
#define B_BATTLE_SPEED 0x342
#define B_CHARS        0x400
#define BLOCK_SIZE     0xA00

#define SETTLE         90                /* frames on the field, the party free, before the fight */
#define SCAN_SETTLE    10                /* (scanning: unseen, so no pause) */
#define SCAN_IDLE      30                /* frames the command menu has waited before the boss is recorded */

static int cur = -1, state = ARENA_OFF, settle, fight_frames, scan, idle;
static uint8_t macros[ARENA_MACROS], fought_macros[ARENA_MACROS];
static int have_macros;
#define MACRO_DATA 0xF444u                       /* RAM; in the save block, $344 */

void arena_set_macros(const uint8_t *m) {
    have_macros = m != NULL;
    if (m) memcpy(macros, m, ARENA_MACROS);
}
const uint8_t *arena_fight_macros(void) { return fought_macros; }

void arena_set_scan(int on) { scan = on; }

int arena_count(void) { return (int)(sizeof ARENA_BOSSES / sizeof ARENA_BOSSES[0]); }
const ArenaBoss *arena_boss(int i) { return i >= 0 && i < arena_count() ? &ARENA_BOSSES[i] : NULL; }
int arena_state(void) { return state; }
int arena_current(void) { return cur; }
int arena_fight_frames(void) { return fight_frames; }
static int poked;                                        /* the fight asked for (fight()) */
int arena_staging(void) { return state == ARENA_LOADING && !poked; }

/* EventBattleMusicData, by Event_Battle_Index */
const char *arena_music(int i) {
    static const char *const NAMES[] = {"Defeat at a Blow", "Meet Them Head-On", "Laughter", "The King of Terrors",
                                        "Her Last Breath", "Abyss"};
    static const uint8_t BY_EVENT[0x1B] = {0, 0, 1, 0, 2, 0, 2, 1, 0, 3, 0, 1, 0, 1, 0, 1, 2, 3, 3, 0, 1, 0, 0, 0, 4, 0, 5};
    const ArenaBoss *b = arena_boss(i);
    return b && b->event < 0x1B ? NAMES[BY_EVENT[b->event]] : "";
}

static unsigned rom16(uint32_t a) { return (unsigned)(m68k_rom[a] << 8 | m68k_rom[a + 1]); }
static uint32_t rom32(uint32_t a) { return (uint32_t)rom16(a) << 16 | rom16(a + 2); }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }

/* the game's letters: A-Z from 1, a-z from $39; $FE ends the name */
static void put_name(uint8_t *p, const char *s) {
    int k = 0;
    for (; s[k] && k < 5; k++)
        p[k] = (uint8_t)(s[k] >= 'a' ? s[k] - 'a' + 0x39 : s[k] - 'A' + 1);
    p[k] = 0xFE;
}

static const char *const NAMES[11] = {"Chaz", "Alys", "Hahn", "Rune", "Gryz", "Rika", "Demi", "Wren", "Raja", "Kyra", "Seth"};

/* a character at a level, as the game itself would have made them: the new game's
 * character, then each level's entry in turn (BattleResults_PartyExp and loc_4156) */
static void make_char(uint8_t *c, const ArenaMember *m) {
    unsigned id = m->id;
    uint32_t in = INITIAL_CHAR_STATS + id * 66;
    const uint8_t *r = &m68k_rom[in];
    memset(c, 0, 0x80);
    put_name(c, NAMES[id]);
    put16(c + 0x06, rom16(in));                          /* profession */
    uint32_t exp = rom32(in + 4);
    unsigned hp = rom16(in + 8), tp = rom16(in + 10);
    uint8_t stats[4], uses[8], techs[16] = {0}, skills[8] = {0};
    memcpy(stats, r + 12, 4);
    memcpy(uses, r + 58, 8);
    int nt = 0, ns = 0;
    for (int k = 0; k < 16; k++) if (r[34 + k]) techs[nt++] = r[34 + k];
    for (int k = 0; k < 8; k++) if (r[50 + k]) skills[ns++] = r[50 + k];
    unsigned start = rom16(CHAR_LEVEL_TABLES + id * 6);
    uint32_t tab = rom32(CHAR_LEVEL_TABLES + id * 6 + 2);
    for (unsigned lv = start; lv < m->level && lv < 99; lv++) {
        uint32_t e = tab + (lv - start) * 22;
        exp = rom32(e);
        hp = rom16(e + 4);
        tp = rom16(e + 6);
        memcpy(stats, &m68k_rom[e + 8], 4);
        if (m68k_rom[e + 12] && nt < 16) techs[nt++] = m68k_rom[e + 12];
        if (m68k_rom[e + 13] && ns < 8) skills[ns++] = m68k_rom[e + 13];
        memcpy(uses, &m68k_rom[e + 14], 8);
    }
    for (int k = 0; k < 4 && m->extra[k] && ns < 8; k++) skills[ns++] = m->extra[k];
    put16(c + 0x08, m->level > start ? m->level : start);
    put32(c + 0x0A, exp);
    put16(c + 0x0E, hp); put16(c + 0x10, hp);
    put16(c + 0x12, tp); put16(c + 0x14, tp);
    for (int k = 0; k < 4; k++) c[0x18 + 3 * k] = c[0x19 + 3 * k] = c[0x1A + 3 * k] = stats[k];
    for (int k = 0; k < 14; k++) c[0x31 + 2 * k] = r[16 + k];   /* the character's own elements (low bytes) */
    for (int k = 0; k < 4; k++) c[0x4C + k] = m->equip[k] == 0xFF ? r[30 + k] : m->equip[k];
    memcpy(c + 0x52, techs, 16);
    memcpy(c + 0x62, skills, 8);
    for (int k = 0; k < 8; k++) c[0x6A + 2 * k] = c[0x6B + 2 * k] = uses[k];
    c[0x7A] = 0xFF;                                      /* gain_exp_flag: has fought */
}

/* the character as a save would hold them (Prepare: their skills' uses) */
void arena_member_char(const ArenaMember *m, uint8_t *c) {
    if (m && m68k_rom) make_char(c, m);
    else memset(c, 0, 0x80);
}

void arena_default_macros(int i, uint8_t *m) {
    const ArenaBoss *b = arena_boss(i);
    memset(m, 0, ARENA_MACROS);
    for (int k = 0; b && k < b->n; k++) {        /* A: everyone attacks, in the party's order */
        m[4 * k] = b->party[k].id;
        m[4 * k + 1] = 1;
    }
}

void arena_member_arts(int i, int k, uint8_t techs[16], int *nt, uint8_t skills[8], int *ns) {
    const ArenaBoss *b = arena_boss(i);
    *nt = *ns = 0;
    if (!b || k < 0 || k >= b->n) return;
    arena_member_arts_of(&b->party[k], techs, nt, skills, ns);
}
void arena_member_arts_of(const ArenaMember *m, uint8_t techs[16], int *nt, uint8_t skills[8], int *ns) {
    *nt = *ns = 0;
    if (!m || !m68k_rom) return;
    uint8_t c[0x80];
    make_char(c, m);
    for (int j = 0; j < 16; j++) if (c[0x52 + j]) techs[(*nt)++] = c[0x52 + j];
    for (int j = 0; j < 8; j++) if (c[0x62 + j]) skills[(*ns)++] = c[0x62 + j];
}

void arena_make_cart(int i, uint8_t *cart) {
    const ArenaBoss *b = arena_boss(i);
    uint8_t m[ARENA_MACROS];
    if (have_macros) memcpy(m, macros, ARENA_MACROS);
    else arena_default_macros(i, m);
    arena_make_cart_for(b, m, cart);
    memcpy(fought_macros, m, ARENA_MACROS);
}

void arena_make_cart_for(const ArenaBoss *b, const uint8_t *mac, uint8_t *cart) {
    memset(cart, 0, ARENA_CART_SIZE);
    if (!b) return;
    uint8_t *h = cart, *k = cart + 0x100;
    memcpy(h + 2, "PHANTASY STAR 4", 15);                /* PS4String; Save_Slot 0 */
    k[0x0C >> 3] |= 0x80 >> (0x0C & 7);                  /* EventFlag_PrincipalConfession: past the Academy's
                                                          * basement, or the world map sends the party back
                                                          * into Piata (RunEvent_ReenterPiata) */
    k[B_TOWN_FLAGS] = 0x80;                              /* Town_Flags as a new game sets them */
    k[B_TOWN_FLAGS + 2] = 0x80;
    k[B_TOWN_FLAGS + 3] = 0x40;
    k[B_WORLD] = b->world;                               /* World_Index (a byte) */
    put16(k + B_MAP2, b->map2);
    put16(k + B_MAP, b->map);
    put16(k + B_X, b->x);
    put16(k + B_Y, b->y);
    memset(k + B_PARTY, 0xFF, 6);
    for (int m = 0; m < b->n; m++) {
        k[B_PARTY + m] = b->party[m].id;
        make_char(k + B_CHARS + b->party[m].id * 0x80, &b->party[m]);
    }
    memcpy(k + B_ITEMS, b->items, 40);
    if (mac) memcpy(k + (MACRO_DATA - 0xF100u), mac, ARENA_MACROS);
    put32(k + B_MONEY, b->meseta);
    put16(k + B_BATTLE_SPEED, 1);
    arena_cart_seal(cart);
}

/* the cartridge's sum (SumSRAMBytes: the block's bytes, 16 bits), after any change to the block */
void arena_cart_seal(uint8_t *cart) {
    unsigned sum = 0;
    for (int n = 0; n < BLOCK_SIZE; n++) sum += cart[0x100 + n];
    put16(cart + 0x14, sum & 0xFFFF);
}

/* the game as it is now, as a save for the place given (Wave Mode, between maps): the save
 * block from RAM ($F100-$FAFF: everyone's state, the items, the flags), the place changed */
void arena_make_cart_here(uint8_t world, uint16_t map2, uint16_t map, uint16_t x, uint16_t y, uint8_t *cart) {
    memset(cart, 0, ARENA_CART_SIZE);
    memcpy(cart + 2, "PHANTASY STAR 4", 15);
    uint8_t *k = cart + 0x100;
    memcpy(k, &m68k_ram[0xF100], BLOCK_SIZE);
    k[B_WORLD] = world;
    put16(k + B_MAP2, map2);
    put16(k + B_MAP, map);
    put16(k + B_X, x);
    put16(k + B_Y, y);
    arena_cart_seal(cart);
}

void arena_begin(int i) {
    cur = arena_boss(i) ? i : -1;
    state = cur >= 0 ? ARENA_LOADING : ARENA_OFF;
    settle = 0;
    fight_frames = 0;
    idle = 0;
    poked = 0;
}

static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }

/* one of the game's routines, run now on the game's own stack (below where the game is),
 * with the registers as the caller set them; the game's own are put back by the caller */
static void game_call(uint32_t addr) {
    uint32_t ret = m68k_last_ret;
    A(7) -= 4;                                           /* a return address for its rts */
    wr32(A(7), 0);
    m68k_call(addr);
    m68k_last_ret = ret;
}

/* the party loaded: their gear's bonuses, by the game's own routines (as InitializeCharStats
 * and the Equip window call them), full HP and TP; then the boss, as its script starts it */
void arena_finish_party(const ArenaMember *party, int n) {
    M68KState keep = m68k;
    for (int m = 0; m < n; m++) {
        uint32_t c = 0xFF0000u + CHAR_STATS + party[m].id * 0x80u;
        m68k = keep;
        A(0) = c;
        A(3) = c + 0x4C;
        game_call(UPDATE_CHAR_MOD);
        m68k = keep;
        A(0) = c;
        game_call(UPDATE_CHAR_ELEMS);
        unsigned a = c & 0xFFFF;
        memcpy(&m68k_ram[a + 0x0E], &m68k_ram[a + 0x10], 2);
        memcpy(&m68k_ram[a + 0x12], &m68k_ram[a + 0x14], 2);
    }
    m68k = keep;
}

static void fight(const ArenaBoss *b) {
    arena_finish_party(b->party, b->n);
    m68k_ram[FIELD_ROUTINE] = 0x00;                      /* FieldRoutine_Event, with no event ... */
    m68k_ram[FIELD_ROUTINE + 1] = 0x0C;
    m68k_ram[EVENT_PTR] = m68k_ram[EVENT_PTR + 1] = 0;
    m68k_ram[EXIT_FLAGS] |= 0x08;                        /* ... and the way out to RunEventBattle */
    m68k_ram[EVENT_BATTLE] = b->event;
    poked = 1;
}

int arena_frame(void) {
    if (state == ARENA_OFF || state == ARENA_WON || state == ARENA_LOST || state == ARENA_CAPTURED) return state;
    if (state == ARENA_CAPTURING) {
        if (arenapic_capture_frame()) state = ARENA_CAPTURED;
        return state;
    }
    const ArenaBoss *b = arena_boss(cur);
    unsigned mode = r16(GAME_MODE);
    if (state == ARENA_LOADING) {
        if (mode == 0x0C && r16(FIELD_ROUTINE) == 0 && m68k_ram[EVENT_BATTLE] == 0xFF) {
            if (++settle >= (scan ? SCAN_SETTLE : SETTLE)) fight(b);
        } else if (mode == 0x14 && m68k_ram[EVENT_BATTLE] == b->event) {
            state = ARENA_FIGHTING;
        } else if (mode != 0x0C) {
            settle = 0;
        }
    } else if (state == ARENA_FIGHTING) {
        fight_frames++;
        if (mode == 0x14) memcpy(fought_macros, &m68k_ram[MACRO_DATA], ARENA_MACROS);   /* (as set in battle) */
        if (scan && mode == 0x14) {                      /* staged only: the boss, once the menu waits */
            if ((r16(BATTLE_ROUTINE) & 0x7FFF) == 2) idle++;
            else idle = 0;
            if (idle >= SCAN_IDLE || fight_frames > 3000) {
                arenapic_capture_begin();
                state = ARENA_CAPTURING;
                return state;
            }
        }
        if (mode < 0x08) state = ARENA_LOST;             /* Game Over: the title */
        else if (mode != 0x14 && m68k_ram[EVENT_BATTLE] == 0xFF) state = ARENA_WON;
    }
    return state;
}
