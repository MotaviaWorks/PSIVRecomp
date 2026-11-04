/*
 * Modern battle menus: see battleui.h.
 */
#include "battleui.h"
#include "battledesc.h"
#include "depth.h"
#include "frame.h"
#include "hdtext.h"
#include "m68k.h"
#include "uikit.h"
#include "vdp.h"

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
/* While commands are chosen the game shows only the member choosing: it
 * clears the others from plane A (and from the window-free copy of the map),
 * and its command window covers part of the one it shows. Their art stays in
 * VRAM, so the port keeps each member's cells from frames where they stand
 * in full and draws the whole party back while commands are chosen. */
#define PARTY_ROW0 4
#define PARTY_ROWS 17                       /* rows 4-20: the battle area above the status window */
typedef struct {
    int valid, base, col0, x;               /* art (first tile), first column, centre (game x) */
    uint16_t cell[PARTY_ROWS][6];           /* name-table entries; 0 = not theirs */
} Member;
static Member members[5];
static int standing;                        /* members on screen at the last main menu */
static int restore;                         /* the members drawn back this frame */

/* their aura (see below): how strong, gold (0) or cyan (1); motes rising */
static float aura_level[5], aura_mix[5];
typedef struct { float x, y, vy, life, max_life, size, phase; } Mote;
#define MAX_MOTES 40
static Mote motes[MAX_MOTES];
static int nmotes;
static float mote_due;

static int party_tile(uint16_t e) { int t = e & 0x7FF; return (e & 0x8000) && t >= 0x180 && t < 0x280; }

/* command input: anything but the main menu and the battle playing out */
static int choosing(int menu) { return menu != M_NONE && menu != M_MAIN; }

static void party_reset(void) {
    memset(members, 0, sizeof members);
    standing = restore = 0;
    memset(aura_level, 0, sizeof aura_level);
    nmotes = 0;
    mote_due = 0;
}

static void party_frame(void) {
    int bases[5];
    for (int k = 0; k < 5; k++)
        bases[k] = fighter_char(k) >= 0 ? (int)(r16(FIGHTERS + (unsigned)k * 0x40 + 0x0E) >> 5) : -1;
    int present = 0;
    for (int k = 0; k < 5; k++) {
        Member *m = &members[k];
        if (bases[k] < 0) { m->valid = 0; continue; }
        int x = (int)r16(FIGHTERS + (unsigned)k * 0x40 + 0x0C) - 128, col0 = (x - 24) >> 3;
        int lo = bases[k], hi = lo + 0x30;          /* their tiles: up to the next member's */
        for (int j = 0; j < 5; j++) if (bases[j] > lo && bases[j] < hi) hi = bases[j];
        if (m->valid && (m->base != lo || m->col0 != col0)) m->valid = 0;
        uint16_t now[PARTY_ROWS][6];
        int any = 0;
        for (int r = 0; r < PARTY_ROWS; r++)
            for (int c = 0; c < 6; c++) {
                uint16_t e = nt(PARTY_ROW0 + r, col0 + c);
                int t = e & 0x7FF;
                now[r][c] = e;
                if (party_tile(e) && t >= lo && t < hi) any = 1;
            }
        if (!any) continue;
        present |= 1 << k;
        for (int r = 0; r < PARTY_ROWS; r++)            /* cells under a window keep what they had */
            for (int c = 0; c < 6; c++) {
                uint16_t e = now[r][c];
                int t = e & 0x7FF;
                if (party_tile(e) && t >= lo && t < hi) m->cell[r][c] = e;
                else if (!m->valid || !ui_tile(e)) m->cell[r][c] = 0;
            }
        m->valid = 1; m->base = lo; m->col0 = col0; m->x = x;
    }
    int r2 = (int)(r16(BATTLE_ROUTINE_2) & 0x7FFF), menu = menu_of(r2);
    if (menu == M_MAIN) standing = present;
    /* after the last command the game clears the picture before it draws the
     * party for the actions: they stay up until it draws someone itself */
    static int hold;
    if (choosing(menu)) { restore = standing; hold = 45; }
    else if (menu == M_NONE && hold > 0 && !present) { restore = standing; hold--; }
    else { restore = 0; hold = 0; }
    {   /* PSIV_BUI_LOG=1: the party on screen as the battle state changes */
        static int log_on = -1, last = -1;
        if (log_on < 0) { const char *v = getenv("PSIV_BUI_LOG"); log_on = v && *v == '1'; }
        int key = r2 << 16 | (int)r16(COMD_INPUT) << 8 | present << 4 | restore;
        if (log_on && key != last)
            printf("battleui: routine %02X fighter %u present %02X standing %02X restore %02X\n",
                   r2, r16(COMD_INPUT), present, standing, restore);
        last = key;
    }
    for (int k = 0; k < 5; k++) {
        const Member *m = &members[k];
        if (!(restore >> k & 1) || !m->valid) continue;
        for (int r = 0; r < PARTY_ROWS; r++)
            for (int c = 0; c < 6; c++) {
                uint16_t e = m->cell[r][c];
                int row = PARTY_ROW0 + r, col = m->col0 + c;
                if (!e || col < 0 || col >= 64) continue;
                uint16_t cur = nt(row, col);
                if ((cur & 0x7FF) && !ui_tile(cur) && !party_tile(cur)) continue;   /* something else is there */
                override[row * 64 + col] = e;
            }
    }
}

static const char *const COMMANDS[5] = {"Attack", "Technique", "Skill", "Item", "Defend"};
static const char *const COMMAND_HELP[5] = {
    "Strike one enemy with your weapon.",
    "Use a technique. Costs TP.",
    "Use a special skill. Uses recover when you rest.",
    "Use something from the bag or your equipment.",
    "Brace yourself and take less damage this turn.",
};
static const uint32_t COMMAND_COLOR[6] = {0xB8C4E8, 0xFFA77A, 0x8FD3FF, 0xFFD27A, 0x9BE89B, 0xC8C8FF};

/* ------------------------------------------------------------------ drawing helpers */
static UiCanvas cv;
static float S;                   /* output pixels per game pixel */
static float OX;                  /* game x 0 in the view, game pixels */
static float fade = 1.0f;         /* the game's own fades */

static float SX(float gx) { return (gx + OX) * S; }
static float SY(float gy) { return gy * S; }
static int AL(float a) { a *= fade; return a <= 0 ? 0 : a >= 255 ? 255 : (int)a; }

static void panel(float x0, float y0, float x1, float y1, float alpha) {
    float r = 4.0f * S;
    ui_round_rect_v(&cv, SX(x0), SY(y0), SX(x1), SY(y1), r, 0x1B2756, 0x0C1230, AL(228 * alpha));
    ui_round_frame(&cv, SX(x0) - 1, SY(y0) - 1, SX(x1) + 1, SY(y1) + 1, r + 1, fmaxf(1.0f, S * 0.4f),
                   0x6F8FE0, AL(120 * alpha));
}

static void pill(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom, float alpha) {
    ui_round_rect_v(&cv, SX(x0), SY(y0), SX(x1), SY(y1), (y1 - y0) * S / 2, top, bottom, AL(255 * alpha));
}

static float text(float gx, float gbase, float cap, const char *s, uint32_t c, float alpha) {
    return ui_text(&cv, SX(gx), SY(gbase), cap * S, s, c, AL(255 * alpha)) / S;
}
static void text_right(float gx, float gbase, float cap, const char *s, uint32_t c, float alpha) {
    ui_text_right(&cv, SX(gx), SY(gbase), cap * S, s, c, AL(255 * alpha));
}
static void text_center(float gx, float gbase, float cap, const char *s, uint32_t c, float alpha) {
    ui_text_center(&cv, SX(gx), SY(gbase), cap * S, s, c, AL(255 * alpha));
}
static float width(const char *s, float cap) { return ui_text_width(s, cap * S) / S; }

/* "AGILITY is UP!" -> "Agility is Up!": all-capital words in title case, the
 * way names read elsewhere in the port (HP, TP, LV and EXP stay) */
static void soften(char *s) {
    for (char *p = s; *p;) {
        char *q = p;
        int letters = 0, caps = 1;
        while (*q && *q != ' ') {
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
 * window, which hid the edge. The party bar sits on a dock across the whole
 * view from just above that line, so the party stands behind it as in the
 * original, and a soft shadow above the dock lets them fade into it. */
#define DOCK_Y    165.0f
#define HUD_Y0    171.0f
#define HUD_Y1    219.0f
/* while commands are chosen the dock opens a strip for them under the party
 * (so no menu covers anyone) and the cards move down to make room */
#define STRIP_Y   167.5f
#define STRIP_H   11.0f
#define HUD_Y0_STRIP 180.0f
#define HUD_Y1_STRIP 222.0f
static float strip_in;            /* 0..1, eased */

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
    static float glow[AURA_W * AURA_H], rim[AURA_W * AURA_H], tmp[AURA_W * AURA_H];
    static uint8_t occ[AURA_W * AURA_H];
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
    float a = strength * fade;
    for (int Y = Y0; Y < Y1; Y++) {
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
typedef struct { float x0, x1; int id; } Card;
static Card cards[5];
static int ncards;

/* the cards in the order the party stands, left to right, as the game's
 * status bar has them (the party slots can be in another order) */
static void card_layout(int view_w) {
    int ids[5], xs[5], n = 0;
    for (int k = 0; k < 5; k++) {
        int id = fighter_char(k);
        if (id < 0) continue;
        int x = (int)r16(FIGHTERS + (unsigned)k * 0x40 + 0x0C), j = n++;
        while (j > 0 && xs[j - 1] > x) { ids[j] = ids[j - 1]; xs[j] = xs[j - 1]; j--; }
        ids[j] = id;
        xs[j] = x;
    }
    float left = -OX + 6.0f, right = (float)view_w - OX - 6.0f, gap = 4.0f;
    float cw = (right - left - gap * (float)(n - 1)) / (float)(n ? n : 1);
    if (cw > 118.0f) cw = 118.0f;
    float total = (float)n * cw + (float)(n - 1) * gap, x = (left + right) / 2.0f - total / 2.0f;
    for (int k = 0; k < n; k++) {
        cards[k].x0 = x + (float)k * (cw + gap);
        cards[k].x1 = cards[k].x0 + cw;
        cards[k].id = ids[k];
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
    ui_round_rect_v(&cv, x0, top, x1, (float)cv.y1, 0, 0x131C3C, 0x070A18, AL(244 * a));
    ui_round_rect(&cv, x0, top, x1, top + fmaxf(1.0f, 0.5f * S), 0, 0x6F8FE0, AL(140 * a));
}

static void draw_hud(int acting, int target_id, int show_commands, int frame, float a) {
    draw_dock(a);
    for (int k = 0; k < ncards; k++) {
        int id = cards[k].id;
        unsigned base = CHAR_STATS + (unsigned)id * 0x80;
        unsigned hp = r16(base + 0x0E), mhp = r16(base + 0x10), tp = r16(base + 0x12), mtp = r16(base + 0x14);
        unsigned status = r8(base + 0x16);
        int dead = (status & 0x44) != 0, me = id == acting, aimed = id == target_id;
        float lift = me ? 2.0f * (1.0f - strip_in) : 0.0f;
        float x0 = cards[k].x0, x1 = cards[k].x1;
        float y0 = HUD_Y0 + (HUD_Y0_STRIP - HUD_Y0) * strip_in - lift, y1 = HUD_Y1 + (HUD_Y1_STRIP - HUD_Y1) * strip_in - lift;
        float cw = x1 - x0;
        /* card */
        if (me || aimed) {
            float pulse = aimed ? 0.8f + 0.2f * sinf((float)frame * 0.25f) : 1.0f;
            uint32_t c = aimed ? 0x7FE0FF : 0xF2C84B;
            ui_round_frame(&cv, SX(x0) - 1.25f * S, SY(y0) - 1.25f * S, SX(x1) + 1.25f * S, SY(y1) + 1.25f * S,
                           5.25f * S, 1.6f * S, c, AL(200 * pulse * a));
        }
        panel(x0, y0, x1, y1, a);
        /* portrait: a whole multiple of its 48 pixels where that fits */
        float avail = (y1 - y0 - 6.0f) * S;
        if (cw < 70.0f) avail = fminf(avail, cw * 0.42f * S);
        float ps = avail >= 48.0f ? floorf(avail / 48.0f) * 48.0f : avail;
        float px = SX(x0) + 3.0f * S, py = SY(y0) + 3.0f * S;
        const UiImage *im = ui_portrait(id);
        if (im) {
            ui_round_rect(&cv, px, py, px + ps, py + ps, ps * 0.16f, 0x101830, AL(255 * a));
            ui_image(&cv, im, px, py, ps, dead, AL(255 * a));
        }
        float tx = x0 + 3.0f + ps / S + 4.0f, tr = x1 - 4.0f;
        if (tr - tx < 20.0f) tx = x0 + 4.0f;
        float name_cap = cw >= 90.0f ? 5.4f : 4.6f;
        text(tx, y0 + 9.5f, name_cap, UI_CHAR_NAMES[id], dead ? 0x8890A8 : 0xFFFFFF, a);
        /* what they will do, or how they are */
        char tag[32] = "";
        uint32_t tc = 0x9FB4E8;
        if (dead) { snprintf(tag, sizeof tag, "Down"); tc = 0xFF6B6B; }
        else if (status & 0x01) { snprintf(tag, sizeof tag, "Poisoned"); tc = 0xC98CFF; }
        else if (status & 0x02) { snprintf(tag, sizeof tag, "Paralyzed"); tc = 0xFFD65C; }
        else if (status & 0x28) { snprintf(tag, sizeof tag, "Asleep"); tc = 0x8CD3FF; }
        if (!tag[0] && show_commands)
            for (int f = 0; f < 5; f++)
                if (fighter_char(f) == id) command_tag(f, tag, sizeof tag, &tc);
        if (tag[0]) text(tx, y0 + 16.5f, 3.7f, tag, tc, a);
        /* HP and TP */
        char nums[24];
        float lab = 3.6f, bh = 2.2f;
        float frac = mhp ? (float)hp / (float)mhp : 0.0f;
        uint32_t hc = frac > 0.5f ? 0x4CD07D : frac > 0.25f ? 0xF2C84B : 0xF0524F;
        snprintf(nums, sizeof nums, "%u/%u", hp, mhp);
        if (tr - tx > 34.0f) text(tx, y0 + 26.0f, lab, "HP", 0x9FB4E8, a);
        text_right(tr, y0 + 26.0f, lab, nums, 0xFFFFFF, a);
        ui_bar(&cv, SX(tx), SY(y0 + 27.6f), SX(tr), bh * S, frac, hc, AL(255 * a));
        if (mtp) {
            snprintf(nums, sizeof nums, "%u/%u", tp, mtp);
            if (tr - tx > 34.0f) text(tx, y0 + 36.5f, lab, "TP", 0x9FB4E8, a);
            text_right(tr, y0 + 36.5f, lab, nums, 0xFFFFFF, a);
            ui_bar(&cv, SX(tx), SY(y0 + 38.1f), SX(tr), bh * S, (float)tp / (float)mtp, 0x47A6F2, AL(255 * a));
        }
    }
}

/* ------------------------------------------------------------------ menus */
static float menu_alpha;          /* fades a menu in */
static float actor_sx = 160.0f;   /* where the character choosing stands on screen, game x */

/* a highlight eased towards the cursor: one for the open menu, one for the
 * command strip (which stays up under a list) */
typedef struct { float x, y, w; int snap; } Glide;
static Glide hl_menu = {0, 0, 0, 1}, hl_strip = {0, 0, 0, 1};

static void highlight_on(Glide *g, float x0, float y0, float x1, float y1, float alpha) {
    if (g->snap) { g->x = x0; g->y = y0; g->w = x1 - x0; g->snap = 0; }
    g->x += (x0 - g->x) * 0.45f;
    g->y += (y0 - g->y) * 0.45f;
    g->w += (x1 - x0 - g->w) * 0.45f;
    pill(g->x, g->y, g->x + g->w, g->y + (y1 - y0), 0xFFE08A, 0xF0A83C, alpha);
}
static void highlight(float x0, float y0, float x1, float y1, float alpha) {
    highlight_on(&hl_menu, x0, y0, x1, y1, alpha);
}

static void draw_main(float alpha) {
    static const char *const label[3] = {"Command", "Macro", "Run"};
    static const char *const help[3] = {"Choose what each member does", "Repeat a saved plan", "Try to escape"};
    int cur = (int)r16(MAIN_OPTION);
    float x0 = 10.0f, y0 = 34.0f, row = 17.0f, x1 = 124.0f, y1 = y0 + 6.0f + 3.0f * row;
    panel(x0, y0, x1, y1, alpha);
    for (int i = 0; i < 3; i++) {
        float ry = y0 + 3.0f + (float)i * row;
        if (i == cur) highlight(x0 + 3.0f, ry, x1 - 3.0f, ry + row, alpha);
        int on = i == cur;
        text(x0 + 9.0f, ry + 8.0f, 5.0f, label[i], on ? 0x1A1406 : 0xFFFFFF, alpha);
        text(x0 + 9.0f, ry + 14.0f, 3.4f, help[i], on ? 0x3A2A08 : 0x9FB4E8, alpha);
    }
}

/* where the party stands (game x, y), from the members drawn this frame,
 * with a margin for the depth camera's drift: menus are placed by it and
 * stay put while the camera moves; 0 if none */
static float party_l, party_r, party_top;
static int party_span(void) {
    int any = 0;
    party_l = 1e9f; party_r = -1e9f; party_top = 1e9f;
    for (int k = 0; k < 5; k++) {
        const Member *m = &members[k];
        if (!m->valid || !(restore >> k & 1)) continue;
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

/* the commands, in the strip the dock opens under the party, centred under
 * the member choosing; what the highlighted one does beside them. `chosen`
 * >= 0: one is picked (a list or a target is open), the rest dim */
static void draw_commands(int fighter, int chosen, const char *prompt, float alpha) {
    int cur = chosen >= 0 ? chosen : (int)r16(COMD_INDEX + (unsigned)fighter * 2) % 5;
    float cap = 4.4f, padx = 5.0f, gap = 2.0f, h = STRIP_H, y0 = STRIP_Y;
    float wsum = 0, w[5];
    for (int i = 0; i < 5; i++) { w[i] = width(COMMANDS[i], cap) + 2 * padx; wsum += w[i]; }
    wsum += 4 * gap;
    float lo = -OX + 4.0f, hi = (float)(cv.x1 / S) - OX - 4.0f;
    float x = actor_sx - wsum / 2.0f;
    if (x < lo + 2.0f) x = lo + 2.0f;
    if (x + wsum > hi - 2.0f) x = hi - 2.0f - wsum;
    ui_round_rect_v(&cv, SX(x - 2.0f), SY(y0 - 0.5f), SX(x + wsum + 2.0f), SY(y0 + h + 0.5f), (h + 1.0f) * S / 2,
                    0x26346A, 0x18214A, AL(235 * alpha));
    ui_round_frame(&cv, SX(x - 2.0f) - 1, SY(y0 - 0.5f) - 1, SX(x + wsum + 2.0f) + 1, SY(y0 + h + 0.5f) + 1,
                   (h + 1.0f) * S / 2 + 1, fmaxf(1.0f, S * 0.4f), 0x6F8FE0, AL(110 * alpha));
    float bx = x;
    for (int i = 0; i < 5; i++) {
        int on = i == cur;
        if (on) highlight_on(&hl_strip, bx, y0 + 0.5f, bx + w[i], y0 + h - 0.5f, alpha);
        text_center(bx + w[i] / 2.0f, y0 + 7.6f, cap, COMMANDS[i],
                    on ? 0x1A1406 : chosen >= 0 ? 0x6F7FA8 : 0xFFFFFF, alpha);
        bx += w[i] + gap;
    }
    /* what it does (or what to pick), on the side with more room: one line,
     * or two smaller ones */
    const char *line = prompt ? prompt : COMMAND_HELP[cur];
    uint32_t col = prompt ? 0xFFE08A : 0xD8E2FF;
    float room_l = x - 6.0f - lo, room_r = hi - (x + wsum + 6.0f);
    int right = room_r >= room_l;
    float room = right ? room_r : room_l;
    float one = width(line, 3.8f);
    if (one <= room) {
        float lx = right ? x + wsum + 6.0f : x - 6.0f - one;
        text(lx, y0 + 7.4f, 3.8f, line, col, alpha);
    } else if (room >= 36.0f) {
        float lw = fminf(room, one * 0.62f + 8.0f), lx = right ? x + wsum + 6.0f : x - 6.0f - lw;
        ui_text_wrap(&cv, SX(lx), SY(y0 + 4.6f), lw * S, 4.6f * S, 2, 3.4f * S, line, col, AL(255 * alpha));
    }
}

typedef struct { char name[24]; char cost[12]; int usable; const char *desc; } Entry;

/* a list in pages of four: the pages side by side, as many as fit */
static void draw_list(const char *title, const char *info, const Entry *e, int count, int cur, float alpha) {
    int pages = (count + 3) / 4;
    if (pages < 1) pages = 1;
    int cols = pages > 3 ? 3 : pages;
    int page = cur / 4, first = page - cols / 2;
    if (first > pages - cols) first = pages - cols;
    if (first < 0) first = 0;
    float colw = 66.0f, row = 10.5f, cap = 4.6f;
    float w = 10.0f + (float)cols * colw, h = 14.0f + 4.0f * row + 18.0f;
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
    float y0 = y1 - h, x1 = x0 + w;
    panel(x0, y0, x1, y1, alpha);
    text(x0 + 6.0f, y0 + 9.0f, 4.2f, title, 0xFFE08A, alpha);
    if (info && *info) text_right(x1 - 6.0f, y0 + 9.0f, 3.8f, info, 0x9FB4E8, alpha);
    if (first > 0) ui_triangle(&cv, SX(x0 + 3.0f), SY(y0 + 14.0f + 2.0f * row), 4.0f * S, 3, 0xFFE08A, AL(220 * alpha));
    if (first + cols < pages)
        ui_triangle(&cv, SX(x1 - 3.0f), SY(y0 + 14.0f + 2.0f * row), 4.0f * S, 2, 0xFFE08A, AL(220 * alpha));
    for (int c = 0; c < cols; c++)
        for (int r = 0; r < 4; r++) {
            int i = (first + c) * 4 + r;
            float ex = x0 + 5.0f + (float)c * colw, ey = y0 + 13.0f + (float)r * row;
            int on = i == cur;
            if (on) highlight(ex, ey, ex + colw - 2.0f, ey + row - 1.0f, alpha);
            if (i >= count || !e[i].name[0]) continue;
            uint32_t col = on ? 0x1A1406 : e[i].usable ? 0xFFFFFF : 0x6F7FA8;
            text(ex + 4.0f, ey + 7.0f, cap, e[i].name, col, alpha);
            if (e[i].cost[0]) text_right(ex + colw - 6.0f, ey + 7.0f, 3.8f, e[i].cost, on ? 0x3A2A08 : 0x9FB4E8, alpha);
        }
    /* what the highlighted one does */
    const char *d = cur >= 0 && cur < count ? e[cur].desc : NULL;
    if (d && *d)
        ui_text_wrap(&cv, SX(x0 + 6.0f), SY(y1 - 10.0f), (w - 12.0f) * S, 5.2f * S, 2, 3.7f * S, d, 0xD8E2FF,
                     AL(255 * alpha));
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
    draw_list("Techniques", info, entries, n, (int)r16(TECH_INDEX + (unsigned)fighter * 2) & 15, alpha);
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
    draw_list("Skills", "uses left", entries, n, (int)r16(SKILL_INDEX + (unsigned)fighter * 2) & 7, alpha);
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
    draw_list("Items", info, entries, n, cur, alpha);
}

/* a chevron over what the game's cursor pointed at */
static void draw_target(const char *label, int party, int frame, float alpha) {
    for (int i = 0; i < npointers; i++) {
        const Pointer *p = &pointers[i];
        if (!p->down) continue;
        float gx = (float)p->x + 8.0f, gy = (float)p->y + 10.0f, sx, sy;
        depth_to_screen(party, gx + OX, gy, &sx, &sy);
        sx -= OX;
        float bob = 1.5f * sinf((float)frame * 0.2f);
        ui_triangle(&cv, SX(sx), SY(sy + bob), 9.0f * S, 0, 0x101830, AL(200 * alpha));
        ui_triangle(&cv, SX(sx), SY(sy + bob - 0.6f), 7.0f * S, 0, 0xFFE08A, AL(255 * alpha));
        if (label && *label) {
            float lw = width(label, 4.4f) + 10.0f, ly = sy - 18.0f + bob;
            if (ly < 2.0f) ly = 2.0f;
            panel(sx - lw / 2.0f, ly, sx + lw / 2.0f, ly + 10.0f, alpha);
            text_center(sx, ly + 7.3f, 4.4f, label, 0xFFFFFF, alpha);
        }
        break;
    }
}

/* any other window: its text, read back from the tiles, in the same style */
static void draw_window(const Win *w, int names, float alpha) {
    float x0 = (float)w->x * 8.0f + 2.0f, y0 = (float)w->y * 8.0f + 2.0f;
    float x1 = (float)(w->x + w->w) * 8.0f - 2.0f, y1 = (float)(w->y + w->h) * 8.0f - 2.0f;
    int any = 0;
    for (int r = 0; r < w->h - 2 && !any; r++)
        for (int c = 0; c < w->w - 2; c++) if (w->ch[r][c]) { any = 1; break; }
    panel(x0, y0, x1, y1, alpha * (any ? 1.0f : 0.6f));
    for (int r = 0; r < w->h - 2; r++) {
        float base = (float)(w->y + 1 + r) * 8.0f + 6.6f;
        if (r == w->cursor) highlight(x0 + 2.0f, base - 6.8f, x1 - 2.0f, base + 1.8f, alpha);
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
            uint32_t col = r == w->cursor ? 0x1A1406 : w->dim[r][s] ? 0x7F8FB8 : 0xFFFFFF;
            float gx = (float)(w->x + 1 + s) * 8.0f;
            if (digits) text_right((float)(w->x + 2 + e) * 8.0f - 1.0f, base, 5.0f, buf, col, alpha);
            else text(gx + 0.5f, base, 5.0f, buf, col, alpha);
        }
    }
}

/* ------------------------------------------------------------------ the frame */
void battleui_draw(uint32_t *out, int out_stride, int scale, int view_w, int h) {
    static int frame, last_key = -1;
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
    if (key != last_key) {
        if (key % 8 != last_key % 8 || last_key < 0) hl_strip.snap = 1;        /* another member */
        menu_alpha = 0; hl_menu.snap = 1; last_key = key;
    }
    menu_alpha = fminf(1.0f, menu_alpha + 0.25f);

    card_layout(view_w);
    int target_id = -1;
    if (menu == M_TARGET_ALLY) {            /* the game counts the party left to right, like the cards */
        int t = (int)r16(CHAR_INDEX);
        if (t >= 0 && t < ncards) target_id = cards[t].id;
    }

    /* the party bar fades in once the game has set up the fighters */
    static float hud_in;
    hud_in = ncards ? fminf(1.0f, hud_in + 0.1f) : 0.0f;

    /* the aura: gold on the member choosing, cyan on an ally being targeted */
    {
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

    /* the commands chosen this round, on the cards: not over the results */
    int tags = !((r2 >= 0x1E && r2 <= 0x26) || r2 == 0x2D || r2 == 0x2F);
    strip_in += ((acting >= 0 && choosing(menu) ? 1.0f : 0.0f) - strip_in) * 0.22f;
    if (ncards) draw_hud(acting, target_id, tags, frame, hud_in);

    /* other windows: enemy names, messages, results */
    for (int i = 0; i < nwins; i++) {
        const Win *w = &wins[i];
        if (w->y >= 20 && w->w >= 30) continue;                  /* the status bar: the party bar instead */
        if (menu != M_NONE && w->y >= 4) continue;               /* a menu: drawn below */
        draw_window(w, w->y < 4, 1.0f);
    }

    /* the commands, in the dock's strip for as long as they are chosen; a
     * list or a target over it, the command that opened it highlighted */
    if (acting >= 0) actor_sx = (float)r16(FIGHTERS + (unsigned)fighter * 0x40 + 0x0C) - 128.0f;
    int chosen = -1;
    const char *prompt = NULL;
    char pbuf[48];
    unsigned cmd = r8(COMMAND_DATA + (unsigned)(fighter < 5 ? fighter : 0) * 4);
    switch (menu) {
    case M_TECH: chosen = 1; prompt = "Choose a technique"; break;
    case M_SKILL: chosen = 2; prompt = "Choose a skill"; break;
    case M_ITEM: chosen = 3; prompt = "Choose an item"; break;
    case M_TARGET_ENEMY: case M_TARGET_ALLY:
        chosen = cmd >= 1 && cmd <= 5 ? (int)cmd - 1 : 0;
        prompt = "Choose a target";
        if (menu == M_TARGET_ALLY && target_id >= 0) {
            snprintf(pbuf, sizeof pbuf, "Target: %s", UI_CHAR_NAMES[target_id]);
            prompt = pbuf;
        }
        break;
    default: break;
    }
    if (acting >= 0 && choosing(menu) && strip_in > 0.01f) draw_commands(fighter, chosen, prompt, strip_in * hud_in);

    switch (menu) {
    case M_MAIN: draw_main(menu_alpha); break;
    case M_TECH: draw_techs(fighter, acting, menu_alpha); break;
    case M_SKILL: draw_skills(fighter, menu_alpha); break;
    case M_ITEM: draw_items(fighter, acting, menu_alpha); break;
    case M_TARGET_ENEMY: {
        char name[32] = "";
        int f = nth_fighter(5, 10, (int)r16(ENEMY_INDEX));
        if (f >= 0) {
            uint32_t st = ui_ram32(FIGHTERS + (unsigned)f * 0x40 + 4) & 0xFFFF;
            if (st >= ENEMY_STATS && st < ENEMY_STATS + 0x200)
                ui_decode_name(&m68k_ram[st], 14, name, (int)sizeof name);
        }
        draw_target(name, 0, frame, menu_alpha);
        break;
    }
    default: break;
    }
}
