/*
 * Widescreen side panels: see panels.h.
 */
#include "panels.h"
#include "hdtext.h"
#include "m68k.h"
#include "romart.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE_INDEX   0xEF00u
#define PARTY_SLOTS       0xF40Au     /* Current_Party_Slots: 5 character IDs, negative = end */
#define INVENTORY         0xF410u     /* 40 item IDs, 0 = empty */
#define MONEY             0xF438u     /* Current_Money, long */
#define CHAR_STATS        0xF500u     /* Character_Stats + id * $80 */
#define LEVEL_TABLES      0x4074u     /* CharLevelTablePtrs (ROM): per id, initial level + table */
#define ITEM_NAMES        0x2AAFB4u   /* InventoryNames (ROM): $FE-terminated, window charset */
#define ITEM_NAMES_END    0x2ABA70u

static const char *NAMES[11] = {"Chaz", "Alys", "Hahn", "Rune", "Gryz", "Rika", "Demi", "Wren", "Raja", "Kyra", "Seth"};

typedef struct { int w, h; uint8_t *rgba; } Image;
static Image portraits[11];
static char item_names[256][24];
static int items_ready;

static unsigned ram8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned ram16(unsigned a) { return ram8(a) << 8 | ram8(a + 1); }
static uint32_t ram32(unsigned a) { return (uint32_t)ram16(a) << 16 | ram16(a + 2); }
static uint32_t rom32(uint32_t a) {
    return a + 3 < m68k_rom_size ? (uint32_t)m68k_rom[a] << 24 | (uint32_t)m68k_rom[a + 1] << 16 |
                                   (uint32_t)m68k_rom[a + 2] << 8 | m68k_rom[a + 3] : 0;
}
static unsigned rom16(uint32_t a) { return a + 1 < m68k_rom_size ? (unsigned)(m68k_rom[a] << 8 | m68k_rom[a + 1]) : 0; }

/* the party's dialogue portraits, from the player's ROM */
int panels_init(void) {
    int n = 0;
    for (int i = 0; i < 11; i++) {
        if (portraits[i].rgba) { n++; continue; }
        uint8_t *px = malloc(ROMART_PORTRAIT * ROMART_PORTRAIT * 4);
        if (px && romart_portrait(i, px)) {
            portraits[i].w = portraits[i].h = ROMART_PORTRAIT;
            portraits[i].rgba = px;
            n++;
        } else {
            free(px);
        }
    }
    return n;
}

/* item names from the ROM, decoded from the window charset and title-cased */
static void load_item_names(void) {
    if (items_ready || !m68k_rom || m68k_rom_size < ITEM_NAMES_END) return;
    uint32_t a = ITEM_NAMES;
    for (int id = 1; id < 256 && a < ITEM_NAMES_END; id++) {
        char *s = item_names[id];
        int n = 0, up = 1;
        while (a < ITEM_NAMES_END && m68k_rom[a] != 0xFE) {
            unsigned c = m68k_rom[a++];
            char ch = c >= 1 && c <= 26 ? (char)('A' + c - 1) : c >= 57 && c <= 82 ? (char)('a' + c - 57)
                    : c >= 27 && c <= 36 ? (char)('0' + c - 27) : c == 0x31 ? '-' : c == 0x32 ? '!'
                    : c == 0x33 ? '?' : c == 0x34 ? ':' : c == 0x53 ? '.' : c == 0x55 ? ',' : ' ';
            if (ch >= 'A' && ch <= 'Z' && !up) ch = (char)(ch - 'A' + 'a');   /* "HUNT-KNIFE" -> "Hunt-Knife" */
            up = ch == ' ' || ch == '-';
            if (n < (int)sizeof item_names[0] - 1) s[n++] = ch;
        }
        s[n] = 0;
        a++;                                                     /* past $FE */
    }
    items_ready = 1;
}

int panels_field(void) {
    int mode = (int)ram16(GAME_MODE_INDEX) / 4;
    return mode == 2 || mode == 3;                               /* LoadFieldMap, Field */
}

/* ------------------------------------------------------------------ drawing */
typedef struct { uint32_t *px; int stride, x0, y0, x1, y1; } Canvas;   /* clip box */

static void blend(uint32_t *d, uint32_t c, int a) {
    if (a <= 0) return;
    if (a >= 255) { *d = 0xFF000000u | (c & 0xFFFFFF); return; }
    int r = (int)(*d >> 16 & 255), g = (int)(*d >> 8 & 255), b = (int)(*d & 255);
    r += ((int)(c >> 16 & 255) - r) * a / 255;
    g += ((int)(c >> 8 & 255) - g) * a / 255;
    b += ((int)(c & 255) - b) * a / 255;
    *d = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* a rounded rectangle, anti-aliased corners */
static void round_rect(const Canvas *cv, float x0, float y0, float x1, float y1, float r, uint32_t c, int alpha) {
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < cv->x0) ix0 = cv->x0;
    if (iy0 < cv->y0) iy0 = cv->y0;
    if (ix1 > cv->x1) ix1 = cv->x1;
    if (iy1 > cv->y1) iy1 = cv->y1;
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float dx = px < x0 + r ? x0 + r - px : px > x1 - r ? px - (x1 - r) : 0;
            float dy = py < y0 + r ? y0 + r - py : py > y1 - r ? py - (y1 - r) : 0;
            float cover = 1.0f;
            if (dx > 0 && dy > 0) cover = r + 0.5f - sqrtf(dx * dx + dy * dy);
            else if (px < x0 || px > x1 || py < y0 || py > y1) cover = 0;
            if (cover <= 0) continue;
            if (cover > 1) cover = 1;
            blend(&cv->px[(size_t)y * (size_t)cv->stride + (size_t)x], c, (int)((float)alpha * cover));
        }
}

/* a portrait, scaled (bilinear) into a rounded square; dead = grey and dim */
static void draw_image(const Canvas *cv, const Image *im, float x0, float y0, float size, int dead) {
    int ix0 = (int)x0, iy0 = (int)y0, n = (int)size;
    float r = size * 0.16f;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int X = ix0 + x, Y = iy0 + y;
            if (X < cv->x0 || X >= cv->x1 || Y < cv->y0 || Y >= cv->y1) continue;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float dx = px < r ? r - px : px > size - r ? px - (size - r) : 0;
            float dy = py < r ? r - py : py > size - r ? py - (size - r) : 0;
            float cover = dx > 0 && dy > 0 ? r + 0.5f - sqrtf(dx * dx + dy * dy) : 1.0f;
            if (cover <= 0) continue;
            int uu = (int)(px / size * (float)im->w), vv = (int)(py / size * (float)im->h);   /* pixel art: nearest */
            uu = uu >= im->w ? im->w - 1 : uu;
            vv = vv >= im->h ? im->h - 1 : vv;
            const uint8_t *p = &im->rgba[((size_t)vv * (size_t)im->w + (size_t)uu) * 4];
            float acc[4] = {(float)p[0], (float)p[1], (float)p[2], (float)p[3] / 255.0f};
            if (acc[3] <= 0.001f) continue;
            float rr = acc[0], gg = acc[1], bb = acc[2];
            if (dead) { float l = (rr * 0.3f + gg * 0.59f + bb * 0.11f) * 0.55f; rr = gg = bb = l; }
            uint32_t c = (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
            blend(&cv->px[(size_t)Y * (size_t)cv->stride + (size_t)X], c, (int)(acc[3] * (cover > 1 ? 1 : cover) * 255.0f));
        }
}

static void text(const Canvas *cv, float x, float base, float cap, const char *s, uint32_t c) {
    hdtext_string(cv->px, cv->stride, cv->x0, cv->y0, cv->x1, cv->y1, x, base, cap, s, c);
}
static void text_right(const Canvas *cv, float x_right, float base, float cap, const char *s, uint32_t c) {
    text(cv, x_right - hdtext_string_width(s, cap), base, cap, s, c);
}

static void bar(const Canvas *cv, float x0, float y0, float x1, float h, float frac, uint32_t c) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    round_rect(cv, x0, y0, x1, y0 + h, h / 2, 0x05080F, 230);
    if (frac > 0) round_rect(cv, x0, y0, x0 + (x1 - x0) * frac, y0 + h, h / 2, c, 255);
}

static void panel_background(const Canvas *cv, int inner_x) {
    for (int y = cv->y0; y < cv->y1; y++) {
        float t = (float)(y - cv->y0) / (float)(cv->y1 - cv->y0);
        uint32_t c = (uint32_t)(14 - 7 * t) << 16 | (uint32_t)(22 - 11 * t) << 8 | (uint32_t)(48 - 24 * t);
        for (int x = cv->x0; x < cv->x1; x++) cv->px[(size_t)y * (size_t)cv->stride + (size_t)x] = 0xFF000000u | c;
    }
    for (int y = cv->y0; y < cv->y1; y++)                       /* a line along the game view */
        blend(&cv->px[(size_t)y * (size_t)cv->stride + (size_t)inner_x], 0x3A5BB0, 150);
}

static void money_string(uint32_t v, char *out, size_t n) {   /* 12,345 */
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%lu", (unsigned long)v);
    size_t len = strlen(tmp), o = 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        out[o++] = tmp[i];
        if ((len - i - 1) % 3 == 0 && i + 1 < len) out[o++] = ',';
    }
    out[o] = 0;
}

void panels_draw(uint32_t *out, int stride, int S, int margin, int view_w, int h) {
    load_item_names();
    float P = (float)(margin * S), H = (float)(h * S), pad = P * 0.07f;
    Canvas left = {out, stride, 0, 0, margin * S, h * S};
    Canvas right = {out, stride, (margin + view_w) * S, 0, (2 * margin + view_w) * S, h * S};
    panel_background(&left, left.x1 - 1);
    panel_background(&right, right.x0);

    /* ---- left: the party */
    int ids[5], n = 0;
    for (int i = 0; i < 5; i++) {
        unsigned id = ram8(PARTY_SLOTS + (unsigned)i);
        if (id & 0x80) break;
        if (id < 11) ids[n++] = (int)id;
    }
    {   /* PSIV_PANELS_PARTY=0,1,2,3,4: show these characters instead (layout testing) */
        const char *test = getenv("PSIV_PANELS_PARTY");
        if (test && *test) {
            n = 0;
            for (const char *t = test; *t && n < 5;) {
                ids[n++] = atoi(t) % 11;
                while (*t && *t != ',') t++;
                if (*t == ',') t++;
            }
        }
    }
    /* cards are sized for a full party of five and the stack is centred, so
     * a smaller party sits in the middle with space above and below */
    float gap = pad * 0.6f;
    float card_h = (H - 2 * pad - 4 * gap) / 5.0f;
    float top = (H - ((float)n * card_h + (float)(n > 0 ? n - 1 : 0) * gap)) / 2.0f;
    for (int k = 0; k < n; k++) {
        unsigned base = CHAR_STATS + (unsigned)ids[k] * 0x80;
        unsigned level = ram16(base + 0x08), hp = ram16(base + 0x0E), mhp = ram16(base + 0x10);
        unsigned tp = ram16(base + 0x12), mtp = ram16(base + 0x14), status = ram8(base + 0x16);
        uint32_t exp = ram32(base + 0x0A);
        int dead = (status & 0x44) != 0;
        float cx0 = pad, cy0 = top + (float)k * (card_h + gap), cx1 = P - pad, cy1 = cy0 + card_h;
        round_rect(&left, cx0, cy0, cx1, cy1, P * 0.05f, 0x2E4C9A, 140);              /* border */
        round_rect(&left, cx0 + 1, cy0 + 1, cx1 - 1, cy1 - 1, P * 0.05f, 0x12204A, 235);
        /* the portrait at a whole multiple of its 48 pixels where it fits,
         * so the pixel art stays even; scaled down only in small windows */
        float ip = pad * 0.7f, room = fminf((cx1 - cx0) * 0.5f, card_h * 0.42f), ps;
        ps = room >= 96.0f ? 96.0f : room >= 46.0f ? 48.0f : room;
        float px = cx0 + ip, py = cy0 + ip;
        if (portraits[ids[k]].rgba) {
            round_rect(&left, px, py, px + ps, py + ps, ps * 0.16f, 0x1C2C5E, 255);
            draw_image(&left, &portraits[ids[k]], px, py, ps, dead);
        } else {                                                                     /* no portrait: initial */
            round_rect(&left, px, py, px + ps, py + ps, ps * 0.16f, 0x2A3F7A, 255);
            char ini[2] = {NAMES[ids[k]][0], 0};
            text(&left, px + ps * 0.3f, py + ps * 0.72f, ps * 0.45f, ini, 0xE8ECFF);
        }
        float tx = px + ps + ip, name_cap = fmaxf(ps * 0.27f, P * 0.1f);   /* text grows with the panel */
        text(&left, tx, py + name_cap * 1.35f, name_cap, NAMES[ids[k]], dead ? 0x8890A8 : 0xFFFFFF);
        char buf[48];
        snprintf(buf, sizeof buf, "Lv %u", level);
        text(&left, tx, py + name_cap * 2.9f, name_cap * 0.78f, buf, 0x9FB4E8);
        /* status tag */
        const char *st = NULL;
        uint32_t stc = 0;
        if (dead) { st = "Down"; stc = 0xFF6B6B; }
        else if (status & 0x01) { st = "Poison"; stc = 0xC98CFF; }
        else if (status & 0x02) { st = "Paralyzed"; stc = 0xFFD65C; }
        else if (status & 0x28) { st = "Asleep"; stc = 0x8CD3FF; }
        else if (status & 0x10) { st = "Sealed"; stc = 0xB8B8B8; }
        if (st) text(&left, tx, py + name_cap * 4.2f, name_cap * 0.72f, st, stc);
        /* bars: HP, TP, EXP to the next level */
        float bx0 = cx0 + ip, bx1 = cx1 - ip, lab = name_cap * 0.68f, bh = fmaxf(2.0f, card_h * 0.045f);
        float y = py + ps + ip * 0.9f;
        float row = (cy1 - ip - y) / 3.0f;                       /* three rows fill the rest of the card */
        char nums[32];
        snprintf(nums, sizeof nums, "%u/%u", hp, mhp);
        text(&left, bx0, y + lab * 1.25f, lab, "HP", 0x9FB4E8);
        text_right(&left, bx1, y + lab * 1.25f, lab, nums, 0xFFFFFF);
        float frac = mhp ? (float)hp / (float)mhp : 0;
        bar(&left, bx0, y + lab * 1.7f, bx1, bh, frac, frac > 0.5f ? 0x4CD07D : frac > 0.25f ? 0xF2C84B : 0xF0524F);
        y += row;
        snprintf(nums, sizeof nums, "%u/%u", tp, mtp);
        text(&left, bx0, y + lab * 1.25f, lab, "TP", 0x9FB4E8);
        text_right(&left, bx1, y + lab * 1.25f, lab, nums, 0xFFFFFF);
        bar(&left, bx0, y + lab * 1.7f, bx1, bh, mtp ? (float)tp / (float)mtp : 0, 0x47A6F2);
        y += row;
        unsigned init = rom16(LEVEL_TABLES + (unsigned)ids[k] * 6);
        uint32_t table = rom32(LEVEL_TABLES + (unsigned)ids[k] * 6 + 2);
        if (level >= 99 || !table || level < init) {
            text(&left, bx0, y + lab * 1.25f, lab, "EXP", 0x9FB4E8);
            text_right(&left, bx1, y + lab * 1.25f, lab, "max", 0xFFFFFF);
            bar(&left, bx0, y + lab * 1.7f, bx1, bh * 0.7f, 1.0f, 0xE8B33B);
        } else {
            uint32_t next = rom32(table + (level - init) * 22);
            uint32_t prev = level > init ? rom32(table + (level - init - 1) * 22) : 0;
            snprintf(nums, sizeof nums, "%lu to go", (unsigned long)(next > exp ? next - exp : 0));
            text(&left, bx0, y + lab * 1.25f, lab, "EXP", 0x9FB4E8);
            text_right(&left, bx1, y + lab * 1.25f, lab, nums, 0xFFFFFF);
            float f = next > prev ? (float)(exp > prev ? exp - prev : 0) / (float)(next - prev) : 1.0f;
            bar(&left, bx0, y + lab * 1.7f, bx1, bh * 0.7f, f, 0xE8B33B);
        }
    }

    /* ---- right: meseta and the inventory */
    float rx0 = right.x0 + pad, rx1 = right.x1 - pad, cap = P * 0.085f;
    float y = pad;
    round_rect(&right, rx0, y, rx1, y + cap * 4.6f, P * 0.05f, 0x2E4C9A, 140);
    round_rect(&right, rx0 + 1, y + 1, rx1 - 1, y + cap * 4.6f - 1, P * 0.05f, 0x12204A, 235);
    text(&right, rx0 + pad * 0.7f, y + cap * 1.75f, cap * 0.72f, "MESETA", 0x9FB4E8);
    char money[24];
    money_string(ram32(MONEY), money, sizeof money);
    text_right(&right, rx1 - pad * 0.7f, y + cap * 3.75f, cap * 1.25f, money, 0xFFE38A);
    y += cap * 4.6f + pad;
    int count = 0;
    for (int i = 0; i < 40; i++) count += ram8(INVENTORY + (unsigned)i) != 0;
    float list_top = y, list_bottom = H - pad;
    round_rect(&right, rx0, list_top, rx1, list_bottom, P * 0.05f, 0x2E4C9A, 140);
    round_rect(&right, rx0 + 1, list_top + 1, rx1 - 1, list_bottom - 1, P * 0.05f, 0x12204A, 235);
    char head[32];
    snprintf(head, sizeof head, "ITEMS  %d/40", count);
    text(&right, rx0 + pad * 0.7f, y + cap * 1.75f, cap * 0.72f, head, 0x9FB4E8);
    float line = cap * 1.75f, item_cap = cap * 0.82f;
    y += cap * 2.3f;
    int shown = 0;
    int fits = (int)((list_bottom - pad * 0.6f - y) / line);
    for (int i = 0; i < 40; i++) {
        unsigned id = ram8(INVENTORY + (unsigned)i);
        if (!id) continue;
        if (shown == fits - 1 && count - shown > 1) {            /* the last line says how many more */
            char more[32];
            snprintf(more, sizeof more, "+%d more", count - shown);
            text(&right, rx0 + pad * 0.7f, y + line * 0.8f, item_cap, more, 0x9FB4E8);
            break;
        }
        const char *nm = items_ready && item_names[id][0] ? item_names[id] : "?";
        text(&right, rx0 + pad * 0.7f, y + line * 0.8f, item_cap, nm, 0xF0F3FF);
        y += line;
        shown++;
        if (shown >= fits) break;
    }
}
