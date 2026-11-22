/*
 * The port's UI drawing kit: see uikit.h.
 */
#include "uikit.h"
#include "hdtext.h"
#include "m68k.h"
#include "romart.h"
#include "par.h"
#include "scalex.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ITEM_NAMES      0x2AAFB4u   /* InventoryNames (US ROM): $FE-terminated, window charset */
#define ITEM_NAMES_END  0x2ABA70u
#define TECH_NAMES      0x2AB622u   /* TechniqueNames: 40, $FE-terminated */
#define SKILL_NAMES     0x2AB702u   /* SkillNames: 54 */
#define NAMES_END       0x2ABA70u   /* InventoryNames2 follows */

const char *const UI_CHAR_NAMES[11] = {"Chaz", "Alys", "Hahn", "Rune", "Gryz", "Rika", "Demi", "Wren", "Raja", "Kyra", "Seth"};

/* ------------------------------------------------------------------ memory */
unsigned ui_ram8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
unsigned ui_ram16(unsigned a) { return ui_ram8(a) << 8 | ui_ram8(a + 1); }
uint32_t ui_ram32(unsigned a) { return (uint32_t)ui_ram16(a) << 16 | ui_ram16(a + 2); }
unsigned ui_rom8(uint32_t a) { return m68k_rom && a < m68k_rom_size ? m68k_rom[a] : 0; }
unsigned ui_rom16(uint32_t a) { return ui_rom8(a) << 8 | ui_rom8(a + 1); }
uint32_t ui_rom32(uint32_t a) { return (uint32_t)ui_rom16(a) << 16 | ui_rom16(a + 2); }

/* ------------------------------------------------------------------ names */
void ui_decode_name(const uint8_t *src, int max, char *out, int out_size) {
    int n = 0, up = 1;
    for (int i = 0; i < max && src[i] != 0xFE && src[i] != 0xFF; i++) {
        unsigned c = src[i];
        char ch = c >= 1 && c <= 26 ? (char)('A' + c - 1) : c >= 57 && c <= 82 ? (char)('a' + c - 57)
                : c >= 27 && c <= 36 ? (char)('0' + c - 27) : c >= 37 && c <= 46 ? (char)('0' + c - 37)
                : c == 0x31 ? '-' : c == 0x32 ? '!' : c == 0x33 ? '?' : c == 0x34 ? ':' : c == 0x53 ? '.'
                : c == 0x54 ? '\'' : c == 0x55 ? ',' : ' ';
        if (ch >= 'A' && ch <= 'Z' && !up) ch = (char)(ch - 'A' + 'a');    /* "HUNT-KNIFE" -> "Hunt-Knife" */
        up = ch == ' ' || ch == '-' || ch == '.';
        if (n < out_size - 1) out[n++] = ch;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    int s = 0;
    while (s < n && out[s] == ' ') s++;
    memmove(out, out + s, (size_t)(n - s));
    out[n - s] = 0;
}

typedef struct { char name[24]; } Name;
static Name items[256], techs[41], skills[55];
static int names_ready;

static int load_table(uint32_t a, uint32_t end, Name *t, int count) {
    int id = 1;
    while (id < count && a < end && a < m68k_rom_size) {
        uint32_t s = a;
        while (a < end && a < m68k_rom_size && m68k_rom[a] != 0xFE) a++;
        ui_decode_name(&m68k_rom[s], (int)(a - s), t[id].name, (int)sizeof t[id].name);
        a++;
        id++;
    }
    return id;
}

static void load_names(void) {
    if (names_ready || !m68k_rom || m68k_rom_size < ITEM_NAMES_END) return;
    load_table(ITEM_NAMES, ITEM_NAMES_END, items, 256);
    load_table(TECH_NAMES, SKILL_NAMES, techs, 41);
    load_table(SKILL_NAMES, NAMES_END, skills, 55);
    names_ready = 1;
}

const char *ui_item_name(int id) { load_names(); return id > 0 && id < 256 ? items[id].name : ""; }
const char *ui_tech_name(int id) { load_names(); return id > 0 && id <= 40 ? techs[id].name : ""; }
const char *ui_skill_name(int id) { load_names(); return id > 0 && id <= 54 ? skills[id].name : ""; }

/* ------------------------------------------------------------------ portraits */
static UiImage portraits[11];

int ui_portraits_init(void) {
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

const UiImage *ui_portrait(int id) {
    return id >= 0 && id < 11 && portraits[id].rgba ? &portraits[id] : NULL;
}

/* the Status page's pictures: each made the first time it is asked for */
const UiImage *ui_profile(int id) {
    static UiImage profiles[11];
    static uint8_t tried[11];
    if (id < 0 || id >= 11) return NULL;
    if (!profiles[id].rgba && !tried[id]) {
        tried[id] = 1;
        uint8_t *px = malloc(ROMART_PROFILE * ROMART_PROFILE * 4);
        if (px && romart_profile(id, px)) {
            profiles[id].w = profiles[id].h = ROMART_PROFILE;
            profiles[id].rgba = px;
        } else {
            free(px);
        }
    }
    return profiles[id].rgba ? &profiles[id] : NULL;
}

const UiImage *ui_face(int id, float side) {
    const UiImage *fine = side >= UI_FACE_FINE ? ui_profile(id) : NULL;
    return fine ? fine : ui_portrait(id);
}

/* ------------------------------------------------------------------ shapes */
static int straight;                       /* ui_set_straight_alpha */
void ui_set_straight_alpha(int on) {
    straight = on;
    hdtext_set_straight_alpha(on);
}

void ui_blend(uint32_t *d, uint32_t c, int a) {
    if (a <= 0) return;
    if (straight) { *d = hdtext_over(*d, c, a); return; }
    if (a >= 255) { *d = 0xFF000000u | (c & 0xFFFFFF); return; }
    int r = (int)(*d >> 16 & 255), g = (int)(*d >> 8 & 255), b = (int)(*d & 255);
    r += ((int)(c >> 16 & 255) - r) * a / 255;
    g += ((int)(c >> 8 & 255) - g) * a / 255;
    b += ((int)(c & 255) - b) * a / 255;
    *d = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    int r = (int)((float)(a >> 16 & 255) + ((float)(b >> 16 & 255) - (float)(a >> 16 & 255)) * t);
    int g = (int)((float)(a >> 8 & 255) + ((float)(b >> 8 & 255) - (float)(a >> 8 & 255)) * t);
    int bl = (int)((float)(a & 255) + ((float)(b & 255) - (float)(a & 255)) * t);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}

/* the span a rounded rectangle covers on the row through py: [*l, *r], or 0 */
static int row_span(float x0, float y0, float x1, float y1, float r, float py, float *l, float *rr) {
    if (py < y0 || py > y1) return 0;
    float dy = py < y0 + r ? y0 + r - py : py > y1 - r ? py - (y1 - r) : 0;
    float inset = dy > 0 ? r - sqrtf(fmaxf(r * r - dy * dy, 0.0f)) : 0;
    *l = x0 + inset;
    *rr = x1 - inset;
    return *rr > *l;
}

/* blend [l, r] of row y: anti-aliased ends, `cov` for the whole row */
static void fill_span(const UiCanvas *cv, int y, float l, float r, uint32_t c, int alpha) {
    int a = alpha;
    if (a <= 0) return;
    int il = (int)floorf(l), ir = (int)ceilf(r);
    if (il < cv->x0) il = cv->x0;
    if (ir > cv->x1) ir = cv->x1;
    uint32_t *row = &cv->px[(size_t)y * (size_t)cv->stride];
    for (int x = il; x < ir; x++) {
        float px = (float)x + 0.5f, e = fminf(px - l, r - px) + 0.5f;
        if (e <= 0) continue;
        if (e >= 1) {                                          /* the inside of the span: no edge maths */
            ui_blend(&row[x], c, a);
            int x2 = (int)floorf(r - 0.5f);                    /* pixels before x2 are wholly inside */
            if (x2 > ir) x2 = ir;
            while (x + 1 < x2) ui_blend(&row[++x], c, a);
            continue;
        }
        ui_blend(&row[x], c, (int)((float)a * e));
    }
}

/* a box's rows (from its first, iy0): each row on its own, so a large box is
 * filled on several cores (par.h) */
typedef struct {
    const UiCanvas *cv;
    float x0, y0, x1, y1, r;
    uint32_t top, bottom;
    int alpha, iy0;
} BoxRows;

static void box_rows(void *ctx, int a, int b) {
    const BoxRows *q = ctx;
    float x0 = q->x0, y0 = q->y0, x1 = q->x1, y1 = q->y1;
    for (int y = q->iy0 + a; y < q->iy0 + b; y++) {
        float py = (float)y + 0.5f, l, rr;
        float vc = fminf(fminf(py - y0, y1 - py) + 0.5f, 1.0f);   /* top and bottom edges */
        if (vc <= 0 || !row_span(x0, y0, x1, y1, q->r, fminf(fmaxf(py, y0), y1), &l, &rr)) continue;
        uint32_t c = q->top == q->bottom ? q->top : mix(q->top, q->bottom, (py - y0) / (y1 - y0));
        fill_span(q->cv, y, l, rr, c, (int)((float)q->alpha * vc));
    }
}

void ui_round_rect_v(const UiCanvas *cv, float x0, float y0, float x1, float y1, float r,
                     uint32_t top, uint32_t bottom, int alpha) {
    if (alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    if (r > (x1 - x0) / 2) r = (x1 - x0) / 2;
    if (r > (y1 - y0) / 2) r = (y1 - y0) / 2;
    int iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
    if (iy0 < cv->y0) iy0 = cv->y0;
    if (iy1 > cv->y1) iy1 = cv->y1;
    if (iy1 <= iy0) return;
    BoxRows q = {cv, x0, y0, x1, y1, r, top, bottom, alpha, iy0};
    if (x1 - x0 >= 200.0f) par_rows(box_rows, &q, iy1 - iy0);          /* a large one: in pieces */
    else box_rows(&q, 0, iy1 - iy0);
}

void ui_round_frame(const UiCanvas *cv, float x0, float y0, float x1, float y1, float r, float w,
                    uint32_t c, int alpha) {
    if (alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    int iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
    if (iy0 < cv->y0) iy0 = cv->y0;
    if (iy1 > cv->y1) iy1 = cv->y1;
    float ri = r - w > 0 ? r - w : 0;
    for (int y = iy0; y < iy1; y++) {
        float py = (float)y + 0.5f, l, rr, il, ir;
        float vc = fminf(fminf(py - y0, y1 - py) + 0.5f, 1.0f);
        if (vc <= 0 || !row_span(x0, y0, x1, y1, r, fminf(fmaxf(py, y0), y1), &l, &rr)) continue;
        int a = (int)((float)alpha * vc);
        if (py <= y0 + w || py >= y1 - w ||
            !row_span(x0 + w, y0 + w, x1 - w, y1 - w, ri, py, &il, &ir)) {
            fill_span(cv, y, l, rr, c, a);                     /* the top and bottom bands */
            continue;
        }
        fill_span(cv, y, l, il, c, a);                         /* the sides */
        fill_span(cv, y, ir, rr, c, a);
    }
}

void ui_round_rect(const UiCanvas *cv, float x0, float y0, float x1, float y1, float r, uint32_t c, int alpha) {
    ui_round_rect_v(cv, x0, y0, x1, y1, r, c, c, alpha);
}

/* Smoothing (ui_set_image_look): how an image's pixels are enlarged. 0: as sharp blocks. 1, Soft:
 * blended, as the game's picture is. 2, Scale2x: read from a copy of the image already enlarged
 * through the filter (scalex.h), made once for each image and size and kept: the images are the
 * party's portraits, which stay where they are for as long as the program runs. */
static int image_look;
void ui_set_image_look(int look) { image_look = look; }

#define FILTERED_MAX 96
static struct { const uint8_t *src; int w, h, f; uint8_t *rgba; } filtered[FILTERED_MAX];
static int filtered_n;

/* im enlarged f times through the filter into *big (im itself when there is no room) */
static const UiImage *image_filtered(const UiImage *im, int f, UiImage *big) {
    for (int i = 0; i < filtered_n; i++)
        if (filtered[i].src == im->rgba && filtered[i].w == im->w && filtered[i].h == im->h && filtered[i].f == f) {
            big->w = im->w * f; big->h = im->h * f; big->rgba = filtered[i].rgba;
            return big;
        }
    if (filtered_n >= FILTERED_MAX) return im;
    uint32_t *in = malloc((size_t)im->w * (size_t)im->h * 4), *out = malloc((size_t)im->w * (size_t)im->h * 4 * (size_t)(f * f));
    if (!in || !out) { free(in); free(out); return im; }
    memcpy(in, im->rgba, (size_t)im->w * (size_t)im->h * 4);         /* (a pixel's four bytes as one value) */
    scalex_upscale(in, im->w, im->w, im->h, out, im->w * f, f);
    free(in);
    filtered[filtered_n].src = im->rgba; filtered[filtered_n].w = im->w; filtered[filtered_n].h = im->h;
    filtered[filtered_n].f = f; filtered[filtered_n].rgba = (uint8_t *)out;
    filtered_n++;
    big->w = im->w * f; big->h = im->h * f; big->rgba = (uint8_t *)out;
    return big;
}

typedef struct {
    const UiCanvas *cv;
    const UiImage *im;
    float x0, y0, size;
    int dim, alpha;
    int soft;                         /* the four pixels round each point, blended */
} ImageRows;

static void image_rows(void *ctx, int a, int b) {
    const ImageRows *q = ctx;
    const UiCanvas *cv = q->cv;
    const UiImage *im = q->im;
    float size = q->size;
    int ix0 = (int)q->x0, iy0 = (int)q->y0, n = (int)size, dim = q->dim, alpha = q->alpha;
    float r = size * 0.16f;
    for (int y = a; y < b; y++)
        for (int x = 0; x < n; x++) {
            int X = ix0 + x, Y = iy0 + y;
            if (X < cv->x0 || X >= cv->x1 || Y < cv->y0 || Y >= cv->y1) continue;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float dx = px < r ? r - px : px > size - r ? px - (size - r) : 0;
            float dy = py < r ? r - py : py > size - r ? py - (size - r) : 0;
            float cover = dx > 0 && dy > 0 ? r + 0.5f - sqrtf(dx * dx + dy * dy) : 1.0f;
            if (cover <= 0) continue;
            uint8_t blended[4];
            const uint8_t *p;
            if (q->soft) {                               /* Soft: the four pixels round the point, by how near */
                float fu = px / size * (float)im->w - 0.5f, fv = py / size * (float)im->h - 0.5f;
                int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
                float tu = fu - (float)u0, tv = fv - (float)v0, sum[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; k++) {
                    int uu = u0 + (k & 1), vv = v0 + (k >> 1);
                    uu = uu < 0 ? 0 : uu >= im->w ? im->w - 1 : uu;
                    vv = vv < 0 ? 0 : vv >= im->h ? im->h - 1 : vv;
                    const uint8_t *s = &im->rgba[((size_t)vv * (size_t)im->w + (size_t)uu) * 4];
                    float wgt = ((k & 1) ? tu : 1.0f - tu) * ((k >> 1) ? tv : 1.0f - tv) * (float)s[3];
                    sum[0] += (float)s[0] * wgt; sum[1] += (float)s[1] * wgt; sum[2] += (float)s[2] * wgt;
                    sum[3] += wgt;
                }
                if (sum[3] < 1.0f) continue;
                blended[0] = (uint8_t)(sum[0] / sum[3]); blended[1] = (uint8_t)(sum[1] / sum[3]);
                blended[2] = (uint8_t)(sum[2] / sum[3]);
                blended[3] = (uint8_t)(sum[3] > 255.0f ? 255.0f : sum[3]);
                p = blended;
            } else {
                int uu = (int)(px / size * (float)im->w), vv = (int)(py / size * (float)im->h);
                uu = uu >= im->w ? im->w - 1 : uu;
                vv = vv >= im->h ? im->h - 1 : vv;
                p = &im->rgba[((size_t)vv * (size_t)im->w + (size_t)uu) * 4];
            }
            if (!p[3]) continue;
            float rr = p[0], gg = p[1], bb = p[2];
            if (dim) { float l = (rr * 0.3f + gg * 0.59f + bb * 0.11f) * 0.55f; rr = gg = bb = l; }
            uint32_t c = (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
            float al = (float)p[3] / 255.0f * (cover > 1 ? 1 : cover) * (float)alpha;
            ui_blend(&cv->px[(size_t)Y * (size_t)cv->stride + (size_t)X], c, (int)al);
        }
}

void ui_mask(const UiCanvas *cv, const uint8_t *mask, int mw, int mh, float x0, float y0, float x1, float y1,
             uint32_t rgb, int alpha) {
    if (x1 <= x0 || y1 <= y0) return;
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < cv->x0) ix0 = cv->x0;
    if (iy0 < cv->y0) iy0 = cv->y0;
    if (ix1 > cv->x1) ix1 = cv->x1;
    if (iy1 > cv->y1) iy1 = cv->y1;
    for (int Y = iy0; Y < iy1; Y++)
        for (int X = ix0; X < ix1; X++) {                /* the mask's four nearest, weighed; nothing outside it */
            float u = ((float)X + 0.5f - x0) / (x1 - x0) * (float)mw - 0.5f;
            float v = ((float)Y + 0.5f - y0) / (y1 - y0) * (float)mh - 0.5f;
            int u0 = (int)floorf(u), v0 = (int)floorf(v);
            float fu = u - (float)u0, fv = v - (float)v0, cover = 0.0f;
            for (int k = 0; k < 4; k++) {
                int uu = u0 + (k & 1), vv = v0 + (k >> 1);
                if (uu < 0 || vv < 0 || uu >= mw || vv >= mh) continue;
                cover += (float)mask[(size_t)vv * (size_t)mw + (size_t)uu] * ((k & 1) ? fu : 1.0f - fu) *
                         ((k >> 1) ? fv : 1.0f - fv);
            }
            if (cover >= 1.0f)
                ui_blend(&cv->px[(size_t)Y * (size_t)cv->stride + (size_t)X], rgb, (int)(cover * (float)alpha / 255.0f));
        }
}

/* The larger pictures (80 across) at a size that is no whole number of times theirs. Enlarged: each of
 * their pixels as wide as the others, only its edges shared with its neighbours. Made smaller: each output
 * pixel the average of what falls in it. A copy at that size is made once and kept (the pictures stand at
 * a few sizes: a battle window tall and short, a save card), and drawn from as it is; a size seen for the
 * first time -- a window on its way from one height to the other -- is drawn the plain way, so nothing is
 * made for a size that lasts a frame. (Made every frame, they cost the Odin enough to lower its picture
 * detail: PsiLok, 2026-11-22, "after 4 or 5 times, it stopped and kept using the smaller sprite version".) */
#define EVEN_MAX 40
static struct { const uint8_t *src; int w, n; uint8_t *rgba; unsigned used; } even[EVEN_MAX];
static struct { const uint8_t *src; int n; } even_seen[32];
static unsigned even_clock;

static void even_make(const UiImage *im, int n, uint8_t *out) {
    float k = (float)n / (float)im->w;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float sum[4] = {0, 0, 0, 0};
            int cnt = 1;
            if (n < im->w) {                             /* smaller: the average */
                int u0 = x * im->w / n, u1 = ((x + 1) * im->w + n - 1) / n, v0 = y * im->h / n, v1 = ((y + 1) * im->h + n - 1) / n;
                cnt = 0;
                for (int vv = v0; vv < v1 && vv < im->h; vv++)
                    for (int uu = u0; uu < u1 && uu < im->w; uu++) {
                        const uint8_t *t = &im->rgba[((size_t)vv * (size_t)im->w + (size_t)uu) * 4];
                        sum[0] += (float)t[0] * (float)t[3]; sum[1] += (float)t[1] * (float)t[3];
                        sum[2] += (float)t[2] * (float)t[3]; sum[3] += (float)t[3];
                        cnt++;
                    }
                if (!cnt) cnt = 1;
            } else {                                     /* larger: even pixels, their edges shared */
                float fu = ((float)x + 0.5f) / k - 0.5f, fv = ((float)y + 0.5f) / k - 0.5f;
                int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
                float tu = (fu - (float)u0 - 0.5f) * k + 0.5f, tv = (fv - (float)v0 - 0.5f) * k + 0.5f;
                tu = tu < 0.0f ? 0.0f : tu > 1.0f ? 1.0f : tu;
                tv = tv < 0.0f ? 0.0f : tv > 1.0f ? 1.0f : tv;
                for (int q = 0; q < 4; q++) {
                    int uu = u0 + (q & 1), vv = v0 + (q >> 1);
                    uu = uu < 0 ? 0 : uu >= im->w ? im->w - 1 : uu;
                    vv = vv < 0 ? 0 : vv >= im->h ? im->h - 1 : vv;
                    const uint8_t *t = &im->rgba[((size_t)vv * (size_t)im->w + (size_t)uu) * 4];
                    float wgt = ((q & 1) ? tu : 1.0f - tu) * ((q >> 1) ? tv : 1.0f - tv) * (float)t[3];
                    sum[0] += (float)t[0] * wgt; sum[1] += (float)t[1] * wgt; sum[2] += (float)t[2] * wgt;
                    sum[3] += wgt;
                }
            }
            uint8_t *o = &out[((size_t)y * (size_t)n + (size_t)x) * 4];
            if (sum[3] < 1.0f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            float al = sum[3] / (float)cnt;
            o[0] = (uint8_t)(sum[0] / sum[3]); o[1] = (uint8_t)(sum[1] / sum[3]); o[2] = (uint8_t)(sum[2] / sum[3]);
            o[3] = (uint8_t)(al > 255.0f ? 255.0f : al);
        }
}

/* im at n across, evenly, in *copy: the kept copy, or one made now if this size was asked for before;
 * im itself the first time a size is seen (and when there is no memory for a copy) */
static const UiImage *image_even(const UiImage *im, int n, UiImage *copy) {
    even_clock++;
    int old = 0;
    for (int i = 0; i < EVEN_MAX; i++) {
        if (even[i].rgba && even[i].src == im->rgba && even[i].w == im->w && even[i].n == n) {
            even[i].used = even_clock;
            copy->w = copy->h = n;
            copy->rgba = even[i].rgba;
            return copy;
        }
        if (!even[i].rgba) old = i;
        else if (even[old].rgba && even[i].used < even[old].used) old = i;
    }
    int k = (int)(((uintptr_t)im->rgba >> 4) % (sizeof even_seen / sizeof even_seen[0]));
    if (even_seen[k].src != im->rgba || even_seen[k].n != n) {      /* the first time: only noted */
        even_seen[k].src = im->rgba;
        even_seen[k].n = n;
        return im;
    }
    uint8_t *px = malloc((size_t)n * (size_t)n * 4);
    if (!px) return im;
    even_make(im, n, px);
    free(even[old].rgba);
    even[old].src = im->rgba;
    even[old].w = im->w;
    even[old].n = n;
    even[old].rgba = px;
    even[old].used = even_clock;
    copy->w = copy->h = n;
    copy->rgba = px;
    return copy;
}

void ui_image(const UiCanvas *cv, const UiImage *im, float x0, float y0, float size, int dim, int alpha) {
    if (!im || !im->rgba || im->w < 1) return;
    UiImage big;
    int n = (int)size, soft = 0;
    if (image_look == 1 && n > im->w) {
        soft = 1;
    } else if (image_look == 2 && n >= im->w * 2) {
        /* Scale2x: by the whole number of times it is being enlarged, up to four; past that by a
         * factor that divides it, so the enlarged copy's pixels come out as even blocks */
        int r = (n + im->w / 2) / im->w;
        int f = r <= 4 ? r : r % 4 == 0 ? 4 : r % 3 == 0 ? 3 : 4;
        im = image_filtered(im, f, &big);
    } else if (image_look == 2 && im->w >= 80 && n > im->w) {
        /* (a larger picture enlarged by less than twice: through the filter at twice, then evened down
         * to its size below, so it is rounded off as the rest of the picture is) */
        im = image_filtered(im, 2, &big);
    }
    UiImage copy;
    if (!soft && im->w >= 80 && n > 0 && n % im->w) im = image_even(im, n, &copy);   /* (the larger pictures: see image_even) */
    ImageRows q = {cv, im, x0, y0, size, dim, alpha, soft};
    if (n >= 96) par_rows(image_rows, &q, n);                          /* a large one: in pieces */
    else if (n > 0) image_rows(&q, 0, n);
}

void ui_sprite(const UiCanvas *cv, const UiImage *im, float x0, float y0, float w, float h, int alpha) {
    if (!im || !im->rgba || im->w < 1 || im->h < 1 || w < 1.0f || h < 1.0f || alpha <= 0) return;
    int ix0 = (int)floorf(x0 + 0.5f), iy0 = (int)floorf(y0 + 0.5f), nw = (int)(w + 0.5f), nh = (int)(h + 0.5f);
    for (int y = 0; y < nh; y++) {
        int Y = iy0 + y;
        if (Y < cv->y0 || Y >= cv->y1) continue;
        int v = y * im->h / nh;
        for (int x = 0; x < nw; x++) {
            int X = ix0 + x;
            if (X < cv->x0 || X >= cv->x1) continue;
            const uint8_t *p = &im->rgba[((size_t)v * (size_t)im->w + (size_t)(x * im->w / nw)) * 4];
            if (!p[3]) continue;
            ui_blend(&cv->px[(size_t)Y * (size_t)cv->stride + (size_t)X],
                     (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2], p[3] * alpha / 255);
        }
    }
}

void ui_frame_window_a(const UiCanvas *cv, float x0, float y0, float x1, float y1, float u, int alpha) {
    ui_round_rect(cv, x0, y0, x1, y1, 3.0f * u, 0x070B26, alpha);
    ui_round_rect(cv, x0 + u, y0 + u, x1 - u, y1 - u, 2.2f * u, 0xDDE1EE, alpha);
    ui_round_rect(cv, x0 + 2.0f * u, y0 + 2.0f * u, x1 - 2.0f * u, y1 - 2.0f * u, 1.4f * u, 0x8A93B8, alpha);
    ui_round_rect_v(cv, x0 + 2.5f * u, y0 + 2.5f * u, x1 - 2.5f * u, y1 - 2.5f * u, 1.2f * u, 0x2A4AAE, 0x1A3084, alpha);
}

void ui_frame_window(const UiCanvas *cv, float x0, float y0, float x1, float y1, float u) {
    ui_frame_window_a(cv, x0, y0, x1, y1, u, 255);
}

void ui_bar(const UiCanvas *cv, float x0, float y0, float x1, float h, float frac, uint32_t c, int alpha) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ui_round_rect(cv, x0, y0, x1, y0 + h, h / 2, 0x05080F, alpha * 230 / 255);
    if (frac > 0) ui_round_rect(cv, x0, y0, x0 + (x1 - x0) * frac, y0 + h, h / 2, c, alpha);
}

void ui_triangle(const UiCanvas *cv, float cx, float cy, float size, int dir, uint32_t rgb, int alpha) {
    /* rasterised with 4x4 supersampling */
    float h = size * 0.5f;
    int x0 = (int)floorf(cx - h), x1 = (int)ceilf(cx + h), y0 = (int)floorf(cy - h), y1 = (int)ceilf(cy + h);
    for (int y = y0; y < y1; y++) {
        if (y < cv->y0 || y >= cv->y1) continue;
        for (int x = x0; x < x1; x++) {
            if (x < cv->x0 || x >= cv->x1) continue;
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float u = ((float)x + (sx + 0.5f) / 4 - cx) / h, v = ((float)y + (sy + 0.5f) / 4 - cy) / h;
                    float along = dir == 0 ? v : dir == 1 ? -v : dir == 2 ? u : -u;      /* towards the tip */
                    float across = dir <= 1 ? u : v;
                    if (along >= -1 && along <= 1 && fabsf(across) <= (1 - along) / 2 * 1.15f) hits++;
                }
            if (hits) ui_blend(&cv->px[(size_t)y * (size_t)cv->stride + (size_t)x], rgb, alpha * hits / 16);
        }
    }
}

/* ------------------------------------------------------------------ text */
int ui_ready(void) { return hdtext_font_ready(); }

float ui_text_width(const char *s, float cap) { return hdtext_string_width(s, cap); }

float ui_text(const UiCanvas *cv, float x, float base, float cap, const char *s, uint32_t c, int alpha) {
    if (alpha <= 0) return ui_text_width(s, cap);
    uint32_t a = (uint32_t)(alpha > 255 ? 255 : alpha);
    return hdtext_string(cv->px, cv->stride, cv->x0, cv->y0, cv->x1, cv->y1, x, base, cap, s, (c & 0xFFFFFF) | a << 24);
}

float ui_text_right(const UiCanvas *cv, float x_right, float base, float cap, const char *s, uint32_t c, int alpha) {
    return ui_text(cv, x_right - ui_text_width(s, cap), base, cap, s, c, alpha);
}

float ui_text_center(const UiCanvas *cv, float x_mid, float base, float cap, const char *s, uint32_t c, int alpha) {
    return ui_text(cv, x_mid - ui_text_width(s, cap) / 2, base, cap, s, c, alpha);
}

int ui_text_lines(float width, float cap, const char *s) {
    char line[256];
    int lines = 0;
    while (*s) {
        while (*s == ' ') s++;
        int best = 0, i = 0;
        for (;;) {
            while (s[i] && s[i] != ' ') i++;
            if (i >= (int)sizeof line) break;
            memcpy(line, s, (size_t)i);
            line[i] = 0;
            if (best && ui_text_width(line, cap) > width) break;
            best = i;
            if (!s[i]) break;
            i++;
        }
        if (!best) break;
        lines++;
        s += best;
    }
    return lines;
}

int ui_text_wrap(const UiCanvas *cv, float x, float base, float width, float line_h, int max_lines,
                 float cap, const char *s, uint32_t c, int alpha) {
    char line[256];
    int lines = 0;
    while (*s && lines < max_lines) {
        while (*s == ' ') s++;
        /* the longest run of whole words that fits */
        int best = 0, i = 0;
        for (;;) {
            while (s[i] && s[i] != ' ') i++;
            if (i >= (int)sizeof line) break;
            memcpy(line, s, (size_t)i);
            line[i] = 0;
            if (best && ui_text_width(line, cap) > width) break;
            best = i;
            if (!s[i]) break;
            i++;
        }
        if (!best) break;
        if (lines == max_lines - 1 && s[best]) {                 /* last line: cut with an ellipsis */
            int n = best;
            memcpy(line, s, (size_t)n);
            strcpy(line + n, "\xE2\x80\xA6");
            while (n > 0 && ui_text_width(line, cap) > width) { n--; memcpy(line + n, "\xE2\x80\xA6", 4); }
        } else {
            memcpy(line, s, (size_t)best);
            line[best] = 0;
        }
        ui_text(cv, x, base + (float)lines * line_h, cap, line, c, alpha);
        lines++;
        s += best;
    }
    return lines;
}
