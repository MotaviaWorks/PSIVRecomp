/*
 * Headless front end: run the game with no window, scripted input and PNG
 * screenshots. Used for testing (and by Claude to see what the port draws).
 *
 *   psiv_headless [options]
 *     --rom PATH         ROM (default rom/psiv.bin)
 *     --frames N         stop after N frames (default 600)
 *     --press F:KEYS:N   hold KEYS from frame F for N frames; KEYS from
 *                        U D L R A B C S (S = Start). Repeatable.
 *     --shot F:PATH      save frame F as a PNG. Repeatable.
 *
 * Prints game-mode changes and, at the end, a RAM checksum. Built with
 * PSIV_TRACE it also prints the hottest code blocks.
 */
#include "bus.h"
#include "frame.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE 0xEF00u

typedef struct { uint64_t frame, len; uint8_t keys; } Press;
typedef struct { uint64_t frame; const char *path; } Shot;
static Press presses[256];
static Shot shots[64];
static int npress, nshot;
static uint64_t frame_limit = 600;
static uint16_t last_mode = 0xFFFF;

/* ------------------------------------------------------------------ PNG (stored deflate, no zlib) */
static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c ^= 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hdr[8];
    be32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_update(0, hdr + 4, 4);
    c = crc32_update(c, data, n);
    be32(hdr, c);
    fwrite(hdr, 1, 4, f);
}
static void save_png(const char *path, const uint32_t *px, int w, int h) {
    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_n), *z = malloc(raw_n + raw_n / 65535 * 5 + 64);
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0;                                   /* filter: none */
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * VDP_MAX_W + x];
            raw[o++] = (uint8_t)(c >> 16); raw[o++] = (uint8_t)(c >> 8); raw[o++] = (uint8_t)c;
        }
    }
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw_n;) {
        size_t n = raw_n - pos > 65535 ? 65535 : raw_n - pos;
        z[zn++] = pos + n == raw_n;                     /* BFINAL, stored */
        z[zn++] = (uint8_t)n; z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)~n; z[zn++] = (uint8_t)(~n >> 8);
        memcpy(z + zn, raw + pos, n);
        zn += n; pos += n;
    }
    be32(z + zn, b << 16 | a); zn += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); free(raw); free(z); return; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}

/* ------------------------------------------------------------------ report */
static void report(void) {
    uint32_t hash = 2166136261u;                       /* FNV-1a over work RAM */
    for (int i = 0; i < 0x10000; i++) hash = (hash ^ m68k_ram[i]) * 16777619u;
    printf("frames %llu, RAM FNV-1a %08X\n", (unsigned long long)frame_count, hash);
}

#ifdef PSIV_TRACE
#define HOT 4096
uint32_t m68k_trace_pc;
static uint32_t hot_pc[HOT];
static uint64_t hot_n[HOT], blocks;
void m68k_trace(uint32_t pc) {
    uint32_t slot = (pc >> 1) % HOT;
    m68k_trace_pc = pc;
    if (hot_pc[slot] != pc) { hot_pc[slot] = pc; hot_n[slot] = 0; }
    hot_n[slot]++;
    blocks++;
}
static void report_hot(void) {
    printf("%llu blocks; hottest:\n", (unsigned long long)blocks);
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int i = 0; i < HOT; i++)
            if (hot_n[i] && (best < 0 || hot_n[i] > hot_n[best])) best = i;
        if (best < 0) break;
        printf("  $%06X  %llu\n", hot_pc[best], (unsigned long long)hot_n[best]);
        hot_n[best] = 0;
    }
}
#endif

void platform_frame(const uint32_t *pixels, int width, int height) {
    uint64_t f = frame_count;
    uint16_t mode = (uint16_t)(m68k_ram[GAME_MODE] << 8 | m68k_ram[GAME_MODE + 1]);
    if (mode != last_mode) {
        printf("frame %llu: game mode %u\n", (unsigned long long)f, mode / 4);
        last_mode = mode;
    }
    for (int i = 0; i < nshot; i++)
        if (shots[i].frame == f) {
            save_png(shots[i].path, pixels, width, height);
            printf("frame %llu: saved %s (%dx%d)\n", (unsigned long long)f, shots[i].path, width, height);
        }
    uint8_t keys = 0;
    for (int i = 0; i < npress; i++)
        if (f >= presses[i].frame && f < presses[i].frame + presses[i].len) keys |= presses[i].keys;
    bus_pad[0] = keys;
    if (f + 1 >= frame_limit) {
        report();
#ifdef PSIV_TRACE
        report_hot();
#endif
        exit(0);
    }
}

static uint8_t parse_keys(const char *s) {
    uint8_t k = 0;
    for (; *s && *s != ':'; s++) {
        switch (*s) {
        case 'U': k |= PAD_UP; break;    case 'D': k |= PAD_DOWN; break;
        case 'L': k |= PAD_LEFT; break;  case 'R': k |= PAD_RIGHT; break;
        case 'A': k |= PAD_A; break;     case 'B': k |= PAD_B; break;
        case 'C': k |= PAD_C; break;     case 'S': k |= PAD_START; break;
        }
    }
    return k;
}

int main(int argc, char **argv) {
    const char *rom_path = "rom/psiv.bin";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npress < 256) {
            char *s = argv[++i], *e;
            presses[npress].frame = strtoull(s, &e, 10);
            if (*e == ':') e++;
            presses[npress].keys = parse_keys(e);
            e = strchr(e, ':');
            presses[npress].len = e ? strtoull(e + 1, NULL, 10) : 1;
            npress++;
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshot < 64) {
            char *s = argv[++i], *e;
            shots[nshot].frame = strtoull(s, &e, 10);
            shots[nshot].path = *e == ':' ? e + 1 : "shot.png";
            nshot++;
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", rom_path); return 1; }
    static uint8_t rom[0x400000];
    uint32_t n = (uint32_t)fread(rom, 1, sizeof rom, f);
    fclose(f);
    bus_init(rom, n);

    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    m68k_call(rd32(4));
    printf("reset handler returned (unexpected)\n");
    report();
    return 1;
}
