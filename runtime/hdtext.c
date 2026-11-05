/*
 * Modern text: see hdtext.h.
 */
#include "hdtext.h"
#include "m68k.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define WIN_FONT_A     0x680           /* window font: tile = $680 + code */
#define WIN_FONT_B     0x7BF           /* second copy (battle numbers): $7BF + code */
#define CANVAS_LO      0x580           /* dialogue canvas: tiles $580-$5FF */
#define CANVAS_HI      0x600
#define DIALOGUE_FONT  0x2A3542u       /* Art_DialogueFont (US ROM): 128 x 16 bytes, 8x16 1-bit */

static int enabled, loaded;
static stbtt_fontinfo font;
static unsigned char *font_data;
static float cap_units;                 /* height of 'H' in font units */

static uint32_t win_char[128];          /* general/tables/wincharset.asm, plus the status labels */
static uint32_t dlg_char[128];          /* script/charset.asm */
static uint8_t dlg_bitmap[128][16];
static int dlg_ready;

static void build_tables(void) {
    for (int c = 0; c < 26; c++) { win_char[1 + c] = 'A' + c; win_char[57 + c] = 'a' + c; }
    for (int c = 0; c < 10; c++) { win_char[27 + c] = '0' + c; win_char[37 + c] = '0' + c; }   /* thin, bold */
    win_char[0] = ' ';
    win_char[0x31] = '-'; win_char[0x32] = '!'; win_char[0x33] = '?'; win_char[0x34] = ':';
    win_char[0x53] = '.'; win_char[0x54] = 0x2019; win_char[0x55] = ','; win_char[0x56] = 0xB7;
    win_char[0x77] = '/'; win_char[0x78] = 'H'; win_char[0x79] = 'P'; win_char[0x7A] = 'T';
    for (int c = 0; c < 26; c++) { dlg_char[1 + c] = 'A' + c; dlg_char[27 + c] = 'a' + c; }
    for (int c = 0; c < 10; c++) dlg_char[64 + c] = '0' + c;
    dlg_char[0] = ' ';
    dlg_char[0x35] = '.'; dlg_char[0x36] = 0x2019; dlg_char[0x37] = ','; dlg_char[0x38] = 0xB7;
    dlg_char[0x39] = ':'; dlg_char[0x3A] = '!'; dlg_char[0x3B] = '?'; dlg_char[0x3C] = '-';
    dlg_char[0x3D] = 0x201C; dlg_char[0x3E] = 0x201D; dlg_char[0x3F] = '%';   /* << >> as quotes */
}

int hdtext_init_mem(void *data, size_t size) {
    build_tables();
    if (!data || size < 12) return 0;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&font, data, offset)) return 0;
    free(font_data);                    /* a font loaded before (the data is now the font's) */
    font_data = data;
    int x0, y0, x1, y1;
    cap_units = stbtt_GetCodepointBox(&font, 'H', &x0, &y0, &x1, &y1) ? (float)(y1 - y0) : 700.0f;
    loaded = 1;
    return 1;
}

int hdtext_init(const char *path) {
    build_tables();
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "modern text: cannot open font %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = n > 0 ? malloc((size_t)n) : NULL;
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return 0; }
    fclose(f);
    if (!hdtext_init_mem(data, (size_t)n)) {
        fprintf(stderr, "modern text: %s is not a TrueType/OpenType font\n", path);
        free(data);
        return 0;
    }
    return 1;
}

void hdtext_set_enabled(int on) { enabled = on && loaded; }
int hdtext_enabled(void) { return enabled; }

static void load_dialogue_font(void) {
    if (dlg_ready || !m68k_rom || m68k_rom_size < DIALOGUE_FONT + sizeof dlg_bitmap) return;
    memcpy(dlg_bitmap, m68k_rom + DIALOGUE_FONT, sizeof dlg_bitmap);
    dlg_ready = 1;
}

/* ------------------------------------------------------------------ finding text */
/* a tile drawn only in the font's two colours: rows as 1-bit bitmaps ($F = 1) */
static int font_rows(int tile, uint8_t *rows) {
    const uint8_t *p = &vdp.vram[(tile & 0x7FF) * 32];
    for (int r = 0; r < 8; r++) {
        uint8_t bits = 0;
        for (int b = 0; b < 4; b++) {
            uint8_t hi = p[r * 4 + b] >> 4, lo = p[r * 4 + b] & 15;
            if ((hi != 14 && hi != 15) || (lo != 14 && lo != 15)) return 0;
            bits = (uint8_t)(bits << 2 | (hi == 15) << 1 | (lo == 15));
        }
        rows[r] = bits;
    }
    return 1;
}

typedef struct { int x, y, h; uint32_t ch; uint8_t pal; } Cell;
#define MAX_CELLS 2048
static Cell cells[MAX_CELLS];
static int ncells;

static void find_cells(const VdpLine *lines, int w, int h) {
    ncells = 0;
    load_dialogue_font();
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        for (int x = 0; x < w && ncells < MAX_CELLS; x++) {
            if (l->plane_a_txy[x] || l->win_in[x]) continue;      /* a tile's top-left pixel only */
            uint16_t e = l->plane_a_tile[x];
            if (!(e & 0x8000) || (e & 0x1800)) continue;         /* text: high priority, not flipped */
            int t = e & 0x7FF;
            uint32_t ch = 0;
            int ch_h = 8;
            uint8_t rows[16];
            if (t >= CANVAS_LO && t < CANVAS_HI) {               /* dialogue: an 8x16 cell */
                if (((t - CANVAS_LO) & 1) || !dlg_ready || y + 16 > h) continue;
                if (!font_rows(t, rows) || !font_rows(t + 1, rows + 8)) continue;
                for (int c = 0; c < 128; c++)
                    if (!memcmp(dlg_bitmap[c], rows, 16)) { ch = dlg_char[c]; break; }
                ch_h = 16;
            } else {
                int code = t >= WIN_FONT_A && t < WIN_FONT_A + 128 ? t - WIN_FONT_A
                         : t > WIN_FONT_B && t <= 0x7FF ? t - WIN_FONT_B : -1;
                if (code < 0 || !win_char[code] || !font_rows(t, rows)) continue;
                ch = win_char[code];
            }
            if (!ch) continue;
            Cell *c = &cells[ncells++];
            c->x = x; c->y = y; c->h = ch_h; c->ch = ch; c->pal = (uint8_t)(e >> 13 & 3);
        }
    }
}

int hdtext_has_text(const VdpLine *lines, int w, int h) {
    if (!enabled) return 0;
    find_cells(lines, w, h);
    for (int i = 0; i < ncells; i++)
        if (cells[i].ch != ' ') return 1;
    return 0;
}

/* ------------------------------------------------------------------ glyphs */
typedef struct { uint32_t cp; int key; unsigned char *bmp; int w, h, xoff, yoff; } Glyph;
#define GLYPHS 2048
static Glyph glyphs[GLYPHS];
static int nglyphs;

static const Glyph *glyph(uint32_t cp, float scale) {
    int key = (int)lrintf(scale * 1000000.0f);
    unsigned slot = (cp * 2654435761u ^ (unsigned)key) % GLYPHS;
    for (int i = 0; i < GLYPHS; i++, slot = (slot + 1) % GLYPHS) {
        Glyph *g = &glyphs[slot];
        if (g->bmp && g->cp == cp && g->key == key) return g;
        if (!g->bmp) {
            if (nglyphs > GLYPHS * 3 / 4) {                       /* full: start over */
                for (int k = 0; k < GLYPHS; k++) { stbtt_FreeBitmap(glyphs[k].bmp, NULL); glyphs[k].bmp = NULL; }
                nglyphs = 0;
                return glyph(cp, scale);
            }
            g->bmp = stbtt_GetCodepointBitmap(&font, scale, scale, (int)cp, &g->w, &g->h, &g->xoff, &g->yoff);
            if (!g->bmp) g->bmp = (unsigned char *)calloc(1, 1);  /* blank (space) */
            g->cp = cp; g->key = key;
            nglyphs++;
            return g;
        }
    }
    return NULL;
}

/* glyph alpha over the image in the text colour, kept inside the clip box
 * and off pixels a high-priority sprite covers (cursors over menus) */
static void blit(const VdpLine *lines, int h, uint32_t *out, int stride, int scale, const Glyph *g, float pen_x,
                 float base_y, uint32_t fg, int cx0, int cy0, int cx1, int cy1) {
    int ox = (int)lrintf(pen_x) + g->xoff, oy = (int)lrintf(base_y) + g->yoff;
    int fr = (int)(fg >> 16 & 255), fgc = (int)(fg >> 8 & 255), fb = (int)(fg & 255);
    int fa = fg >> 24 ? (int)(fg >> 24) : 255;                   /* top byte: opacity (0 = opaque) */
    for (int y = 0; y < g->h; y++) {
        int Y = oy + y;
        if (Y < cy0 || Y >= cy1 || (lines && Y / scale >= h)) continue;
        const VdpLine *l = lines ? &lines[Y / scale] : NULL;
        for (int x = 0; x < g->w; x++) {
            int X = ox + x, a = g->bmp[y * g->w + x] * fa / 255;
            if (!a || X < cx0 || X >= cx1 || (l && (l->sprite[X / scale] & 0x80))) continue;
            uint32_t *d = &out[(size_t)Y * (size_t)stride + (size_t)X];
            int r = (int)(*d >> 16 & 255), gg = (int)(*d >> 8 & 255), b = (int)(*d & 255);
            r += (fr - r) * a / 255; gg += (fgc - gg) * a / 255; b += (fb - b) * a / 255;
            *d = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
        }
    }
}

static int numeric(uint32_t c) { return (c >= '0' && c <= '9') || c == ' ' || c == ':' || c == '/' || c == '-'; }
static int digit(uint32_t c) { return c >= '0' && c <= '9'; }

/* cells a..b-1 as a number: laid out naturally and right-aligned to its last
 * cell, as the game aligns numbers, so columns still line up */
static void draw_cells(const VdpLine *lines, int h, uint32_t *out, int stride, int S, int a, int b, float sc,
                       float base_y, uint32_t fg, int rx0, int ry0, int rx1, int clip_y1) {
    while (b > a && cells[b - 1].ch == ' ') b--;
    while (a < b && cells[a].ch == ' ') a++;
    if (b <= a) return;
    float width = 0;
    for (int k = a; k < b; k++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, (int)cells[k].ch, &adv, &lsb);
        width += (float)adv * sc;
    }
    float pen = (float)((cells[b - 1].x + 8) * S) - 0.5f * (float)S - width;
    for (int k = a; k < b; k++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, (int)cells[k].ch, &adv, &lsb);
        if (cells[k].ch != ' ') {
            const Glyph *g = glyph(cells[k].ch, sc);
            if (g) blit(lines, h, out, stride, S, g, pen, base_y, fg, rx0, ry0, rx1, clip_y1);
        }
        pen += (float)adv * sc;
    }
}

/* cells a..b-1 as text flowing from the first one, never wider than the cells */
static void draw_words(const VdpLine *lines, int h, uint32_t *out, int stride, int S, int a, int b, float sc,
                       float base_y, uint32_t fg, int ry0, int clip_y1) {
    while (b > a && cells[b - 1].ch == ' ') b--;                 /* trailing spaces don't count */
    if (b <= a) return;
    int x0 = cells[a].x * S, x1 = (cells[b - 1].x + 8) * S;
    float width = 0;
    for (int k = a; k < b; k++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, (int)cells[k].ch, &adv, &lsb);
        width += (float)adv * sc;
        if (k + 1 < b) width += (float)stbtt_GetCodepointKernAdvance(&font, (int)cells[k].ch, (int)cells[k + 1].ch) * sc;
    }
    float room = (float)(x1 - x0) - 0.5f * (float)S;
    float squeeze = width > room && width > 0 ? room / width : 1.0f;
    float pen = (float)x0 + 0.5f * (float)S;
    for (int k = a; k < b; k++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, (int)cells[k].ch, &adv, &lsb);
        if (cells[k].ch != ' ') {
            const Glyph *g = glyph(cells[k].ch, sc);
            if (g) blit(lines, h, out, stride, S, g, pen, base_y, fg, x0, ry0, x1 + S, clip_y1);
        }
        float step = (float)adv * sc;
        if (k + 1 < b) step += (float)stbtt_GetCodepointKernAdvance(&font, (int)cells[k].ch, (int)cells[k + 1].ch) * sc;
        pen += step * squeeze;
    }
}

void hdtext_draw(const VdpLine *lines, int w, int h, uint32_t *out, int stride, int scale) {
    if (!enabled) return;
    find_cells(lines, w, h);
    int S = scale;
    for (int i = 0; i < ncells;) {
        /* a run: same row, same height and colour, cell after cell */
        int j = i + 1;
        while (j < ncells && cells[j].y == cells[i].y && cells[j].h == cells[i].h && cells[j].pal == cells[i].pal
               && cells[j].x == cells[j - 1].x + 8)
            j++;
        const Cell *c0 = &cells[i];
        uint32_t fg = vdp_color(c0->pal * 16 + 15), bg = vdp_color(c0->pal * 16 + 14);
        int rx0 = c0->x * S, rx1 = (cells[j - 1].x + 8) * S, ry0 = c0->y * S, ry1 = (c0->y + c0->h) * S;
        /* cover the original letters (not where a sprite is on top) */
        for (int k = i; k < j; k++)
            for (int y = cells[k].y; y < cells[k].y + cells[k].h && y < h; y++)
                for (int x = cells[k].x; x < cells[k].x + 8 && x < w; x++) {
                    if (lines[y].sprite[x] & 0x80) continue;
                    for (int yy = 0; yy < S; yy++)
                        for (int xx = 0; xx < S; xx++) out[(size_t)(y * S + yy) * (size_t)stride + (size_t)(x * S + xx)] = bg;
                }
        /* the original's proportions: 8x16 dialogue capitals are 10 tall on a
         * baseline at 13; 8x8 window capitals 7 tall on a baseline at 7 */
        float cap = c0->h == 16 ? 10.0f : 6.6f, base = c0->h == 16 ? 13.0f : 7.0f;
        float sc = cap * (float)S / cap_units;
        float base_y = ((float)c0->y + base) * (float)S;
        int clip_y1 = ry1 + (c0->h == 8 ? S : 0);              /* window text: a pixel of descender room */
        /* dialogue lines flow as one; in windows, digit groups keep their
         * cells (numbers are right-aligned in columns) and the text between
         * them flows from where it starts */
        for (int a = i; a < j;) {
            int b = a + 1;
            if (c0->h == 8)
                while (b < j && digit(cells[b].ch) == digit(cells[a].ch)) b++;
            else
                b = j;
            int all_numeric = 1;
            for (int k = a; k < b; k++) all_numeric &= numeric(cells[k].ch);
            if (all_numeric) draw_cells(lines, h, out, stride, S, a, b, sc, base_y, fg, rx0, ry0, rx1, clip_y1);
            else draw_words(lines, h, out, stride, S, a, b, sc, base_y, fg, ry0, clip_y1);
            a = b;
        }
        i = j;
    }
}

void hdtext_upscale(const uint32_t *src, int src_stride, int w, int h, uint32_t *out, int out_stride, int scale) {
    for (int y = 0; y < h; y++) {
        const uint32_t *s = src + (size_t)y * (size_t)src_stride;
        uint32_t *d = out + (size_t)y * (size_t)scale * (size_t)out_stride;
        for (int x = 0; x < w; x++)
            for (int k = 0; k < scale; k++) d[x * scale + k] = s[x];
        for (int k = 1; k < scale; k++)
            memcpy(d + (size_t)k * (size_t)out_stride, d, (size_t)(w * scale) * sizeof(uint32_t));
    }
}

/* ------------------------------------------------------------------ strings (panels) */
int hdtext_font_ready(void) { return loaded; }

uint32_t hdtext_win_char(int code) {
    if (!win_char[1]) build_tables();                       /* before the font loads */
    return code >= 0 && code < 128 ? win_char[code] : 0;
}

static uint32_t next_codepoint(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    if (c >= 0xC0 && (*s & 0xC0) == 0x80) {                  /* 2- and 3-byte UTF-8 */
        if (c < 0xE0) { c = (c & 0x1F) << 6 | (*s++ & 0x3F); }
        else if ((s[1] & 0xC0) == 0x80) { c = (c & 0x0F) << 12 | (uint32_t)(s[0] & 0x3F) << 6 | (s[1] & 0x3F); s += 2; }
    }
    *p = (const char *)s;
    return c;
}

float hdtext_string_width(const char *text, float cap_px) {
    if (!loaded) return 0;
    float sc = cap_px / cap_units, w = 0;
    uint32_t prev = 0;
    for (const char *p = text; *p;) {
        uint32_t c = next_codepoint(&p);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, (int)c, &adv, &lsb);
        if (prev) w += (float)stbtt_GetCodepointKernAdvance(&font, (int)prev, (int)c) * sc;
        w += (float)adv * sc;
        prev = c;
    }
    return w;
}

float hdtext_string(uint32_t *out, int stride, int cx0, int cy0, int cx1, int cy1, float x, float baseline,
                    float cap_px, const char *text, uint32_t color) {
    if (!loaded) return 0;
    float sc = cap_px / cap_units, pen = x;
    uint32_t prev = 0;
    for (const char *p = text; *p;) {
        uint32_t c = next_codepoint(&p);
        if (prev) pen += (float)stbtt_GetCodepointKernAdvance(&font, (int)prev, (int)c) * sc;
        if (c != ' ') {
            const Glyph *g = glyph(c, sc);
            if (g) blit(NULL, 0, out, stride, 1, g, pen, baseline, color, cx0, cy0, cx1, cy1);
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, (int)c, &adv, &lsb);
        pen += (float)adv * sc;
        prev = c;
    }
    return pen - x;
}
