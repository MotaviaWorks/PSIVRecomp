/*
 * A player for testing: see testbot.h. The battle menus are the game's own
 * (Battle_Routine_2 says which one is waiting; see BattleRoutines2 in the
 * disassembly), driven the way battleui.c drives them for a mouse: one press
 * at a time, let go before the next, the game's own cursor walked to the
 * entry wanted, then C.
 */
#include "testbot.h"
#include "bus.h"
#include "m68k.h"

#include <stdlib.h>
#include <string.h>

/* RAM ($FFxxxx), from the disassembly's ps4.constants.asm */
#define GAME_MODE   0xEF00u   /* 0 Sega, 4 title, 8 load a field map, $C field, $10 load a battle, $14 battle */
#define FIELD_MAP   0xEC28u
#define WINDOW_END  0xEC98u   /* $6040 while no window is open in the field */
#define BR2         0x4108u   /* Battle_Routine_2 */
#define COMD_INPUT  0x4106u   /* the fighter whose command is being chosen */
#define MAIN_OPTION 0x41A0u   /* COMD / MACR / RUN */
#define COMD_INDEX  0x41A2u   /* per fighter (words): attack, tech, skill, item, defend */
#define TECH_INDEX  0x41AEu
#define SKILL_INDEX 0x41BAu
#define ITEM_INDEX  0x41C4u
#define MACRO_INDEX 0x41D6u
#define TECH_LIST   0x412Eu   /* 16 IDs, 4 a page; bit 7: not enough TP */
#define SKILL_LIST  0x4142u   /* 8 x (ID, uses left); bit 7: unusable */
#define ITEM_LIST   0x4152u   /* 44 IDs, the fighter's equipment first */
#define FIGHTERS    0x4400u   /* $40 each: +4 the stats' address */
#define MACRO_DATA  0xF444u   /* 8 macros of 20 bytes */
#define CHAR_STATS  0xF500u   /* $80 each; +$4C the equipment */
#define INVENTORY   0xF410u   /* 40 item IDs */
#define CAMERA_Y    0xEF90u   /* the field camera (plane A) */
#define CAMERA_X    0xEF94u
#define VEHICLE     0xF43Cu   /* Vehicle_Index: 0 on foot, 1 Land Rover, 2 Ice Digger, 3 Hydrofoil */

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]; }

enum { BOOT, BOARD, WALK, BATTLE, AFTER, SAVE, DONE };
enum { P_ATTACK, P_TECH, P_SKILL, P_ITEM, P_MACRO, P_RUN, P_MIX, NPLANS };
static const char *const plan_name[NPLANS] = { "attack", "tech", "skill", "item", "macro", "run", "mix" };
static const char *const cmd_name[5] = { "attack", "tech", "skill", "item", "defend" };
static const char *const char_name[11] = { "Chaz", "Alys", "Hahn", "Rune", "Gryz", "Rika", "Demi", "Wren", "Raja", "Kyra", "Seth" };

static struct {
    int on, phase, battles, max_battles, plan, round, run_tried, detours;
    uint32_t rng;
    uint64_t t0, since;            /* the phase's start; the battle state's */
    uint8_t a, b;                  /* the walk: first leg, then back */
    int hold, rest, taps;
    uint8_t press;
    unsigned st, mode, map0;
    int cmd[5], pick, targets, main, macro;
    int saw_victory, saw_run, saw_defeat;
    int vehicle, tries, slot;      /* board this vehicle first (1-3); attempts; its place in the item list */
    unsigned cam;                  /* the camera when it last moved, and when */
    uint64_t moved;
    uint8_t *keys;                 /* the pad given at each logical frame */
    size_t nkeys, cap;
} b;

static uint32_t rnd(uint32_t n) {
    b.rng ^= b.rng << 13;
    b.rng ^= b.rng >> 17;
    b.rng ^= b.rng << 5;
    return n ? b.rng % n : 0;
}

/* a press: held two frames, then let go for three (the menus act on new presses) */
static uint8_t tap(uint8_t k) {
    b.press = k;
    b.hold = 1;
    b.rest = 3;
    b.taps++;
    return k;
}

static const char *who(int f) {
    unsigned stats = r16(FIGHTERS + 0x40u * (unsigned)f + 6);
    if (f < 0 || f > 4 || stats < CHAR_STATS || stats >= CHAR_STATS + 11 * 0x80) return "?";
    return char_name[(stats - CHAR_STATS) / 0x80];
}

static int equipped(int f) {
    unsigned stats = r16(FIGHTERS + 0x40u * (unsigned)f + 6), n = 0;
    if (stats < CHAR_STATS || stats >= CHAR_STATS + 11 * 0x80) return 0;
    for (unsigned i = 0; i < 4; i++) n += r8(stats + 0x4C + i) != 0;
    return (int)n;
}

void testbot_start(uint32_t seed, const char *walk, int battles, int vehicle) {
    memset(&b, 0, sizeof b);
    b.vehicle = vehicle >= 1 && vehicle <= 3 ? vehicle : 0;
    b.on = 1;
    b.rng = (seed + 1) * 2654435761u ^ 0x9E3779B9u;
    if (!b.rng) b.rng = 1;
    b.max_battles = battles > 0 ? battles : NPLANS;
    b.a = PAD_LEFT;
    b.b = PAD_RIGHT;
    if (walk) {
        if (!strcmp(walk, "UD")) { b.a = PAD_UP; b.b = PAD_DOWN; }
        if (!strcmp(walk, "RL")) { b.a = PAD_RIGHT; b.b = PAD_LEFT; }
        if (!strcmp(walk, "DU")) { b.a = PAD_DOWN; b.b = PAD_UP; }
    }
    b.mode = 0xFFFF;
    b.st = 0xFFFF;
    printf("bot: seed %u, walk %s, %d battles%s\n", seed, walk ? walk : "LR", b.max_battles,
           b.vehicle == 1 ? ", in the Land Rover" : b.vehicle == 2 ? ", in the Ice Digger"
           : b.vehicle == 3 ? ", in the Hydrofoil" : "");
}

int testbot_on(void) { return b.on; }
int testbot_done(void) { return b.on && b.phase == DONE; }

static void begin_battle(uint64_t L) {
    b.phase = BATTLE;
    b.battles++;
    b.plan = b.battles <= NPLANS ? b.battles - 1 : P_MIX;
    b.round = b.run_tried = b.saw_victory = b.saw_run = b.saw_defeat = 0;
    b.st = 0xFFFF;
    b.pick = -1;
    for (int k = 0; k < 5; k++) b.cmd[k] = -1;
    printf("bot %llu: battle %d (%s)\n", (unsigned long long)L, b.battles, plan_name[b.plan]);
}

static void end_battle(uint64_t L) {
    printf("bot %llu: battle %d over after %d round%s: %s\n", (unsigned long long)L, b.battles, b.round,
           b.round == 1 ? "" : "s",
           b.saw_defeat ? "the party fell" : b.saw_victory ? "won" : b.saw_run ? "ran" : "ended");
    b.phase = AFTER;
    b.t0 = L;
}

/* ------------------------------------------------------------------ the field */
static uint8_t walk(uint64_t L) {
    if (L - b.t0 > 6000) {                          /* 100 seconds and no battle */
        printf("bot %llu: no battle while walking; stopping\n", (unsigned long long)L);
        b.phase = DONE;
        return 0;
    }
    if (b.mode != 0x0C) return 0;
    if (r16(WINDOW_END) > 0x6040)                   /* a message: read on */
        return (L - b.t0) % 20 == 10 ? tap(PAD_C) : 0;
    /* blocked (a hydrofoil at the shore, a wall): the other axis, then back */
    unsigned cam = r16(CAMERA_X) << 16 | r16(CAMERA_Y);
    if (cam != b.cam || L - b.t0 < 2) {
        b.cam = cam;
        b.moved = L;
    } else if (L - b.moved > 48) {
        static const uint8_t turn[4][2] = { { PAD_UP, PAD_DOWN }, { PAD_RIGHT, PAD_LEFT },
                                            { PAD_DOWN, PAD_UP }, { PAD_LEFT, PAD_RIGHT } };
        int k = b.a == PAD_LEFT ? 0 : b.a == PAD_UP ? 1 : b.a == PAD_RIGHT ? 2 : 3;
        b.a = turn[k][0];
        b.b = turn[k][1];
        b.t0 = b.moved = L;
        printf("bot %llu: blocked; walking %s\n", (unsigned long long)L,
               b.a == PAD_UP ? "up" : b.a == PAD_RIGHT ? "right" : b.a == PAD_DOWN ? "down" : "left");
    }
    /* a vehicle covers twice the ground: shorter legs, or it reaches a town */
    uint64_t leg = r16(VEHICLE) ? 12 : 24;
    return ((L - b.t0) / leg) % 2 ? b.b : b.a;
}

/* board the vehicle: walk a leg off the start (a town's doorstep won't do),
 * then the field menu (A): ITEM, down to the vehicle, USE. On "Can't get on
 * it here!" close the menus and try again further on. */
static uint8_t board(uint64_t L) {
    uint64_t t = L - b.t0, walkfor = 32 + 24 * (uint64_t)b.tries;
    if (t < walkfor) return b.mode == 0x0C && r16(WINDOW_END) <= 0x6040 ? b.a : 0;
    uint64_t m = t - walkfor, pick = 110 + 10 * (uint64_t)b.slot;
    if (m > pick + 90 && r16(VEHICLE)) {
        printf("bot %llu: in the vehicle (%u)\n", (unsigned long long)L, r16(VEHICLE));
        b.phase = WALK;
        b.t0 = L;
        return 0;
    }
    if (m == 10) return tap(PAD_A);                 /* the field menu */
    if (m == 60) return tap(PAD_C);                 /* ITEM */
    if (m >= 110 && m < pick && (m - 110) % 10 == 0) return tap(PAD_DOWN);
    if (m == pick + 20) return tap(PAD_C);          /* the vehicle: USE / LOOK / DSCD */
    if (m == pick + 80) return tap(PAD_C);          /* USE */
    if (m == pick + 240) printf("bot %llu: couldn't board here (try %d)\n", (unsigned long long)L, b.tries + 1);
    if (m == pick + 240 || m == pick + 280 || m == pick + 320 || m == pick + 360) return tap(PAD_B);
    if (m >= pick + 420) {
        b.t0 = L;
        if (++b.tries >= 6) {
            printf("bot %llu: never got on; on foot then\n", (unsigned long long)L);
            b.phase = WALK;
        }
    }
    return 0;
}

static uint8_t save(uint64_t L) {
    uint64_t t = L - b.t0;
    if (t == 0) return tap(PAD_START);              /* the SYS menu */
    if (t == 60) return tap(PAD_C);                 /* SAVE: "Save in number- ?" */
    if (t == 120) return tap(PAD_C);                /* slot 1 */
    if (t == 200) return tap(PAD_C);                /* "Previous Data will be erased. OK?": YES */
    if (t == 380 || t == 420 || t == 460) return tap(PAD_B);   /* the message, then the menus */
    if (t >= 540) {
        b.phase = DONE;
        printf("bot %llu: saved; done\n", (unsigned long long)L);
    }
    return 0;
}

/* ------------------------------------------------------------------ battles */
static int choose(void) {
    switch (b.plan) {
    case P_ATTACK: return 0;
    case P_TECH: return 1;
    case P_SKILL: return 2;
    case P_ITEM: return rnd(2) ? 3 : 0;
    default: {
        uint32_t x = rnd(100);
        return x < 30 ? 0 : x < 60 ? 1 : x < 80 ? 2 : x < 90 ? 3 : 4;
    }
    }
}

static int macro_pick(void) {
    int c[8], n = 0;
    for (unsigned s = 0; s < 8; s++) {
        int used = 0;
        for (unsigned i = 0; i < 20; i++) used |= r8(MACRO_DATA + 20 * s + i) != 0;
        if (used) c[n++] = (int)s;
    }
    if (!n) return -1;
    return c[n > 1 ? 1 + (int)rnd((uint32_t)n - 1) : 0];   /* the first is often "everyone attacks" */
}

static uint8_t main_menu(uint64_t L) {
    if (b.taps == 0) {                              /* a new round */
        b.round++;
        for (int k = 0; k < 5; k++) b.cmd[k] = -1;
        b.pick = -1;
        b.main = 0;
        if (b.plan == P_RUN && !b.run_tried) {
            b.main = 2;
            b.run_tried = 1;
        } else if (b.plan == P_MACRO && b.round <= 2 && (b.macro = macro_pick()) >= 0) {
            b.main = 1;
        }
        printf("bot %llu: round %d: %s\n", (unsigned long long)L, b.round,
               b.main == 2 ? "run" : b.main == 1 ? "macro" : "commands");
    }
    if (b.taps > 12) b.main = 0;                    /* that option can't be reached */
    int cur = (int)(r16(MAIN_OPTION) & 3);
    if (cur < b.main) return tap(PAD_DOWN);
    if (cur > b.main) return tap(PAD_UP);
    return tap(PAD_C);
}

/* a vehicle's battle menu: attack, its skills, the third option; the same cursor */
static uint8_t vehicle_menu(uint64_t L) {
    if (b.taps == 0) {
        b.round++;
        if (b.plan == P_RUN && !b.run_tried) {
            b.main = 2;
            b.run_tried = 1;
        } else {
            b.main = b.round % 2 == 0 ? 1 : 0;
        }
        printf("bot %llu: round %d: vehicle %s\n", (unsigned long long)L, b.round,
               b.main == 2 ? "option 3" : b.main == 1 ? "skill" : "attack");
    }
    if (b.taps > 12) b.main = 0;
    int cur = (int)(r16(MAIN_OPTION) & 3);
    if (cur < b.main) return tap(PAD_DOWN);
    if (cur > b.main) return tap(PAD_UP);
    return tap(PAD_C);
}

static uint8_t command(uint64_t L, int f) {
    if (f < 0 || f > 4) return 0;
    if (b.cmd[f] < 0) b.cmd[f] = choose();
    if (b.taps > 12) b.cmd[f] = 0;
    int cur = (int)(r16(COMD_INDEX + 2u * (unsigned)f) % 5);
    if (cur < b.cmd[f]) return tap(PAD_RIGHT);
    if (cur > b.cmd[f]) return tap(PAD_LEFT);
    b.pick = -1;
    b.targets = (int)rnd(4);
    if (b.cmd[f] == 0 || b.cmd[f] == 4)
        printf("bot %llu:   %s: %s\n", (unsigned long long)L, who(f), cmd_name[b.cmd[f]]);
    return tap(PAD_C);
}

static uint8_t list(uint64_t L, int f, unsigned st) {
    int kind = st == 0x08 ? 1 : st == 0x11 ? 2 : 3;        /* tech, skill, item */
    if (f < 0 || f > 4) return 0;
    if (b.pick < 0) {
        int c[44], n = 0;
        if (kind == 1) {
            for (int i = 0; i < 16; i++) {
                unsigned id = r8(TECH_LIST + (unsigned)i);
                if ((id & 0x7F) && !(id & 0x80)) c[n++] = i;
            }
        } else if (kind == 2) {
            for (int i = 0; i < 8; i++) {
                unsigned id = r8(SKILL_LIST + 2u * (unsigned)i);
                if ((id & 0x7F) && !(id & 0x80) && r8(SKILL_LIST + 2u * (unsigned)i + 1)) c[n++] = i;
            }
        } else {
            for (int i = equipped(f); i < 44; i++)
                if (r8(ITEM_LIST + (unsigned)i)) c[n++] = i;
        }
        if (!n) {
            printf("bot %llu:   %s: no %s to use; attacks instead\n", (unsigned long long)L, who(f), cmd_name[kind]);
            b.cmd[f] = 0;
            return tap(PAD_B);
        }
        b.pick = c[rnd((uint32_t)n)];
    }
    if (b.taps > 40) {                              /* the cursor won't get there: back out */
        printf("bot %llu:   %s: can't reach %s entry %d; attacks instead\n", (unsigned long long)L, who(f),
               cmd_name[kind], b.pick);
        b.cmd[f] = 0;
        b.pick = -1;
        return tap(PAD_B);
    }
    unsigned base = kind == 1 ? TECH_INDEX : kind == 2 ? SKILL_INDEX : ITEM_INDEX;
    int cur = (int)r16(base + 2u * (unsigned)f), t = b.pick;
    if (cur >> 2 != t >> 2) return tap(cur >> 2 < t >> 2 ? PAD_RIGHT : PAD_LEFT);
    if ((cur & 3) != (t & 3)) return tap((cur & 3) < (t & 3) ? PAD_DOWN : PAD_UP);
    unsigned id = kind == 1 ? r8(TECH_LIST + (unsigned)t) : kind == 2 ? r8(SKILL_LIST + 2u * (unsigned)t)
                            : r8(ITEM_LIST + (unsigned)t);
    printf("bot %llu:   %s: %s %02X (entry %d)\n", (unsigned long long)L, who(f), cmd_name[kind], id, t);
    b.targets = (int)rnd(4);
    return tap(PAD_C);
}

static uint8_t macro_menu(uint64_t L) {
    int cur = (int)(r16(MACRO_INDEX) & 7);
    if (b.taps <= 16) {
        if (cur < b.macro) return tap(PAD_DOWN);
        if (cur > b.macro) return tap(PAD_UP);
    }
    printf("bot %llu:   macro %d\n", (unsigned long long)L, cur + 1);
    return tap(PAD_C);
}

static int is_message(unsigned st) {
    switch (st) {
    case 0x1F: case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A:
    case 0x2D: case 0x2E: case 0x2F: return 1;
    default: return 0;
    }
}

static uint8_t battle(uint64_t L) {
    if (b.mode != 0x14) return 0;
    unsigned r2 = r16(BR2), st = r2 & 0x7FFF;
    if (r2 != b.st) {
        b.st = r2;
        b.since = L;
        b.taps = 0;
    }
    uint64_t still = L - b.since;
    if (still < 3) return 0;
    int f = (int)r16(COMD_INPUT);
    switch (st) {
    case 0x02: return main_menu(L);
    case 0x05: return command(L, f);
    case 0x08: case 0x11: case 0x17: return list(L, f, st);
    case 0x0C: case 0x0D:                           /* a target */
        if (b.targets > 0 && b.taps < 8) {
            b.targets--;
            return tap(PAD_RIGHT);
        }
        /* C not taken: the game refuses that one (a Monomate for an android): the next */
        if (b.taps >= 6 && b.taps % 3 == 0) return tap(PAD_RIGHT);
        return tap(PAD_C);
    case 0x30: return macro_menu(L);
    case 0x34: return vehicle_menu(L);
    case 0x37:                                      /* a vehicle's skills: one of the first three */
        if (b.taps == 0) b.targets = (int)rnd(3);
        if (b.targets > 0 && b.taps < 6) {
            b.targets--;
            return tap(PAD_DOWN);
        }
        if (b.taps < 8) printf("bot %llu:   vehicle skill %d\n", (unsigned long long)L, b.taps + 1);
        return tap(PAD_C);
    case 0x1E: case 0x1F: b.saw_victory = 1; break;
    case 0x23: case 0x24: b.saw_run = 1; break;
    case 0x29: case 0x2A: b.saw_defeat = 1; break;
    default: break;
    }
    if (is_message(st) && still >= 12) return tap(PAD_C);
    if (still >= 1200 && still % 120 == 0) {
        printf("bot %llu: battle state %04X unchanged for %llu frames\n", (unsigned long long)L, r2,
               (unsigned long long)still);
        return tap(still % 240 ? PAD_C : PAD_B);
    }
    return 0;
}

/* ------------------------------------------------------------------ each frame */
static void remember(uint64_t L, uint8_t k) {
    if (L >= b.cap) {
        size_t cap = b.cap ? b.cap * 2 : 65536;
        while (cap <= L) cap *= 2;
        uint8_t *p = realloc(b.keys, cap);
        if (!p) return;
        memset(p + b.cap, 0, cap - b.cap);
        b.keys = p;
        b.cap = cap;
    }
    b.keys[L] = k;
    if (L + 1 > b.nkeys) b.nkeys = (size_t)L + 1;
}

uint8_t testbot_keys(uint64_t L) {
    if (!b.on) return 0;
    unsigned mode = r16(GAME_MODE);
    if (mode != b.mode) {
        b.mode = mode;
        if ((mode == 0x10 || mode == 0x14) && b.phase != BATTLE && b.phase != BOOT && b.phase != DONE) {
            begin_battle(L);
        } else if (b.phase == BATTLE && (mode == 0x08 || mode == 0x0C)) {
            end_battle(L);
        } else if (mode <= 0x04 && b.phase != BOOT && b.phase != DONE) {
            printf("bot %llu: back at the title\n", (unsigned long long)L);
            b.phase = DONE;
        } else if (b.phase == WALK && mode == 0x08) {
            /* walked into a town or a cave: turn round */
            printf("bot %llu: walked into another map; turning round\n", (unsigned long long)L);
            uint8_t t = b.a;
            b.a = b.b;
            b.b = t;
            b.t0 = L;
            if (++b.detours > 3) b.phase = DONE;
        }
    }
    uint8_t k = 0;
    if (b.hold > 0) {
        b.hold--;
        k = b.press;
    } else if (b.rest > 0) {
        b.rest--;
    } else {
        switch (b.phase) {
        case BOOT:                                  /* the title, Continue, slot 1 (as battle_probe.py) */
            if ((L >= 290 && L < 294) || (L >= 460 && L < 464)) k = PAD_START;
            else if ((L >= 560 && L < 562) || (L >= 620 && L < 622)) k = PAD_C;
            else if (L >= 760 && mode == 0x0C) {
                b.phase = WALK;
                b.t0 = L;
                b.map0 = r16(FIELD_MAP);
                printf("bot %llu: in the field (map %X)\n", (unsigned long long)L, b.map0);
                if (b.vehicle) {
                    b.slot = -1;
                    for (int i = 0; i < 40 && b.slot < 0; i++)
                        if (r8(INVENTORY + (unsigned)i) == 0x95u + (unsigned)b.vehicle) b.slot = i;
                    if (b.slot >= 0) b.phase = BOARD;
                    else printf("bot %llu: the party hasn't that vehicle\n", (unsigned long long)L);
                }
                k = b.phase == BOARD ? board(L) : walk(L);
            } else if (L > 3000) {
                printf("bot %llu: never reached the field\n", (unsigned long long)L);
                b.phase = DONE;
            }
            break;
        case BOARD: k = board(L); break;
        case WALK: k = walk(L); break;
        case BATTLE: k = battle(L); break;
        case AFTER:
            if (mode == 0x0C && r16(WINDOW_END) > 0x6040) {
                if ((L - b.t0) % 20 == 10) k = tap(PAD_C);
            } else if (mode == 0x0C && L - b.t0 > 60) {
                b.t0 = L;
                if (b.battles >= b.max_battles) {
                    b.phase = SAVE;
                    printf("bot %llu: saving\n", (unsigned long long)L);
                    k = save(L);
                } else {
                    b.phase = WALK;
                    k = walk(L);
                }
            }
            break;
        case SAVE: k = save(L); break;
        default: break;
        }
    }
    remember(L, k);
    return k;
}

void testbot_movie(FILE *f) {
    static const char names[] = "UDLRABCS";
    fprintf(f, "# psiv movie v1\n# made by psiv_headless --bot\n");
    for (size_t i = 0; i < b.nkeys;) {
        uint8_t k = b.keys[i];
        size_t j = i + 1;
        while (j < b.nkeys && b.keys[j] == k) j++;
        if (k) {
            char s[9];
            int n = 0;
            for (int bit = 0; bit < 8; bit++)
                if (k & (1 << bit)) s[n++] = names[bit];
            s[n] = 0;
            fprintf(f, "%zu %s %zu\n", i, s, j - i);
        }
        i = j;
    }
}
