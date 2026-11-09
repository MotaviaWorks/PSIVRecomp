/*
 * Cheats: see cheats.h.
 */
#include "cheats.h"
#include "m68k.h"

#include <string.h>

#define GAME_MODE      0xEF00u           /* Game_Mode_Index: / 4 = 5 in a battle */
#define FIELD_MAP      0xEC28u
#define RANDOM_BATTLES 0xECE7u           /* Random_Battles_Flag */
#define MONEY          0xF438u           /* Current_Money, a longword */
#define CHAR_STATS     0xF500u           /* $80 a character, 11 of them */
#define FIGHTERS       0x4400u           /* $40 a fighter: 5 party, then the enemies */
#define EXP            0x0A
#define CURR_HP        0x0E
#define MAX_HP         0x10
#define CURR_TP        0x12
#define MAX_TP         0x14
#define STATUS         0x16              /* bit 2: dead; bit 6: dead (androids) */

static int on[CHEATS] = {0, 0, 0, 0, 1, 0};
static int battles_saved = -1;           /* Random_Battles_Flag as the map had it */
static unsigned battles_map;
static uint32_t exp_seen[11];
static int exp_valid;

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }
static uint32_t r32(unsigned a) { return (uint32_t)r16(a) << 16 | r16(a + 2); }
static void w8(unsigned a, unsigned v) { m68k_ram[a & 0xFFFF] = (uint8_t)v; }
static void w16(unsigned a, unsigned v) { m68k_ram[a & 0xFFFF] = (uint8_t)(v >> 8); m68k_ram[(a + 1) & 0xFFFF] = (uint8_t)v; }
static void w32(unsigned a, uint32_t v) { w16(a, v >> 16); w16(a + 2, v & 0xFFFF); }

void cheats_set(int c, int v) {
    if (c < 0 || c >= CHEATS) return;
    if (c == CHEAT_EXP) v = v == 2 || v == 4 ? v : 1;
    else v = v != 0;
    if (c == CHEAT_NO_BATTLES && !v && on[c] && battles_saved >= 0) {   /* off again: as the map had it */
        if (r16(FIELD_MAP) == battles_map) w8(RANDOM_BATTLES, (unsigned)battles_saved);
        battles_saved = -1;
    }
    on[c] = v;
}

int cheats_get(int c) { return c >= 0 && c < CHEATS ? on[c] : 0; }

void cheats_frame(void) {
    int battle = r16(GAME_MODE) / 4 == 5;
    for (int id = 0; id < 11; id++) {
        unsigned s = CHAR_STATS + (unsigned)id * 0x80;
        int dead = (r8(s + STATUS) & 0x44) != 0;
        if (on[CHEAT_HP] && !dead && r16(s + CURR_HP) && r16(s + CURR_HP) < r16(s + MAX_HP)) w16(s + CURR_HP, r16(s + MAX_HP));
        if (on[CHEAT_TP] && !dead && r16(s + CURR_TP) < r16(s + MAX_TP)) w16(s + CURR_TP, r16(s + MAX_TP));
        /* experience: what a battle gave, again */
        uint32_t e = r32(s + EXP);
        if (on[CHEAT_EXP] > 1 && battle && exp_valid && e > exp_seen[id] && e - exp_seen[id] < 0x100000) {
            uint32_t more = (e - exp_seen[id]) * (uint32_t)(on[CHEAT_EXP] - 1);
            e = e + more > 9999999u ? 9999999u : e + more;
            w32(s + EXP, e);
        }
        exp_seen[id] = e;
    }
    exp_valid = 1;
    if (on[CHEAT_MONEY] && r32(MONEY) != 999999u) w32(MONEY, 999999u);   /* exactly: the game shows six digits */
    if (on[CHEAT_NO_BATTLES] && r8(RANDOM_BATTLES)) {
        battles_saved = (int)r8(RANDOM_BATTLES);
        battles_map = r16(FIELD_MAP);
        w8(RANDOM_BATTLES, 0);
    }
    if (on[CHEAT_WEAK] && battle)
        for (int k = 5; k < 9; k++) {
            unsigned f = FIGHTERS + (unsigned)k * 0x40;
            if (!r16(f) || (r16(f + 4) & 0xFF) != 0xFF) continue;
            unsigned s = r16(f + 6);
            if (r16(s + CURR_HP) > 1) w16(s + CURR_HP, 1);
        }
}
