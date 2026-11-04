/*
 * Widescreen side panels: see panels.h.
 */
#include "panels.h"
#include "hdtext.h"
#include "uikit.h"

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

#define ram8  ui_ram8
#define ram16 ui_ram16
#define ram32 ui_ram32
#define rom16 ui_rom16
#define rom32 ui_rom32
#define NAMES UI_CHAR_NAMES

/* the party's dialogue portraits, from the player's ROM */
int panels_init(void) { return ui_portraits_init(); }

int panels_field(void) {
    int mode = (int)ram16(GAME_MODE_INDEX) / 4;
    return mode == 2 || mode == 3;                               /* LoadFieldMap, Field */
}

/* ------------------------------------------------------------------ drawing */
typedef UiCanvas Canvas;

static void round_rect(const Canvas *cv, float x0, float y0, float x1, float y1, float r, uint32_t c, int alpha) {
    ui_round_rect(cv, x0, y0, x1, y1, r, c, alpha);
}
static void text(const Canvas *cv, float x, float base, float cap, const char *s, uint32_t c) {
    ui_text(cv, x, base, cap, s, c, 255);
}
static void text_right(const Canvas *cv, float x_right, float base, float cap, const char *s, uint32_t c) {
    ui_text_right(cv, x_right, base, cap, s, c, 255);
}
static void bar(const Canvas *cv, float x0, float y0, float x1, float h, float frac, uint32_t c) {
    ui_bar(cv, x0, y0, x1, h, frac, c, 255);
}

static void panel_background(const Canvas *cv, int inner_x) {
    for (int y = cv->y0; y < cv->y1; y++) {
        float t = (float)(y - cv->y0) / (float)(cv->y1 - cv->y0);
        uint32_t c = (uint32_t)(14 - 7 * t) << 16 | (uint32_t)(22 - 11 * t) << 8 | (uint32_t)(48 - 24 * t);
        for (int x = cv->x0; x < cv->x1; x++) cv->px[(size_t)y * (size_t)cv->stride + (size_t)x] = 0xFF000000u | c;
    }
    for (int y = cv->y0; y < cv->y1; y++)                       /* a line along the game view */
        ui_blend(&cv->px[(size_t)y * (size_t)cv->stride + (size_t)inner_x], 0x3A5BB0, 150);
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
        if (ui_portrait(ids[k])) {
            round_rect(&left, px, py, px + ps, py + ps, ps * 0.16f, 0x1C2C5E, 255);
            ui_image(&left, ui_portrait(ids[k]), px, py, ps, dead, 255);
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
        const char *nm = ui_item_name((int)id)[0] ? ui_item_name((int)id) : "?";
        text(&right, rx0 + pad * 0.7f, y + line * 0.8f, item_cap, nm, 0xF0F3FF);
        y += line;
        shown++;
        if (shown >= fits) break;
    }
}
