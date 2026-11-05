/*
 * Instrument banks from a SNES game you own: see snesbank.h.
 *
 * SNES music plays samples in BRR (9-byte blocks of 16 4-bit samples, a
 * shift and one of four prediction filters per block), from the sound chip's
 * 64 KB, where each game's driver uploads them from the cartridge. A recipe
 * finds them in the ROM itself; the decoder is the S-DSP's own (blargg's
 * SPC_DSP), so they sound as the console made them, before its filtering.
 *
 * Lufia II (Neverland, 1995) keeps every instrument in one chain of records:
 *   [length 16][loop offset 16][ADSR1][ADSR2][tuning 8.8][BRR data]
 * (an empty slot is the 8 bytes 08 00 00 00 00 00 00 00). The tuning is the
 * waveform's period in BRR blocks: the sample sounds 2000 / tuning Hz at
 * 32 kHz. Found by dumping the sound RAM in an emulator (the directory at
 * $0100 points at these samples) and finding the bytes in the ROM.
 */
#include "snesbank.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 128
static BankSample samples[MAX_SAMPLES];
static int nsamples;
static char bank_name[64];

typedef struct { const char *name; int sample; float transpose; } Voice;
#define MAX_VOICES 24
static Voice voicing[MAX_VOICES];
static int nvoicing;

/* the S-DSP's envelope rates: samples (at 32 kHz) between steps, by index */
static const int RATE[32] = {0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                             64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};

/* one BRR sample -> PCM, the S-DSP's way */
static void brr_decode(const uint8_t *d, int len, float *out) {
    int p1 = 0, p2 = 0, k = 0;
    for (int o = 0; o + 9 <= len; o += 9) {
        int h = d[o], shift = h >> 4, filter = h >> 2 & 3;
        for (int j = 0; j < 16; j++) {
            int nib = j & 1 ? d[o + 1 + j / 2] & 15 : d[o + 1 + j / 2] >> 4;
            int s = nib >= 8 ? nib - 16 : nib;
            s = (s << shift) >> 1;
            if (shift >= 13) s = s < 0 ? -2048 : 0;
            int q2 = p2 >> 1;
            if (filter == 1) s += (p1 >> 1) + ((-p1) >> 5);
            else if (filter == 2) s += p1 - q2 + (q2 >> 4) + ((p1 * -3) >> 6);
            else if (filter == 3) s += p1 - q2 + ((p1 * -13) >> 7) + ((q2 * 3) >> 4);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            s = (int16_t)(s * 2);
            p2 = p1;
            p1 = s;
            out[k++] = (float)s / 32768.0f;
        }
    }
}

/* ------------------------------------------------------------------ Lufia II */
static int lufia2_record(const uint8_t *rom, size_t n, size_t a, size_t *next) {
    static const uint8_t EMPTY[8] = {8, 0, 0, 0, 0, 0, 0, 0};
    if (a + 8 > n) return 0;
    if (!memcmp(rom + a, EMPTY, 8)) { *next = a + 8; return 1; }
    unsigned len = rom[a] | rom[a + 1] << 8, loop = rom[a + 2] | rom[a + 3] << 8;
    if (len < 18 || len % 9 || len > 0x8000 || loop > len || loop % 9 || a + 8 + len > n) return 0;
    for (unsigned k = 0; k < len; k += 9) {                 /* range <= 12; END on the last block only */
        unsigned h = rom[a + 8 + k];
        if (h >> 4 > 12 || (int)(h & 1) != (k == len - 9)) return 0;
    }
    *next = a + 8 + len;
    return 2;
}

static int lufia2(const uint8_t *rom, size_t n) {
    /* the longest chain of records in the ROM */
    size_t best = 0;
    int best_count = 0;
    for (size_t o = 0; o + 8 < n; o++) {
        size_t a = o, next;
        int count = 0, kind;
        if (lufia2_record(rom, n, a, &next) != 2) continue;     /* chains start with a sample */
        while ((kind = lufia2_record(rom, n, a, &next)) != 0) { count++; a = next; }
        if (count > best_count) { best_count = count; best = o; }
        if (count > 8) o = a - 1;                           /* skip what this chain covered */
    }
    if (best_count < 30) return 0;
    size_t a = best, next;
    int kind;
    while ((kind = lufia2_record(rom, n, a, &next)) != 0 && nsamples < MAX_SAMPLES) {
        if (kind == 2) {
            unsigned len = rom[a] | rom[a + 1] << 8, loop = rom[a + 2] | rom[a + 3] << 8;
            unsigned adsr1 = rom[a + 4], adsr2 = rom[a + 5];
            float tuning = (float)rom[a + 6] + (float)rom[a + 7] / 256.0f;
            BankSample *s = &samples[nsamples];
            s->len = (int)(len / 9 * 16);
            s->pcm = malloc(sizeof(float) * (size_t)s->len);
            if (!s->pcm) break;
            brr_decode(rom + a + 8, (int)len, s->pcm);
            int loops = (rom[a + 8 + len - 9] & 2) && loop < len;
            s->loop = loops ? (int)(loop / 9 * 16) : -1;
            s->root = tuning > 0.0f ? 2000.0f / tuning : 440.0f;
            s->level = (float)((adsr2 >> 5) + 1) / 8.0f;
            int sr = adsr2 & 31;
            s->decay = (adsr1 & 0x80) && sr ? 256.0f * (float)RATE[sr] / 32000.0f : 0.0f;
            /* loudness: what sustains (the loop), or the body of a one-shot */
            int from = s->loop >= 0 && s->len - s->loop >= 64 ? s->loop : 0;
            int to = s->loop >= 0 ? s->len : (s->len < 4000 ? s->len : 4000);
            double e = 0.0;
            for (int i = from; i < to; i++) e += (double)s->pcm[i] * s->pcm[i];
            float rms = (float)sqrt(e / (double)(to - from > 0 ? to - from : 1));
            s->gain = rms > 0.02f ? 0.25f / rms : 4.0f;
            if (s->gain > 4.0f) s->gain = 4.0f;
            nsamples++;
        } else if (nsamples < MAX_SAMPLES) {
            memset(&samples[nsamples], 0, sizeof samples[0]);  /* an empty slot keeps its number */
            samples[nsamples].loop = -1;
            nsamples++;
        }
        a = next;
    }
    snprintf(bank_name, sizeof bank_name, "Lufia II");
    /* the voicing: picked from the samples' shapes and envelopes (a first
     * pass; algol.ini changes any) -- numbers are the records in order */
    static const Voice LUFIA2[] = {
        {"lead", 35, 0}, {"brass", 37, 0}, {"strings", 6, 0}, {"bass", 26, 0}, {"pluck", 25, 0},
        {"guitar", 13, 0}, {"bell", 39, 0}, {"organ", 10, 0}, {"keys", 20, 0}, {"chip", 21, 0},
        {"kick", 29, 0}, {"snare", 38, 0}, {"hat", 17, 0}, {"crash", 34, 0}, {"tom", 28, 0},
    };
    nvoicing = 0;
    for (size_t i = 0; i < sizeof LUFIA2 / sizeof LUFIA2[0]; i++) voicing[nvoicing++] = LUFIA2[i];
    return 1;
}

/* ------------------------------------------------------------------ the bank */
static void unload(void) {
    for (int i = 0; i < nsamples; i++) free(samples[i].pcm);
    memset(samples, 0, sizeof samples);
    nsamples = nvoicing = 0;
    bank_name[0] = 0;
}

int snesbank_load(const char *path) {
    unload();
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8L * 1024 * 1024) { fclose(f); return 0; }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    size_t hdr = n % 1024 == 512 ? 512 : 0;                 /* a copier's header */
    const uint8_t *rom = buf + hdr;
    n -= hdr;
    /* the title in the internal header (LoROM at $7FC0, HiROM at $FFC0) */
    char title[22] = {0};
    for (size_t base = 0x7FC0; base <= 0xFFC0 && !title[0]; base += 0x8000) {
        if (base + 0x20 > n) break;
        unsigned chk = rom[base + 0x1E] | rom[base + 0x1F] << 8, cpl = rom[base + 0x1C] | rom[base + 0x1D] << 8;
        if ((chk ^ cpl) == 0xFFFF) memcpy(title, rom + base, 21);
    }
    int ok = 0;
    if (!strncmp(title, "LUFIA II", 8) || !strncmp(title, "ESTPOLIS II", 11) || !strncmp(title, "LUFIA ", 6))
        ok = lufia2(rom, n);
    free(buf);
    if (!ok) unload();
    return ok;
}

const char *snesbank_name(void) { return bank_name; }
int snesbank_count(void) { return nsamples; }
const BankSample *snesbank_sample(int i) {
    return i >= 0 && i < nsamples && samples[i].pcm ? &samples[i] : NULL;
}

int snesbank_voice(const char *name, float *transpose) {
    for (int i = 0; i < nvoicing; i++)
        if (!strcmp(voicing[i].name, name)) {
            if (transpose) *transpose = voicing[i].transpose;
            return snesbank_sample(voicing[i].sample) ? voicing[i].sample : -1;
        }
    if (transpose) *transpose = 0.0f;
    return -1;
}

void snesbank_set_voice(const char *name, int sample, float transpose) {
    for (int i = 0; i < nvoicing; i++)
        if (!strcmp(voicing[i].name, name)) { voicing[i].sample = sample; voicing[i].transpose = transpose; return; }
    if (nvoicing < MAX_VOICES) {
        static char names[MAX_VOICES][16];
        snprintf(names[nvoicing], sizeof names[0], "%s", name);
        voicing[nvoicing].name = names[nvoicing];
        voicing[nvoicing].sample = sample;
        voicing[nvoicing].transpose = transpose;
        nvoicing++;
    }
}
