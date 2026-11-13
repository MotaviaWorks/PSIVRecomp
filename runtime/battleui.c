/*
 * Modern battle menus: see battleui.h.
 */
#include "battleui.h"
#include "battledesc.h"
#include "bus.h"
#include "fpview.h"
#include "depth.h"
#include "frame.h"
#include "hdtext.h"
#include "m68k.h"
#include "uikit.h"
#include "vdp.h"
#include "par.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAM ($FFxxxx); see the disassembly's ps4.constants.asm */
#define GAME_MODE_INDEX  0xEF00u
#define MODE_BATTLE      5
#define VEHICLE_INDEX    0xF43Cu   /* 0 = on foot */
#define BATTLE_ROUTINE_2 0x4108u   /* command input and messages; bit 15 = still opening */
#define COMD_INPUT       0x4106u   /* the fighter whose command is being chosen */
#define COMMAND_DATA     0x410Au   /* per fighter, 4 bytes: command, ID, target (word) */
#define TECH_LIST        0x412Eu   /* 16 technique IDs, 4 a page; bit 7: not enough TP */
#define SKILL_LIST       0x4142u   /* 8 x (skill ID, uses left), 4 a page; bit 7: unusable */
#define ITEM_LIST        0x4152u   /* item IDs, equipment first, packed; 4 a page */
#define ITEM_SLOTS       44
#define MAIN_OPTION      0x41A0u   /* COMD / MACR / RUN */
#define COMD_INDEX       0x41A2u   /* per fighter (words): command cursor 0-4 */
#define ENEMY_INDEX      0x41ACu   /* the target among the enemies present */
#define TECH_INDEX       0x41AEu   /* per fighter: 0-15 */
#define CHAR_INDEX       0x41B8u   /* the target among the party */
#define SKILL_INDEX      0x41BAu   /* per fighter: 0-7 */
#define ITEM_INDEX       0x41C4u   /* per fighter: 0-43 */
#define FIGHTERS         0x4400u   /* $40 each, 5 party then enemies: +0 slot (0 = none), +4 stats */
#define CHAR_STATS       0xF500u
#define ENEMY_STATS      0x4200u   /* $80 each: +0 name (window charset) */
#define CLEAN_MAP        0x2540u   /* plane A rows 4-20 without windows, 40 cells a row */
#define CLEAN_ROW0       4
#define CLEAN_ROWS       17
/* ROM */
#define TECH_DATA        0x2A9BE8u /* TechniqueData: 8 bytes each; +1 TP cost, +2 target */
#define SKILL_DATA       0x2A9D28u

/* the window art: frame, cursor boxes, font, command icons */
#define T_CORNER  0x6E9
#define T_HORIZ   0x6EA
#define T_VERT    0x6F3
#define T_TEE     0x6F4
#define T_BOX     0x6E7          /* blue checkbox */
#define T_BOX_ON  0x6E8          /* red: the cursor */

static int enabled, active, vx0;
static uint16_t override[64 * 32];
static uint8_t cell_flags[64 * 32];       /* see vdp_plane_a_flags */
static uint8_t shadow[0x10000];           /* the party's art as they stand, at its VRAM addresses */
static uint16_t back_cells[64 * 32];      /* see vdp_plane_a_back */
static uint8_t hidden[80];

void battleui_set_enabled(int on) { enabled = on; }
int  battleui_enabled(void) { return enabled; }
int  battleui_active(void) { return active; }

static unsigned r8(unsigned a) { return ui_ram8(a); }
static unsigned r16(unsigned a) { return ui_ram16(a); }

/* window art, at either priority (the list checkboxes are low priority) */
static int ui_tile(uint16_t e) {
    int t = e & 0x7FF;
    return (t >= 0x15E && t <= 0x17F) || t >= 0x680;
}

/* ------------------------------------------------------------------ the game's windows */
typedef struct {
    int x, y, w, h;               /* cells, frame included */
    uint32_t ch[26][40];          /* interior, as Unicode; 0 = blank */
    uint8_t dim[26][40];          /* greyed (palette 2) */
    int cursor;                   /* interior row with the red checkbox, or -1 */
} Win;
#define MAX_WINS 8
static Win wins[MAX_WINS];
static int nwins;

/* cursor sprites taken out of the picture */
typedef struct { int x, y, down; } Pointer;
static Pointer pointers[8];
static int npointers;

static uint16_t nt(int r, int c) {
    uint32_t base = (uint32_t)(vdp.reg[2] & 0x38) << 10;
    uint32_t a = (base + ((uint32_t)(r & 31) * 64u + (uint32_t)(c & 63)) * 2u) & 0xFFFF;
    return (uint16_t)(vdp.vram[a] << 8 | vdp.vram[a + 1]);
}

static int is(uint16_t e, int tile, int flips) { return (e & 0x7FF) == tile && (e & 0x1800) == flips; }

static void read_windows(void) {
    nwins = 0;
    for (int r = 0; r < 28 && nwins < MAX_WINS; r++)
        for (int c = 0; c < 40 && nwins < MAX_WINS; c++) {
            if (!is(nt(r, c), T_CORNER, 0)) continue;
            int c2 = c + 1, r2 = r + 1;
            while (c2 < 64 && ((nt(r, c2) & 0x7FF) == T_HORIZ || (nt(r, c2) & 0x7FF) == T_TEE)) c2++;
            if (c2 >= 64 || !is(nt(r, c2), T_CORNER, 0x800)) continue;
            while (r2 < 32 && (nt(r2, c) & 0x7FF) == T_VERT) r2++;
            if (r2 >= 32 || !is(nt(r2, c), T_CORNER, 0x1000)) continue;
            Win *w = &wins[nwins];
            w->x = c; w->y = r; w->w = c2 - c + 1; w->h = r2 - r + 1;
            w->cursor = -1;
            if (w->w < 3 || w->h < 3 || w->h - 2 > 26 || w->w - 2 > 40) continue;
            for (int y = 0; y < w->h - 2; y++)
                for (int x = 0; x < w->w - 2; x++) {
                    uint16_t e = nt(r + 1 + y, c + 1 + x);
                    int t = e & 0x7FF;
                    uint32_t cp = 0;
                    if (t == T_BOX_ON) w->cursor = y;
                    else if (t >= 0x680 && t < 0x700 && t != T_BOX) cp = hdtext_win_char(t - 0x680);
                    else if (t >= 0x7BF && t < 0x7BF + 0x40) cp = hdtext_win_char(t - 0x7BF);
                    w->ch[y][x] = cp == ' ' ? 0 : cp;
                    w->dim[y][x] = (uint8_t)((e >> 13 & 3) == 2);
                }
            nwins++;
        }
}

static void party_reset(void);
static void party_frame(void);

void battleui_frame(void) {
    active = 0;
    vdp_plane_a_override = NULL;
    vdp_sprite_hidden = NULL;
    vdp_plane_a_flags = NULL;
    vdp_tile_shadow = NULL;
    vdp_plane_a_back = NULL;
    if (!enabled || !ui_ready()) { party_reset(); return; }
    if (r16(GAME_MODE_INDEX) / 4 != MODE_BATTLE || r16(VEHICLE_INDEX) != 0) { party_reset(); return; }
    if (!(vdp.reg[1] & 0x40) || (vdp.reg[16] & 3) != 1) return;    /* display on, 64-cell planes */
    active = 1;
    vx0 = vdp_view_x0;

    /* the game's windows as they are now, then out of the picture */
    read_windows();
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 64; c++) {
            uint16_t e = nt(r, c), o = 0xFFFF;
            if (ui_tile(e)) {
                o = 0;
                if (r >= CLEAN_ROW0 && r < CLEAN_ROW0 + CLEAN_ROWS && c < 40) {
                    uint16_t k = (uint16_t)r16(CLEAN_MAP + (unsigned)(r - CLEAN_ROW0) * 0x50u + (unsigned)c * 2u);
                    if (!ui_tile(k)) o = k;
                }
            }
            override[r * 64 + c] = o;
        }
    vdp_plane_a_override = override;
    memset(cell_flags, 0, sizeof cell_flags);
    vdp_plane_a_flags = cell_flags;
    vdp_tile_shadow = shadow;
    memset(back_cells, 0xFF, sizeof back_cells);
    vdp_plane_a_back = back_cells;

    /* cursor sprites: high priority, palette 3, the arrow tiles */
    memset(hidden, 0, sizeof hidden);
    npointers = 0;
    uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
    for (int i = 0; i < 80; i++) {
        const uint8_t *s = &vdp.vram[(sat + (uint32_t)i * 8) & 0xFFFF];
        uint16_t attr = (uint16_t)(s[4] << 8 | s[5]);
        int tile = attr & 0x7FF;
        if ((attr & 0xE000) == 0xE000 && tile >= 0x17C && tile <= 0x183) {
            hidden[i] = 1;
            int y = ((s[0] << 8 | s[1]) & 0x3FF) - 128, x = ((s[6] << 8 | s[7]) & 0x1FF) - 128;
            if (npointers < 8 && x > -16 && x < 336 && y > -16 && y < 240) {
                pointers[npointers].x = x;
                pointers[npointers].y = y;
                pointers[npointers].down = (attr & 0x1000) != 0;
                npointers++;
            }
        }
    }
    vdp_sprite_hidden = hidden;
    party_frame();
}

/* ------------------------------------------------------------------ battle state */
enum { M_NONE, M_MAIN, M_COMMAND, M_TECH, M_SKILL, M_ITEM, M_TARGET_ENEMY, M_TARGET_ALLY };

static int menu_of(int r2) {
    switch (r2) {
    case 0x01: case 0x02: case 0x03: return M_MAIN;
    case 0x04: case 0x05: case 0x06: case 0x0A: case 0x09: case 0x12: case 0x18: return M_COMMAND;
    case 0x07: case 0x08: case 0x0B: case 0x0E: case 0x0F: return M_TECH;
    case 0x10: case 0x11: case 0x13: case 0x14: case 0x15: return M_SKILL;
    case 0x16: case 0x17: case 0x19: case 0x1A: case 0x1B: return M_ITEM;
    case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:   /* the item pages */
    case 0x40: case 0x41: case 0x42: case 0x43: return M_ITEM;                       /* being built */
    case 0x0C: return M_TARGET_ENEMY;
    case 0x0D: return M_TARGET_ALLY;
    default: return M_NONE;
    }
}

/* the character a fighter is (party fighters 0-4), or -1 */
static int fighter_char(int k) {
    if (k < 0 || k > 4 || !r16(FIGHTERS + (unsigned)k * 0x40)) return -1;
    uint32_t stats = ui_ram32(FIGHTERS + (unsigned)k * 0x40 + 4) & 0xFFFF;
    if (stats < CHAR_STATS || stats >= CHAR_STATS + 11 * 0x80 || (stats - CHAR_STATS) % 0x80) return -1;
    return (int)((stats - CHAR_STATS) / 0x80);
}

/* the n-th fighter present from `first`, as an index (or -1) */
static int nth_fighter(int first, int last, int n) {
    for (int k = first; k <= last; k++)
        if (r16(FIGHTERS + (unsigned)k * 0x40) && n-- == 0) return k;
    return -1;
}

/* ------------------------------------------------------------------ the party in the picture */
/* PSIV shows the whole party only at the main menu (and for techniques on all
 * of them): while commands are chosen it shows only the member choosing (its
 * command window over part of them); while the battle plays out, only the
 * one acting (often as a sprite at the enemy instead) or the one an enemy
 * attacks, stood in front of that enemy. The port keeps everyone standing in
 * their place: each member's name-table cells and the tiles they use are
 * kept while they stand at the main menu (and while they choose), and drawn
 * back into plane A wherever the game leaves a member out -- from the port's
 * copy of the tiles (the game reuses an attacker's VRAM for its attack
 * sprites) and behind low-priority sprites (so the game's attacks and
 * effects still pass in front). Members the game draws itself are left to it,
 * where it stands them (one acting or attacked, under the enemy), with
 * whoever's place that is showing behind them; the one acting stays out of
 * their place while action sprites are on screen; the dead stay out. */
#define PARTY_ROW0 4
#define PARTY_ROWS 17                       /* rows 4-20: the battle area above the status window */
#define CURRENT_ACTOR 0x4142u               /* during the actions: fighter + 1 (shares RAM with the skill list) */
typedef struct {
    int valid, base, col0, x;               /* art (first tile); their place: first column, centre (game x) */
    uint16_t cell[PARTY_ROWS][6];           /* name-table entries; 0 = not theirs */
} Member;
static Member members[5];
static int home_x[5];                       /* each one's place (fighter +$C) as they stand at the menus */
/* ... and the game's own table of those places, by fighter (ROM $9A62: centre,
 * left, right, far left, far right), whatever the party's size */
static const int LINE_X[5] = {0x120, 0xF0, 0x150, 0xC0, 0x180};
static int standing;                        /* members on screen at the last main menu */
static int restore;                         /* the members drawn back this frame */
static int shown;                           /* the members in the picture this frame (theirs or the game's) */

/* their aura (see below): how strong, gold (0) or cyan (1); motes rising */
static float aura_level[5], aura_mix[5];
typedef struct { float x, y, vy, life, max_life, size, phase; } Mote;
#define MAX_MOTES 40
static Mote motes[MAX_MOTES];
static int nmotes;
static float mote_due;

static int party_tile(uint16_t e) { int t = e & 0x7FF; return (e & 0x8000) && t >= 0x180 && t < 0x280; }

/* The camera pan (battleui_pan, drawn by depth.c): while the battle plays
 * out, the one the game stands away from their place (attacked, or a caster
 * or archer, under an enemy) and how far; whose each party tile is; and
 * everyone else's picture as kept, at their places, to slide along. */
static int pan_on = 1, pan_member = -1, pan_dx;
static uint8_t tile_owner[0x800];                 /* member + 1, by tile */
static uint16_t pan_cells[32 * 64];               /* the others' kept cells, at their places */
static uint8_t pan_owner[32 * 64];                /* member + 1 */
void battleui_set_pan(int on) { pan_on = on; }
int  battleui_pan_enabled(void) { return pan_on; }

/* rewind: the party as kept (the members' cells and their tiles' copy go
 * with the game's state) */
#define PARTY_FIELDS(F) F(members) F(home_x) F(standing) F(restore) F(shown) F(aura_level) F(aura_mix) F(shadow)
size_t battleui_state_save(uint8_t *out) {
    size_t n = 0;
#define SAVE(x) if (out) memcpy(out + n, &(x), sizeof(x)); n += sizeof(x);
    PARTY_FIELDS(SAVE)
#undef SAVE
    return n;
}

size_t battleui_state_load(const uint8_t *in) {
    size_t n = 0;
#define LOAD(x) memcpy(&(x), in + n, sizeof(x)); n += sizeof(x);
    PARTY_FIELDS(LOAD)
#undef LOAD
    nmotes = 0;
    return n;
}

/* command input: anything but the main menu and the battle playing out */
static int choosing(int menu) { return menu != M_NONE && menu != M_MAIN; }

static void party_reset(void) {
    memset(members, 0, sizeof members);
    memset(home_x, 0, sizeof home_x);
    standing = restore = shown = 0;
    memset(aura_level, 0, sizeof aura_level);
    nmotes = 0;
    mote_due = 0;
}

/* the game's low-priority sprites on screen: a character mid-action, its
 * weapon, a technique's effect (the menu cursors are high priority) */
static int low_sprites(void) {
    uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
    int link = 0, count = 0, n = 0;
    do {
        const uint8_t *s = &vdp.vram[(sat + (uint32_t)link * 8) & 0xFFFF];
        const uint8_t *c = &vdp.sat_cache[((uint32_t)link * 8) & 0x3FF];
        int sy = ((c[0] << 8 | c[1]) & 0x3FF) - 128, sx = ((s[6] << 8 | s[7]) & 0x1FF) - 128;
        if (!(s[4] & 0x80) && sx > -32 && sx < 352 && sy > -32 && sy < 224) n++;
        link = c[3] & 0x7F;
    } while (link && ++count < 80);
    return n;
}

/* Where the game draws a member when not in the columns of their x: an
 * archer (Kyra's throw) or a caster is drawn in the middle of the line with
 * the x left as it was. Their tiles, wherever they are, matched against the
 * picture kept at the menus (an action's frames go into the member's own
 * VRAM, under the same name-table entries); -1 if not found. */
static int find_member(const Member *m, int lo, int hi) {
    if (!m->valid) return -1;
    for (int r = 0; r < PARTY_ROWS; r++)
        for (int col = 0; col < 64; col++) {
            uint16_t e = nt(PARTY_ROW0 + r, col);
            int t = e & 0x7FF;
            if (!party_tile(e) || t < lo || t >= hi) continue;
            for (int c = 0; c < 6; c++)
                if (m->cell[r][c] && (m->cell[r][c] & 0x7FF) == t) return col - c;
        }
    return -1;
}

static void party_frame(void) {
    int r2 = (int)(r16(BATTLE_ROUTINE_2) & 0x7FFF), menu = menu_of(r2);
    int results = (r2 >= 0x1E && r2 <= 0x26) || (r2 >= 0x2D && r2 <= 0x2F);
    /* their cells and tiles are kept while they stand at the main menu or choose */
    int keep = menu == M_MAIN || choosing(menu);
    int bases[5], alive = 0;
    for (int k = 0; k < 5; k++) {
        int id = fighter_char(k);
        bases[k] = id >= 0 ? (int)(r16(FIGHTERS + (unsigned)k * 0x40 + 0x0E) >> 5) : -1;
        if (id >= 0 && r16(CHAR_STATS + (unsigned)id * 0x80 + 0x0E) && !(r8(CHAR_STATS + (unsigned)id * 0x80 + 0x16) & 0x44))
            alive |= 1 << k;                         /* HP left, not dead (bit 2; bit 6 for androids) */
    }
    int present = 0, moved = 0, now_col[5] = {0};
    memset(tile_owner, 0, sizeof tile_owner);
    for (int k = 0; k < 5; k++) {
        Member *m = &members[k];
        if (bases[k] < 0) { m->valid = 0; continue; }
        /* where the game has them now; their home is m->col0 */
        int x = (int)r16(FIGHTERS + (unsigned)k * 0x40 + 0x0C) - 128, col0 = (x - 24) >> 3;
        int lo = bases[k], hi = lo + 0x30;          /* their tiles: up to the next member's */
        for (int j = 0; j < 5; j++) if (bases[j] > lo && bases[j] < hi) hi = bases[j];
        for (int t = lo; t < hi && t < 0x800; t++) tile_owner[t] = (uint8_t)(k + 1);
        if (m->valid && (m->base != lo || (keep && m->col0 != col0))) m->valid = 0;
        uint16_t now[PARTY_ROWS][6];
        int any = 0;
        for (int pass = 0; pass < 2 && !any; pass++) {
            if (pass) {                               /* not there: drawn elsewhere? */
                if (keep || (col0 = find_member(m, lo, hi)) < 0) break;
            }
            for (int r = 0; r < PARTY_ROWS; r++)
                for (int c = 0; c < 6; c++) {
                    uint16_t e = nt(PARTY_ROW0 + r, col0 + c);
                    int t = e & 0x7FF;
                    now[r][c] = e;
                    if (party_tile(e) && t >= lo && t < hi) any = 1;
                }
        }
        if (!any) continue;
        present |= 1 << k;
        now_col[k] = col0;
        if (m->valid && col0 != m->col0) moved |= 1 << k;   /* stood in front of the action */
        if (!keep) continue;                          /* mid-action they may be in another pose */
        for (int r = 0; r < PARTY_ROWS; r++)          /* cells under a window keep what they had */
            for (int c = 0; c < 6; c++) {
                uint16_t e = now[r][c];
                int t = e & 0x7FF;
                if (party_tile(e) && t >= lo && t < hi) {
                    m->cell[r][c] = e;
                    memcpy(&shadow[t * 32], &vdp.vram[t * 32], 32);
                } else if (!m->valid || !ui_tile(e)) m->cell[r][c] = 0;
            }
        m->valid = 1; m->base = lo; m->col0 = col0; m->x = x;
        home_x[k] = x + 128;
    }
    if (menu == M_MAIN) standing = present;
    else if (!keep) standing |= present & alive;     /* e.g. revived, and drawn by the game */
    /* while the party acts, the one acting is out of their place while their
     * sprites (or their technique's) are on screen */
    int out = 0;
    if (!keep && !results) {
        unsigned a = r16(CURRENT_ACTOR);
        if (a >= 1 && a <= 5 && low_sprites()) out = 1 << (a - 1);
    }
    restore = standing & alive & ~out;
    if (!keep) restore &= ~present;                   /* the ones the game draws are its own */
    shown = restore | present;
    /* the pan: the first one the game stood away from their place */
    pan_member = -1;
    pan_dx = 0;
    if (pan_on && !keep && !results)
        for (int k = 0; k < 5; k++)
            if ((moved >> k & 1) && members[k].valid) { pan_member = k; pan_dx = (now_col[k] - members[k].col0) * 8; break; }
    memset(pan_owner, 0, sizeof pan_owner);
    for (int k = 0; k < 5; k++) {
        const Member *m = &members[k];
        if (k == pan_member || !(shown >> k & 1) || !m->valid) continue;
        for (int r = 0; r < PARTY_ROWS; r++)
            for (int c = 0; c < 6; c++) {
                int row = PARTY_ROW0 + r, col = m->col0 + c;
                if (!m->cell[r][c] || col < 0 || col >= 64) continue;
                pan_cells[row * 64 + col] = m->cell[r][c];
                pan_owner[row * 64 + col] = (uint8_t)(k + 1);
            }
    }
    {   /* PSIV_BUI_LOG=1: the party on screen as the battle state changes */
        static int log_on = -1, last = -1;
        if (log_on < 0) { const char *v = getenv("PSIV_BUI_LOG"); log_on = v && *v == '1'; }
        int key = r2 << 24 | ((int)r16(COMD_INPUT) & 15) << 20 | moved << 15 | present << 10 | restore << 5
                | ((int)r16(CURRENT_ACTOR) & 31);
        if (log_on && key != last)
            printf("battleui: routine %02X fighter %u actor %04X present %02X moved %02X standing %02X restore %02X\n",
                   r2, r16(COMD_INPUT), r16(CURRENT_ACTOR), present, moved, standing, restore);
        last = key;
    }
    /* The game stands one acting (a caster, an archer) or one an enemy
     * attacks under that enemy, often in another member's place, and plays
     * the action there: what they cast reaches from their hands to the
     * enemy, a lunge or a bolt lands on them. That stays as the game draws it
     * (moving any of it apart from the rest is what broke sprites up); the
     * rest keep their places, and one whose place the game drew someone in
     * shows behind them, pixel by pixel (back_cells). */
    for (int k = 0; k < 5; k++) {
        const Member *m = &members[k];
        if (!(restore >> k & 1) || !m->valid) continue;
        for (int r = 0; r < PARTY_ROWS; r++)
            for (int c = 0; c < 6; c++) {
                uint16_t e = m->cell[r][c];
                int row = PARTY_ROW0 + r, col = m->col0 + c;
                if (!e || col < 0 || col >= 64) continue;
                uint16_t cur = nt(row, col);
                if ((cur & 0x7FF) && !ui_tile(cur)) {
                    back_cells[row * 64 + col] = e;           /* behind what the game drew there */
                    continue;
                }
                override[row * 64 + col] = e;
                cell_flags[row * 64 + col] = 3;               /* behind low sprites, from the copy */
            }
    }
}

static const char *const COMMANDS[5] = {"Attack", "Technique", "Skill", "Item", "Defend"};
static const char *const COMMAND_HELP[5] = {
    "Strike an enemy with your weapon.",
    "Use a technique. Costs TP.",
    "Use a skill. Its uses come back with rest.",
    "Use something from the pack, or what you wear.",
    "Brace, and take less damage this turn.",
};
static const uint32_t COMMAND_COLOR[6] = {0xB8C4E8, 0xFFA77A, 0x8FD3FF, 0xFFD27A, 0x9BE89B, 0xC8C8FF};

/* ------------------------------------------------------------------ drawing helpers */
/* The look is the camp menu's (fieldmenu.c): its framed windows, its
 * colours, one size of text, a pale bar under the cursor's row. */
#define WHITE   0xFFFFFFu
#define SOFT    0xAEBEF0u         /* labels */
#define FAINT   0x7C8CCCu         /* what cannot be chosen */
#define NAVY    0x16287Au         /* text on the cursor's bar */
#define BAR     0xE8ECF8u         /* the cursor's row */
#define BAR_OFF 0x4868C8u         /* a choice made, the cursor elsewhere */
#define ILL     0xFF9E9Eu         /* down, poisoned, ... */
#define CAP     5.6f              /* capitals' height, game pixels */

static UiCanvas cv;
static float S;                   /* output pixels per game pixel */
static float OX;                  /* game x 0 in the view, game pixels */
static float fade = 1.0f;         /* the game's own fades */

static float SX(float gx) { return (gx + OX) * S; }
static float SY(float gy) { return gy * S; }
static int AL(float a) { a *= fade; return a <= 0 ? 0 : a >= 255 ? 255 : (int)a; }

static void win(float x0, float y0, float x1, float y1, float alpha) {
    ui_frame_window_a(&cv, SX(x0), SY(y0), SX(x1), SY(y1), S, AL(255 * alpha));
}
/* the bar behind a row whose text sits on `base` */
static void bar(float x0, float x1, float base, uint32_t c, float alpha) {
    ui_round_rect(&cv, SX(x0), SY(base - 8.6f), SX(x1), SY(base + 3.2f), 1.5f * S, c, AL(255 * alpha));
}
static float text(float gx, float base, const char *s, uint32_t c, float alpha) {
    return ui_text(&cv, SX(gx), SY(base), CAP * S, s, c, AL(255 * alpha)) / S;
}
static void text_right(float gx, float base, const char *s, uint32_t c, float alpha) {
    ui_text_right(&cv, SX(gx), SY(base), CAP * S, s, c, AL(255 * alpha));
}
static void text_mid(float gx, float base, const char *s, uint32_t c, float alpha) {
    ui_text_center(&cv, SX(gx), SY(base), CAP * S, s, c, AL(255 * alpha));
}
static float width(const char *s) { return ui_text_width(s, CAP * S) / S; }
/* a line in the room it has: at the usual size, or smaller (down to three
 * quarters); 0 if it cannot be fitted */
static int text_fit(float gx, float base, float room, const char *s, uint32_t c, float alpha) {
    float w = width(s), cap = CAP;
    if (w > room) {
        cap = CAP * room / w;
        if (cap < CAP * 0.75f) return 0;
    }
    ui_text(&cv, SX(gx), SY(base), cap * S, s, c, AL(255 * alpha));
    return 1;
}
/* text cut off at x_right */
static void text_clip(float gx, float base, float x_right, const char *s, uint32_t c, float alpha) {
    int keep = cv.x1, lim = (int)SX(x_right);
    if (lim < cv.x1) cv.x1 = lim;
    if (cv.x1 > cv.x0) text(gx, base, s, c, alpha);
    cv.x1 = keep;
}

/* "AGILITY is UP!" -> "Agility is Up!": all-capital words in title case, the
 * way names read elsewhere in the port (HP, TP, LV and EXP stay) */
static void soften(char *s) {
    for (char *p = s; *p;) {
        char *q = p;
        int letters = 0, caps = 1;
        while (*q && *q != ' ' && *q != ':') {
            if (*q >= 'a' && *q <= 'z') caps = 0;
            if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) letters++;
            q++;
        }
        int keep = (q - p == 2 && (!strncmp(p, "HP", 2) || !strncmp(p, "TP", 2) || !strncmp(p, "LV", 2)))
                || (q - p == 3 && !strncmp(p, "EXP", 3));
        if (caps && letters >= 2 && !keep) {
            int first = 1;
            for (char *k = p; k < q; k++) {
                if (*k >= 'A' && *k <= 'Z') { if (!first) *k = (char)(*k - 'A' + 'a'); first = 0; }
                else if (*k == '-' || *k == '.') first = 1;
            }
        }
        p = *q ? q + 1 : q;
    }
}

static int utf8(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63)); o[2] = (char)(0x80 | (cp & 63));
    return 3;
}

/* The party's battle art is cut off at y 168, the top of the game's status
 * window, which hid the edge. The party's windows sit on a dock across the
 * whole view from just above that line, so the party stands behind it as in
 * the original, and a soft shadow above the dock lets them fade into it. */
#define DOCK_Y      165.0f
#define CARD_Y0     167.0f        /* a member's window, the battle playing out */
#define CARD_Y1     223.0f
/* while something is chosen the dock opens a bar for it under the party (so
 * no menu covers anyone) and the members' windows shorten to make room */
#define BAR_Y0      166.5f
#define BAR_Y1      184.0f
#define CARD_Y0_BAR 185.0f
static float strip_in;            /* 0..1, eased: the bar is up */

/* ------------------------------------------------------------------ the aura */
/* The member choosing glows: a soft light around their figure as it is in
 * the frame (after the depth camera), brightest at its outline, with motes
 * drifting up from it. Drawn before the dock and never over a party member's
 * own pixels, so it sits behind the party the way the party sits behind the
 * dock. An ally being targeted glows cyan the same way. */
#define AURA_PAD 12
#define AURA_W   (48 + 2 * AURA_PAD)
#define AURA_H   (PARTY_ROWS * 8 + 2 * AURA_PAD)
static uint32_t mote_rng = 0x2545F491u;
static float mrand(void) {                                     /* xorshift: 0..1 */
    mote_rng ^= mote_rng << 13; mote_rng ^= mote_rng >> 17; mote_rng ^= mote_rng << 5;
    return (float)(mote_rng & 0xFFFFFF) / 16777216.0f;
}

/* a box blur of radius r, in place (tmp: scratch of the same size) */
static void box_blur(float *f, float *tmp, int w, int h, int r) {
    float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < h; y++) {
        const float *s = f + y * w;
        float *d = tmp + y * w, acc = 0;
        for (int x = 0; x <= r && x < w; x++) acc += s[x];
        for (int x = 0; x < w; x++) {
            d[x] = acc * inv;
            if (x + r + 1 < w) acc += s[x + r + 1];
            if (x - r >= 0) acc -= s[x - r];
        }
    }
    for (int x = 0; x < w; x++) {
        float acc = 0;
        for (int y = 0; y <= r && y < h; y++) acc += tmp[y * w + x];
        for (int y = 0; y < h; y++) {
            f[y * w + x] = acc * inv;
            if (y + r + 1 < h) acc += tmp[(y + r + 1) * w + x];
            if (y - r >= 0) acc -= tmp[(y - r) * w + x];
        }
    }
}

static float field_at(const float *f, int w, int h, int x, int y) {
    return x < 0 || y < 0 || x >= w || y >= h ? 0.0f : f[y * w + x];
}

static float bilinear(const float *f, int w, int h, float x, float y) {
    x -= 0.5f; y -= 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    float a = field_at(f, w, h, x0, y0), b = field_at(f, w, h, x0 + 1, y0);
    float c = field_at(f, w, h, x0, y0 + 1), d = field_at(f, w, h, x0 + 1, y0 + 1);
    float top = a + (b - a) * fx;
    return top + (c + (d - c) * fx - top) * fy;
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
    uint32_t o = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float ca = (float)(a >> sh & 255), cb = (float)(b >> sh & 255);
        o |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return o;
}

static void add_light(uint32_t *p, uint32_t rgb, float a) {
    if (a <= 0.0f) return;
    int r = (int)(*p >> 16 & 255) + (int)((float)(rgb >> 16 & 255) * a);
    int g = (int)(*p >> 8 & 255) + (int)((float)(rgb >> 8 & 255) * a);
    int b = (int)(*p & 255) + (int)((float)(rgb & 255) * a);
    *p = 0xFF000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8
       | (uint32_t)(b > 255 ? 255 : b);
}

/* where the party layer is in the output: output = (s * frame + b) * S (the
 * depth camera's last frame; s = 1, b = 0 without it) */
static void party_xform(float *s, float *bx, float *by) {
    float ex, ey;
    depth_to_screen(1, 0.0f, 0.0f, bx, by);
    depth_to_screen(1, 1.0f, 1.0f, &ex, &ey);
    *s = ex - *bx;
    if (*s < 0.5f || *s > 2.0f) { *s = 1.0f; *bx = *by = 0.0f; }
}

static int party_px(const VdpLine *l, int x) {
    uint8_t a = l->win_in[x] ? 0 : l->plane_a[x];
    return a && (a & 0x80) && party_tile(l->plane_a_tile[x]);
}

/* the aura's light on the picture, a row at a time (in pieces on several
 * cores, par.h): the halo and the rim, sampled from their blurred fields */
static float aura_glow[AURA_W * AURA_H], aura_rim[AURA_W * AURA_H];
static uint8_t aura_occ[AURA_W * AURA_H];
typedef struct {
    int X0, X1, Y0, rw, rh, fx0, fy0;
    float s, bx, by, a;
    uint32_t glow_rgb, rim_rgb;
} AuraRows;

static void aura_rows(void *ctx, int r0, int r1) {
    const AuraRows *q = ctx;
    const float *glow = aura_glow, *rim = aura_rim;
    const uint8_t *occ = aura_occ;
    int X0 = q->X0, X1 = q->X1, rw = q->rw, rh = q->rh, fx0 = q->fx0, fy0 = q->fy0;
    float s = q->s, bx = q->bx, by = q->by, a = q->a;
    uint32_t glow_rgb = q->glow_rgb, rim_rgb = q->rim_rgb;
    for (int Y = q->Y0 + r0; Y < q->Y0 + r1; Y++) {
        float ly = (((float)Y + 0.5f) / S - by) / s - (float)fy0;
        int iy = (int)floorf(ly);
        uint32_t *row = cv.px + (size_t)Y * (size_t)cv.stride;
        for (int X = X0; X < X1; X++) {
            float lx = (((float)X + 0.5f) / S - bx) / s - (float)fx0;
            int ix = (int)floorf(lx);
            if (ix >= 0 && iy >= 0 && ix < rw && iy < rh && occ[iy * rw + ix]) continue;
            float g = bilinear(glow, rw, rh, lx, ly), e = bilinear(rim, rw, rh, lx, ly);
            if (g <= 0.002f && e <= 0.002f) continue;
            g = fminf(1.0f, g * 2.4f);
            e = fminf(1.0f, e * 2.4f);
            add_light(&row[X], glow_rgb, a * 0.6f * g * sqrtf(g));
            add_light(&row[X], rim_rgb, a * 0.4f * e);
        }
    }
}

/* k: the fighter; glow / rim: the light's colours; strength 0..1; t: seconds */
static void draw_aura(int k, uint32_t glow_rgb, uint32_t rim_rgb, float strength, float t, int frame_w,
                      int frame_h, int with_motes) {
    const Member *m = &members[k];
    if (!m->valid || strength <= 0.003f) return;
    int r0 = PARTY_ROWS, r1 = -1;
    for (int r = 0; r < PARTY_ROWS; r++)
        for (int c = 0; c < 6; c++)
            if (m->cell[r][c]) { if (r < r0) r0 = r; r1 = r; }
    if (r1 < 0) return;
    /* their pixels, and everyone's (the light never covers the party) */
    static float tmp[AURA_W * AURA_H];
    float *glow = aura_glow, *rim = aura_rim;
    uint8_t *occ = aura_occ;
    int fx0 = m->col0 * 8 + (int)OX - AURA_PAD, fy0 = (PARTY_ROW0 + r0) * 8 - AURA_PAD;
    int rw = AURA_W, rh = (r1 - r0 + 1) * 8 + 2 * AURA_PAD;
    int any = 0;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            int fx = fx0 + x, fy = fy0 + y, i = y * rw + x, p = 0;
            if (fx >= 0 && fy >= 0 && fx < frame_w && fy < frame_h) p = party_px(&frame_lines[fy], fx);
            occ[i] = (uint8_t)p;
            glow[i] = rim[i] = p && x >= AURA_PAD && x < AURA_PAD + 48 ? 1.0f : 0.0f;
            any |= p;
        }
    if (!any) return;
    for (int i = 0; i < 3; i++) box_blur(glow, tmp, rw, rh, 4);
    box_blur(rim, tmp, rw, rh, 1);
    /* the halo shimmers, a slow pattern flowing upward */
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            float w = sinf((float)x * 0.35f + t * 1.3f);
            glow[y * rw + x] *= 0.78f + 0.22f * sinf((float)y * 0.45f + t * 5.0f + 1.7f * w);
        }

    float s, bx, by;
    party_xform(&s, &bx, &by);
    int X0 = (int)floorf(((float)fx0 * s + bx) * S), Y0 = (int)floorf(((float)fy0 * s + by) * S);
    int X1 = (int)ceilf(((float)(fx0 + rw) * s + bx) * S), Y1 = (int)ceilf(((float)(fy0 + rh) * s + by) * S);
    if (X0 < cv.x0) X0 = cv.x0;
    if (Y0 < cv.y0) Y0 = cv.y0;
    if (X1 > cv.x1) X1 = cv.x1;
    if (Y1 > cv.y1) Y1 = cv.y1;
    AuraRows q = {X0, X1, Y0, rw, rh, fx0, fy0, s, bx, by, strength * fade, glow_rgb, rim_rgb};
    par_rows(aura_rows, &q, Y1 - Y0);

    if (!with_motes) return;
    /* motes: born just outside the figure (above the dock) */
    mote_due += 12.0f * strength / 60.0f;
    while (mote_due >= 1.0f && nmotes < MAX_MOTES) {
        mote_due -= 1.0f;
        for (int tries = 0; tries < 24; tries++) {
            int x = AURA_PAD - 2 + (int)(mrand() * 52.0f), y = (int)(mrand() * (float)rh * 0.7f);
            if (fy0 + y > (int)DOCK_Y - 8) continue;
            int i = y * rw + x;
            if (x < 0 || x >= rw || occ[i] || rim[i] < 0.15f) continue;
            Mote *q = &motes[nmotes++];
            q->x = (float)(fx0 + x) + mrand();
            q->y = (float)(fy0 + y) + mrand();
            q->vy = -(14.0f + 10.0f * mrand());
            q->life = q->max_life = 1.2f + 0.8f * mrand();
            q->size = 0.8f + 0.6f * mrand();
            q->phase = mrand() * 6.2831853f;
            break;
        }
    }
    if (mote_due > 1.0f) mote_due = 1.0f;
}

/* the motes drift up and fade, whoever they came from */
static void draw_motes(uint32_t rgb) {
    const float dt = 1.0f / 60.0f;
    float s, bx, by;
    party_xform(&s, &bx, &by);
    for (int i = 0; i < nmotes;) {
        Mote *q = &motes[i];
        q->life -= dt;
        if (q->life <= 0.0f) { motes[i] = motes[--nmotes]; continue; }
        q->y += q->vy * dt;
        q->phase += dt * 3.1f;
        float age = 1.0f - q->life / q->max_life;             /* 0 -> 1 */
        float alpha = (age < 0.2f ? age / 0.2f : 1.0f) * (1.0f - age) * fade;
        float mx = (q->x + 1.4f * sinf(q->phase)) * s + bx, my = q->y * s + by;
        float R = q->size * S * 1.6f;
        int x0 = (int)(mx * S - R), x1 = (int)(mx * S + R) + 1, y0 = (int)(my * S - R), y1 = (int)(my * S + R) + 1;
        for (int Y = y0 < cv.y0 ? cv.y0 : y0; Y < y1 && Y < cv.y1; Y++)
            for (int X = x0 < cv.x0 ? cv.x0 : x0; X < x1 && X < cv.x1; X++) {
                float dx = ((float)X + 0.5f - mx * S) / R, dy = ((float)Y + 0.5f - my * S) / R, d2 = dx * dx + dy * dy;
                if (d2 >= 1.0f) continue;
                float f = (1.0f - d2) * (1.0f - d2);
                add_light(&cv.px[(size_t)Y * (size_t)cv.stride + (size_t)X], rgb, alpha * f * 0.9f);
            }
        i++;
    }
}

/* ------------------------------------------------------------------ the party bar */
typedef struct { float x0, x1; int id; float stand; } Card;   /* stand: where they stand, game x */
static Card cards[5];
static int ncards;

/* the cards in the order the party stands, left to right, as the game's
 * status bar has them (the party slots can be in another order) */
static void card_layout(int view_w) {
    int ids[5], xs[5], n = 0;
    for (int k = 0; k < 5; k++) {
        int id = fighter_char(k);
        if (id < 0) continue;
        /* by their place in the line, left to right: where they stand at the
         * menus (the game moves the one in front of the action now and then,
         * never there), or before the first menu where they are. (Fighter
         * +$13 is not the place: it is the character's ID + 1.) Someone
         * down since before the battle is never drawn or placed (+$C stays
         * 0): their card keeps the place the game's table gives them, the
         * gap in the line, instead of going to the far left. */
        int x = home_x[k] ? home_x[k] : (int)r16(FIGHTERS + (unsigned)k * 0x40 + 0x0C), j = n++;
        if (!x) x = LINE_X[k];
        while (j > 0 && xs[j - 1] > x) { ids[j] = ids[j - 1]; xs[j] = xs[j - 1]; j--; }
        ids[j] = id;
        xs[j] = x;
    }
    float left = -OX + 4.0f, right = (float)view_w - OX - 4.0f, gap = 3.0f;
    float cw = (right - left - gap * (float)(n - 1)) / (float)(n ? n : 1);
    if (cw > 118.0f) cw = 118.0f;
    float total = (float)n * cw + (float)(n - 1) * gap, x = (left + right) / 2.0f - total / 2.0f;
    for (int k = 0; k < n; k++) {
        cards[k].x0 = x + (float)k * (cw + gap);
        cards[k].x1 = cards[k].x0 + cw;
        cards[k].id = ids[k];
        cards[k].stand = (float)xs[k] - 128.0f + 8.0f;
    }
    ncards = n;
}

static void command_tag(int fighter, char *out, size_t n, uint32_t *color) {
    unsigned cmd = r8(COMMAND_DATA + (unsigned)fighter * 4), id = r8(COMMAND_DATA + (unsigned)fighter * 4 + 1);
    out[0] = 0;
    *color = COMMAND_COLOR[0];
    if (cmd < 1 || cmd > 5) return;
    *color = COMMAND_COLOR[cmd];
    const char *s = cmd == 2 ? ui_tech_name((int)id & 0x7F) : cmd == 3 ? ui_skill_name((int)id & 0x7F)
                  : cmd == 4 ? ui_item_name((int)id) : NULL;
    snprintf(out, n, "%s", s && *s ? s : COMMANDS[cmd - 1]);
}

static void draw_dock(float a) {
    float x0 = SX(-OX), x1 = (float)cv.x1, top = SY(DOCK_Y);
    int rows = (int)(9.0f * S);
    for (int i = 0; i < rows; i++) {                         /* the shadow: 0 to ~60% just above the dock */
        float t = (float)(i + 1) / (float)rows;
        ui_round_rect(&cv, x0, top - (float)(rows - i), x1, top - (float)(rows - i) + 1.0f, 0, 0x04060E,
                      AL(150.0f * t * t * a));
    }
    ui_round_rect_v(&cv, x0, top, x1, (float)cv.y1, 0, 0x101A4E, 0x070B26, AL(250 * a));
    ui_round_rect(&cv, x0, top, x1, top + fmaxf(1.0f, 0.6f * S), 0, 0x8A93B8, AL(190 * a));
}

/* a portrait's side in `room` game pixels: a whole multiple of its 48 pixels
 * on the screen where one fits; 0 if too small to tell who it is */
static float portrait_side(float room) {
    float out = room * S;
    if (room < 18.0f) return 0.0f;
    return out >= 48.0f ? floorf(out / 48.0f) * 48.0f / S : room;
}

/* HP or TP in a member's window: the label where there is room, the figures
 * from the right, a thin gauge under them */
static void card_gauge(float xl, float xr, float base, const char *label, const char *nums, float frac,
                       uint32_t numc, uint32_t barc, float a) {
    if (label) text(xl, base, label, SOFT, a);
    text_right(xr, base, nums, numc, a);
    ui_bar(&cv, SX(xl), SY(base + 1.7f), SX(xr), 1.8f * S, frac, barc, AL(255 * a));
}

/* The party's windows: each member's portrait, name, what they will do (or
 * how they are), HP and TP. Tall while the battle plays out; short, under
 * the bar, while something is chosen. */
static void draw_hud(int acting, int target_id, int show_commands, int frame, float a) {
    draw_dock(a);
    float y0 = CARD_Y0 + (CARD_Y0_BAR - CARD_Y0) * strip_in, y1 = CARD_Y1;
    /* while a member chooses (their window in gold), the others' windows go
     * faint: eased, a veil of the dock's dark over each (an ally being picked
     * stays clear) */
    static float faint[11];
    int focus = 0;
    if (acting >= 0)
        for (int k = 0; k < ncards; k++)
            if (cards[k].id == acting && !(r8(CHAR_STATS + (unsigned)acting * 0x80 + 0x16) & 0x44)) focus = 1;
    for (int k = 0; k < ncards; k++) {
        int id = cards[k].id;
        if (id < 0 || id >= 11) continue;
        float want = focus && id != acting && id != target_id ? 1.0f : 0.0f;
        faint[id] += (want - faint[id]) * 0.25f;
        if (fabsf(want - faint[id]) < 0.01f) faint[id] = want;
    }
    for (int k = 0; k < ncards; k++) {
        int id = cards[k].id;
        unsigned base = CHAR_STATS + (unsigned)id * 0x80;
        unsigned hp = r16(base + 0x0E), mhp = r16(base + 0x10), tp = r16(base + 0x12), mtp = r16(base + 0x14);
        unsigned status = r8(base + 0x16);
        int dead = (status & 0x44) != 0, me = id == acting && !dead, aimed = id == target_id;
        float x0 = cards[k].x0, x1 = cards[k].x1, cw = x1 - x0;
        /* the one choosing in gold, an ally being picked in cyan: as their auras */
        if (me || aimed) {
            float pulse = aimed ? 0.8f + 0.2f * sinf((float)frame * 0.25f) : 1.0f;
            ui_round_frame(&cv, SX(x0) - 1.3f * S, SY(y0) - 1.3f * S, SX(x1) + 1.3f * S, SY(y1) + 1.3f * S,
                           4.2f * S, 1.5f * S, aimed ? 0x7FE0FF : 0xF2C84B, AL(235 * pulse * a));
        }
        win(x0, y0, x1, y1, a);

        char hps[24], tps[24];
        snprintf(hps, sizeof hps, "%u/%u", hp, mhp);
        snprintf(tps, sizeof tps, "%u/%u", tp, mtp);
        float numw = width(mhp > 999 ? "9999/9999" : "999/999");
        /* the portrait: as large as the window's height and the figures' width leave */
        const UiImage *im = ui_portrait(id);
        float px = x0 + 4.0f, py = y0 + 4.0f, ps = 0.0f, widest = 0.0f;   /* (py: centred, below) */
        if (im) {
            float room_w = cw - 8.0f - 3.0f - numw;
            float tall = portrait_side(fminf(CARD_Y1 - CARD_Y0 - 8.0f, room_w));
            float brief = portrait_side(fminf(CARD_Y1 - CARD_Y0_BAR - 8.0f, room_w));
            ps = tall + (brief - tall) * strip_in;
            widest = tall;
            if (tall <= 0.0f || brief <= 0.0f) ps = widest = 0.0f;
        }
        if (ps > 0.0f) {
            py = (y0 + y1 - ps) / 2.0f;
            ui_round_rect(&cv, SX(px), SY(py), SX(px + ps), SY(py + ps), 1.2f * S, 0x0C1440, AL(255 * a));
            ui_image(&cv, im, SX(px), SY(py), ps * S, dead, AL(255 * a));
        }
        float tx = ps > 0.0f ? px + ps + 3.5f : x0 + 6.0f, tr = x1 - 6.0f;
        float name_base = y0 + 10.6f;
        const char *name = UI_CHAR_NAMES[id];
        text_clip(tx, name_base, tr, name, dead ? FAINT : WHITE, a);

        /* how they are, or what they will do: on a row of its own while the
         * window is tall; beside the name, if it fits, while it is short */
        char tag[32] = "";
        const char *brief_tag = NULL;
        uint32_t tc = ILL;
        if (dead) snprintf(tag, sizeof tag, "Down");
        else if (status & 0x01) { snprintf(tag, sizeof tag, "Poisoned"); brief_tag = "Psn"; }
        else if (status & 0x02) { snprintf(tag, sizeof tag, "Paralyzed"); brief_tag = "Par"; }
        else if (status & 0x28) { snprintf(tag, sizeof tag, "Asleep"); brief_tag = "Slp"; }
        if (!tag[0] && show_commands) {
            uint32_t unused;
            for (int f = 0; f < 5; f++)
                if (fighter_char(f) == id) command_tag(f, tag, sizeof tag, &unused);
            tc = SOFT;
        }
        if (tag[0]) {
            float room = tr - (tx + width(name) + 4.0f);
            if (strip_in < 0.5f) text_clip(tx, y0 + 21.6f, tr, tag, tc, a);
            else if (width(tag) <= room) text_right(tr, name_base, tag, tc, a);
            else if (brief_tag && width(brief_tag) <= room) text_right(tr, name_base, brief_tag, tc, a);
        }

        /* HP and TP, from the window's foot, beside the portrait */
        float hb = y1 - 18.0f, tb = y1 - 7.4f;
        float frac = mhp ? (float)hp / (float)mhp : 0.0f;
        uint32_t hc = frac > 0.5f ? 0x6FD080 : frac > 0.25f ? 0xF2C84B : 0xF0524F;
        /* the labels, if there is room for them beside the portrait at its largest */
        float least = tr - (widest > 0.0f ? px + widest + 3.5f : x0 + 6.0f);
        int labels = least >= fmaxf(width(hps), mtp ? width(tps) : 0.0f) + 15.0f;
        card_gauge(tx, tr, hb, labels ? "HP" : NULL, hps, frac, frac > 0.25f ? WHITE : ILL, hc, a);
        if (mtp) card_gauge(tx, tr, tb, labels ? "TP" : NULL, tps, (float)tp / (float)mtp, WHITE, 0x6AA8F0, a);
        if (id >= 0 && id < 11 && faint[id] > 0.0f)
            ui_round_rect(&cv, SX(x0) - 0.5f * S, SY(y0) - 0.5f * S, SX(x1) + 0.5f * S, SY(y1) + 0.5f * S, 3.0f * S,
                          0x070B26, AL(150 * faint[id] * a));
    }
}

/* ------------------------------------------------------------------ the mouse */
/* What a click can pick, where it was drawn this frame (frame pixels): a main
 * option, a command, a list entry or a page arrow, a target. A click is
 * played as the pad: the game's own cursor is walked to it a press at a time
 * (each press let go before the next: the menus act on new presses), then C. */
typedef struct { float x0, y0, x1, y1; int menu, index, confirm; } Hit;
#define MAX_HITS 64
static Hit hits[MAX_HITS];
static int nhits;

static void hit_add(float x0, float y0, float x1, float y1, int menu, int index, int confirm) {
    if (nhits < MAX_HITS) {
        Hit *h = &hits[nhits++];
        h->x0 = x0 + OX; h->y0 = y0; h->x1 = x1 + OX; h->y1 = y1;
        h->menu = menu; h->index = index; h->confirm = confirm;
    }
}

/* ------------------------------------------------------------------ menus */
static float menu_alpha;          /* the open menu's (1: windows do not fade in) */
static float actor_sx = 160.0f;   /* where the character choosing stands on screen, game x */
static const char *list_line;     /* what the entry under the cursor of the list drawn this frame does */

/* The bar the dock opens under the party: what is being chosen, across, and
 * a line on the choice under the cursor -- the battle's three choices (the
 * game's own window is steered Up / Down: Left / Right here, see
 * battleui_pad); the member's five commands; with a list or a target open,
 * the command `chosen` and `line`: what the list's entry does, the target. */
static void draw_bar(int menu, int fighter, const char *chosen, const char *line, float alpha) {
    static const char *const MAIN[3] = {"Command", "Macro", "Run"};
    static const char *const MAIN_HELP[3] = {
        "Choose what each member does.", "Play back a plan you have saved.", "Try to get away.",
    };
    float lo = -OX + 4.0f, hi = (float)cv.x1 / S - OX - 4.0f, base = BAR_Y0 + 11.6f;
    /* it opens downward as the members' windows make room: cut off at its foot until then */
    float foot = BAR_Y0 + (BAR_Y1 - BAR_Y0) * strip_in;
    if (foot - BAR_Y0 < 6.0f) return;
    win(lo, BAR_Y0, hi, foot, alpha);
    int clip = cv.y1, lim = (int)SY(foot - 2.5f);
    if (lim < cv.y1) cv.y1 = lim;
    if (menu == M_NONE) { cv.y1 = clip; return; }            /* closing: empty */
    float x = lo + 4.0f;
    uint32_t lc = WHITE;
    if (chosen) {
        float w = width(chosen) + 10.0f;
        bar(x, x + w, base, BAR_OFF, alpha);
        text(x + 5.0f, base, chosen, WHITE, alpha);
        x += w;
    } else {
        int main_menu = menu == M_MAIN, n = main_menu ? 3 : 5;
        const char *const *names = main_menu ? MAIN : COMMANDS;
        int cur = main_menu ? (int)r16(MAIN_OPTION) % 3 : (int)r16(COMD_INDEX + (unsigned)fighter * 2) % 5;
        for (int i = 0; i < n; i++) {
            float w = width(names[i]) + 10.0f;
            hit_add(x, BAR_Y0, x + w, BAR_Y1, main_menu ? M_MAIN : M_COMMAND, i, 1);
            if (i == cur) bar(x, x + w, base, BAR, alpha);
            text(x + 5.0f, base, names[i], i == cur ? NAVY : WHITE, alpha);
            x += w;
        }
        if (!line) line = main_menu ? MAIN_HELP[cur] : COMMAND_HELP[cur];
        lc = SOFT;
    }
    if (line && *line) text_fit(x + 7.0f, base, hi - 7.0f - (x + 7.0f), line, lc, alpha);
    cv.y1 = clip;
}

/* where the party stands (game x, y), from the members in the picture this
 * frame, with a margin for the depth camera's drift: menus are placed by it
 * and stay put while the camera moves; 0 if none */
static float party_l, party_r, party_top;
static int party_span(void) {
    int any = 0;
    party_l = 1e9f; party_r = -1e9f; party_top = 1e9f;
    for (int k = 0; k < 5; k++) {
        const Member *m = &members[k];
        if (!m->valid || !(shown >> k & 1)) continue;
        for (int r = 0; r < PARTY_ROWS; r++)
            for (int c = 0; c < 6; c++) {
                if (!m->cell[r][c]) continue;
                party_l = fminf(party_l, (float)((m->col0 + c) * 8) - 3.0f);
                party_r = fmaxf(party_r, (float)((m->col0 + c) * 8 + 8) + 3.0f);
                party_top = fminf(party_top, (float)((PARTY_ROW0 + r) * 8) - 3.0f);
                any = 1;
            }
    }
    return any;
}

typedef struct { char name[24]; char cost[12]; int usable; const char *desc; } Entry;

/* a list in pages of four: the pages side by side, as many as fit. What the
 * entry under the cursor does goes to the bar (list_line). */
static void draw_list(int menu, const char *title, const char *info, const Entry *e, int count, int cur, float alpha) {
    int pages = (count + 3) / 4;
    if (pages < 1) pages = 1;
    int cols = pages > 3 ? 3 : pages;
    int page = cur / 4, first = page - cols / 2;
    if (first > pages - cols) first = pages - cols;
    if (first < 0) first = 0;
    /* a column as wide as the longest entry needs, the window at least as wide as its heading */
    float colw = 52.0f, row = 11.4f;
    for (int i = 0; i < count; i++) {
        if (!e[i].name[0]) continue;
        float w = width(e[i].name) + (e[i].cost[0] ? width(e[i].cost) + 7.0f : 0.0f) + 11.0f;
        if (w > colw) colw = w;
    }
    if (colw > 86.0f) colw = 86.0f;
    int turns = pages > cols;                                 /* more pages than shown: arrows in the heading */
    float need = 16.0f + width(title) + (info && *info ? 8.0f + width(info) : 0.0f) + (turns ? 20.0f : 0.0f);
    if ((float)cols * colw + 8.0f < need) colw = (need - 8.0f) / (float)cols;
    float w = 8.0f + (float)cols * colw, h = 66.0f;
    float lo = -OX + 4.0f, hi = (float)(cv.x1 / S) - OX - 4.0f;
    /* beside the party where there is room, on the side of the member
     * choosing, so it covers no one; else above everyone's heads */
    float x0, y1;
    int have = party_span();
    float lx0 = party_l - 2.0f - w, rx0 = party_r + 2.0f, mid = (party_l + party_r) / 2.0f;
    /* the member in the middle: the side with more room */
    int near_left = actor_sx < mid - 4.0f || (actor_sx <= mid + 4.0f && lx0 - lo >= hi - (rx0 + w));
    int fit_l = have && lx0 >= lo, fit_r = have && rx0 + w <= hi;
    if (fit_l && (near_left || !fit_r)) { x0 = lx0; y1 = DOCK_Y - 3.0f; }
    else if (fit_r) { x0 = rx0; y1 = DOCK_Y - 3.0f; }
    else {
        x0 = actor_sx - w / 2.0f;
        y1 = (have ? party_top : 117.0f) - 1.0f;
        if (x0 < lo) x0 = lo;
        if (x0 + w > hi) x0 = hi - w;
    }
    float y0 = y1 - h, x1 = x0 + w, head = y0 + 12.4f, ix = x1 - 8.0f;
    win(x0, y0, x1, y1, alpha);
    text(x0 + 8.0f, head, title, SOFT, alpha);
    if (turns) {
        int before = first > 0, after = first + cols < pages;
        ui_triangle(&cv, SX(x1 - 17.0f), SY(head - 2.8f), 5.0f * S, 3, before ? WHITE : FAINT, AL(255 * alpha));
        ui_triangle(&cv, SX(x1 - 8.5f), SY(head - 2.8f), 5.0f * S, 2, after ? WHITE : FAINT, AL(255 * alpha));
        if (before) hit_add(x1 - 22.0f, y0 + 2.0f, x1 - 12.75f, y0 + 17.0f, menu, (first - 1) * 4, 0);
        if (after) hit_add(x1 - 12.75f, y0 + 2.0f, x1 - 2.0f, y0 + 17.0f, menu, (first + cols) * 4, 0);
        ix = x1 - 24.0f;
    }
    if (info && *info) text_right(ix, head, info, SOFT, alpha);
    for (int c = 0; c < cols; c++)
        for (int r = 0; r < 4; r++) {
            int i = (first + c) * 4 + r;
            float ex = x0 + 4.0f + (float)c * colw, base = y0 + 25.0f + (float)r * row;
            int on = i == cur;
            if (on) bar(ex, ex + colw, base, BAR, alpha);
            if (i >= count || !e[i].name[0]) continue;
            hit_add(ex, base - 8.6f, ex + colw, base + 2.8f, menu, i, 1);
            text(ex + 5.0f, base, e[i].name, on ? NAVY : e[i].usable ? WHITE : FAINT, alpha);
            if (e[i].cost[0]) text_right(ex + colw - 5.0f, base, e[i].cost, on ? NAVY : e[i].usable ? SOFT : FAINT, alpha);
        }
    list_line = cur >= 0 && cur < count && e[cur].name[0] ? e[cur].desc : NULL;
}

static Entry entries[ITEM_SLOTS];

static void draw_techs(int fighter, int id, float alpha) {
    int n = 0;
    for (int i = 0; i < 16; i++) {
        unsigned b = r8(TECH_LIST + (unsigned)i), t = b & 0x7F;
        Entry *e = &entries[i];
        memset(e, 0, sizeof *e);
        if (!t || t > 40) continue;
        snprintf(e->name, sizeof e->name, "%s", ui_tech_name((int)t));
        snprintf(e->cost, sizeof e->cost, "%u", ui_rom8(TECH_DATA + (t - 1) * 8 + 1));
        e->usable = !(b & 0x80);
        e->desc = battledesc_tech((int)t);
        n = i + 1;
    }
    char info[32] = "";
    if (id >= 0) {
        unsigned base = CHAR_STATS + (unsigned)id * 0x80;
        snprintf(info, sizeof info, "TP %u/%u", r16(base + 0x12), r16(base + 0x14));
    }
    draw_list(M_TECH, "Techniques", info, entries, n, (int)r16(TECH_INDEX + (unsigned)fighter * 2) & 15, alpha);
}

static void draw_skills(int fighter, float alpha) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        unsigned b = r8(SKILL_LIST + (unsigned)i * 2), uses = r8(SKILL_LIST + (unsigned)i * 2 + 1), s = b & 0x7F;
        Entry *e = &entries[i];
        memset(e, 0, sizeof *e);
        if (!s || s > 54) continue;
        snprintf(e->name, sizeof e->name, "%s", ui_skill_name((int)s));
        snprintf(e->cost, sizeof e->cost, "\xC3\x97%u", uses);           /* x3 */
        e->usable = !(b & 0x80);
        e->desc = battledesc_skill((int)s);
        n = i + 1;
    }
    draw_list(M_SKILL, "Skills", "uses left", entries, n, (int)r16(SKILL_INDEX + (unsigned)fighter * 2) & 7, alpha);
}

static void draw_items(int fighter, int id, float alpha) {
    int n = 0, equipped = 0;
    if (id >= 0)                                     /* the list starts with what they wear (stats +$4C) */
        for (int k = 0; k < 4; k++) equipped += r8(CHAR_STATS + (unsigned)id * 0x80 + 0x4C + (unsigned)k) != 0;
    for (int i = 0; i < ITEM_SLOTS; i++) {
        unsigned it = r8(ITEM_LIST + (unsigned)i);
        Entry *e = &entries[i];
        memset(e, 0, sizeof *e);
        if (!it) continue;
        snprintf(e->name, sizeof e->name, "%s", ui_item_name((int)it));
        e->usable = 1;
        e->desc = battledesc_item((int)it);
        if (!e->desc && i < equipped) e->desc = "Equipped.";
        n = i + 1;
    }
    int cur = (int)r16(ITEM_INDEX + (unsigned)fighter * 2);
    char info[24];
    snprintf(info, sizeof info, "page %d/%d", cur / 4 + 1, (n + 3) / 4 > 0 ? (n + 3) / 4 : 1);
    draw_list(M_ITEM, "Items", info, entries, n, cur, alpha);
}

/* a chevron over what the game's cursor pointed at, its name in a window above */
static void draw_target(const char *label, int party, int frame, float alpha) {
    for (int i = 0; i < npointers; i++) {
        const Pointer *p = &pointers[i];
        if (!p->down) continue;
        float gx = (float)p->x + 8.0f, gy = (float)p->y + 10.0f, sx, sy;
        depth_to_screen(party, gx + OX, gy, &sx, &sy);
        sx -= OX;
        float bob = 1.5f * sinf((float)frame * 0.2f);
        ui_triangle(&cv, SX(sx), SY(sy + bob), 9.4f * S, 0, 0x070B26, AL(235 * alpha));
        ui_triangle(&cv, SX(sx), SY(sy + bob - 0.7f), 7.0f * S, 0, BAR, AL(255 * alpha));
        if (label && *label) {
            float lw = width(label) + 15.0f, ly = sy - 23.5f + bob;
            if (ly < 2.0f) ly = 2.0f;
            win(sx - lw / 2.0f, ly, sx + lw / 2.0f, ly + 16.0f, alpha);
            text_mid(sx, ly + 10.9f, label, WHITE, alpha);
        }
        break;
    }
}

/* any other window: its text, read back from the tiles, in the same frame.
 * The game puts its messages during the battle (a technique's name,
 * "Victory!") where the party stood, as they were gone by then: one that
 * would cover them goes to the top of the screen, under the enemy names if
 * those are up, clear of the action */
static void draw_window(const Win *w, int names, float alpha) {
    float x0 = (float)w->x * 8.0f + 1.5f, y0 = (float)w->y * 8.0f + 1.5f;
    float x1 = (float)(w->x + w->w) * 8.0f - 1.5f, y1 = (float)(w->y + w->h) * 8.0f - 1.5f;
    float dy = 0.0f;
    if (w->y >= 4 && party_span() && x1 > party_l && x0 < party_r && y1 > party_top) {
        float top = 5.5f;
        for (int i = 0; i < nwins; i++) {
            const Win *o = &wins[i];
            if (o->y < 4 && (float)(o->x + o->w) * 8.0f > x0 && (float)o->x * 8.0f < x1)
                top = fmaxf(top, (float)(o->y + o->h) * 8.0f + 1.5f);
        }
        dy = top - y0;
    }
    y0 += dy;
    y1 += dy;
    win(x0, y0, x1, y1, alpha);
    for (int r = 0; r < w->h - 2; r++) {
        float base = (float)(w->y + 1 + r) * 8.0f + 6.9f + dy;
        if (r == w->cursor) bar(x0 + 3.5f, x1 - 3.5f, base, BAR, alpha);
        /* runs of text split at gaps of two cells or more; a run of digits
         * is right-aligned to its last cell so columns of numbers line up */
        int c = 0, cols = w->w - 2;
        while (c < cols) {
            while (c < cols && !w->ch[r][c]) c++;
            if (c >= cols) break;
            int s = c, gap = 0, e = c;
            while (c < cols && gap < 2) {
                if (w->ch[r][c]) { gap = 0; e = c; } else gap++;
                c++;
            }
            char buf[160];
            int n = 0, digits = 1;
            for (int k = s; k <= e && n < (int)sizeof buf - 4; k++) {
                uint32_t cp = w->ch[r][k] ? w->ch[r][k] : ' ';
                if (!(cp >= '0' && cp <= '9') && cp != '/' && cp != ' ') digits = 0;
                n += utf8(cp, buf + n);
            }
            buf[n] = 0;
            if (names || !digits) soften(buf);
            uint32_t col = r == w->cursor ? NAVY : w->dim[r][s] ? FAINT : WHITE;
            float gx = (float)(w->x + 1 + s) * 8.0f;
            if (digits) text_right((float)(w->x + 2 + e) * 8.0f - 1.0f, base, buf, col, alpha);
            else text(gx + 0.5f, base, buf, col, alpha);
        }
    }
}

/* the targets a click can pick, as the game counts them: each enemy present
 * (a band round where it stands, the depth camera's drift included), or each
 * member of the party (their card, or a band round where they stand) */
static void band_hits(const float *xs, int n, float y0, float y1, int menu) {
    for (int i = 0; i < n; i++) {
        float l = -OX, r = (float)cv.x1 / S - OX;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            float mid = (xs[i] + xs[j]) / 2.0f;
            if (xs[j] < xs[i] && mid > l) l = mid;
            if (xs[j] > xs[i] && mid < r) r = mid;
        }
        hit_add(l, y0, r, y1, menu, i, 1);
    }
}

static void target_hits(int menu) {
    float xs[6], sx, sy;
    int n = 0;
    if (menu == M_TARGET_ENEMY) {
        for (int f; n < 6 && (f = nth_fighter(5, 10, n)) >= 0; n++) {
            float gx = (float)r16(FIGHTERS + (unsigned)f * 0x40 + 0x0C) - 128.0f + 8.0f;
            depth_to_screen(0, gx + OX, 60.0f, &sx, &sy);
            xs[n] = sx - OX;
        }
        band_hits(xs, n, 0.0f, DOCK_Y, M_TARGET_ENEMY);
    } else if (menu == M_TARGET_ALLY) {
        float y0 = CARD_Y0 + (CARD_Y0_BAR - CARD_Y0) * strip_in, y1 = CARD_Y1;
        for (int k = 0; k < ncards; k++) {
            hit_add(cards[k].x0, y0, cards[k].x1, y1, M_TARGET_ALLY, k, 1);
            depth_to_screen(1, cards[k].stand + OX, 120.0f, &sx, &sy);
            xs[n++] = sx - OX;
        }
        band_hits(xs, n, 32.0f, DOCK_Y, M_TARGET_ALLY);
    }
}

/* ------------------------------------------------------------------ the frame */
void battleui_draw(uint32_t *out, int out_stride, int scale, int view_w, int h) {
    static int frame, last_key = -1;
    nhits = 0;
    if (!active) return;
    frame++;
    cv.px = out; cv.stride = out_stride; cv.x0 = 0; cv.y0 = 0; cv.x1 = view_w * scale; cv.y1 = h * scale;
    S = (float)scale;
    OX = (float)-vx0;
    /* follow the game's fades with the window palette: its text colour ($F,
     * white) dims in a fade to black; its background ($E, dark blue) turns
     * grey in the fade from white that opens a battle */
    uint16_t text_c = vdp.cram[63], back_c = vdp.cram[62];
    int hi = text_c >> 1 & 7, lo = back_c >> 1 & 7;
    if ((text_c >> 5 & 7) > hi) hi = text_c >> 5 & 7;
    if ((text_c >> 9 & 7) > hi) hi = text_c >> 9 & 7;
    if ((back_c >> 5 & 7) < lo) lo = back_c >> 5 & 7;
    if ((back_c >> 9 & 7) < lo) lo = back_c >> 9 & 7;
    fade = (float)hi / 7.0f * (1.0f - (float)lo / 7.0f);

    int r2 = (int)(r16(BATTLE_ROUTINE_2) & 0x7FFF), menu = menu_of(r2);
    int fighter = (int)r16(COMD_INPUT), acting = menu != M_NONE ? fighter_char(fighter) : -1;
    if (fighter > 4) acting = -1;
    int key = menu * 8 + fighter;
    if (key != last_key) { menu_alpha = 0; last_key = key; }
    menu_alpha = 1.0f;                      /* a window is there or not, as in the camp menu */

    card_layout(view_w);
    int target_id = -1;
    if (menu == M_TARGET_ALLY) {            /* the game counts the party left to right, like the cards */
        int t = (int)r16(CHAR_INDEX);
        if (t >= 0 && t < ncards) target_id = cards[t].id;
    }

    /* the party bar fades in once the game has set up the fighters */
    static float hud_in;
    hud_in = ncards ? fminf(1.0f, hud_in + 0.1f) : 0.0f;

    /* the aura: gold on the member choosing, cyan on an ally being targeted
     * (not in a first-person battle: the party is not in the picture) */
    if (!fpview_in_battle()) {
        int actor_k = choosing(menu) && fighter < 5 && (restore >> fighter & 1) ? fighter : -1, target_k = -1;
        for (int k = 0; k < 5 && target_id >= 0; k++)
            if (fighter_char(k) == target_id && (restore >> k & 1)) target_k = k;
        float t = (float)frame / 60.0f;
        float gold_pulse = 0.84f + 0.16f * sinf(t * 6.2831853f / 1.6f);
        float cyan_pulse = 0.82f + 0.18f * sinf((float)frame * 0.25f);
        for (int k = 0; k < 5; k++) {
            /* while an ally is picked, the one choosing steps back a little */
            float goal = k == target_k ? 1.0f : k == actor_k ? (target_k >= 0 ? 0.45f : 1.0f) : 0.0f;
            aura_level[k] += (goal - aura_level[k]) * 0.16f;
            if (goal > 0.0f) aura_mix[k] += ((k == target_k ? 1.0f : 0.0f) - aura_mix[k]) * 0.25f;
            if (aura_level[k] < 0.004f) continue;
            float mx = aura_mix[k];
            uint32_t glow = mix_rgb(0xF0A040, 0x3FA8F0, mx), rim = mix_rgb(0xFFE08A, 0x9FEAFF, mx);
            float pulse = gold_pulse + (cyan_pulse - gold_pulse) * mx;
            draw_aura(k, glow, rim, aura_level[k] * pulse * hud_in, t, view_w, h, mx < 0.5f);
        }
        draw_motes(0xFFE08A);
    }

    /* the commands chosen this round, in the members' windows: not over the results */
    int tags = !((r2 >= 0x1E && r2 <= 0x26) || r2 == 0x2D || r2 == 0x2F);
    int bar_up = menu == M_MAIN || (acting >= 0 && choosing(menu));
    strip_in += ((bar_up ? 1.0f : 0.0f) - strip_in) * 0.22f;
    if (ncards) draw_hud(menu == M_MAIN ? -1 : acting, target_id, tags, frame, hud_in);

    /* other windows: enemy names, messages, results */
    for (int i = 0; i < nwins; i++) {
        const Win *w = &wins[i];
        if (w->y >= 20 && w->w >= 30) continue;                  /* the status bar: the party's windows instead */
        if (menu != M_NONE && w->y >= 4) continue;               /* a menu: drawn below */
        draw_window(w, w->y < 4, 1.0f);
    }

    /* the open list or the target; then the bar: the choices across it, or
     * the command chosen and a line on what the cursor is at */
    if (acting >= 0) actor_sx = (float)r16(FIGHTERS + (unsigned)fighter * 0x40 + 0x0C) - 128.0f;
    const char *chosen = NULL, *line = NULL;
    char cbuf[32] = "", lbuf[64];
    uint32_t unused;
    list_line = NULL;
    switch (menu) {
    case M_TECH:
        draw_techs(fighter, acting, menu_alpha);
        chosen = COMMANDS[1];
        line = list_line && *list_line ? list_line : "Choose a technique.";
        break;
    case M_SKILL:
        draw_skills(fighter, menu_alpha);
        chosen = COMMANDS[2];
        line = list_line && *list_line ? list_line : "Choose a skill.";
        break;
    case M_ITEM:
        draw_items(fighter, acting, menu_alpha);
        chosen = COMMANDS[3];
        line = list_line && *list_line ? list_line : "Choose an item.";
        break;
    case M_TARGET_ENEMY: case M_TARGET_ALLY: {
        char name[32] = "";
        if (menu == M_TARGET_ENEMY) {
            int f = nth_fighter(5, 10, (int)r16(ENEMY_INDEX));
            if (f >= 0) {
                uint32_t st = ui_ram32(FIGHTERS + (unsigned)f * 0x40 + 4) & 0xFFFF;
                if (st >= ENEMY_STATS && st < ENEMY_STATS + 0x200)
                    ui_decode_name(&m68k_ram[st], 14, name, (int)sizeof name);
            }
            draw_target(name, 0, frame, menu_alpha);
        } else if (target_id >= 0) {
            snprintf(name, sizeof name, "%s", UI_CHAR_NAMES[target_id]);
        }
        if (fighter < 5) command_tag(fighter, cbuf, sizeof cbuf, &unused);   /* the technique, skill or item by name */
        chosen = cbuf[0] ? cbuf : COMMANDS[0];
        if (name[0]) snprintf(lbuf, sizeof lbuf, "On whom?  %s", name);
        else snprintf(lbuf, sizeof lbuf, "On whom?");
        line = lbuf;
        break;
    }
    default: break;
    }
    if (bar_up || strip_in > 0.05f) draw_bar(bar_up ? menu : M_NONE, fighter, chosen, line, hud_in);
    target_hits(menu);
}

/* ------------------------------------------------------------------ clicks, as the pad */
static struct {
    int on, menu, fighter, target, confirm;
    int hold, rest, presses, waited;
    uint8_t press;
} drive;
static int pulse_b;

int battleui_click(float fx, float fy, int right) {
    if (!active) return 0;
    if (right) { drive.on = 0; pulse_b = 3; return 1; }   /* back */
    for (int i = nhits - 1; i >= 0; i--) {                 /* the last drawn is on top */
        const Hit *h = &hits[i];
        if (fx < h->x0 || fx >= h->x1 || fy < h->y0 || fy >= h->y1) continue;
        memset(&drive, 0, sizeof drive);
        drive.on = 1;
        drive.menu = h->menu;
        drive.fighter = (int)r16(COMD_INPUT);
        drive.target = h->index;
        drive.confirm = h->confirm;
        return 1;
    }
    return 0;
}

/* the cursor of the menu being driven, and the press that moves it towards
 * the target (0: it's there) */
static uint8_t drive_step(void) {
    unsigned f = (unsigned)drive.fighter * 2;
    int t = drive.target, cur;
    switch (drive.menu) {
    case M_MAIN:                                        /* up / down */
        cur = (int)r16(MAIN_OPTION) & 3;
        return cur < t ? PAD_DOWN : cur > t ? PAD_UP : 0;
    case M_COMMAND:                                     /* left / right */
        cur = (int)r16(COMD_INDEX + f) % 5;
        return cur < t ? PAD_RIGHT : cur > t ? PAD_LEFT : 0;
    case M_TARGET_ENEMY: case M_TARGET_ALLY:
        cur = (int)r16(drive.menu == M_TARGET_ENEMY ? ENEMY_INDEX : CHAR_INDEX);
        return cur < t ? PAD_RIGHT : cur > t ? PAD_LEFT : 0;
    default:                                            /* the lists: pages of four */
        cur = (int)r16((drive.menu == M_TECH ? TECH_INDEX : drive.menu == M_SKILL ? SKILL_INDEX : ITEM_INDEX) + f);
        if (cur >> 2 != t >> 2) return cur >> 2 < t >> 2 ? PAD_RIGHT : PAD_LEFT;   /* another page: its top */
        return (cur & 3) < (t & 3) ? PAD_DOWN : (cur & 3) > (t & 3) ? PAD_UP : 0;
    }
}

uint8_t battleui_pad(uint8_t user, uint8_t pad) {
    /* the battle's three choices lie across the bar: Left and Right move
     * among them as Up and Down do in the game's own window */
    if (active && (pad & (PAD_LEFT | PAD_RIGHT)) && menu_of((int)(r16(BATTLE_ROUTINE_2) & 0x7FFF)) == M_MAIN)
        pad = (uint8_t)((pad & ~(PAD_LEFT | PAD_RIGHT)) | (pad & PAD_LEFT ? PAD_UP : 0) | (pad & PAD_RIGHT ? PAD_DOWN : 0));
    if (pulse_b > 0) {                                  /* a right click: B */
        if (user) pulse_b = 0;
        else return --pulse_b > 0 ? PAD_B : 0;
    }
    if (!drive.on) return pad;
    if (user || !active) { drive.on = 0; return pad; }  /* the player takes over */
    if (drive.hold > 0) { drive.hold--; return drive.press; }
    if (drive.rest > 0) { drive.rest--; return 0; }     /* let go: the menus act on new presses */
    if (drive.press == PAD_C) { drive.on = 0; return 0; }   /* chosen */
    int menu = menu_of((int)(r16(BATTLE_ROUTINE_2) & 0x7FFF));
    if (menu != drive.menu || (int)r16(COMD_INPUT) != drive.fighter) {
        if (++drive.waited > 40) drive.on = 0;          /* a page turning, a window opening: wait */
        return 0;
    }
    drive.waited = 0;
    uint8_t p = drive_step();
    if (!p) {
        if (!drive.confirm) { drive.on = 0; return 0; } /* a page arrow: just there */
        p = PAD_C;
    }
    if (++drive.presses > 48) { drive.on = 0; return 0; }
    drive.press = p;
    drive.hold = 1;
    drive.rest = 2;
    return p;
}

/* ------------------------------------------------------------------ the camera pan */
int battleui_pan(int *member, int *dx) {
    if (!active || pan_member < 0) { *member = -1; *dx = 0; return 0; }
    *member = pan_member;
    *dx = pan_dx;
    return 1;
}

int battleui_tile_member(int tile) { return active ? (int)tile_owner[tile & 0x7FF] - 1 : -1; }

uint8_t battleui_pan_pixel(int px, int py, int *member) {
    int r = (py >> 3) & 31, c = (px >> 3) & 63, i = r * 64 + c;
    if (!active || !pan_owner[i]) return 0;
    uint16_t e = pan_cells[i];
    int tx = px & 7, ty = py & 7;
    if (e & 0x0800) tx = 7 - tx;
    if (e & 0x1000) ty = 7 - ty;
    uint8_t b = shadow[((unsigned)(e & 0x7FF) * 32 + (unsigned)ty * 4 + (unsigned)(tx >> 1)) & 0xFFFF];
    uint8_t v = (tx & 1) ? (b & 0x0F) : (b >> 4);
    if (!v) return 0;
    *member = pan_owner[i] - 1;
    return (uint8_t)((e >> 13 & 3) << 4 | v | 0x80);
}
