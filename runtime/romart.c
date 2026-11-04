/*
 * Art from the player's ROM: see romart.h.
 */
#include "romart.h"
#include "m68k.h"

#include <string.h>

#define PORTRAIT_PTRS    0x6A4B0u     /* DialoguePortraitArtPtrs: 1 Chaz .. $B Seth, Nemesis art */
#define PORTRAIT_LAYOUT  0x2A2B36u    /* 6x6 name-table words, tiles $55C.., palette line 2 */
#define PORTRAIT_FIRST   0x55C
#define PAL_CHARACTERS   0x9F2FCu     /* Pal_Init line 2: the characters' palette */

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
