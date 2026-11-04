/*
 * Sound effect replacement: see sfx.h.
 */
#include "sfx.h"
#include "sound.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

#define MAX_FILES  16
#define MAX_VOICES 16

typedef struct {
    int nfiles, last;
    int16_t *pcm[MAX_FILES];             /* stereo, SOUND_RATE */
    int frames[MAX_FILES];
    float gain;
} Bank;

typedef struct {
    const int16_t *pcm;
    int frames, pos;
    float gain, fade, fade_step;
    uint8_t id;
} Voice;

static Bank banks[256];
static Voice voices[MAX_VOICES];
static int enabled, mapped;
static uint32_t rng = 0x2545F491u;

static uint32_t next_rand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* a WAV file as stereo int16 at SOUND_RATE: PCM 8/16/24/32-bit or 32-bit
 * float, any rate and channel count (first two channels are used) */
static int16_t *load_wav(const char *path, int *frames_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return NULL; }
    fclose(f);
    int16_t *out = NULL;
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) goto done;
    int tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    uint32_t data_n = 0;
    for (long p = 12; p + 8 <= size;) {
        uint32_t n = le32(buf + p + 4);
        const uint8_t *c = buf + p + 8;
        if (p + 8 + (long)n > size) n = (uint32_t)(size - p - 8);
        if (!memcmp(buf + p, "fmt ", 4) && n >= 16) {
            tag = le16(c); channels = le16(c + 2); rate = le32(c + 4); bits = le16(c + 14);
            if (tag == 0xFFFE && n >= 26) tag = le16(c + 24);          /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(buf + p, "data", 4)) {
            data = c; data_n = n;
        }
        p += 8 + (long)n + (n & 1);                                  /* chunks are word-aligned */
    }
    if (!data || !channels || !rate || (tag != 1 && tag != 3)) goto done;
    int bps = bits / 8, frame_bytes = bps * channels;
    if (bps < 1 || bps > 4 || (tag == 3 && bits != 32)) goto done;
    long in_frames = (long)(data_n / (uint32_t)frame_bytes);
    if (in_frames < 1) goto done;
    float *lr = malloc((size_t)in_frames * 2 * sizeof(float));
    if (!lr) goto done;
    for (long i = 0; i < in_frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            const uint8_t *s = data + i * frame_bytes + (ch < channels ? ch : 0) * bps;
            float v;
            if (tag == 3) { uint32_t u = le32(s); memcpy(&v, &u, 4); }
            else if (bps == 1) v = ((float)s[0] - 128.0f) / 128.0f;
            else if (bps == 2) v = (float)(int16_t)le16(s) / 32768.0f;
            else if (bps == 3) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
            else v = (float)(int32_t)le32(s) / 2147483648.0f;
            lr[i * 2 + ch] = v;
        }
    }
    /* resample (linear) to the output rate */
    long n_out = (long)((double)in_frames * SOUND_RATE / (double)rate);
    out = malloc((size_t)n_out * 2 * sizeof(int16_t));
    if (out) {
        double step = (double)rate / SOUND_RATE;
        for (long i = 0; i < n_out; i++) {
            double src = (double)i * step;
            long j = (long)src;
            float fr = (float)(src - (double)j);
            long j1 = j + 1 < in_frames ? j + 1 : j;
            for (int ch = 0; ch < 2; ch++) {
                float v = lr[j * 2 + ch] + (lr[j1 * 2 + ch] - lr[j * 2 + ch]) * fr;
                v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                out[i * 2 + ch] = (int16_t)lrintf(v * 32767.0f);
            }
        }
        *frames_out = (int)n_out;
    }
    free(lr);
done:
    free(buf);
    return out;
}

static void add_file(Bank *b, const char *path) {
    if (b->nfiles >= MAX_FILES) return;
    int frames = 0;
    int16_t *pcm = load_wav(path, &frames);
    if (!pcm) { fprintf(stderr, "sfx: cannot read %s (WAV PCM or float expected)\n", path); return; }
    b->pcm[b->nfiles] = pcm;
    b->frames[b->nfiles] = frames;
    b->nfiles++;
}

static int ends_with_wav(const char *s) {
    size_t n = strlen(s);
    return n > 4 && (!strcmp(s + n - 4, ".wav") || !strcmp(s + n - 4, ".WAV"));
}

/* a file, or every .wav in a folder (sorted, so the set is stable) */
static void add_path(Bank *b, const char *path) {
    if (ends_with_wav(path)) { add_file(b, path); return; }
    char names[MAX_FILES][260];
    int n = 0;
#ifdef _WIN32
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s/*.wav", path);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) { fprintf(stderr, "sfx: no .wav files in %s\n", path); return; }
    do {
        if (n < MAX_FILES) snprintf(names[n++], sizeof names[0], "%s", fd.name);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "sfx: cannot open %s\n", path); return; }
    struct dirent *e;
    while ((e = readdir(d)))
        if (ends_with_wav(e->d_name) && n < MAX_FILES) snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    closedir(d);
#endif
    qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i < n; i++) {
        char full[1600];
        snprintf(full, sizeof full, "%s/%s", path, names[i]);
        add_file(b, full);
    }
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int sfx_load(const char *dir) {
    char path[1200], line[1024];
    snprintf(path, sizeof path, "%s/sfx.ini", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        char *comment = strchr(s, ';');
        if (comment) *comment = 0;
        s = trim(s);
        char *eq = strchr(s, '=');
        if (!*s || *s == '[' || *s == '#' || !eq) continue;
        *eq = 0;
        unsigned long id = strtoul(trim(s), NULL, 16);
        char *rhs = trim(eq + 1), *comma = strchr(rhs, ',');
        float gain_db = 0.0f;
        if (comma) { *comma = 0; gain_db = (float)atof(comma + 1); }
        rhs = trim(rhs);
        if (id > 255 || !*rhs) continue;
        Bank *b = &banks[id];
        char target[1200];
        snprintf(target, sizeof target, "%s/%s", dir, rhs);
        add_path(b, target);
        b->gain = powf(10.0f, gain_db / 20.0f);
        b->last = -1;
        if (b->nfiles) mapped++;
    }
    fclose(f);
    return mapped;
}

void sfx_set_enabled(int on) { enabled = on && mapped; }
int sfx_enabled(void) { return enabled; }

int sfx_request(uint8_t id) {
    static int log_on = -1;                              /* PSIV_SFX_LOG=1: every request, replaced or not */
    if (log_on < 0) { const char *e = getenv("PSIV_SFX_LOG"); log_on = e && *e == '1'; }
    Bank *b = &banks[id];
    if (log_on) fprintf(stderr, "sound request %02X%s\n", id, enabled && b->nfiles ? " (replaced)" : "");
    if (!enabled || !b->nfiles) return 0;
    int pick = b->nfiles == 1 ? 0 : (int)(next_rand() % (uint32_t)b->nfiles);
    if (b->nfiles > 1 && pick == b->last) pick = (pick + 1) % b->nfiles;   /* no immediate repeats */
    b->last = pick;
    /* the same sound again cuts the previous one short (a 30 ms fade) */
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].pcm && voices[i].id == id && voices[i].fade_step >= 0)
            voices[i].fade_step = -1.0f / (0.03f * SOUND_RATE);
    int slot = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].pcm) { slot = i; break; }
        if (voices[i].pos > voices[slot].pos) slot = i;              /* none free: steal the oldest */
    }
    Voice *v = &voices[slot];
    v->pcm = b->pcm[pick];
    v->frames = b->frames[pick];
    v->pos = 0;
    v->gain = b->gain;
    v->fade = 1.0f;
    v->fade_step = 0.0f;
    v->id = id;
    return 1;
}

void sfx_mix(int16_t *samples, int frames) {
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (!v->pcm) continue;
        for (int n = 0; n < frames; n++) {
            if (v->pos >= v->frames || v->fade <= 0.0f) { v->pcm = NULL; break; }
            float g = v->gain * v->fade;
            for (int ch = 0; ch < 2; ch++) {
                int s = samples[n * 2 + ch] + (int)lrintf((float)v->pcm[v->pos * 2 + ch] * g);
                samples[n * 2 + ch] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            }
            v->pos++;
            v->fade += v->fade_step;
        }
    }
}
