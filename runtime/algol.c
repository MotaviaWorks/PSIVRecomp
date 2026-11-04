/*
 * Algol, a modern sound chip for the game's music: see algol.h.
 *
 *   decoder   the chips' writes -> per channel: key on/off, pitch (MIDI, from
 *             F-number and block times the main carrier's multiple), the
 *             game's loudness (carrier total levels / PSG attenuation), pan,
 *             and the FM patch (a hash of its operator registers)
 *   voices    one per chip channel: unison oscillators (band-limited saw and
 *             square, sine, two-operator FM, drawbar organ) with a sub, a
 *             resonant filter with its own envelope, vibrato, drive
 *   kit       the drum samples by number: kick, snares, hats, ride, tom and
 *             crash synthesized; the rest (a pitched instrument, a voice,
 *             effects) played from the ROM, cleanly
 *   effects   reverb (eight-line feedback delay network), chorus, stereo delay
 *
 * Patches get an instrument from their sound (envelope, brightness,
 * algorithm, register) unless algol.ini names one: "patch-hash = instrument"
 * (PSIV_ALGOL_LOG=1 prints each patch as it first plays).
 */
#include "algol.h"
#include "m68k.h"
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR      ((float)SOUND_RATE)
#define TWO_PI  6.28318531f

static int enabled;
static void silence(void);
void algol_set_enabled(int on) {
    if (on && !enabled) silence();         /* the chips' registers are tracked all along */
    enabled = on;
}
int  algol_enabled(void) { return enabled; }

static float note_hz(float n) { return 440.0f * exp2f((n - 69.0f) / 12.0f); }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float rnd(uint32_t *s) {                          /* xorshift: -1..1 */
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return (float)(*s & 0xFFFFFF) / 8388608.0f - 1.0f;
}
static float coef(float seconds) { return seconds <= 0.0f ? 1.0f : 1.0f - expf(-1.0f / (seconds * SR)); }

/* ------------------------------------------------------------------ instruments */
enum { W_SAW, W_SQUARE, W_SINE, W_FM, W_ORGAN };
typedef struct {
    const char *name;
    int wave, unison;
    float detune, width;          /* cents across the unison; stereo spread 0..1 */
    float sub;                    /* a sine on the fundamental, under the oscillators */
    float a, d, s, r;             /* amp envelope: attack s, decay s (time constant), sustain, release s */
    float cut, track, q;          /* low-pass: Hz at middle C, key tracking (1 = follows pitch), resonance */
    float hp;                     /* high-pass, Hz: the lows left to the bass and kick */
    float famt, fd, fs;           /* filter envelope: octaves above at the start, decay s, sustain 0..1 */
    float vib, vrate, vdelay;     /* vibrato: semitones, Hz, delay s */
    float ratio, index, idecay;   /* two-operator FM: modulator ratio, index, its decay s */
    float drive;                  /* saturation, before the filter (an amp, then its cabinet) */
    float level;
    float rev, dly, cho;          /* sends */
    float glide;                  /* s, between notes tied without a key-off */
} Inst;

enum { I_BASS, I_LEAD, I_BRASS, I_STRINGS, I_PLUCK, I_GUITAR, I_BELL, I_ORGAN, I_KEYS, I_CHIP, N_INST };
static const Inst INST[N_INST] = {
    /* name      wave     uni det   wid   sub    a       d      s      r       cut    trk   q      hp    famt  fd     fs     vib    vr    vd     ratio idx   idec   drive lvl    rev    dly    cho    glide */
    {"bass",    W_SAW,    2,  7.0f, 0.10f, 0.45f, 0.003f, 0.30f, 0.65f, 0.050f, 700,   0.55f, 1.10f, 30,   2.2f, 0.16f, 0.25f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     1.6f, 0.26f, 0.04f, 0.00f, 0.00f, 0.02f},
    {"lead",    W_SAW,    5, 16.0f, 0.65f, 0.00f, 0.008f, 0.40f, 0.85f, 0.160f, 7000,  0.45f, 0.90f, 160,  0.7f, 0.30f, 0.60f, 0.10f, 5.5f, 0.30f, 0,    0,    0,     0.5f, 0.40f, 0.22f, 0.16f, 0.00f, 0.03f},
    {"brass",   W_SAW,    3, 11.0f, 0.50f, 0.00f, 0.030f, 0.45f, 0.80f, 0.140f, 3400,  0.70f, 0.80f, 130,  1.4f, 0.30f, 0.55f, 0.08f, 5.0f, 0.30f, 0,    0,    0,     0.7f, 0.40f, 0.25f, 0.05f, 0.20f, 0.00f},
    {"strings", W_SAW,    6, 18.0f, 0.90f, 0.00f, 0.110f, 0.60f, 0.90f, 0.450f, 4200,  0.40f, 0.70f, 170,  0.3f, 0.60f, 0.80f, 0.06f, 5.0f, 0.40f, 0,    0,    0,     0.0f, 0.26f, 0.35f, 0.00f, 0.60f, 0.00f},
    {"pluck",   W_SAW,    2,  6.0f, 0.40f, 0.00f, 0.002f, 0.30f, 0.00f, 0.180f, 1400,  0.80f, 1.00f, 140,  3.0f, 0.11f, 0.00f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     0.4f, 0.55f, 0.22f, 0.14f, 0.10f, 0.00f},
    {"guitar",  W_SAW,    2,  9.0f, 0.35f, 0.00f, 0.002f, 0.10f, 0.20f, 0.040f, 5200,  0.15f, 1.40f, 110,  0.6f, 0.06f, 0.30f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     3.0f, 0.30f, 0.12f, 0.06f, 0.00f, 0.00f},
    {"bell",    W_FM,     1,  0.0f, 0.30f, 0.00f, 0.001f, 1.20f, 0.00f, 0.700f, 12000, 0.00f, 0.70f, 200,  0.0f, 0.00f, 0.00f, 0.00f, 0.0f, 0.00f, 3.5f, 2.2f, 0.60f, 0.0f, 0.36f, 0.35f, 0.15f, 0.00f, 0.00f},
    {"organ",   W_ORGAN,  1,  0.0f, 0.40f, 0.00f, 0.004f, 0.10f, 1.00f, 0.060f, 7000,  0.20f, 0.70f, 90,   0.0f, 0.00f, 0.00f, 0.06f, 6.0f, 0.20f, 0,    0,    0,     0.3f, 0.34f, 0.25f, 0.00f, 0.50f, 0.00f},
    {"keys",    W_FM,     2,  5.0f, 0.40f, 0.00f, 0.002f, 0.90f, 0.35f, 0.300f, 9000,  0.00f, 0.70f, 120,  0.0f, 0.00f, 0.00f, 0.05f, 5.0f, 0.30f, 1.0f, 1.6f, 0.35f, 0.0f, 0.46f, 0.28f, 0.08f, 0.25f, 0.00f},
    {"chip",    W_SQUARE, 2,  9.0f, 0.50f, 0.00f, 0.003f, 0.20f, 1.00f, 0.040f, 5200,  0.50f, 0.80f, 200,  0.6f, 0.20f, 0.50f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     0.0f, 0.21f, 0.22f, 0.20f, 0.15f, 0.010f},
};
static float k_d[N_INST], k_r[N_INST], k_fd[N_INST];
static float uni_ratio[N_INST][8], uni_l[N_INST][8], uni_r[N_INST][8];

static void inst_init(void) {
    for (int i = 0; i < N_INST; i++) {
        const Inst *I = &INST[i];
        k_d[i] = coef(I->d);
        k_r[i] = coef(I->r);
        k_fd[i] = coef(I->fd);
        int n = I->unison < 1 ? 1 : I->unison > 8 ? 8 : I->unison;
        float norm = 1.0f / sqrtf((float)n);
        for (int u = 0; u < n; u++) {
            float x = n > 1 ? (float)u / (float)(n - 1) * 2.0f - 1.0f : 0.0f;   /* -1..1 across the detune */
            uni_ratio[i][u] = exp2f(x * I->detune * 0.5f / 1200.0f);
            /* alternate sides, the more detuned the wider */
            float p = 0.5f + 0.5f * fabsf(x) * I->width * (u & 1 ? 1.0f : -1.0f);
            uni_l[i][u] = cosf(p * 1.5707963f) * 1.41421356f * norm;
            uni_r[i][u] = sinf(p * 1.5707963f) * 1.41421356f * norm;
        }
    }
}

static int inst_by_name(const char *s) {
    for (int i = 0; i < N_INST; i++) if (!strcmp(INST[i].name, s)) return i;
    return -1;
}

/* ------------------------------------------------------------------ voices */
enum { V_FM = 0, V_PSG = 6, V_NOISE = 9, NVOICES = 10 };
typedef struct {
    int inst, gate, live, stage;  /* stage: 0 attack, 1 decay/sustain, 2 release */
    float note, target;           /* MIDI */
    float gain, gain_to;          /* the game's loudness */
    float pl, pr;                 /* pan */
    float t, env, fenv, vph;
    float ph[8], sub, mph, mph2;
    float s1[2], s2[2], g, k;
    float hpz[2];                 /* high-pass state */
    int coef_n;
    uint32_t rng;
    uint32_t patch;
    float hp;                     /* noise voice: high-pass state */
} Voice;
static Voice voice[NVOICES];

static void pan_of(Voice *v, float p) {
    p = clampf(p, 0.0f, 1.0f);
    v->pl = cosf(p * 1.5707963f) * 1.41421356f;
    v->pr = sinf(p * 1.5707963f) * 1.41421356f;
}

static void voice_on(Voice *v, int inst, float note, float gain, float pan) {
    v->inst = inst;
    v->gate = v->live = 1;
    v->stage = 0;
    v->note = v->target = note;
    v->gain = v->gain_to = gain;
    v->t = 0.0f;
    v->fenv = 1.0f;
    v->vph = 0.0f;
    v->coef_n = 0;
    pan_of(v, pan);
    for (int u = 0; u < 8; u++) v->ph[u] = (rnd(&v->rng) + 1.0f) * 0.5f;
    v->mph = v->mph2 = 0.0f;
}

static void voice_off(Voice *v) {
    if (v->gate) { v->gate = 0; v->stage = 2; }
}

static float polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/* one sample of a voice into l/r, and its effect sends */
static void voice_run(Voice *v, float *l, float *r, float *rev, float *dly, float *cho) {
    const Inst *I = &INST[v->inst];
    const float dt = 1.0f / SR;
    v->t += dt;
    /* envelopes */
    if (v->stage == 0) {
        v->env += dt / (I->a > 0.0005f ? I->a : 0.0005f);
        if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 1; }
    } else if (v->stage == 1) {
        v->env += (I->s - v->env) * k_d[v->inst];
    } else {
        v->env -= v->env * k_r[v->inst];
        if (v->env < 0.0003f) { v->live = 0; v->env = 0.0f; return; }
    }
    if (I->s <= 0.0f && v->stage == 1 && v->env < 0.0003f) { v->live = 0; return; }
    v->fenv += (I->fs - v->fenv) * k_fd[v->inst];
    v->gain += (v->gain_to - v->gain) * 0.004f;
    /* pitch: glides towards a tied note, plus vibrato */
    if (v->note != v->target) {
        float k = I->glide > 0.0f ? dt / I->glide : 1.0f;
        v->note += (v->target - v->note) * (k > 1.0f ? 1.0f : k);
        if (fabsf(v->target - v->note) < 0.001f) v->note = v->target;
    }
    float n = v->note;
    if (I->vib > 0.0f && v->t > I->vdelay) {
        float ramp = (v->t - I->vdelay) / 0.4f;
        v->vph += TWO_PI * I->vrate * dt;
        if (v->vph > TWO_PI) v->vph -= TWO_PI;
        n += I->vib * (ramp > 1.0f ? 1.0f : ramp) * sinf(v->vph);
    }
    float hz = note_hz(n), inc = hz / SR;
    if (inc > 0.45f) inc = 0.45f;
    /* oscillators */
    float sl = 0.0f, sr = 0.0f;
    int nu = I->unison < 1 ? 1 : I->unison > 8 ? 8 : I->unison;
    switch (I->wave) {
    case W_SAW: case W_SQUARE:
        for (int u = 0; u < nu; u++) {
            float d = inc * uni_ratio[v->inst][u];
            float p = v->ph[u] += d;
            if (p >= 1.0f) p = v->ph[u] -= 1.0f;
            float s;
            if (I->wave == W_SAW) s = 2.0f * p - 1.0f - polyblep(p, d);
            else {
                float q = p + 0.5f;
                if (q >= 1.0f) q -= 1.0f;
                s = (p < 0.5f ? 1.0f : -1.0f) + polyblep(p, d) - polyblep(q, d);
            }
            sl += s * uni_l[v->inst][u];
            sr += s * uni_r[v->inst][u];
        }
        break;
    case W_SINE:
        v->ph[0] += inc;
        if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
        sl = sr = sinf(TWO_PI * v->ph[0]);
        break;
    case W_FM: {
        float idx = I->index * (I->idecay > 0.0f ? expf(-v->t / I->idecay) : 1.0f) + 0.15f;
        for (int u = 0; u < nu; u++) {
            float d = inc * uni_ratio[v->inst][u];
            v->ph[u] += d;
            if (v->ph[u] >= 1.0f) v->ph[u] -= 1.0f;
        }
        v->mph += inc * I->ratio;
        if (v->mph >= 1.0f) v->mph -= floorf(v->mph);
        float m = sinf(TWO_PI * v->mph) * idx;
        for (int u = 0; u < nu; u++) {
            float s = sinf(TWO_PI * v->ph[u] + m);
            sl += s * uni_l[v->inst][u];
            sr += s * uni_r[v->inst][u];
        }
        break;
    }
    case W_ORGAN: {                                         /* drawbars 16' 8' 5 1/3' 4' 2 2/3' 2' */
        static const float H[6] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f}, W[6] = {0.55f, 0.9f, 0.45f, 0.5f, 0.25f, 0.25f};
        float s = 0.0f;
        for (int h = 0; h < 6; h++) {
            float d = inc * H[h];
            if (d >= 0.45f) continue;
            v->ph[h] += d;
            if (v->ph[h] >= 1.0f) v->ph[h] -= 1.0f;
            s += W[h] * sinf(TWO_PI * v->ph[h]);
        }
        if (v->t < 0.006f) s += 0.3f * rnd(&v->rng) * (1.0f - v->t / 0.006f);   /* key click */
        sl = sr = s * 0.55f;
        break;
    }
    }
    float sub = I->sub;                                     /* a sine on the fundamental */
    if (sub > 0.0f && inc < 0.4f) {
        v->sub += inc;
        if (v->sub >= 1.0f) v->sub -= 1.0f;
        float s = sinf(TWO_PI * v->sub) * sub;
        sl += s; sr += s;
    }
    /* drive, then the filter after it (an amp, then its cabinet): the filter
     * also takes away what the drive would fold back above Nyquist */
    if (I->drive > 0.0f) {
        float k = 1.0f + I->drive, norm = 1.0f / tanhf(k);
        sl = tanhf(sl * k) * norm;
        sr = tanhf(sr * k) * norm;
    }
    /* filter: low-pass (TPT state variable), cutoff from the key and the envelope */
    if (I->cut < 8000.0f || I->famt > 0.0f) {
        if (v->coef_n-- <= 0) {
            v->coef_n = 7;
            float fc = I->cut * powf(hz / 261.63f, I->track) * exp2f(I->famt * v->fenv);
            fc = clampf(fc, 30.0f, 0.42f * SR);
            v->g = tanf(3.14159265f * fc / SR);
            v->k = 1.0f / (I->q > 0.5f ? I->q : 0.5f);
        }
        float g = v->g, k = v->k, a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        for (int c = 0; c < 2; c++) {
            float x = c ? sr : sl;
            float v3 = x - v->s2[c], v1 = a1 * v->s1[c] + a2 * v3, v2 = v->s2[c] + a2 * v->s1[c] + a3 * v3;
            v->s1[c] = 2.0f * v1 - v->s1[c];
            v->s2[c] = 2.0f * v2 - v->s2[c];
            if (c) sr = v2; else sl = v2;
        }
    }
    if (I->hp > 0.0f) {                                     /* one-pole high-pass */
        float kh = I->hp * TWO_PI / SR;
        v->hpz[0] += (sl - v->hpz[0]) * kh;
        v->hpz[1] += (sr - v->hpz[1]) * kh;
        sl -= v->hpz[0];
        sr -= v->hpz[1];
    }
    float a = v->env * v->gain * I->level;
    sl *= a * v->pl;
    sr *= a * v->pr;
    *l += sl;
    *r += sr;
    float m = (sl + sr) * 0.5f;
    *rev += m * I->rev;
    *dly += m * I->dly;
    *cho += m * I->cho;
}

/* the PSG's noise channel: filtered noise following the game's volume */
static void noise_run(Voice *v, float *l, float *r, float *rev) {
    v->gain += (v->gain_to * (v->gate ? 1.0f : 0.0f) - v->gain) * 0.02f;
    if (!v->gate && v->gain < 0.0005f) { v->live = 0; return; }
    float x = rnd(&v->rng);
    v->hp += (x - v->hp) * 0.35f;                           /* keep the top: a hat */
    float s = (x - v->hp) * v->gain * 0.33f;
    *l += s * v->pl;
    *r += s * v->pr;
    *rev += s * 0.15f;
}

/* ------------------------------------------------------------------ patches */
static uint8_t ym[2][256];
static int fm_key[6], dac_on;
static unsigned fx_fm, fx_psg;
static const int SLOT_OFF[4] = {0, 8, 4, 12};               /* S1, S2, S3, S4 */
static const uint8_t CARRIER[8] = {0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF};   /* bit s = slot S(s+1) */

/* patches given an instrument by ear, before the rules below (algol.ini,
 * read from the working folder, can override any of them) */
static const struct { uint32_t hash; int inst; } CURATED[] = {
    {0xE6A7D5B2u, I_LEAD},        /* Meet Them Head-On: the melody (three channels in unison) */
    {0x10ADE332u, I_BRASS},       /* Meet Them Head-On: the stabs */
};

typedef struct { uint32_t hash; int inst; int notes; } Patch;
#define NPATCH 256
static Patch patches[NPATCH];
static int npatches;
static struct { uint32_t hash; int inst; } user_map[256];
static int nuser, user_loaded;
static int log_on = -1;

static void load_user_map(void) {
    user_loaded = 1;
    FILE *f = fopen("algol.ini", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && nuser < 256) {
        char name[64];
        unsigned h;
        if (line[0] == '#' || sscanf(line, " %x = %63s", &h, name) != 2) continue;
        int i = inst_by_name(name);
        if (i >= 0) { user_map[nuser].hash = h; user_map[nuser].inst = i; nuser++; }
    }
    fclose(f);
}

static uint32_t patch_hash(int part, int c) {
    const uint8_t *R = ym[part];
    int alg = R[0xB0 + c] & 7;
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)(R[0xB0 + c] & 0x3F)) * 16777619u;
    for (int s = 0; s < 4; s++) {
        int o = SLOT_OFF[s] + c;
        uint8_t v[7] = {R[0x30 + o], (uint8_t)((CARRIER[alg] >> s & 1) ? 0 : R[0x40 + o]), R[0x50 + o],
                        R[0x60 + o], R[0x70 + o], R[0x80 + o], R[0x90 + o]};
        for (int i = 0; i < 7; i++) h = (h ^ v[i]) * 16777619u;
    }
    return h;
}

/* an instrument from the patch's own sound */
static int classify(int part, int c, float note) {
    const uint8_t *R = ym[part];
    int alg = R[0xB0 + c] & 7, fb = R[0xB0 + c] >> 3 & 7, main = 3;
    for (int s = 0; s < 4; s++)
        if ((CARRIER[alg] >> s & 1) && (R[0x40 + SLOT_OFF[s] + c] & 127) <= (R[0x40 + SLOT_OFF[main] + c] & 127)) main = s;
    int o = SLOT_OFF[main] + c;
    int ar = R[0x50 + o] & 31, d1r = R[0x60 + o] & 31, d2r = R[0x70 + o] & 31, sl = R[0x80 + o] >> 4;
    int sustained = d1r <= 4 || (sl <= 3 && d2r <= 9);
    int fast_decay = d2r >= 24 || (d1r >= 20 && sl >= 4);
    float cmul = (R[0x30 + o] & 15) ? (float)(R[0x30 + o] & 15) : 0.5f;
    int nonint = 0, high = 0;              /* a modulator off the harmonic series / far up it */
    float bright = (float)fb / 7.0f * 0.4f;
    for (int s = 0; s < 4; s++) {
        if (CARRIER[alg] >> s & 1) continue;
        int so = SLOT_OFF[s] + c;
        float m = (R[0x30 + so] & 15) ? (float)(R[0x30 + so] & 15) : 0.5f, ratio = m / cmul;
        int tl = R[0x40 + so] & 127;
        if (tl < 50 && fabsf(ratio - roundf(ratio)) > 0.2f) nonint = 1;
        if (tl < 50 && ratio >= 7.0f) high = 1;
        if (tl < 40) bright += (40.0f - (float)tl) / 40.0f;
    }
    if (alg == 7) return sustained ? I_ORGAN : I_KEYS;
    if (note < 47.0f) return fast_decay ? I_GUITAR : I_BASS;
    if (ar < 18) return sustained ? I_STRINGS : I_BRASS;
    if (!sustained) return nonint || (high && note >= 72.0f) ? I_BELL : fast_decay ? I_PLUCK : I_KEYS;
    if (alg >= 4) return bright > 1.2f ? I_BRASS : I_LEAD;
    return I_LEAD;
}

static int patch_inst(int part, int c, float note, uint32_t *hash_out) {
    uint32_t h = patch_hash(part, c);
    *hash_out = h;
    for (int i = 0; i < npatches; i++)
        if (patches[i].hash == h) { patches[i].notes++; return patches[i].inst; }
    if (!user_loaded) load_user_map();
    int inst = -1;
    for (int i = 0; i < nuser; i++) if (user_map[i].hash == h) inst = user_map[i].inst;
    for (size_t i = 0; inst < 0 && i < sizeof CURATED / sizeof CURATED[0]; i++) if (CURATED[i].hash == h) inst = CURATED[i].inst;
    if (inst < 0) inst = classify(part, c, note);
    if (npatches < NPATCH) {
        patches[npatches].hash = h;
        patches[npatches].inst = inst;
        patches[npatches].notes = 1;
        npatches++;
    }
    if (log_on < 0) { const char *v = getenv("PSIV_ALGOL_LOG"); log_on = v && *v == '1'; }
    if (log_on) {
        const uint8_t *R = ym[part];
        printf("algol: patch %08X -> %s (FM%d, note %.1f, alg %d fb %d)\n", h, INST[inst].name,
               c + 3 * part + 1, note, R[0xB0 + c] & 7, R[0xB0 + c] >> 3 & 7);
    }
    return inst;
}

/* ------------------------------------------------------------------ the FM decoder */
static float fm_mul[6];               /* the main carrier's multiple, per channel's patch */

static float fm_note_of(int ch) {
    int part = ch / 3, c = ch % 3;
    const uint8_t *R = ym[part];
    int fnum = (R[0xA4 + c] & 7) << 8 | R[0xA0 + c], block = R[0xA4 + c] >> 3 & 7;
    float f = (float)fnum * (float)(1 << block) * SR / 2097152.0f * fm_mul[ch];
    return f > 8.0f ? 69.0f + 12.0f * log2f(f / 440.0f) : 0.0f;
}

static float fm_gain_of(int ch) {
    int part = ch / 3, c = ch % 3, alg = ym[part][0xB0 + c] & 7, tl = 127;
    for (int s = 0; s < 4; s++)
        if (CARRIER[alg] >> s & 1) {
            int t = ym[part][0x40 + SLOT_OFF[s] + c] & 127;
            if (t < tl) tl = t;
        }
    float g = powf(10.0f, -0.75f * (float)(tl - 14) / 20.0f);
    return g > 2.0f ? 2.0f : g;
}

static float fm_pan_of(int ch) {
    uint8_t b = ym[ch / 3][0xB4 + ch % 3];
    int L = b >> 7 & 1, R = b >> 6 & 1;
    return L && !R ? 0.2f : R && !L ? 0.8f : 0.5f;
}

static void fm_note_on(int ch) {
    Voice *v = &voice[V_FM + ch];
    if ((ch == 5 && dac_on) || (fx_fm >> ch & 1)) { voice_off(v); return; }
    int part = ch / 3, c = ch % 3, alg = ym[part][0xB0 + c] & 7, main = 3;
    for (int s = 0; s < 4; s++)
        if ((CARRIER[alg] >> s & 1) && (ym[part][0x40 + SLOT_OFF[s] + c] & 127) <= (ym[part][0x40 + SLOT_OFF[main] + c] & 127))
            main = s;
    int mul = ym[part][0x30 + SLOT_OFF[main] + c] & 15;
    fm_mul[ch] = mul ? (float)mul : 0.5f;
    float note = fm_note_of(ch);
    if (note <= 0.0f || note > 120.0f) { voice_off(v); return; }
    uint32_t h;
    int inst = patch_inst(part, c, note, &h);
    float gain = fm_gain_of(ch);
    /* the game doubles parts on several channels for thickness: an exact
     * double of a note already sounding is left out (the unison covers it) */
    for (int k = 0; k < 6; k++) {
        Voice *o = &voice[V_FM + k];
        if (k != ch && o->gate && o->patch == h && fabsf(o->target - note) < 0.3f) { voice_off(v); v->patch = h; return; }
    }
    voice_on(v, inst, note, gain, fm_pan_of(ch));
    v->patch = h;
}

void algol_ym_write(int part, uint8_t reg, uint8_t val) {
    ym[part][reg] = val;
    if (part == 0 && reg == 0x2B) dac_on = val >> 7;
    if (!enabled) {                                          /* only the registers */
        if (part == 0 && reg == 0x28 && (val & 3) != 3) fm_key[(val & 3) + 3 * (val >> 2 & 1)] = (val >> 4) != 0;
        return;
    }
    if (part == 0 && reg == 0x28) {
        int c = val & 3, g = val >> 2 & 1;
        if (c == 3) return;
        int ch = c + 3 * g, slots = val >> 4;
        if (slots) fm_note_on(ch);
        else if (fm_key[ch]) voice_off(&voice[V_FM + ch]);
        fm_key[ch] = slots != 0;
        return;
    }
    if (part == 0 && reg == 0x2B) {
        if (dac_on) voice_off(&voice[V_FM + 5]);
        return;
    }
    if (reg >= 0xA0 && reg <= 0xA2) {                       /* a new F-number: vibrato, slides */
        int ch = (reg - 0xA0) + 3 * part;
        Voice *v = &voice[V_FM + ch];
        if (fm_key[ch] && v->gate) {
            float n = fm_note_of(ch);
            if (n > 0.0f) v->target = n;
            if (INST[v->inst].glide <= 0.0f && fabsf(n - v->note) < 1.5f) v->note = n;   /* follow vibrato */
        }
        return;
    }
    if (reg >= 0x40 && reg <= 0x4F && (reg & 3) < 3) {     /* total level: the game's volume */
        int ch = (reg & 3) + 3 * part;
        if (fm_key[ch] && voice[V_FM + ch].gate) voice[V_FM + ch].gain_to = fm_gain_of(ch);
        return;
    }
    if (reg >= 0xB4 && reg <= 0xB6) pan_of(&voice[V_FM + (reg - 0xB4) + 3 * part], fm_pan_of((reg - 0xB4) + 3 * part));
}

/* ------------------------------------------------------------------ the PSG decoder */
static int psg_latch, psg_period[3], psg_att[4] = {15, 15, 15, 15}, psg_noise;

static void psg_update(int ch) {
    Voice *v = &voice[ch < 3 ? V_PSG + ch : V_NOISE];
    int audible = psg_att[ch] < 15 && !(fx_psg >> ch & 1);
    float gain = powf(10.0f, -2.0f * (float)psg_att[ch] / 20.0f) * 2.2f;
    if (ch == 3) {
        if (audible) { v->gain_to = gain; if (!v->gate) { v->gate = v->live = 1; pan_of(v, 0.55f); } }
        else v->gate = 0;
        return;
    }
    float note = psg_period[ch] > 1 ? 69.0f + 12.0f * log2f(3579545.0f / (32.0f * (float)psg_period[ch]) / 440.0f) : 0.0f;
    if (!audible || note <= 0.0f || note > 108.0f) { voice_off(v); return; }
    static const float PAN[3] = {0.3f, 0.7f, 0.5f};
    if (!v->gate || fabsf(note - v->target) > 0.6f) {      /* a new note */
        if (v->gate && v->live) { v->target = note; v->note = note; v->stage = 0; v->t = 0.0f; v->fenv = 1.0f; }
        else voice_on(v, I_CHIP, note, gain, PAN[ch]);
    } else v->target = note;
    v->gain_to = gain;
}

void algol_psg_write(uint8_t val) {
    if (val & 0x80) psg_latch = val >> 4 & 7;
    int ch = psg_latch >> 1;
    if (psg_latch & 1) psg_att[ch] = val & 15;
    else if (ch < 3) {
        if (val & 0x80) psg_period[ch] = (psg_period[ch] & 0x3F0) | (val & 15);
        else psg_period[ch] = (psg_period[ch] & 15) | (val & 0x3F) << 4;
    } else psg_noise = val & 7;
    if (enabled) psg_update(ch);
}

/* ------------------------------------------------------------------ the drum kit */
enum { D_KICK, D_SNARE, D_HAT, D_RIDE, D_TOM, D_CRASH, D_SAMPLE };
static const struct { int kind; float tone, decay, level; } KIT[18] = {
    {D_SAMPLE, 0, 0, 0},
    {D_SNARE, 190, 0.17f, 0.95f},  /*  1 snare */
    {D_KICK,   52, 0.30f, 1.20f},  /*  2 kick */
    {D_RIDE,    0, 0.45f, 0.45f},  /*  3 ride / shaker */
    {D_HAT,     0, 0.05f, 0.55f},  /*  4 closed hat */
    {D_TOM,    95, 0.40f, 1.00f},  /*  5 low tom */
    {D_CRASH,   0, 1.40f, 0.55f},  /*  6 crash */
    {D_SAMPLE,  0, 0, 0.90f},      /*  7 a pitched instrument */
    {D_SAMPLE,  0, 0, 0.90f},      /*  8 a voice */
    {D_HAT,     0, 0.32f, 0.45f},  /*  9 open hat */
    {D_CRASH,   0, 1.20f, 0.50f},  /* 10 crash */
    {D_SNARE, 175, 0.20f, 0.95f},  /* 11 snare */
    {D_SNARE, 160, 0.24f, 0.90f},  /* 12 snare, lower */
    {D_KICK,   46, 0.34f, 1.25f},  /* 13 kick, deep */
    {D_KICK,   58, 0.24f, 1.15f},  /* 14 kick, punchy */
    {D_SAMPLE,  0, 0, 0.90f},      /* 15 */
    {D_SAMPLE,  0, 0, 0.90f},      /* 16 */
    {D_SAMPLE,  0, 0, 0.90f},      /* 17 */
};
static const uint8_t BANK_TBL[17][2] = {{0,0},{0,1},{0,2},{0,3},{0,4},{0,5},{1,0},{0,7},{2,0},{1,1},{0,8},{2,1},{0,9},{2,2},{0,6},{3,0},{2,3}};

typedef struct {
    int on, kind;
    float t, tone, decay, gain, pl, pr, ph;
    float m[6];                   /* metallic oscillators: cycles per sample */
    float hp1, hp2, lp1, bp1;
    uint32_t rng;
    const uint8_t *pcm;
    int len;
    double pos, step;
    float prev;
} Drum;
#define NDRUMS 10
static Drum drums[NDRUMS];

void algol_dac_hit(int sample, int pitch, int volume, int pan) {
    if (!enabled || sample < 1 || sample > 17) return;
    if (fx_fm >> 5 & 1) return;                             /* an effect holds the DAC */
    Drum *d = NULL;
    for (int i = 0; i < NDRUMS; i++) if (!drums[i].on) { d = &drums[i]; break; }
    if (!d) {                                               /* steal the oldest */
        d = &drums[0];
        for (int i = 1; i < NDRUMS; i++) if (drums[i].t > d->t) d = &drums[i];
    }
    uint32_t rng = d->rng ? d->rng : 0x1234567u + (uint32_t)sample;
    memset(d, 0, sizeof *d);
    d->rng = rng;
    d->on = 1;
    d->kind = KIT[sample].kind;
    float semis = (float)(pitch - 0x9A) * (12.0f / 12.6f);  /* the DAC's rate steps, about a semitone */
    d->tone = KIT[sample].tone * exp2f(semis / 12.0f);
    d->decay = KIT[sample].decay;
    d->gain = KIT[sample].level * (volume > 0 && volume <= 16 ? (float)volume / 16.0f : 1.0f);
    float p = (pan & 0xC0) == 0x80 ? 0.25f : (pan & 0xC0) == 0x40 ? 0.75f : 0.5f;
    if (d->kind == D_HAT || d->kind == D_RIDE) p += 0.12f;
    if (d->kind == D_CRASH) p -= 0.15f;
    d->pl = cosf(p * 1.5707963f) * 1.41421356f;
    d->pr = sinf(p * 1.5707963f) * 1.41421356f;
    static const float METAL[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    for (int i = 0; i < 6; i++) d->m[i] = METAL[i] * 2.6f / SR;
    if (d->kind == D_SAMPLE) {
        static const uint32_t BASE[4] = {0xE0000, 0xE8000, 0xF0000, 0xF8000};
        uint32_t base = BASE[BANK_TBL[sample - 1][0]], e = base + BANK_TBL[sample - 1][1] * 8u;
        if (!m68k_rom || e + 4 > m68k_rom_size) { d->on = 0; return; }
        uint32_t start = (uint32_t)(m68k_rom[e] | m68k_rom[e + 1] << 8), len = (uint32_t)(m68k_rom[e + 2] | m68k_rom[e + 3] << 8);
        uint32_t off = base + start - 0x8000;
        if (start < 0x8000 || off + len > m68k_rom_size) { d->on = 0; return; }
        d->pcm = m68k_rom + off;
        d->len = (int)len;
        float rate = 2000.0f * powf(10300.0f / 2000.0f, (float)(pitch - 0x81) / 28.0f);
        d->step = rate / SR;
    }
}

/* six square waves at inharmonic ratios: the metal of hats and cymbals */
static float metal(Drum *d) {
    float s = 0.0f;
    for (int i = 0; i < 6; i++) s += fmodf(d->t * SR * d->m[i], 1.0f) < 0.5f ? 1.0f : -1.0f;
    return s / 6.0f;
}

static void drum_run(Drum *d, float *l, float *r, float *rev) {
    const float dt = 1.0f / SR;
    float s = 0.0f, t = d->t;
    d->t += dt;
    switch (d->kind) {
    case D_KICK: {
        float f = d->tone + 120.0f * expf(-t / 0.032f);
        d->ph += f / SR;
        if (d->ph >= 1.0f) d->ph -= 1.0f;
        float body = sinf(TWO_PI * d->ph) * expf(-t / d->decay);
        float click = rnd(&d->rng) * expf(-t / 0.0025f) * 0.5f;
        d->lp1 += (click - d->lp1) * 0.25f;
        s = tanhf((body + d->lp1) * 1.8f) * 0.85f;
        if (t > d->decay * 6.0f) d->on = 0;
        break;
    }
    case D_SNARE: {
        d->ph += d->tone / SR;
        if (d->ph >= 1.0f) d->ph -= 1.0f;
        float tone = (sinf(TWO_PI * d->ph) + 0.45f * sinf(TWO_PI * d->ph * 1.78f)) * expf(-t / 0.06f);
        float n = rnd(&d->rng);
        d->hp1 += (n - d->hp1) * 0.12f;                     /* high-pass ~1 kHz */
        float hn = n - d->hp1;
        d->lp1 += (hn - d->lp1) * 0.55f;                    /* and soften the very top */
        s = 0.55f * tone + 0.95f * d->lp1 * expf(-t / d->decay);
        if (t > d->decay * 6.0f) d->on = 0;
        break;
    }
    case D_HAT: case D_RIDE: case D_CRASH: {
        float n = rnd(&d->rng), mt = metal(d);
        float x = d->kind == D_RIDE ? 0.6f * mt + 0.4f * n : d->kind == D_CRASH ? 0.45f * mt + 0.55f * n : 0.5f * mt + 0.5f * n;
        float k = d->kind == D_HAT ? 0.5f : d->kind == D_RIDE ? 0.3f : 0.35f;
        d->hp1 += (x - d->hp1) * k;
        float h = x - d->hp1;
        d->hp2 += (h - d->hp2) * k;
        h = h - d->hp2;
        float att = d->kind == D_CRASH ? (t < 0.002f ? t / 0.002f : 1.0f) : 1.0f;
        s = h * att * expf(-t / d->decay) * (d->kind == D_RIDE ? 0.8f + 0.2f * sinf(TWO_PI * 3200.0f * t) : 1.0f);
        if (t > d->decay * 7.0f) d->on = 0;
        break;
    }
    case D_TOM: {
        float f = d->tone * (1.0f + 0.6f * expf(-t / 0.045f));
        d->ph += f / SR;
        if (d->ph >= 1.0f) d->ph -= 1.0f;
        float n = rnd(&d->rng) * expf(-t / 0.02f) * 0.25f;
        s = tanhf((sinf(TWO_PI * d->ph) * expf(-t / d->decay) + n) * 1.4f) * 0.9f;
        if (t > d->decay * 6.0f) d->on = 0;
        break;
    }
    case D_SAMPLE: {                                        /* the ROM's own: interpolated, smoothed */
        int i = (int)d->pos;
        if (i + 1 >= d->len) { d->on = 0; return; }
        float fr = (float)(d->pos - (double)i);
        float a = ((float)d->pcm[i] - 128.0f) / 128.0f, b = ((float)d->pcm[i + 1] - 128.0f) / 128.0f;
        float x = a + (b - a) * fr;
        float k = (float)d->step * 2.4f;                    /* low-pass near the sample's own Nyquist */
        if (k > 1.0f) k = 1.0f;
        d->lp1 += (x - d->lp1) * k;
        d->bp1 += (d->lp1 - d->bp1) * k;
        s = d->bp1;
        d->pos += d->step;
        break;
    }
    }
    s *= d->gain * 0.2f;                                    /* the kit sits with the original's */
    *l += s * d->pl;
    *r += s * d->pr;
    *rev += s * (d->kind == D_KICK ? 0.04f : d->kind == D_SNARE ? 0.22f : d->kind == D_TOM ? 0.18f : 0.12f);
}

/* ------------------------------------------------------------------ effects */
#define RV_N 8
static const int RV_LEN[RV_N] = {1103, 1361, 1553, 1801, 2039, 2297, 2551, 2803};
static float rv_buf[RV_N][2816], rv_lp[RV_N], rv_g[RV_N];
static int rv_pos[RV_N];
static float pre_buf[1024], rv_hp;
static int pre_pos;
#define CH_LEN 2048
static float ch_buf[CH_LEN];
static int ch_pos;
static float ch_ph;
#define DL_LEN 24000
static float dl_l[DL_LEN], dl_r[DL_LEN], dl_lp_l, dl_lp_r, dl_hp;
static int dl_pos;

static void fx_init(void) {
    for (int i = 0; i < RV_N; i++) {
        rv_g[i] = powf(10.0f, -3.0f * (float)RV_LEN[i] / (SR * 1.9f));   /* 1.9 s to -60 dB */
        rv_pos[i] = 0;
        rv_lp[i] = 0.0f;
    }
    memset(rv_buf, 0, sizeof rv_buf);
    memset(pre_buf, 0, sizeof pre_buf);
    memset(ch_buf, 0, sizeof ch_buf);
    memset(dl_l, 0, sizeof dl_l);
    memset(dl_r, 0, sizeof dl_r);
}

static void reverb(float in, float *l, float *r) {
    rv_hp += (in - rv_hp) * 0.03f;                          /* send high-passed (~260 Hz): no mud */
    float x = in - rv_hp;
    pre_buf[pre_pos] = x;                                   /* ~16 ms pre-delay */
    pre_pos = (pre_pos + 1) % 860;
    x = pre_buf[pre_pos];
    float y[RV_N];
    for (int i = 0; i < RV_N; i++) {
        float v = rv_buf[i][rv_pos[i]];
        rv_lp[i] += (v - rv_lp[i]) * 0.42f;                 /* damping */
        y[i] = rv_lp[i];
    }
    /* Hadamard mix, 8 x 8 */
    float h[RV_N];
    for (int i = 0; i < RV_N; i++) h[i] = y[i];
    for (int len = 1; len < RV_N; len <<= 1)
        for (int i = 0; i < RV_N; i += len << 1)
            for (int j = i; j < i + len; j++) { float a = h[j], b = h[j + len]; h[j] = a + b; h[j + len] = a - b; }
    for (int i = 0; i < RV_N; i++) {
        rv_buf[i][rv_pos[i]] = x + h[i] * 0.35355339f * rv_g[i];
        if (++rv_pos[i] >= RV_LEN[i]) rv_pos[i] = 0;
    }
    *l += (y[0] - y[2] + y[4] - y[6]) * 0.5f;
    *r += (y[1] - y[3] + y[5] - y[7]) * 0.5f;
}

static void chorus(float in, float *l, float *r) {
    ch_buf[ch_pos] = in;
    ch_ph += 0.27f / SR;
    if (ch_ph >= 1.0f) ch_ph -= 1.0f;
    for (int c = 0; c < 2; c++) {
        float d = (14.0f + 3.0f * sinf(TWO_PI * (ch_ph + (float)c * 0.25f))) * 0.001f * SR;
        float p = (float)ch_pos - d;
        while (p < 0.0f) p += CH_LEN;
        int i = (int)p;
        float f = p - (float)i, a = ch_buf[i % CH_LEN], b = ch_buf[(i + 1) % CH_LEN];
        if (c) *r += a + (b - a) * f; else *l += a + (b - a) * f;
    }
    ch_pos = (ch_pos + 1) % CH_LEN;
}

static void delay_fx(float in, float *l, float *r) {
    int tl = (int)(0.36f * SR), tr = (int)(0.27f * SR);
    float yl = dl_l[(dl_pos - tl + DL_LEN) % DL_LEN], yr = dl_r[(dl_pos - tr + DL_LEN) % DL_LEN];
    dl_lp_l += (yl - dl_lp_l) * 0.35f;
    dl_lp_r += (yr - dl_lp_r) * 0.35f;
    dl_hp += (in - dl_hp) * 0.05f;
    dl_l[dl_pos] = (in - dl_hp) + dl_lp_r * 0.38f;           /* ping-pong */
    dl_r[dl_pos] = dl_lp_l * 0.38f;
    dl_pos = (dl_pos + 1) % DL_LEN;
    *l += dl_lp_l;
    *r += dl_lp_r;
}

/* ------------------------------------------------------------------ the frame */
static int ready;

/* every voice and drum quiet, the effects emptied (the registers kept) */
static void silence(void) {
    if (!ready) { inst_init(); ready = 1; }
    memset(voice, 0, sizeof voice);
    memset(drums, 0, sizeof drums);
    for (int i = 0; i < NVOICES; i++) voice[i].rng = 0x9E3779B9u * (uint32_t)(i + 1);
    fx_init();
}

/* the console reset: the chips start over too */
void algol_reset(void) {
    silence();
    memset(ym, 0, sizeof ym);
    memset(fm_key, 0, sizeof fm_key);
    dac_on = 0;
    fx_fm = fx_psg = 0;
    psg_latch = psg_noise = 0;
    memset(psg_period, 0, sizeof psg_period);
    for (int i = 0; i < 4; i++) psg_att[i] = 15;
}

unsigned algol_fm_effects(void) { return fx_fm; }
unsigned algol_psg_effects(void) { return fx_psg; }

/* which channels the driver's sound-effect tracks hold: the music has the rest */
void algol_driver_tick(void) {
    unsigned fm = 0, psg = 0;
    for (int i = 0; i < 9; i++) {
        unsigned a = 0x5220u + (unsigned)i * 0x30u;
        uint8_t flags = m68k_ram[a], chn = m68k_ram[a + 1];
        if (!(flags & 0x80)) continue;
        if (chn & 0x80) psg |= 1u << (chn >> 5 & 3);
        else if ((chn & 3) < 3) fm |= 1u << ((chn & 3) + 3 * (chn >> 2 & 1));
    }
    unsigned gained_fm = fm & ~fx_fm, gained_psg = psg & ~fx_psg;
    fx_fm = fm;
    fx_psg = psg;
    for (int ch = 0; ch < 6; ch++) if (gained_fm >> ch & 1) voice_off(&voice[V_FM + ch]);
    for (int ch = 0; ch < 4; ch++) if (gained_psg >> ch & 1) voice_off(&voice[ch < 3 ? V_PSG + ch : V_NOISE]);
}

/* PSIV_CHANNELS=hex (for tuning): only these are heard -- bits 0-5 FM 1-6,
 * 6-8 PSG 1-3, 9 noise, 10 drums; sound.c applies the same to the chips */
static unsigned solo_mask(void) {
    static int read;
    static unsigned mask = 0x7FF;
    if (!read) { const char *v = getenv("PSIV_CHANNELS"); if (v && *v) mask = (unsigned)strtoul(v, NULL, 16); read = 1; }
    return mask;
}

void algol_render(float *left, float *right) {
    if (!ready) silence();
    float l = 0.0f, r = 0.0f, rev = 0.0f, dly = 0.0f, cho = 0.0f;
    unsigned solo = solo_mask();
    for (int i = 0; i < NVOICES; i++) {
        Voice *v = &voice[i];
        if (!v->live) continue;
        float vl = 0.0f, vr = 0.0f, vrev = 0.0f, vdly = 0.0f, vcho = 0.0f;
        if (i == V_NOISE) noise_run(v, &vl, &vr, &vrev);
        else voice_run(v, &vl, &vr, &vrev, &vdly, &vcho);
        if (!(solo >> i & 1)) continue;
        l += vl; r += vr; rev += vrev; dly += vdly; cho += vcho;
    }
    for (int i = 0; i < NDRUMS; i++) {
        if (!drums[i].on) continue;
        float dl = 0.0f, dr = 0.0f, drev = 0.0f;
        drum_run(&drums[i], &dl, &dr, &drev);
        if (!(solo >> 10 & 1)) continue;
        l += dl; r += dr; rev += drev;
    }
    float el = 0.0f, er = 0.0f;
    reverb(rev, &el, &er);
    chorus(cho, &el, &er);
    delay_fx(dly, &el, &er);
    l += el;
    r += er;
    /* master: some air (about +4 dB above 3 kHz), then a soft knee instead of clipping */
    static float air_l, air_r;
    air_l += (l - air_l) * 0.30f;
    air_r += (r - air_r) * 0.30f;
    l += 0.6f * (l - air_l);
    r += 0.6f * (r - air_r);
    l *= 0.42f;
    r *= 0.42f;
    l = l > 0.8f ? 0.8f + 0.2f * tanhf((l - 0.8f) / 0.2f) : l < -0.8f ? -0.8f + 0.2f * tanhf((l + 0.8f) / 0.2f) : l;
    r = r > 0.8f ? 0.8f + 0.2f * tanhf((r - 0.8f) / 0.2f) : r < -0.8f ? -0.8f + 0.2f * tanhf((r + 0.8f) / 0.2f) : r;
    *left = l * 32767.0f;
    *right = r * 32767.0f;
}
