/*
 * Art from the player's ROM: see romart.h.
 */
#include "romart.h"
#include "m68k.h"

#include <stdlib.h>
#include <string.h>

#define PORTRAIT_PTRS    0x6A4B0u     /* DialoguePortraitArtPtrs: 1 Chaz .. $B Seth, Nemesis art */
#define PORTRAIT_LAYOUT  0x2A2B36u    /* 6x6 name-table words, tiles $55C.., palette line 2 */
#define PORTRAIT_FIRST   0x55C
#define PAL_CHARACTERS   0x9F2FCu     /* Pal_Init line 2: the characters' palette */
#define PROFILE_ART_PTRS 0x5D548u     /* Win_StatusCharList's pictures: 0 Chaz .. $A Seth, Nemesis art (77 to 95 tiles) */
#define PROFILE_MAP_PTRS 0x5D574u     /* ... and their 10x10 name-table words, tiles $580.., palette line 2 */
#define PROFILE_FIRST    0x580
#define FIELD_ART_PTRS   0x5E732u     /* CharFieldArtPtrs: 0 Chaz .. $A Seth, raw tiles, 9 frames of 8 */
#define FIELD_FRAME      256          /* a frame: 2 x 4 tiles, a column at a time (as sprites are) */

/* Nemesis: a header word (bit 15 = rows XORed with the previous one, then
 * the number of tiles), a table of prefix codes (each giving a pixel value
 * and how many times it repeats) ending with $FF, then the bitstream; the
 * code 111111 is followed by an inline 3-bit count - 1 and 4-bit value. */
int romart_nemesis(const uint8_t *src, uint32_t n, uint8_t *dst, int max) {
    if (n < 3) return 0;
    int xor_mode = src[0] & 0x80, tiles = (src[0] & 0x7F) << 8 | src[1];
    uint32_t p = 2;
    struct { uint8_t len, value, count; } table[256];
    memset(table, 0, sizeof table);
    int value = 0;
    while (p < n) {
        uint8_t b = src[p++];
        if (b == 0xFF) break;
        if (b & 0x80) {                                  /* a new pixel value */
            value = b & 0x0F;
            if (p >= n) return 0;
            b = src[p++];
        }
        int count = (b >> 4 & 7) + 1, len = b & 0x0F;
        if (p >= n || len == 0 || len > 8) return 0;
        int code = src[p++];
        int first = code << (8 - len), last = (code + 1) << (8 - len);
        for (int i = first; i < last && i < 256; i++) {
            table[i].len = (uint8_t)len; table[i].value = (uint8_t)value; table[i].count = (uint8_t)count;
        }
    }
    uint32_t bits = 0;                                   /* bit buffer, MSB first */
    int nbits = 0, out = 0, need = tiles * 32 > max ? max : tiles * 32;
    uint32_t row = 0, prev = 0;
    int nyb = 0;
    while (out < need) {
        while (nbits <= 24 && p < n) { bits |= (uint32_t)src[p++] << (24 - nbits); nbits += 8; }
        if (nbits < 6) break;
        int count, val;
        if ((bits >> 26) == 0x3F) {                       /* inline: 111111 ccc vvvv */
            if (nbits < 13) break;
            count = (int)(bits >> 23 & 7) + 1;
            val = (int)(bits >> 19 & 15);
            bits <<= 13; nbits -= 13;
        } else {
            int idx = (int)(bits >> 24);
            if (!table[idx].len || table[idx].len > nbits) break;
            count = table[idx].count; val = table[idx].value;
            bits <<= table[idx].len; nbits -= table[idx].len;
        }
        while (count-- > 0 && out < need) {
            row = row << 4 | (uint32_t)val;
            if (++nyb == 8) {                             /* a row of 8 pixels */
                uint32_t r = xor_mode ? row ^ prev : row;
                prev = r;
                dst[out++] = (uint8_t)(r >> 24); dst[out++] = (uint8_t)(r >> 16);
                dst[out++] = (uint8_t)(r >> 8); dst[out++] = (uint8_t)r;
                row = 0; nyb = 0;
            }
        }
    }
    return out;
}

static uint32_t rom32(uint32_t a) {
    return a + 3 < m68k_rom_size ? (uint32_t)m68k_rom[a] << 24 | (uint32_t)m68k_rom[a + 1] << 16 |
                                   (uint32_t)m68k_rom[a + 2] << 8 | m68k_rom[a + 3] : 0;
}

int romart_portrait(int character, uint8_t *rgba) {
    if (character < 0 || character > 10 || !m68k_rom || m68k_rom_size < PORTRAIT_LAYOUT + 72) return 0;
    uint32_t art = rom32(PORTRAIT_PTRS + (uint32_t)(character + 1) * 4);
    if (!art || art >= m68k_rom_size) return 0;
    uint8_t tiles[36 * 32];
    if (romart_nemesis(m68k_rom + art, m68k_rom_size - art, tiles, sizeof tiles) < (int)sizeof tiles) return 0;
    uint8_t pal[16][3];
    for (int i = 0; i < 16; i++) {                        /* ----bbb-ggg-rrr- */
        unsigned c = (unsigned)(m68k_rom[PAL_CHARACTERS + i * 2] << 8 | m68k_rom[PAL_CHARACTERS + i * 2 + 1]);
        pal[i][0] = (uint8_t)((c >> 1 & 7) * 255 / 7);
        pal[i][1] = (uint8_t)((c >> 5 & 7) * 255 / 7);
        pal[i][2] = (uint8_t)((c >> 9 & 7) * 255 / 7);
    }
    for (int ty = 0; ty < 6; ty++)
        for (int tx = 0; tx < 6; tx++) {
            uint32_t a = PORTRAIT_LAYOUT + (uint32_t)(ty * 6 + tx) * 2;
            unsigned e = (unsigned)(m68k_rom[a] << 8 | m68k_rom[a + 1]);
            int t = (int)(e & 0x7FF) - PORTRAIT_FIRST;
            if (t < 0 || t >= 36) return 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int sx = e & 0x0800 ? 7 - x : x, sy = e & 0x1000 ? 7 - y : y;
                    uint8_t b = tiles[t * 32 + sy * 4 + sx / 2];
                    int v = sx & 1 ? b & 15 : b >> 4;
                    uint8_t *o = &rgba[((ty * 8 + y) * ROMART_PORTRAIT + tx * 8 + x) * 4];
                    o[0] = pal[v][0]; o[1] = pal[v][1]; o[2] = pal[v][2]; o[3] = v ? 255 : 0;
                }
        }
    return 1;
}

int romart_profile_index(int character, uint8_t *index) {
    if (character < 0 || character > 10 || !m68k_rom || m68k_rom_size < PROFILE_MAP_PTRS + 44) return 0;
    uint32_t art = rom32(PROFILE_ART_PTRS + (uint32_t)character * 4), map = rom32(PROFILE_MAP_PTRS + (uint32_t)character * 4);
    if (!art || art >= m68k_rom_size || !map || map + 200 > m68k_rom_size) return 0;
    static uint8_t tiles[100 * 32];
    int have = romart_nemesis(m68k_rom + art, m68k_rom_size - art, tiles, sizeof tiles) / 32;
    for (int ty = 0; ty < 10; ty++)
        for (int tx = 0; tx < 10; tx++) {
            uint32_t a = map + (uint32_t)(ty * 10 + tx) * 2;
            unsigned e = (unsigned)(m68k_rom[a] << 8 | m68k_rom[a + 1]);
            int t = (int)(e & 0x7FF) - PROFILE_FIRST;
            if (t < 0 || t >= have) return 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int sx = e & 0x0800 ? 7 - x : x, sy = e & 0x1000 ? 7 - y : y;
                    uint8_t b = tiles[t * 32 + sy * 4 + sx / 2];
                    index[(ty * 8 + y) * ROMART_PROFILE + tx * 8 + x] = (uint8_t)(sx & 1 ? b & 15 : b >> 4);
                }
        }
    return 1;
}

int romart_profile(int character, uint8_t *rgba) {
    static uint8_t index[ROMART_PROFILE * ROMART_PROFILE];
    if (!romart_profile_index(character, index) || m68k_rom_size < PAL_CHARACTERS + 32) return 0;
    uint8_t pal[16][3];
    for (int i = 0; i < 16; i++) {                        /* ----bbb-ggg-rrr- */
        unsigned c = (unsigned)(m68k_rom[PAL_CHARACTERS + i * 2] << 8 | m68k_rom[PAL_CHARACTERS + i * 2 + 1]);
        pal[i][0] = (uint8_t)((c >> 1 & 7) * 255 / 7);
        pal[i][1] = (uint8_t)((c >> 5 & 7) * 255 / 7);
        pal[i][2] = (uint8_t)((c >> 9 & 7) * 255 / 7);
    }
    for (int i = 0; i < ROMART_PROFILE * ROMART_PROFILE; i++) {
        int v = index[i];
        uint8_t *o = &rgba[i * 4];
        o[0] = pal[v][0]; o[1] = pal[v][1]; o[2] = pal[v][2]; o[3] = v ? 255 : 0;
    }
    return 1;
}

int romart_portrait_who(const uint8_t *tiles) {
    static uint8_t known[11][36 * 32];
    static int ready[11];                                /* 0 not tried, 1 there, -1 not to be had */
    if (!m68k_rom || m68k_rom_size < PORTRAIT_LAYOUT + 72) return -1;
    for (int c = 0; c < 11; c++) {
        if (!ready[c]) {
            uint32_t art = rom32(PORTRAIT_PTRS + (uint32_t)(c + 1) * 4);
            ready[c] = art && art < m68k_rom_size
                       && romart_nemesis(m68k_rom + art, m68k_rom_size - art, known[c], sizeof known[c]) >= (int)sizeof known[c] ? 1 : -1;
        }
        if (ready[c] > 0 && !memcmp(known[c], tiles, sizeof known[c])) return c;
    }
    return -1;
}

/* ---- the story panels. Panel_GetData ($06B29A): PanelPtrs, a table for every 64 panels, each a record of $1E
 * bytes -- the tile its art (Nemesis) is loaded at and the art; its size in cells and its place; then two
 * layers (the first in front), each the base name-table word, 16 colours and an Enigma map of cells. */
#define PANEL_PTRS   0x06B2B6u
#define PANEL_TABLES 7
#define PANEL_TILES  1400

/* Sega's Enigma compression: name-table words into out (at most max); returns how many */
static int enigma(const uint8_t *src, uint32_t len, unsigned base, uint16_t *out, int max) {
    if (len < 7) return 0;
    int inline_bits = src[0], flags = src[1], n = 0;
    unsigned incr = ((unsigned)(src[2] << 8 | src[3]) + base) & 0xFFFF, common = ((unsigned)(src[4] << 8 | src[5]) + base) & 0xFFFF;
    uint32_t bit = 6 * 8, end = len * 8;
#define BITS(k, v) do { v = 0; for (int b_ = 0; b_ < (k); b_++) { if (bit >= end) return n; \
        v = v << 1 | (unsigned)(src[bit >> 3] >> (7 - (bit & 7)) & 1); bit++; } } while (0)
#define INLINE(v) do { unsigned f_ = 0, t_; static const unsigned M_[5] = {0x8000, 0x4000, 0x2000, 0x1000, 0x0800}; \
        for (int i_ = 0; i_ < 5; i_++) if (flags & (0x10 >> i_)) { BITS(1, t_); if (t_) f_ |= M_[i_]; } \
        BITS(inline_bits, t_); v = ((f_ | t_) + base) & 0xFFFF; } while (0)
    for (;;) {
        unsigned first, mode, count, v;
        BITS(1, first);
        if (first) {
            BITS(2, mode);
            BITS(4, count);
            if (mode == 3) {
                if (count == 0xF) break;
                for (unsigned i = 0; i <= count; i++) { INLINE(v); if (n < max) out[n++] = (uint16_t)v; }
            } else {
                INLINE(v);
                for (unsigned i = 0; i <= count; i++) {
                    if (n < max) out[n++] = (uint16_t)v;
                    v = (v + (mode == 1 ? 1u : mode == 2 ? 0xFFFFu : 0u)) & 0xFFFF;
                }
            }
        } else {
            BITS(1, mode);
            BITS(4, count);
            for (unsigned i = 0; i <= count; i++) {
                if (n >= max) continue;
                if (mode) out[n++] = (uint16_t)common;
                else { out[n++] = (uint16_t)incr; incr = (incr + 1) & 0xFFFF; }
            }
        }
    }
#undef BITS
#undef INLINE
    return n;
}

uint8_t *romart_panel(int index, int *w, int *h) {
    if (index < 0 || index >= PANEL_TABLES * 64 || !m68k_rom || m68k_rom_size < PANEL_PTRS + PANEL_TABLES * 4) return NULL;
    uint32_t table = rom32(PANEL_PTRS + (uint32_t)(index >> 6) * 4), rec = table + (uint32_t)(index & 0x3F) * 0x1E;
    if (!table || rec + 0x1E > m68k_rom_size) return NULL;
    unsigned dest = (unsigned)(m68k_rom[rec] << 8 | m68k_rom[rec + 1]);
    uint32_t art = rom32(rec + 2);
    int cols = m68k_rom[rec + 6], rows = m68k_rom[rec + 7];
    if (dest == 0xFFFF || dest >= 0x800 || !art || art >= m68k_rom_size || cols < 1 || rows < 1 || cols > 40 || rows > 28) return NULL;
    static uint8_t tiles[PANEL_TILES * 32];
    static uint16_t cells[40 * 28];
    int have = romart_nemesis(m68k_rom + art, m68k_rom_size - art, tiles, sizeof tiles) / 32;
    if (have < 1) return NULL;
    int W = cols * 8, H = rows * 8, any = 0;
    uint8_t *px = calloc((size_t)W * (size_t)H, 4);
    if (!px) return NULL;
    for (int k = 0; k < 2; k++) {                        /* the front layer first: what it draws stays */
        uint32_t at = rec + 10 + (uint32_t)k * 10;
        unsigned word = (unsigned)(m68k_rom[at] << 8 | m68k_rom[at + 1]);
        uint32_t pal_at = rom32(at + 2), map_at = rom32(at + 6);
        if (word == 0xFFFF || !pal_at || pal_at + 32 > m68k_rom_size || !map_at || map_at >= m68k_rom_size) continue;
        uint8_t pal[16][3];
        for (int i = 0; i < 16; i++) {                    /* ----bbb-ggg-rrr- */
            unsigned c = (unsigned)(m68k_rom[pal_at + i * 2] << 8 | m68k_rom[pal_at + i * 2 + 1]);
            pal[i][0] = (uint8_t)((c >> 1 & 7) * 255 / 7);
            pal[i][1] = (uint8_t)((c >> 5 & 7) * 255 / 7);
            pal[i][2] = (uint8_t)((c >> 9 & 7) * 255 / 7);
        }
        int n = enigma(m68k_rom + map_at, m68k_rom_size - map_at, word, cells, cols * rows);
        for (int i = 0; i < n; i++) {
            unsigned e = cells[i];
            int t = (int)(e & 0x7FF) - (int)dest, cx = i % cols, cy = i / cols;
            if (t < 0 || t >= have) continue;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int sx = e & 0x0800 ? 7 - x : x, sy = e & 0x1000 ? 7 - y : y;
                    uint8_t b = tiles[t * 32 + sy * 4 + sx / 2];
                    int v = sx & 1 ? b & 15 : b >> 4;
                    uint8_t *o = &px[((size_t)(cy * 8 + y) * (size_t)W + (size_t)(cx * 8 + x)) * 4];
                    if (!v || o[3]) continue;
                    o[0] = pal[v][0]; o[1] = pal[v][1]; o[2] = pal[v][2]; o[3] = 255;
                    any = 1;
                }
        }
    }
    if (!any) { free(px); return NULL; }
    for (size_t i = 0; i < (size_t)W * (size_t)H; i++) px[i * 4 + 3] = 255;   /* (nothing drawn: black) */
    *w = W;
    *h = H;
    return px;
}

int romart_field_sprite(int character, int frame, int flip, uint8_t *rgba) {
    if (character < 0 || character > 10 || frame < 0 || frame > 8 || !m68k_rom ||
        m68k_rom_size < PAL_CHARACTERS + 32 || m68k_rom_size < FIELD_ART_PTRS + 44) return 0;
    uint32_t art = rom32(FIELD_ART_PTRS + (uint32_t)character * 4) + (uint32_t)frame * FIELD_FRAME;
    if (art < 0x200 || art + FIELD_FRAME > m68k_rom_size) return 0;
    uint8_t pal[16][3];
    for (int i = 0; i < 16; i++) {                        /* ----bbb-ggg-rrr- */
        unsigned c = (unsigned)(m68k_rom[PAL_CHARACTERS + i * 2] << 8 | m68k_rom[PAL_CHARACTERS + i * 2 + 1]);
        pal[i][0] = (uint8_t)((c >> 1 & 7) * 255 / 7);
        pal[i][1] = (uint8_t)((c >> 5 & 7) * 255 / 7);
        pal[i][2] = (uint8_t)((c >> 9 & 7) * 255 / 7);
    }
    for (int t = 0; t < 8; t++) {
        int tx = t / 4, ty = t % 4;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                uint8_t b = m68k_rom[art + (uint32_t)(t * 32 + y * 4 + x / 2)];
                int v = x & 1 ? b & 15 : b >> 4, X = tx * 8 + x;
                if (flip) X = ROMART_FIELD_W - 1 - X;
                uint8_t *o = &rgba[((ty * 8 + y) * ROMART_FIELD_W + X) * 4];
                o[0] = pal[v][0]; o[1] = pal[v][1]; o[2] = pal[v][2]; o[3] = v ? 255 : 0;
            }
    }
    return 1;
}
