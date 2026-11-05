/*
 * Your own music: audio files in place of the game's songs (see music.h).
 *
 * The song playing is known from $FF5020, where PlayMusic leaves its voice
 * table's address (unique for all 52 songs; table MusicPtrs at ROM $D1C40);
 * a restart from the music tracks ($FF5040 + $30 n, bit 7) coming back on.
 * The driver's fade-out (sound command $FB) counts $FF5004 down from 15,
 * one step every other tick, then stops everything; $FF5007 pauses it.
 */
#include "music.h"
#include "m68k.h"
#include "sound.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEVEL 0.85f                   /* a mastered file, about the game's music level (a gain in music.ini adjusts) */

typedef struct {
    char path[600];
    int16_t *pcm;                     /* stereo frames */
    int frames, rate;
    int loop_start, loop_end;         /* frames; loop_end 0 = no loop */
    int start;                        /* where it begins: its first sound (or music.ini's) */
    float gain;
    unsigned used;
} Track;
#define CACHE 3                       /* the battle theme, the field's, and one more */
static Track cache[CACHE];
static unsigned use_count;

static char folder[600];
static int enabled, fresh = 1;
static int song, music_on;
static Track *cur;                    /* playing, or NULL */
static double pos;
static float fade = 1.0f, ramp, ramp_to;
static int paused;
static unsigned fx_fm, fx_psg;
static int log_on = -1;

static const char *const SONGS[0x34] = {
    "Tonoe de Pon", "Inn", "Motavia Village", "Motavia Town", "Organic Beat", "Dezolis Town 1", "Now on Sale",
    "Behind the Circuit", "Machine Center", "In the Cave", "Winners", "Field Motavia", "Land Master",
    "Requiem for Lutz", "Meet Them Head-On", "Ryucross Field", "Dungeon Arrange 1", "Fal", "Temple Ngangbius",
    "Thray", "Defeat at a Blow", "Cybernetic Carnival", "Terrible Sight", "Edge of Darkness", "Dezolis Field 1",
    "Tower", "Take Off Landeel", "Dezolis Town 2", "Dezolis Field 2", "A Happy Settlement", "Suspicion",
    "The King of Terrors", "The Age of Fables", "Abyss", "Enemy Appearance", "Her Last Breath", "Pain",
    "Jijy no Rag", "Dungeon Arrange 2 (cont.)", "The Black Blood", "Red Alert", "Laughter", "Mystery",
    "The End of the Millennium", "Explosion", "Staff Roll", "The Promising Future 1", "Pao Pao",
    "Dungeon Arrange 2", "The Promising Future 2", "Dezolis de Don", "Ooze",
};

/* ------------------------------------------------------------------ WAV */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_load(const char *path, Track *t) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = size > 44 && size < 1024L * 1024 * 1024 ? malloc((size_t)size) : NULL;
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) { free(b); fclose(f); return 0; }
    fclose(f);
    int ok = 0, fmt = 0, ch = 0, bits = 0, rate = 0, ls = -1, le = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
        for (long o = 12; o + 8 <= size;) {
            uint32_t n = le32(b + o + 4);
            const uint8_t *c = b + o + 8;
            if (o + 8 + (long)n > size) n = (uint32_t)(size - o - 8);
            if (!memcmp(b + o, "fmt ", 4) && n >= 16) {
                fmt = le16(c);
                ch = le16(c + 2);
                rate = (int)le32(c + 4);
                bits = le16(c + 14);
                if (fmt == 0xFFFE && n >= 26) fmt = le16(c + 24);   /* extensible: the subformat */
            } else if (!memcmp(b + o, "data", 4)) {
                data = c;
                dlen = n;
            } else if (!memcmp(b + o, "smpl", 4) && n >= 60 && le32(c + 28) > 0) {
                ls = (int)le32(c + 36 + 8);                    /* the first loop: start, end (inclusive) */
                le = (int)le32(c + 36 + 12) + 1;
            }
            o += 8 + n + (n & 1);
        }
    }
    int bps = bits / 8;
    if (data && ch >= 1 && ch <= 8 && rate >= 8000 && rate <= 192000 && bps >= 1 && bps <= 4
        && (fmt == 1 || (fmt == 3 && bits == 32))) {
        int frames = (int)(dlen / (uint32_t)(bps * ch));
        t->pcm = malloc(sizeof(int16_t) * 2 * (size_t)(frames > 0 ? frames : 1));
        if (t->pcm) {
            for (int i = 0; i < frames; i++)
                for (int k = 0; k < 2; k++) {
                    const uint8_t *s = data + ((size_t)i * (size_t)ch + (size_t)(k < ch ? k : 0)) * (size_t)bps;
                    float v;
                    if (fmt == 3) { float x; memcpy(&x, s, 4); v = x; }
                    else if (bps == 1) v = ((float)s[0] - 128.0f) / 128.0f;
                    else if (bps == 2) v = (float)(int16_t)le16(s) / 32768.0f;
                    else if (bps == 3) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
                    else v = (float)(int32_t)le32(s) / 2147483648.0f;
                    v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                    t->pcm[(size_t)i * 2 + (size_t)k] = (int16_t)lrintf(v * 32767.0f);
                }
            t->frames = frames;
            t->rate = rate;
            t->loop_start = ls >= 0 && ls < le && le <= frames ? ls : 0;
            t->loop_end = ls >= 0 && ls < le && le <= frames ? le : 0;
            ok = 1;
        }
    }
    free(b);
    return ok;
}

/* ------------------------------------------------------------------ which file */
static int ini_lookup(int id, char *file, size_t n, double *loop_a, double *loop_b, float *gain_db, double *start) {
    char path[700];
    snprintf(path, sizeof path, "%s/music.ini", folder);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[700];
    int named = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';') continue;
        unsigned h;
        int used;
        if (sscanf(p, "%x%n", &h, &used) != 1 || (int)h != id) continue;
        p += used;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '=') {                                        /* 8F = some file.wav */
            p++;
            while (*p == ' ' || *p == '\t') p++;
            size_t l = strlen(p);
            while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
            snprintf(file, n, "%s", p);
            named = 1;
        } else if (!strncmp(p, "loop", 4)) {
            double a = -1.0, b = -1.0;
            if (sscanf(p + 4, "%lf %lf", &a, &b) >= 1) { *loop_a = a; *loop_b = b; }
        } else if (!strncmp(p, "start", 5)) {
            double a;
            if (sscanf(p + 5, "%lf", &a) == 1) *start = a;
        } else if (!strncmp(p, "gain", 4)) {
            float g;
            if (sscanf(p + 4, "%f", &g) == 1) *gain_db = g;
        }
    }
    fclose(f);
    return named;
}

static Track *track_for(int id) {
    char file[600] = {0}, path[1300];
    double la = -1.0, lb = -1.0, st = -1.0;
    float gdb = 0.0f;
    if (!ini_lookup(id, file, sizeof file, &la, &lb, &gdb, &st)) snprintf(file, sizeof file, "%02X.wav", id);
    if (file[0] == '/' || file[0] == '\\' || (file[0] && file[1] == ':')) snprintf(path, sizeof path, "%s", file);
    else snprintf(path, sizeof path, "%s/%s", folder, file);
    Track *t = NULL;
    for (int i = 0; i < CACHE; i++) if (cache[i].pcm && !strcmp(cache[i].path, path)) t = &cache[i];
    if (!t) {
        FILE *f = fopen(path, "rb");
        if (!f) return NULL;
        fclose(f);
        t = &cache[0];                                          /* the least recently used */
        for (int i = 1; i < CACHE; i++) if (cache[i].used < t->used) t = &cache[i];
        free(t->pcm);
        memset(t, 0, sizeof *t);
        if (!wav_load(path, t)) {
            if (log_on > 0) printf("music: %s is not a WAV the port reads (PCM or float)\n", path);
            free(t->pcm);
            memset(t, 0, sizeof *t);
            return NULL;
        }
        snprintf(t->path, sizeof t->path, "%s", path);
    }
    /* music.ini's loop and gain win over the file's own */
    if (la >= 0.0) {
        int a = (int)lround(la * t->rate), b2 = lb > la ? (int)lround(lb * t->rate) : t->frames;
        if (a < t->frames && b2 <= t->frames && a < b2) { t->loop_start = a; t->loop_end = b2; }
    }
    t->gain = powf(10.0f, gdb / 20.0f);
    /* the start: music.ini's, or the first sound (an export's or an encoder's
     * silence in front would hold up the battle) */
    t->start = 0;
    if (st >= 0.0) t->start = (int)lround(st * t->rate);
    else while (t->start < t->frames && abs(t->pcm[(size_t)t->start * 2]) < 33 && abs(t->pcm[(size_t)t->start * 2 + 1]) < 33) t->start++;
    if (t->start >= t->frames) t->start = 0;
    t->used = ++use_count;
    return t;
}

/* ------------------------------------------------------------------ following the game */
static uint32_t song_vt[0x34];

static void song_tables(void) {
    static int done;
    if (done || !m68k_rom) return;
    done = 1;
    for (int i = 0; i < 0x34; i++) {
        uint32_t p = 0xD1C40u + (uint32_t)i * 4u;
        if (p + 4 > m68k_rom_size) return;
        uint32_t hdr = ((uint32_t)m68k_rom[p + 1] << 16 | (uint32_t)m68k_rom[p + 2] << 8 | m68k_rom[p + 3]);
        if (hdr + 2 > m68k_rom_size) continue;
        song_vt[i] = hdr + (uint32_t)(m68k_rom[hdr] << 8 | m68k_rom[hdr + 1]);
    }
}

void music_set_folder(const char *dir) {
    int on = dir && *dir;
    if (on && strcmp(dir, folder)) {                         /* another folder: its files */
        for (int i = 0; i < CACHE; i++) { free(cache[i].pcm); memset(&cache[i], 0, sizeof cache[i]); }
    }
    snprintf(folder, sizeof folder, "%s", on ? dir : "");
    if (on && !enabled) fresh = 1;                           /* a song already going keeps the game's */
    enabled = on;
    if (!on) cur = NULL;
}

void music_reset(void) {
    cur = NULL;
    song = music_on = 0;
    fresh = 1;
    fx_fm = fx_psg = 0;
}

void music_tick(void) {
    if (!enabled) return;
    if (log_on < 0) { const char *v = getenv("PSIV_MUSIC_LOG"); log_on = v ? atoi(v) : 0; }
    song_tables();
    uint32_t vt = (uint32_t)m68k_ram[0x5021] << 16 | (uint32_t)m68k_ram[0x5022] << 8 | m68k_ram[0x5023];
    int now_song = 0;
    for (int i = 0; i < 0x34; i++) if (song_vt[i] && song_vt[i] == vt) { now_song = 0x81 + i; break; }
    int playing = 0;
    for (int i = 0; i < 10; i++) if (m68k_ram[0x5040u + (unsigned)i * 0x30u] & 0x80) playing = 1;
    if (fresh) {
        fresh = 0;
        song = now_song;
    } else if (now_song != song || (playing && !music_on)) {   /* a song (re)starts */
        song = now_song;
        Track *t = song && playing ? track_for(song) : NULL;
        if (log_on > 0 && song)
            printf("music: song %02X %s%s%s\n", song, SONGS[song - 0x81], t ? " -> " : "", t ? t->path : "");
        cur = t;
        pos = t ? (double)t->start : 0.0;
        fade = 1.0f;
        ramp = 0.0f;
        ramp_to = 1.0f;
    }
    if (cur && !playing) ramp_to = 0.0f;                     /* the game stopped its music */
    music_on = playing;
    /* the game's fade-out: 15 steps, about 1.5 dB each */
    int fo = m68k_ram[0x5004];
    fade = fo && fo <= 15 ? powf(10.0f, -1.5f * (float)(15 - fo) / 20.0f) : 1.0f;
    paused = m68k_ram[0x5007] != 0;
    /* the channels the sound-effect tracks hold */
    unsigned fm = 0, psg = 0;
    for (int i = 0; i < 9; i++) {
        unsigned a = 0x5220u + (unsigned)i * 0x30u;
        uint8_t flags = m68k_ram[a], chn = m68k_ram[a + 1];
        if (!(flags & 0x80)) continue;
        if (chn & 0x80) psg |= 1u << (chn >> 5 & 3);
        else if ((chn & 3) < 3) fm |= 1u << ((chn & 3) + 3 * (chn >> 2 & 1));
    }
    fx_fm = fm;
    fx_psg = psg;
}

int music_active(void) { return enabled && cur != NULL; }
unsigned music_fx_fm(void) { return fx_fm; }
unsigned music_fx_psg(void) { return fx_psg; }

void music_render(float *left, float *right) {
    *left = *right = 0.0f;
    if (!cur || paused) return;
    Track *t = cur;
    if (t->loop_end > 0 && pos >= (double)t->loop_end) pos -= (double)(t->loop_end - t->loop_start);
    int i = (int)pos;
    if (i + 1 >= t->frames) { cur = NULL; return; }           /* the end, no loop */
    float f = (float)(pos - (double)i);
    const int16_t *a = t->pcm + (size_t)i * 2, *b = a + 2;
    ramp += (ramp_to - ramp) * (ramp_to > ramp ? 0.004f : 0.0012f);   /* a click-free start, a short fade at a stop */
    if (ramp_to == 0.0f && ramp < 0.0005f) { cur = NULL; return; }
    float g = t->gain * fade * ramp * LEVEL;
    *left = ((float)a[0] + ((float)b[0] - (float)a[0]) * f) * g;
    *right = ((float)a[1] + ((float)b[1] - (float)a[1]) * f) * g;
    pos += (double)t->rate / (double)SOUND_RATE;
}
