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
 *     --replay TRACE     replay a BizHawk reference trace (tools/bizhawk/record.lua):
 *                        its input and lag per logical frame, checking memory
 *                        hashes; stops at the first difference and dumps the
 *                        port's memory next to the trace
 *     --keep-going       with --replay: count differing frames instead of stopping
 *     --wav PATH         write the audio (stereo 16-bit, 53,267 Hz) to a WAV file
 *     --ymlog PATH       log every YM2612/PSG write ("sample chip reg value")
 *     --music ID:F       jukebox: at frame F stop all sound, then request sound ID
 *                        (hex, e.g. 84) the way the game does, via Sound_Index ($FF500A)
 *     --dump-at N        dump raw RAM/VRAM/CRAM/VSRAM at logical frame N
 *                        (next to the trace, or build/bizhawk/port.*)
 *
 * Prints game-mode changes and, at the end, a RAM checksum. Built with
 * PSIV_TRACE it also prints the hottest code blocks.
 */
#include "bus.h"
#include "frame.h"
#include "m68k.h"
#include "sha256.h"
#include "sound.h"
#include "vdp.h"

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
static int music_id = -1;
static uint64_t music_frame;
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

/* ------------------------------------------------------------------ audio */
static FILE *wav;
static uint32_t wav_samples;
static int32_t audio_peak;

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void wav_header(FILE *f, uint32_t samples) {
    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    le32(hdr + 4, samples * 4 + 36);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    le32(hdr + 16, 16);
    hdr[20] = 1;                       /* PCM */
    hdr[22] = 2;                       /* stereo */
    le32(hdr + 24, SOUND_RATE);
    le32(hdr + 28, SOUND_RATE * 4);
    hdr[32] = 4;
    hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    le32(hdr + 40, samples * 4);
    fseek(f, 0, SEEK_SET);
    fwrite(hdr, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

void platform_audio(const int16_t *samples, int count) {
    for (int i = 0; i < count * 2; i++) {
        int32_t a = samples[i] < 0 ? -samples[i] : samples[i];
        if (a > audio_peak) audio_peak = a;
    }
    if (wav) {
        fwrite(samples, 4, (size_t)count, wav);           /* little-endian hosts */
        wav_samples += (uint32_t)count;
    }
}

/* ------------------------------------------------------------------ report */
static void report(void) {
    uint32_t hash = 2166136261u;                       /* FNV-1a over work RAM */
    for (int i = 0; i < 0x10000; i++) hash = (hash ^ m68k_ram[i]) * 16777619u;
    printf("frames %llu, RAM FNV-1a %08X, audio peak %d\n", (unsigned long long)frame_count, hash, audio_peak);
    if (wav) {
        wav_header(wav, wav_samples);
        fclose(wav);
        wav = NULL;
    }
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

/* ------------------------------------------------------------------ reference replay */
static FILE *replay;
static const char *replay_path;
static uint64_t logical, dump_at, mismatched_frames;
static int keep_going;

/* memory regions serialized the way BizHawk's domains present them */
static void region_bytes(int r, uint8_t *buf, size_t *len) {
    switch (r) {
    case 0: memcpy(buf, m68k_ram, 0x10000); *len = 0x10000; break;
    case 1: memcpy(buf, vdp.vram, 0x10000); *len = 0x10000; break;
    case 2:
        for (int i = 0; i < 64; i++) { buf[i * 2] = (uint8_t)(vdp.cram[i] >> 8); buf[i * 2 + 1] = (uint8_t)vdp.cram[i]; }
        *len = 128;
        break;
    case 3:
        for (int i = 0; i < 40; i++) { buf[i * 2] = (uint8_t)(vdp.vsram[i] >> 8); buf[i * 2 + 1] = (uint8_t)vdp.vsram[i]; }
        *len = 80;
        break;
    }
}

static void dump_state(const char *prefix) {
    static const char *names[4] = {"ram", "vram", "cram", "vsram"};
    static uint8_t buf[0x10000];
    char path[512];
    for (int r = 0; r < 4; r++) {
        size_t n;
        region_bytes(r, buf, &n);
        snprintf(path, sizeof path, "%s.%s.bin", prefix, names[r]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(buf, 1, n, f); fclose(f); }
    }
    printf("dumped %s.{ram,vram,cram,vsram}.bin\n", prefix);
}

static void dump_prefix(char *out, size_t n, const char *tag) {
    if (replay_path) {
        const char *dot = strrchr(replay_path, '.');
        size_t base = dot ? (size_t)(dot - replay_path) : strlen(replay_path);
        snprintf(out, n, "%.*s.%s", (int)base, replay_path, tag);
    } else {
        snprintf(out, n, "build/bizhawk/%s", tag);
    }
}

#define SOUND_LO 0x5000u          /* sound driver RAM, compared as a soft check */
#define SOUND_HI 0x5600u
static char cur_line[2048];
static int have_cur, cur_lag_delivered;
static uint64_t sound_mismatches, first_sound_mismatch;

static int read_line(void) {
    do {
        if (!fgets(cur_line, sizeof cur_line, replay)) return 0;
    } while (cur_line[0] == '#');
    return 1;
}

/* returns 0 when the trace has ended */
static int replay_step(void) {
    char line[2048], *tok[40];
    int nt = 0;
    if (!have_cur) {
        if (!read_line()) return 0;
        have_cur = 1;
        cur_lag_delivered = 0;
    }
    memcpy(line, cur_line, sizeof line);
    for (char *t = strtok(line, " |\r\n"); t && nt < 40; t = strtok(NULL, " |\r\n")) tok[nt++] = t;
    if (nt < 39) { fprintf(stderr, "bad trace line at logical frame %llu (need trace v3)\n", (unsigned long long)logical); exit(2); }
    uint64_t n = strtoull(tok[0], NULL, 10);
    int lag = atoi(tok[1]);
    uint32_t sp = (uint32_t)strtoul(tok[3], NULL, 16);
    char **hashes = tok + 4;
    if (n != logical) { fprintf(stderr, "trace out of step: %llu vs %llu\n", (unsigned long long)n, (unsigned long long)logical); exit(2); }

    /* normally this frame's lag VInts already ran right after the previous
     * VInt (scheduled below); only the first line has nowhere earlier to go */
    if (!cur_lag_delivered)
        for (int i = 0; i < lag; i++) frame_lag_vint();
    bus_pad[0] = (uint8_t)atoi(tok[2]);

    /* look ahead: the next frame's lag runs at the start of its main-code work */
    char hashes_copy[34][17], sound_want[17];
    for (int i = 0; i < 34; i++) { strncpy(hashes_copy[i], hashes[i], 16); hashes_copy[i][16] = 0; }
    strncpy(sound_want, hashes[34], 16);
    sound_want[16] = 0;
    have_cur = read_line();
    if (have_cur) {
        char peek[2048], *t1, *t2;
        memcpy(peek, cur_line, sizeof peek);
        t1 = strtok(peek, " |\r\n");
        t2 = t1 ? strtok(NULL, " |\r\n") : NULL;
        frame_pending_lag = t2 ? atoi(t2) : 0;
        cur_lag_delivered = 1;
    }

    static uint8_t buf[0x10000];
    static const char *names[4] = {"RAM", "VRAM", "CRAM", "VSRAM"};
    int bad = 0;
    char got[17];
    if ((A(7) & 0xFFFF) != sp) {
        printf("logical frame %llu (port frame %llu): MISMATCH\n  stack pointer  port $%04X  reference $%04X\n",
               (unsigned long long)logical, (unsigned long long)frame_count, A(7) & 0xFFFF, sp);
        bad = 1;
    }
    for (int r = 0; r < 4; r++) {
        size_t len;
        region_bytes(r, buf, &len);
        int blocks = r < 2 ? 16 : 1;
        for (int b = 0; b < blocks; b++) {
            const char *want = hashes_copy[r == 0 ? b : r == 1 ? 16 + b : r == 2 ? 32 : 33];
            size_t bl = blocks == 16 ? 0x1000 : len;
            size_t lo = (size_t)b * bl, hi = lo + bl;
            if (r == 0 && sp > lo && sp < hi) lo = sp;          /* live stack only */
            if (r == 0 && lo == SOUND_LO) lo = SOUND_HI;         /* sound driver: soft check below */
            sha256_hex16(buf + lo, hi - lo, got);
            if (strcmp(got, want)) {
                int quiet = keep_going && mismatched_frames >= 3;
                if (!bad && !quiet) printf("logical frame %llu (port frame %llu): MISMATCH\n",
                                           (unsigned long long)logical, (unsigned long long)frame_count);
                if (!quiet) {
                    printf("  %s", names[r]);
                    if (blocks == 16) printf(" $%04X-$%04X", b * 0x1000, b * 0x1000 + 0xFFF);
                    printf("  port %s  reference %s\n", got, want);
                }
                bad = 1;
            }
        }
    }
    sha256_hex16(m68k_ram + SOUND_LO, SOUND_HI - SOUND_LO, got);
    if (strcmp(got, sound_want)) {
        if (!sound_mismatches) first_sound_mismatch = logical;
        sound_mismatches++;
    }
    if (bad && keep_going) {
        mismatched_frames++;
        return 1;
    }
    if (bad) {
        char prefix[512], tag[64];
        snprintf(tag, sizeof tag, "port%llu", (unsigned long long)logical);
        dump_prefix(prefix, sizeof prefix, tag);
        dump_state(prefix);
        printf("re-record with PSIV_DUMP_AT = %llu for the reference side\n", (unsigned long long)logical);
        exit(1);
    }
    return 1;
}

void platform_frame(const uint32_t *pixels, int width, int height) {
    uint64_t f = frame_count;
    if (frame_vint_will_fire()) {
        logical++;
        if (dump_at && logical == dump_at) {
            char prefix[512];
            dump_prefix(prefix, sizeof prefix, "port");
            dump_state(prefix);
        }
        if (replay && !replay_step()) {
            if (sound_mismatches)
                printf("sound driver RAM differed on %llu logical frames (first: %llu); "
                       "lag timing; the driver itself is checked by tools/soundcheck.py --bizhawk\n",
                       (unsigned long long)sound_mismatches, (unsigned long long)first_sound_mismatch);
            if (mismatched_frames)
                printf("trace ended: %llu of %llu logical frames differ\n",
                       (unsigned long long)mismatched_frames, (unsigned long long)(logical - 1));
            else
                printf("trace ended: all %llu logical frames match\n", (unsigned long long)(logical - 1));
            report();
            exit(0);
        }
    }
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
    /* Sound_Index: a request only beats what is playing if its priority is at
     * least as high, so stop everything ($FE) first, then ask for the song */
    if (music_id >= 0 && f == music_frame) m68k_ram[0x500A] = 0xFE;
    if (music_id >= 0 && f == music_frame + 2) m68k_ram[0x500A] = (uint8_t)music_id;
    if (!replay) {
        uint8_t keys = 0;
        for (int i = 0; i < npress; i++)
            if (f >= presses[i].frame && f < presses[i].frame + presses[i].len) keys |= presses[i].keys;
        bus_pad[0] = keys;
    }
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
        } else if (!strcmp(argv[i], "--replay") && i + 1 < argc) {
            replay_path = argv[++i];
            replay = fopen(replay_path, "r");
            if (!replay) { fprintf(stderr, "cannot open %s\n", replay_path); return 1; }
            frame_limit = UINT64_MAX;
        } else if (!strcmp(argv[i], "--dump-at") && i + 1 < argc) {
            dump_at = strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--keep-going")) {
            keep_going = 1;
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = fopen(argv[++i], "wb");
            if (wav) wav_header(wav, 0);
        } else if (!strcmp(argv[i], "--music") && i + 1 < argc) {
            char *e;
            music_id = (int)strtol(argv[++i], &e, 16);
            music_frame = *e == ':' ? strtoull(e + 1, NULL, 10) : 30;
        } else if (!strcmp(argv[i], "--ymlog") && i + 1 < argc) {
            FILE *lf = fopen(argv[++i], "w");
            if (lf) sound_set_log(lf);
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
