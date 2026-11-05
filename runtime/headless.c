/*
 * Headless front end: run the game with no window, scripted input and PNG
 * screenshots. Used for testing (and by Claude to see what the port draws).
 *
 *   psiv_headless [options]
 *     --rom PATH         ROM (default rom/psiv.bin)
 *     --frames N         stop after N frames (default 600)
 *     --press F:KEYS:N   hold KEYS from frame F for N frames; KEYS from
 *                        U D L R A B C S (S = Start). Repeatable.
 *     --shot F:PATH      save frame F as a PNG. Repeatable. With --movie, F is a
 *                        logical frame.
 *     --movie FILE       input movie by logical frame (the frames where the game
 *                        reads the pad), so the same file drives BizHawk
 *                        (tools/bizhawk/movie.lua) identically. Lines:
 *                          L KEYS [N]    hold KEYS from logical frame L for N frames (default 1)
 *                          +D KEYS [N]   the same, D frames after the previous line's start
 *                        KEYS from U D L R A B C S, or - for none; # starts a comment
 *     --shot-every N:DIR with --movie or --replay: a PNG every N logical frames into DIR
 *     --shot-range F1-F2 limit --shot-every to these logical frames
 *     --depth L[:S]      battle depth at level L (0 off, 1 subtle, 2 strong); shots
 *                        of battle frames show it, composed at S x (default 2)
 *     --layers F:PREFIX  frame F's layers as PNGs, plane A's tiles and the sprite
 *                        table as text (for working out what the game draws where)
 *     --sram FILE        battery save to load before power-on (8 KB, the odd
 *                        bytes of $200001-$203FFF, as saves/psiv.srm; or an
 *                        emulator's .srm: BizHawk, Gens, Kega Fusion)
 *     --sram-out FILE    write the battery save when the run ends
 *     --where L,L,...    print the 68000 call chain at these logical frames
 *     --replay TRACE     replay a BizHawk reference trace (tools/bizhawk/record.lua):
 *                        its input and lag per logical frame, checking memory
 *                        hashes; stops at the first difference and dumps the
 *                        port's memory next to the trace
 *     --keep-going       with --replay: count differing frames instead of stopping
 *     --wav PATH         write the audio (stereo 16-bit, 53,267 Hz) to a WAV file
 *     --ymlog PATH       log every YM2612/PSG write ("sample chip reg value")
 *     --music ID:F       jukebox: at frame F stop all sound, then request sound ID
 *                        (hex, e.g. 84) the way the game does, via Sound_Index ($FF500A)
 *     --algol            the music on Algol, the port's modern sound chip (algol.h)
 *     --algol-bank ROM   with the instruments of a SNES game you own (snesbank.h)
 *     --custom-music DIR your own music files in place of the game's songs (music.h)
 *     --fpview S         first-person dungeons (fpview.h); shots at S x
 *     --click F:X,Y[,R]  with --movie: a mouse click at logical frame F on frame
 *                        pixel (X, Y) -- click to walk (walkto.h), battle menus
 *                        (battleui.h); R: the right button; repeatable
 *     --dump-at N        dump raw RAM/VRAM/CRAM/VSRAM at logical frame N
 *                        (next to the trace, or build/bizhawk/port.*)
 *     --rumble-log       print each rumble effect as it's detected (rumble.h)
 *
 * Prints game-mode changes and, at the end, a RAM checksum. Built with
 * PSIV_TRACE it also prints the hottest code blocks.
 */
#include "bus.h"
#include "diag.h"
#include "depth.h"
#include "hdtext.h"
#include "panels.h"
#include "battleui.h"
#include "algol.h"
#include "music.h"
#include "fpview.h"
#include "walkto.h"
#include "widemap.h"
#include "rumble.h"
#include "sfx.h"
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
static struct { uint64_t frame; int x, y, right; } clicks[16];   /* --click */
static int nclick;
static Press *movie;                 /* --movie: events by logical frame */
static int nmovie;
static uint64_t movie_end;           /* last logical frame the movie touches */
static uint64_t shot_every;
static const char *shot_dir;
static int trace_pad;                /* --trace-pad: print movie input and the game's joypad bytes */
static uint64_t frame_limit = 600;
static int music_id = -1;
static uint64_t music_frame;
static uint64_t test_fatal_frame;          /* --test-fatal: exercise the crash report */
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
static void save_png_stride(const char *path, const uint32_t *px, int w, int h, int stride) {
    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_n), *z = malloc(raw_n + raw_n / 65535 * 5 + 64);
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0;                                   /* filter: none */
        for (int x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * (size_t)stride + (size_t)x];
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
static void save_png(const char *path, const uint32_t *px, int w, int h) {
    save_png_stride(path, px, w, h, VDP_MAX_W);
}

/* --depth L[:S]: battle depth at level L; screenshots of battle frames show
 * it, composed at S times the game's resolution (default 2) */
static int depth_scale, depth_active, text_scale = 3, panels_on, out_w, fp_scale;
static uint64_t shot_from, shot_to;              /* --shot-range F1-F2 */
static uint32_t depth_buf[VDP_MAX_W * 4 * VDP_MAX_H * 4];

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
void platform_fatal(const char *text) {
    fflush(stdout);
    printf("FATAL at port frame %llu: %s", (unsigned long long)frame_count, text);
    fflush(stdout);
    exit(1);
}

static const char *sram_out;           /* --sram-out */

static void report(void) {
    if (sram_out) {
        FILE *sf = fopen(sram_out, "wb");
        if (sf) { fwrite(bus_sram, 1, sizeof bus_sram, sf); fclose(sf); printf("wrote %s\n", sram_out); }
    }
    uint32_t hash = 2166136261u;                       /* FNV-1a over work RAM */
    for (int i = 0; i < 0x10000; i++) hash = (hash ^ m68k_ram[i]) * 16777619u;
    printf("frames %llu, RAM FNV-1a %08X, audio peak %d\n", (unsigned long long)frame_count, hash, audio_peak);
    if (wav) {
        wav_header(wav, wav_samples);
        fclose(wav);
        wav = NULL;
    }
}

static uint64_t hot_from;              /* --hot-from: count hot blocks from this logical frame */
static uint64_t where_at[16];          /* --where: print the 68000 call chain at these logical frames */
static int nwhere;

static void print_where(uint64_t at) {
    uint32_t calls[24];
    int n = diag_callstack(calls, 24);
    printf("logical %llu: waiting in", (unsigned long long)at);
    for (int i = 0; i < n; i++) {
        uint32_t off;
        const char *fn = diag_function_name(calls[i] - 2, &off);
        printf("%s $%06X %s+$%X", i ? " <-" : "", calls[i], fn, off + 2);
    }
    printf("\n");
}
/* --watch N:PC,PC,...: during the work leading up to logical frame N, print
 * the registers each time one of these instructions is about to run (trace
 * build only; tools/bizhawk/record.lua does the same with PSIV_WATCH) */
static uint32_t watch_pc[64];
static int watch_n;
static uint64_t watch_frame;

#ifdef PSIV_TRACE
static void watch_check(uint32_t pc);
#define HOT 4096
uint32_t m68k_trace_pc;
static uint32_t hot_pc[HOT];
static uint64_t hot_n[HOT], blocks;
static void reset_hot(void) { memset(hot_n, 0, sizeof hot_n); blocks = 0; }
void m68k_trace(uint32_t pc) {
    uint32_t slot = (pc >> 1) % HOT;
    m68k_trace_pc = pc;
    if (hot_pc[slot] != pc) { hot_pc[slot] = pc; hot_n[slot] = 0; }
    hot_n[slot]++;
    blocks++;
    if (watch_n) watch_check(pc);
}
static void report_hot(void) {
    printf("%llu blocks; hottest outside the sound driver ($D0000+):\n", (unsigned long long)blocks);
    for (int k = 0; k < 16; k++) {
        int best = -1;
        for (int i = 0; i < HOT; i++)
            if (hot_n[i] && hot_pc[i] < 0xD0000 && (best < 0 || hot_n[i] > hot_n[best])) best = i;
        if (best < 0) break;
        uint32_t off;
        const char *fn = diag_function_name(hot_pc[best], &off);
        printf("  $%06X  %-10llu %s+$%X\n", hot_pc[best], (unsigned long long)hot_n[best], fn, off);
        hot_n[best] = 0;
    }
}
#endif

/* ------------------------------------------------------------------ reference replay */
static FILE *replay;
static const char *replay_path;
static uint64_t logical, dump_at, mismatched_frames;

/* --rumble-log: each effect with the logical frame it came on */
static void rumble_print(const char *line) {
    printf("logical %llu: rumble: %s\n", (unsigned long long)logical, line);
}

#ifdef PSIV_TRACE
static void watch_check(uint32_t pc) {
    if (logical != watch_frame) return;
    for (int i = 0; i < watch_n; i++) {
        if (watch_pc[i] != pc) continue;
        printf("watch %llu $%06X", (unsigned long long)logical, pc);
        for (int r = 0; r < 8; r++) printf(" d%d=%08X", r, D(r));
        for (int r = 0; r < 7; r++) printf(" a%d=%08X", r, A(r));
        printf(" sp=%08X\n", A(7));
        return;
    }
}
#endif
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
    snprintf(path, sizeof path, "%s.regs.bin", prefix);       /* VDP registers, for analysis only */
    FILE *rf = fopen(path, "wb");
    if (rf) { fwrite(vdp.reg, 1, sizeof vdp.reg, rf); fwrite(vdp.sat_cache, 1, sizeof vdp.sat_cache, rf); fclose(rf); }
    printf("dumped %s.{ram,vram,cram,vsram,regs}.bin\n", prefix);
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
/* VDP_Status_Reg: VInt's raw copy of the status word. The game only tests its
 * vblank bit; the collision bit depends on how an emulator treats off-screen
 * sprite pixels, so both sides leave the word out of the hash */
#define STATUS_LO 0xEF22u
#define STATUS_HI 0xEF24u
/* the stack's 4 KB block also holds game variables up to Vehicle_Stats
 * ($4700-$47FF); from here up to sp is dead stack. Trace v5 hashes the
 * variables and the live stack ("a+b"); v4 hashed only from sp up */
#define STACK_DEAD_LO 0x4800u
#define TRACE_LINE 16384
static char cur_line[TRACE_LINE];
static int have_cur, cur_lag_delivered, trace_version = 4;
static uint64_t sound_mismatches, first_sound_mismatch, hv_mismatches;

static int read_line(void) {
    do {
        if (!fgets(cur_line, sizeof cur_line, replay)) return 0;
        const char *v = strstr(cur_line, "# psiv reference trace v");
        if (v) trace_version = atoi(v + 24);
    } while (cur_line[0] == '#');
    return 1;
}

/* trace v5: "hv:1A2B,3C4D" (or "hv:-") lists the beam-counter values the game
 * read during the work leading up to that frame; give them to the VDP before
 * that work starts */
static void feed_hv(const char *line) {
    const char *p = strstr(line, "hv:");
    static uint16_t v[1024];
    int n = 0;
    if (!p) { vdp_hv_feed(v, 0); return; }
    for (p += 3; *p && *p != '-' && n < 1024; ) {
        char *end;
        unsigned long x = strtoul(p, &end, 16);
        if (end == p) break;
        v[n++] = (uint16_t)x;
        p = *end == ',' ? end + 1 : end;
    }
    int off = vdp_hv_feed(v, n);
    if (off && hv_mismatches++ < 3)
        printf("logical frame %llu: the port read %d %s beam-counter value%s than the reference\n",
               (unsigned long long)logical, off < 0 ? -off : off, off < 0 ? "more" : "fewer",
               (off == 1 || off == -1) ? "" : "s");
}

/* returns 0 when the trace has ended */
static int replay_step(void) {
    char line[TRACE_LINE], *tok[48];
    int nt = 0;
    if (!have_cur) {
        if (!read_line()) return 0;
        have_cur = 1;
        cur_lag_delivered = 0;
        feed_hv(cur_line);
    }
    memcpy(line, cur_line, sizeof line);
    for (char *t = strtok(line, " |\r\n"); t && nt < 48; t = strtok(NULL, " |\r\n")) tok[nt++] = t;
    if (nt < 39) { fprintf(stderr, "bad trace line at logical frame %llu (need trace v4)\n", (unsigned long long)logical); exit(2); }
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
    char hashes_copy[34][40], sound_want[17];
    for (int i = 0; i < 34; i++) snprintf(hashes_copy[i], sizeof hashes_copy[i], "%s", hashes[i]);
    strncpy(sound_want, hashes[34], 16);
    sound_want[16] = 0;
    have_cur = read_line();
    if (have_cur) {
        char peek[TRACE_LINE], *t1, *t2;
        memcpy(peek, cur_line, sizeof peek);
        feed_hv(cur_line);
        t1 = strtok(peek, " |\r\n");
        t2 = t1 ? strtok(NULL, " |\r\n") : NULL;
        frame_pending_lag = t2 ? atoi(t2) : 0;
        cur_lag_delivered = 1;
    }

    static uint8_t buf[0x10000];
    static const char *names[4] = {"RAM", "VRAM", "CRAM", "VSRAM"};
    int bad = 0;
    char got[40];
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
            if (r == 0 && lo == SOUND_LO) lo = SOUND_HI;         /* sound driver: soft check below */
            if (r == 0 && sp > lo && sp < hi && trace_version < 5) lo = sp;   /* v4: live stack only */
            if (r == 0 && sp > lo && sp < hi) {                  /* variables + live stack */
                size_t var_hi = sp < STACK_DEAD_LO ? sp : STACK_DEAD_LO;
                char vars[17], live[17];
                sha256_hex16(buf + lo, var_hi - lo, vars);
                sha256_hex16(buf + sp, hi - sp, live);
                snprintf(got, sizeof got, "%s+%s", vars, live);
            } else if (r == 0 && lo <= STATUS_LO && hi >= STATUS_HI) {  /* skip VDP_Status_Reg */
                char a[17], b[17];
                sha256_hex16(buf + lo, STATUS_LO - lo, a);
                sha256_hex16(buf + STATUS_HI, hi - STATUS_HI, b);
                snprintf(got, sizeof got, "%s+%s", a, b);
            } else {
                sha256_hex16(buf + lo, hi - lo, got);
            }
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

/* --layers F:PREFIX: frame F's layers as separate PNGs (magenta = transparent;
 * dark grey = outside the window), a map of which layer each pixel shows, and
 * the sprite table as text. F is a logical frame with --movie or --replay. */
typedef struct { uint64_t frame; const char *prefix; } LayerDump;
static LayerDump layer_dumps[16];
static int nlayer_dumps;

/* --ramlog ADDR[.b|.w|.l],...: print these RAM values (hex, $FFxxxx) on each
 * logical frame where one of them changes */
static uint32_t ramlog_addr[32], ramlog_last[32];
static char ramlog_size[32];
static int ramlog_n, ramlog_primed;

static uint32_t ramlog_read(int i) {
    uint32_t a = ramlog_addr[i] & 0xFFFF;
    const uint8_t *r = m68k_ram;
    switch (ramlog_size[i]) {
    case 'b': return r[a];
    case 'l': return (uint32_t)r[a] << 24 | r[a + 1] << 16 | r[a + 2] << 8 | r[a + 3];
    default:  return (uint32_t)(r[a] << 8 | r[a + 1]);
    }
}

static void ramlog_frame(uint64_t when) {
    int changed = !ramlog_primed;
    uint32_t v[32];
    for (int i = 0; i < ramlog_n; i++) {
        v[i] = ramlog_read(i);
        if (v[i] != ramlog_last[i]) changed = 1;
    }
    ramlog_primed = 1;
    if (!changed) return;
    printf("ram %llu:", (unsigned long long)when);
    for (int i = 0; i < ramlog_n; i++) {
        int w = ramlog_size[i] == 'b' ? 2 : ramlog_size[i] == 'l' ? 8 : 4;
        printf(" %04X=%0*X%s", ramlog_addr[i] & 0xFFFF, w, v[i], v[i] != ramlog_last[i] ? "*" : "");
        ramlog_last[i] = v[i];
    }
    printf("\n");
}

static void dump_layers(const char *prefix, uint64_t when, int w, int h) {
    static uint32_t img[VDP_MAX_W * VDP_MAX_H];
    static const char *names[5] = {"b", "a", "w", "s", "top"};
    char path[600];
    for (int layer = 0; layer < 5; layer++) {
        for (int y = 0; y < h; y++) {
            const VdpLine *l = &frame_lines[y];
            for (int x = 0; x < w; x++) {
                uint8_t b = l->plane_b[x], a = l->win_in[x] ? l->window[x] : l->plane_a[x], s = l->sprite[x];
                uint32_t c;
                switch (layer) {
                case 0: c = vdp_color((b ? b : l->backdrop) & 0x3F); break;
                case 1: c = l->plane_a[x] ? vdp_color(l->plane_a[x] & 0x3F) : 0xFFFF00FFu; break;
                case 2: c = !l->win_in[x] ? 0xFF202020u : l->window[x] ? vdp_color(l->window[x] & 0x3F) : 0xFFFF00FFu; break;
                case 3: c = s ? vdp_color(s & 0x3F) : 0xFFFF00FFu; break;
                default: {
                    /* which layer shows: blue B, green A, yellow window, red sprite; bright = high priority */
                    c = 0xFF000000u;
                    int win = l->win_in[x];
                    if (b && !(b & 0x80)) c = 0xFF000080u;
                    if (a && !(a & 0x80)) c = win ? 0xFF808000u : 0xFF008000u;
                    if (s && !(s & 0x80)) c = 0xFF800000u;
                    if (b & 0x80) c = 0xFF4040FFu;
                    if (a & 0x80) c = win ? 0xFFFFFF40u : 0xFF40FF40u;
                    if (s & 0x80) c = 0xFFFF4040u;
                }
                }
                img[y * VDP_MAX_W + x] = c;
            }
        }
        snprintf(path, sizeof path, "%s.%s.png", prefix, names[layer]);
        save_png(path, img, w, h);
    }
    snprintf(path, sizeof path, "%s.c.png", prefix);
    save_png(path, frame_pixels, w, h);
    /* plane A's tiles per screen cell (sampled mid-cell): tile number, * = high priority */
    snprintf(path, sizeof path, "%s.plane_a.txt", prefix);
    FILE *t = fopen(path, "w");
    if (!t) return;
    for (int cy = 0; cy < h / 8; cy++) {
        for (int cx = 0; cx < w / 8; cx++) {
            uint16_t e = frame_lines[cy * 8 + 4].plane_a_tile[cx * 8 + 4];
            if (e & 0x7FF) fprintf(t, "%03X%c", e & 0x7FF, e & 0x8000 ? '*' : ' ');
            else fprintf(t, "  . ");
        }
        fprintf(t, "\n");
    }
    fclose(t);
    /* the sprite table, in link order, with the pixels each one put on screen */
    snprintf(path, sizeof path, "%s.sprites.txt", prefix);
    t = fopen(path, "w");
    if (!t) return;
    int h40 = w == 320;
    uint32_t base = (uint32_t)(vdp.reg[5] & (h40 ? 0x7E : 0x7F)) << 9;
    fprintf(t, "# idx  x    y   size  tile  pal pri hf vf  pixels   (sprite table at VRAM $%04X)\n", base);
    int link = 0, count = 0;
    do {
        const uint8_t *s = &vdp.vram[(base + (uint32_t)link * 8) & 0xFFFF];
        const uint8_t *c = &vdp.sat_cache[((uint32_t)link * 8) & (h40 ? 0x3FF : 0x1FF)];
        int sy = ((c[0] << 8 | c[1]) & 0x3FF) - 128, sx = ((s[6] << 8 | s[7]) & 0x1FF) - 128;
        int vs = (c[2] & 3) + 1, hs = (c[2] >> 2 & 3) + 1;
        uint16_t attr = (uint16_t)(s[4] << 8 | s[5]);
        long px = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (frame_lines[y].sprite[x] && frame_lines[y].sprite_no[x] == link) px++;
        fprintf(t, "%4d %4d %4d  %dx%d  $%03X  %d   %d   %d  %d  %6ld\n", link, sx, sy, hs, vs,
                attr & 0x7FF, attr >> 13 & 3, attr >> 15, attr >> 11 & 1, attr >> 12 & 1, px);
        link = c[3] & 0x7F;
    } while (link && ++count < 80);
    fprintf(t, "# planes: A at $%04X, B at $%04X, window at $%04X; size %dx%d cells; "
               "scroll mode H%d V%d; window reg17 %02X reg18 %02X\n",
            (vdp.reg[2] & 0x38) << 10, (vdp.reg[4] & 7) << 13, (vdp.reg[3] & 0x3C) << 10,
            (vdp.reg[16] & 3) == 1 ? 64 : (vdp.reg[16] & 3) == 3 ? 128 : 32,
            (vdp.reg[16] >> 4 & 3) == 1 ? 64 : (vdp.reg[16] >> 4 & 3) == 3 ? 128 : 32,
            vdp.reg[11] & 3, vdp.reg[11] >> 2 & 1, vdp.reg[17], vdp.reg[18]);
    fclose(t);
    printf("layers of %llu: %s.{b,a,w,s,top,c}.png, %s.{plane_a,sprites}.txt\n",
           (unsigned long long)when, prefix, prefix);
}

void platform_frame(const uint32_t *pixels, int width, int height) {
    uint64_t f = frame_count;
    if (frame_vint_will_fire()) {
        logical++;
#ifdef PSIV_TRACE
        if (hot_from && logical == hot_from) reset_hot();
#endif
        for (int i = 0; i < nwhere; i++)
            if (where_at[i] == logical) print_where(logical);
        if (ramlog_n) ramlog_frame(logical);
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
            if (hv_mismatches)
                printf("beam-counter reads differed in number on %llu frames\n",
                       (unsigned long long)hv_mismatches);
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
    /* with a movie or a replay, screenshots and input are by logical frame */
    uint64_t when = (movie || replay) ? logical : f;
    int fresh = !movie || frame_vint_will_fire();      /* a logical frame only counts once */
    /* battle depth and modern text: shots show what the player sees */
    int hs = depth_scale ? depth_scale : fp_scale ? fp_scale : text_scale;
    if (fresh) {
        int pm = panels_on && frame_wide_margin > 0 && width == 320 && panels_field() ? frame_wide_margin : 0;
        int hires = 0;
        out_w = width + 2 * pm;
        if (pm) {                                          /* side panels around the 320 view */
            uint32_t *mid = depth_buf + (size_t)pm * (size_t)hs;
            hdtext_upscale(pixels, VDP_MAX_W, width, height, mid, out_w * hs, hs);
            if (hdtext_enabled() && hdtext_has_text(frame_lines, width, height))
                hdtext_draw(frame_lines, width, height, mid, out_w * hs, hs);
            panels_draw(depth_buf, out_w * hs, hs, pm, width, height);
            hires = 1;
        } else {
            hires = fpview_compose(frame_lines, pixels, width, height, depth_buf, width * hs, hs)
                    || (depth_scale ? depth_compose(frame_lines, width, height, depth_buf, width * hs, hs) : 0);
            if (hdtext_enabled() && hdtext_has_text(frame_lines, width, height)) {
                if (!hires) hdtext_upscale(pixels, VDP_MAX_W, width, height, depth_buf, width * hs, hs);
                hdtext_draw(frame_lines, width, height, depth_buf, width * hs, hs);
                hires = 1;
            }
            if (battleui_active()) {                       /* modern battle menus */
                if (!hires) hdtext_upscale(pixels, VDP_MAX_W, width, height, depth_buf, width * hs, hs);
                battleui_draw(depth_buf, width * hs, hs, width, height);
                hires = 1;
            }
        }
        depth_active = hires;
    }
    int sw = depth_active ? out_w * hs : width, sh = depth_active ? height * hs : height;
    for (int i = 0; i < nshot && fresh; i++)
        if (shots[i].frame == when) {
            if (depth_active) save_png_stride(shots[i].path, depth_buf, sw, sh, sw);
            else save_png(shots[i].path, pixels, width, height);
            printf("%s %llu: saved %s (%dx%d)\n", (movie || replay) ? "logical" : "frame",
                   (unsigned long long)when, shots[i].path, sw, sh);
        }
    for (int i = 0; i < nlayer_dumps; i++) {
        if (layer_dumps[i].frame == when && frame_vint_will_fire())
            dump_layers(layer_dumps[i].prefix, when, width, height);
    }
    if ((movie || replay) && shot_every && fresh && logical % shot_every == 0
        && logical >= shot_from && (!shot_to || logical <= shot_to)) {
        char p[512];
        snprintf(p, sizeof p, "%s/%06llu.png", shot_dir, (unsigned long long)logical);
        if (depth_active) save_png_stride(p, depth_buf, sw, sh, sw);
        else save_png(p, pixels, width, height);
    }
    if (movie && fresh) {
        uint8_t keys = 0;
        for (int i = 0; i < nmovie; i++)
            if (logical >= movie[i].frame && logical < movie[i].frame + movie[i].len) keys |= movie[i].keys;
        if (trace_pad && (keys || bus_pad[0]))
            printf("logical %llu: pad %02X -> %02X; game Joypad_Held %02X Pressed %02X\n",
                   (unsigned long long)logical, bus_pad[0], keys, m68k_ram[0xEF02], m68k_ram[0xEF03]);
        for (int i = 0; i < nclick; i++)                   /* click to walk (walkto.h) */
            if (clicks[i].frame == logical) {
                int cx = -1, cy = -1, slot = -1, ok;
                if (battleui_active()) ok = battleui_click((float)clicks[i].x, (float)clicks[i].y, clicks[i].right) ? 3 : 0;
                else if (clicks[i].right) { ok = walkto_click_button(1) ? 2 : 0; walkto_cancel(); }
                else if (walkto_click_button(0)) ok = 2;   /* text up: C */
                else if (fpview_active() && fpview_pick((float)clicks[i].x, (float)clicks[i].y, width, height, &cx, &cy, &slot))
                    ok = walkto_go(cx, cy, slot);
                else ok = walkto_click_screen(clicks[i].x + vdp_view_x0, clicks[i].y);
                printf("logical %llu: click %d,%d (first person: cell %d,%d object %d): %s\n", (unsigned long long)logical,
                       clicks[i].x, clicks[i].y, cx, cy, slot,
                       ok == 3 ? "a battle menu pick" : ok == 2 ? "C (text up)" : ok ? "walking" : "nowhere to go");
            }
        bus_pad[0] = battleui_pad(keys, walkto_pad(keys, keys));
    }
    /* Sound_Index: a request only beats what is playing if its priority is at
     * least as high, so stop everything ($FE) first, then ask for the song */
    if (test_fatal_frame && f == test_fatal_frame) m68k_call(0x123456);
    if (music_id >= 0 && f == music_frame) m68k_ram[0x500A] = 0xFE;
    if (music_id >= 0 && f == music_frame + 2) m68k_ram[0x500A] = (uint8_t)music_id;
    if (!replay && !movie) {
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

/* "L KEYS [N]" / "+D KEYS [N]" lines; KEYS "-" = nothing pressed */
static int load_movie(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    char line[256];
    uint64_t prev = 0;
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, keys[32] = "";
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        int rel = *s == '+';
        if (rel) s++;
        unsigned long long at = 0, len = 1;
        if (sscanf(s, "%llu %31s %llu", &at, keys, &len) < 2) continue;
        if (nmovie == cap) {
            cap = cap ? cap * 2 : 256;
            movie = realloc(movie, (size_t)cap * sizeof *movie);
        }
        Press *p = &movie[nmovie++];
        p->frame = rel ? prev + at : at;
        p->keys = strcmp(keys, "-") ? parse_keys(keys) : 0;
        p->len = len;
        prev = p->frame;
        if (p->frame + p->len > movie_end) movie_end = p->frame + p->len;
    }
    fclose(f);
    printf("movie %s: %d events, last logical frame %llu\n", path, nmovie, (unsigned long long)movie_end);
    return 1;
}

int main(int argc, char **argv) {
    const char *rom_path = "rom/psiv.bin";
    const char *sram_in = NULL;
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
        } else if (!strcmp(argv[i], "--movie") && i + 1 < argc) {
            if (!load_movie(argv[++i])) return 1;
        } else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) {
            char *e;
            shot_every = strtoull(argv[++i], &e, 10);
            shot_dir = *e == ':' ? e + 1 : ".";
        } else if (!strcmp(argv[i], "--trace-pad")) {
            trace_pad = 1;
        } else if (!strcmp(argv[i], "--sram") && i + 1 < argc) {
            sram_in = argv[++i];
        } else if (!strcmp(argv[i], "--sram-out") && i + 1 < argc) {
            sram_out = argv[++i];
        } else if (!strcmp(argv[i], "--hot-from") && i + 1 < argc) {
            hot_from = strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--where") && i + 1 < argc) {
            for (char *s = argv[++i]; *s && nwhere < 16; ) {   /* comma-separated logical frames */
                where_at[nwhere++] = strtoull(s, &s, 10);
                if (*s == ',') s++;
            }
        } else if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
            char *e;
            depth_set_level((int)strtol(argv[++i], &e, 10));
            depth_scale = *e == ':' ? atoi(e + 1) : 2;
            if (depth_scale < 1) depth_scale = 1;
            if (depth_scale > 4) depth_scale = 4;
        } else if (!strcmp(argv[i], "--algol")) {
            algol_set_enabled(1);                          /* the music on the modern chip */
        } else if (!strcmp(argv[i], "--click") && i + 1 < argc && nclick < 16) {
            char *e;
            clicks[nclick].frame = strtoull(argv[++i], &e, 10);
            if (*e == ':') clicks[nclick].x = (int)strtol(e + 1, &e, 10);
            if (*e == ',') clicks[nclick].y = (int)strtol(e + 1, &e, 10);
            clicks[nclick].right = *e == ',' && (e[1] == 'R' || e[1] == 'r');
            nclick++;
        } else if (!strcmp(argv[i], "--fpview") && i + 1 < argc) {
            fpview_set_enabled(1);                         /* first-person dungeons (fpview.h) */
            fp_scale = atoi(argv[++i]);
            if (fp_scale < 1) fp_scale = 1;
            if (fp_scale > 4) fp_scale = 4;
        } else if (!strcmp(argv[i], "--custom-music") && i + 1 < argc) {
            music_set_folder(argv[++i]);                   /* your own music files (music.h) */
        } else if (!strcmp(argv[i], "--algol-bank") && i + 1 < argc) {
            const char *rom = argv[++i];                   /* a SNES game's instruments (snesbank.h) */
            if (!algol_set_bank(rom)) { fprintf(stderr, "no instrument bank in %s\n", rom); return 1; }
            printf("instrument bank: %s\n", algol_bank_name());
        } else if (!strcmp(argv[i], "--sfx-synth")) {
            int n = sfx_synth_load();                      /* the port's own sound effects */
            sfx_set_mode(SFX_MODERN);
            printf("sound effects: %d synthesized\n", n);
        } else if (!strcmp(argv[i], "--sfx-dump") && i + 1 < argc) {
            const char *dir = argv[++i];                   /* every synthesized effect as a WAV */
            printf("sound effects: %d written to %s\n", sfx_synth_dump(dir), dir);
            return 0;
        } else if (!strcmp(argv[i], "--sfx") && i + 1 < argc) {
            sfx_synth_load();                              /* what sfx.ini doesn't map stays modern */
            int n = sfx_load(argv[++i]);                   /* your sound effects (DIR/sfx.ini) */
            sfx_set_mode(SFX_CUSTOM);
            printf("sound effects: %d replaced from %s\n", n, argv[i]);
        } else if (!strcmp(argv[i], "--fx") && i + 1 < argc) {
            depth_set_fx(atoi(argv[++i]));                 /* battle effects level (with --depth) */
        } else if (!strcmp(argv[i], "--text") && i + 1 < argc) {
            char *font = argv[++i], *colon = strrchr(font, ':');   /* FONT[:SCALE] */
            if (colon && colon[1] && strchr("0123456789", colon[1]) && colon - font > 1) {
                text_scale = atoi(colon + 1);
                *colon = 0;
            }
            if (text_scale < 1) text_scale = 1;
            if (text_scale > 4) text_scale = 4;
            hdtext_set_enabled(hdtext_init(font));
        } else if (!strcmp(argv[i], "--battle-ui")) {
            battleui_set_enabled(1);                       /* modern battle menus (needs --text FONT) */
        } else if (!strcmp(argv[i], "--panels")) {
            panels_on = 1;                                 /* with --wide: side panels in exploration */
            frame_wide_field = 0;
        } else if (!strcmp(argv[i], "--wide") && i + 1 < argc) {
            frame_wide_margin = atoi(argv[++i]);           /* widescreen: pixels beyond each side */
        } else if (!strcmp(argv[i], "--wide-plain")) {
            widemap_set_enabled(0);                        /* field margins straight from the planes */
        } else if (!strcmp(argv[i], "--wide-edge") && i + 1 < argc) {
            widemap_set_edge(atoi(argv[++i]));             /* beyond a map's edge: 0 backdrop, 1 edge repeated */
        } else if (!strcmp(argv[i], "--fog") && i + 1 < argc) {
            depth_set_fog(atoi(argv[++i]));                /* battle fog level (with --depth) */
        } else if (!strcmp(argv[i], "--shot-range") && i + 1 < argc) {
            char *e;                                       /* F1-F2: limit --shot-every */
            shot_from = strtoull(argv[++i], &e, 10);
            shot_to = *e == '-' ? strtoull(e + 1, NULL, 10) : 0;
        } else if (!strcmp(argv[i], "--layers") && i + 1 < argc && nlayer_dumps < 16) {
            char *e;
            layer_dumps[nlayer_dumps].frame = strtoull(argv[++i], &e, 10);
            layer_dumps[nlayer_dumps].prefix = *e == ':' ? e + 1 : "layers";
            nlayer_dumps++;
        } else if (!strcmp(argv[i], "--ramlog") && i + 1 < argc) {
            for (char *s = argv[++i]; *s && ramlog_n < 32; ) {   /* hex ADDR[.b|.w|.l],... */
                ramlog_addr[ramlog_n] = (uint32_t)strtoul(s, &s, 16);
                ramlog_size[ramlog_n] = 'w';
                if (*s == '.' && s[1]) { ramlog_size[ramlog_n] = s[1]; s += 2; }
                ramlog_n++;
                if (*s == ',') s++; else break;
            }
        } else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            char *s = argv[++i];                               /* N:PC,PC,... (hex PCs) */
            watch_frame = strtoull(s, &s, 10);
            if (*s == ':') s++;
            while (*s && watch_n < 64) {
                watch_pc[watch_n++] = (uint32_t)strtoul(s, &s, 16);
                if (*s == ',') s++; else break;
            }
#ifndef PSIV_TRACE
            fprintf(stderr, "--watch needs psiv_headless_trace\n");
#endif
        } else if (!strcmp(argv[i], "--test-fatal") && i + 1 < argc) {
            test_fatal_frame = strtoull(argv[++i], NULL, 10);
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
        } else if (!strcmp(argv[i], "--rumble-log")) {
            rumble_set_log(rumble_print);
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
    if (panels_on || battleui_enabled()) printf("portraits: %d from the ROM\n", panels_init());
    if (sram_in) {
        FILE *sf = fopen(sram_in, "rb");
        if (!sf) { fprintf(stderr, "cannot open %s\n", sram_in); return 1; }
        static uint8_t buf[2 * BUS_SRAM_SIZE];
        size_t got = fread(buf, 1, sizeof buf, sf);
        fclose(sf);
        int format = bus_sram_import(buf, got);
        printf("loaded save %s (%u bytes, %s layout)\n", sram_in, (unsigned)got, bus_sram_format_name(format));
    }

    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    diag_stack_top = A(7);
    m68k_call(rd32(4));
    printf("reset handler returned (unexpected)\n");
    report();
    return 1;
}
