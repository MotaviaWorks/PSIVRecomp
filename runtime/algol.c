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
#include "snesbank.h"
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR      ((float)SOUND_RATE)
#define BANK_LEVEL      3.3f                /* a bank's instruments at Algol's level (each sample's own gain evens them) */
#define BANK_DRUM_LEVEL 2.7f
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
/* (levels 2026-11-13: brass, bell, guitar and the lead down, strings, bass and the chip voices up, part of the way to
 * the game's own balance -- each instrument's share of the mix against the chips' channels, over twelve songs) */
static const Inst INST[N_INST] = {
    /* name      wave     uni det   wid   sub    a       d      s      r       cut    trk   q      hp    famt  fd     fs     vib    vr    vd     ratio idx   idec   drive lvl    rev    dly    cho    glide */
    {"bass",    W_SAW,    2,  7.0f, 0.10f, 0.45f, 0.003f, 0.30f, 0.65f, 0.050f, 700,   0.55f, 1.10f, 30,   2.2f, 0.16f, 0.25f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     1.6f, 0.30f, 0.04f, 0.00f, 0.00f, 0.02f},
    {"lead",    W_SAW,    5, 16.0f, 0.65f, 0.00f, 0.008f, 0.40f, 0.85f, 0.160f, 7000,  0.45f, 0.90f, 160,  0.7f, 0.30f, 0.60f, 0.10f, 5.5f, 0.30f, 0,    0,    0,     0.5f, 0.34f, 0.22f, 0.16f, 0.00f, 0.03f},
    {"brass",   W_SAW,    3, 11.0f, 0.50f, 0.00f, 0.030f, 0.45f, 0.80f, 0.140f, 3400,  0.70f, 0.80f, 130,  1.4f, 0.30f, 0.55f, 0.08f, 5.0f, 0.30f, 0,    0,    0,     0.7f, 0.31f, 0.25f, 0.05f, 0.20f, 0.00f},
    {"strings", W_SAW,    6, 18.0f, 0.90f, 0.00f, 0.110f, 0.60f, 0.90f, 0.450f, 4200,  0.40f, 0.70f, 170,  0.3f, 0.60f, 0.80f, 0.06f, 5.0f, 0.40f, 0,    0,    0,     0.0f, 0.36f, 0.35f, 0.00f, 0.60f, 0.00f},
    {"pluck",   W_SAW,    2,  6.0f, 0.40f, 0.00f, 0.002f, 0.30f, 0.00f, 0.180f, 1400,  0.80f, 1.00f, 140,  3.0f, 0.11f, 0.00f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     0.4f, 0.55f, 0.22f, 0.14f, 0.10f, 0.00f},
    {"guitar",  W_SAW,    2,  9.0f, 0.35f, 0.00f, 0.002f, 0.10f, 0.20f, 0.040f, 5200,  0.15f, 1.40f, 110,  0.6f, 0.06f, 0.30f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     3.0f, 0.24f, 0.12f, 0.06f, 0.00f, 0.00f},
    {"bell",    W_FM,     1,  0.0f, 0.30f, 0.00f, 0.001f, 1.20f, 0.00f, 0.700f, 12000, 0.00f, 0.70f, 200,  0.0f, 0.00f, 0.00f, 0.00f, 0.0f, 0.00f, 3.5f, 2.2f, 0.60f, 0.0f, 0.28f, 0.35f, 0.15f, 0.00f, 0.00f},
    {"organ",   W_ORGAN,  1,  0.0f, 0.40f, 0.00f, 0.004f, 0.10f, 1.00f, 0.060f, 7000,  0.20f, 0.70f, 90,   0.0f, 0.00f, 0.00f, 0.06f, 6.0f, 0.20f, 0,    0,    0,     0.3f, 0.34f, 0.25f, 0.00f, 0.50f, 0.00f},
    {"keys",    W_FM,     2,  5.0f, 0.40f, 0.00f, 0.002f, 0.90f, 0.35f, 0.300f, 9000,  0.00f, 0.70f, 120,  0.0f, 0.00f, 0.00f, 0.05f, 5.0f, 0.30f, 1.0f, 1.6f, 0.35f, 0.0f, 0.46f, 0.28f, 0.08f, 0.25f, 0.00f},
    {"chip",    W_SQUARE, 2,  9.0f, 0.50f, 0.00f, 0.003f, 0.20f, 1.00f, 0.040f, 5200,  0.50f, 0.80f, 200,  0.6f, 0.20f, 0.50f, 0.00f, 0.0f, 0.00f, 0,    0,    0,     0.0f, 0.23f, 0.22f, 0.20f, 0.15f, 0.010f},
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
    int bs;                       /* the bank sample it plays (snesbank.h), -1: Algol's own */
    double spos[2];               /* where in the sample (two reads, detuned, for width) */
} Voice;
static Voice voice[NVOICES];

/* ------------------------------------------------------------------ an instrument bank */
/* A SNES game's samples (snesbank.h) in place of the synth: per instrument,
 * per drum, a sample (-1: keep Algol's own) and a transposition */
static int bank_on, bank_inst[N_INST], bank_drum[8];
static float bank_tr[N_INST];
static struct { char name[16]; int sample; float transpose; } bank_user[24];   /* algol.ini: "bank lead = 35 [-12]" */
static int nbank_user;
static const char *const DRUM_NAMES[] = {"kick", "snare", "hat", "ride", "tom", "crash"};   /* D_KICK .. D_CRASH */

static void bank_resolve(void) {
    for (int u = 0; u < nbank_user; u++) snesbank_set_voice(bank_user[u].name, bank_user[u].sample, bank_user[u].transpose);
    for (int i = 0; i < N_INST; i++) bank_inst[i] = bank_on ? snesbank_voice(INST[i].name, &bank_tr[i]) : -1;
    for (int k = 0; k < 8; k++) bank_drum[k] = bank_on && k < 6 ? snesbank_voice(DRUM_NAMES[k], NULL) : -1;
}

int algol_set_bank(const char *rom_path) {
    bank_on = rom_path && *rom_path && snesbank_load(rom_path);
    bank_resolve();
    const char *lv = getenv("PSIV_ALGOL_LOG");
    if (bank_on && lv && atoi(lv) > 0) {                 /* the samples, and who plays what */
        printf("algol: bank %s, %d samples\n", snesbank_name(), snesbank_count());
        for (int i = 0; i < snesbank_count(); i++) {
            const BankSample *s = snesbank_sample(i);
            if (s) printf("algol:   %2d  %7.1f Hz  %s%s\n", i, s->root, s->loop >= 0 ? "loops" : "once",
                          s->decay > 0.0f ? ", decays" : "");
        }
        for (int i = 0; i < N_INST; i++) printf("algol: bank %-8s -> %d\n", INST[i].name, bank_inst[i]);
        for (int k = 0; k < 6; k++) printf("algol: bank %-8s -> %d\n", DRUM_NAMES[k], bank_drum[k]);
    }
    return bank_on;
}
const char *algol_bank_name(void) { return bank_on ? snesbank_name() : ""; }

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
    v->bs = bank_on ? bank_inst[inst] : -1;
    v->spos[0] = v->spos[1] = 0.0;
}

/* one read of a bank sample at pos (cubic), advancing by step; looping, or
 * 0 once a one-shot is over */
static float bank_read(const BankSample *s, double *pos, double step) {
    double p = *pos;
    if (p >= (double)(s->len - 1)) {
        if (s->loop < 0) return 0.0f;
        double span = (double)(s->len - s->loop);
        p = (double)s->loop + fmod(p - (double)s->loop, span);
    }
    int i = (int)p;
    float f = (float)(p - (double)i);
    #define AT(k) (s->pcm[(k) < s->len ? (k) : (s->loop >= 0 ? s->loop + ((k) - s->len) % (s->len - s->loop) : s->len - 1)])
    float y0 = i > 0 ? s->pcm[i - 1] : s->pcm[0], y1 = s->pcm[i], y2 = AT(i + 1), y3 = AT(i + 2);
    #undef AT
    *pos = p + step;
    /* Catmull-Rom */
    return y1 + 0.5f * f * (y2 - y0 + f * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + f * (3.0f * (y1 - y2) + y3 - y0)));
}

static void voice_off(Voice *v) {
    if (v->gate) { v->gate = 0; v->stage = 2; }
}

static float polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/* Algol's own sound: the oscillators, a sub, drive, then the filter */
static void synth_osc(Voice *v, const Inst *I, float n, float *out_l, float *out_r) {
    float hz = note_hz(n), inc = hz / SR;
    if (inc > 0.45f) inc = 0.45f;
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
    *out_l = sl;
    *out_r = sr;
}

/* A bank's sample at the note (snesbank.h): read twice, a few cents apart,
 * left and right, for the instruments Algol voices in unison, once for the
 * rest; under the game's own envelope after its attack: down to its
 * sustain level, then its decay */
static void bank_osc(Voice *v, const Inst *I, float n, float *out_l, float *out_r) {
    const BankSample *s = snesbank_sample(v->bs);
    if (!s) { *out_l = *out_r = 0.0f; return; }
    double step = (double)note_hz(n + bank_tr[v->inst]) / (double)s->root * (32000.0 / (double)SR);
    if (step > 6.0) step = 6.0;
    float a, b;
    if (I->unison >= 3) {
        a = bank_read(s, &v->spos[0], step * 0.9965);
        b = bank_read(s, &v->spos[1], step * 1.0035);
    } else a = b = bank_read(s, &v->spos[0], step);
    float lvl = s->level + (1.0f - s->level) * expf(-v->t / 0.016f);
    if (s->decay > 0.0f) lvl *= expf(-v->t / s->decay);
    float g = s->gain * lvl * BANK_LEVEL;
    *out_l = (a * 0.8f + b * 0.2f) * g;
    *out_r = (b * 0.8f + a * 0.2f) * g;
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
    float sl = 0.0f, sr = 0.0f;
    if (v->bs >= 0) bank_osc(v, I, n, &sl, &sr);           /* a SNES game's sample (snesbank.h) */
    else synth_osc(v, I, n, &sl, &sr);
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

/* Drum styles, per song: standard, or hard rock -- a kick with a beater
 * knock and drive, a snare with a crack and long wires, deeper toms,
 * brighter cymbals, all through a room and a punchy bus compressor, and a
 * hi-hat on the eighths locked to the song's own backbeat, a ride in its
 * place in the louder sections */
enum { STYLE_STANDARD, STYLE_ROCK };
static const uint8_t ROCK_SONGS[] = {
    0xAC,                         /* The End of the Millennium (the title) */
    0x8F,                         /* Meet Them Head-On */
    0x95,                         /* Defeat at a Blow */
    0xA0,                         /* The King of Terrors */
    0x98,                         /* Edge of Darkness */
    0xA9,                         /* Red Alert */
    0xAA,                         /* Laughter (the battles with Zio) */
};
static uint8_t song_style[0x34];                       /* songs $81-$B4 */
static int song, style;                                /* the one playing (0 = unknown) and its drums */
/* where the song is: driver ticks since it started (-1: not known, Algol
 * having been switched on partway through it) */
static int song_tick = -1, music_on, fresh = 1;

static void song_tables(void);

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
        int bn;
        float btr = 0.0f;
        if (sscanf(line, " bank %15s = %d %f", name, &bn, &btr) >= 2 && nbank_user < 24) {   /* "bank lead = 35 [-12]" */
            snprintf(bank_user[nbank_user].name, sizeof bank_user[0].name, "%s", name);
            bank_user[nbank_user].sample = bn;
            bank_user[nbank_user].transpose = btr;
            nbank_user++;
            continue;
        }
        if (sscanf(line, " song %x = %63s", &h, name) == 2) {   /* "song 8F = rock" / "= standard" */
            song_tables();
            if (h >= 0x81 && h <= 0xB4) song_style[h - 0x81] = !strcmp(name, "rock") ? STYLE_ROCK : STYLE_STANDARD;
            continue;
        }
        if (line[0] == '#' || sscanf(line, " %x = %63s", &h, name) != 2) continue;
        int i = inst_by_name(name);
        if (i >= 0) { user_map[nuser].hash = h; user_map[nuser].inst = i; nuser++; }
    }
    fclose(f);
    bank_resolve();
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
    if (log_on < 0) { const char *v = getenv("PSIV_ALGOL_LOG"); log_on = v ? atoi(v) : 0; }
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
enum { D_KICK, D_SNARE, D_HAT, D_RIDE, D_TOM, D_CRASH, D_SAMPLE, D_BANK };
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
    int on, kind, rock, added;    /* added: Algol's own (the rock hat, a crash into the ride) */
    float t, tone, decay, gain, pl, pr, ph, room;
    float m[6];                   /* metallic oscillators: cycles per sample */
    float hp1, hp2, lp1, bp1;
    uint32_t rng;
    const uint8_t *pcm;
    const float *fpcm;            /* D_BANK: a bank's drum (snesbank.h) */
    int len;
    double pos, step;
} Drum;
#define NDRUMS 24
static Drum drums[NDRUMS];
static double now;                                     /* samples rendered */
static double last_hit[18];                            /* when each sample last played */

static Drum *drum_new(void) {
    Drum *d = NULL;
    for (int i = 0; i < NDRUMS; i++) if (!drums[i].on) { d = &drums[i]; break; }
    if (!d) {                                           /* steal the oldest */
        d = &drums[0];
        for (int i = 1; i < NDRUMS; i++) if (drums[i].t > d->t) d = &drums[i];
    }
    uint32_t rng = d->rng ? d->rng : 0x1234567u + (uint32_t)(d - drums);
    memset(d, 0, sizeof *d);
    d->rng = rng;
    d->on = 1;
    return d;
}

static void drum_pan(Drum *d, float p) {
    d->pl = cosf(p * 1.5707963f) * 1.41421356f;
    d->pr = sinf(p * 1.5707963f) * 1.41421356f;
}

static void drum_metal(Drum *d, float scale) {
    static const float METAL[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    for (int i = 0; i < 6; i++) d->m[i] = METAL[i] * scale / SR;
}

/* The hi-hat that follows the backbeat (hard rock style). The snare on two
 * and four gives the pulse: the time between backbeats is the commonest gap
 * between snares (gaps under 0.4 s -- rolls, flams -- don't count), when at
 * least half the recent gaps agree on it -- a syncopated part (Red Alert's)
 * has no backbeat, and gets no hat. An eighth is a quarter of it, and that
 * tempo then holds: a run of fills doesn't move it. The hat plays every
 * eighth, re-locked to each snare that lands on that grid, and stops when
 * the drums do. */
#define SN_N 12
static double sn_gap[SN_N], sn_last, hat_p8, hat_anchor, hat_last_drum, hat_last;
static int sn_n, hat_k, hat_running;

static void hat_follow_snare(void) {
    double t = now, gap = t - sn_last;
    sn_last = t;
    if (gap >= 0.4 * SR && gap <= 2.0 * SR) sn_gap[sn_n++ % SN_N] = gap;
    if (!hat_running) {
        int n = sn_n < SN_N ? sn_n : SN_N, best = 0;
        double beat2 = 0.0;
        for (int i = 0; i < n; i++) {                   /* the gap most others agree with */
            int votes = 0;
            double sum = 0.0;
            for (int j = 0; j < n; j++)
                if (fabs(sn_gap[j] - sn_gap[i]) < 0.03 * sn_gap[i]) { votes++; sum += sn_gap[j]; }
            /* a tie: the one nearer a rock backbeat (~0.85 s, two beats at 140) */
            if (votes > best || (votes == best && fabs(sum / votes - 0.85 * SR) < fabs(beat2 - 0.85 * SR))) {
                best = votes;
                beat2 = sum / votes;
            }
        }
        if (best < 2 || best * 2 < n) return;           /* no backbeat (yet) */
        hat_p8 = beat2 / 4.0;
        hat_anchor = t;
        hat_k = 0;
        hat_running = 1;
        if (log_on >= 2) printf("algol: backbeat %.3f s, eighth %.3f s\n", beat2 / SR, hat_p8 / SR);
        return;
    }
    double ph = (t - hat_anchor) / hat_p8, err = ph - floor(ph + 0.5);
    if (fabs(err) < 0.3) {                              /* on the grid: re-lock here */
        hat_anchor = t;
        hat_k = t - hat_last < 0.4 * hat_p8 ? 1 : 0;   /* no second hat on one just played */
    }
}

static void rock_hat(float accent) {
    hat_last = now;
    if (log_on >= 2) printf("algol: hat %.3f s (eighth %.3f s)\n", now / SR, hat_p8 / SR);
    Drum *d = drum_new();
    d->kind = D_HAT;
    d->rock = d->added = 1;
    d->decay = 0.03f + 0.012f * accent;
    d->gain = (0.30f + 0.20f * accent) * (0.92f + 0.08f * rnd(&d->rng));
    d->room = 0.05f;
    drum_pan(d, 0.64f);
    drum_metal(d, 2.9f);
}

/* The ride, in place of the hat in the louder sections of the rock songs.
 * One cymbal struck again and again: each stroke adds to the wash still
 * ringing from the ones before, over the stick's ping, and the strokes on
 * the beat are harder and catch some of the bell. A crash marks the way in,
 * where the game's own drums have none. */
typedef struct { uint16_t from, to; uint8_t crash; } RideRange;
typedef struct { uint8_t song; uint16_t loop, end; RideRange r[6]; } RideMap;
/* driver ticks from each song's start (tools/algol_sections.py lists the
 * bars); from `end` the song plays on from `loop` */
static const RideMap RIDE_MAP[] = {
    {0x8F, 288, 4320, {{1824, 2592, 1}}},         /* Meet Them Head-On: the melody (riff, build, melody, bridge) */
    {0xAC,   0,    0, {{2048, 2764, 1}}},         /* the title: the break, over the falling bass */
    {0x95,  72, 2016, {{1152, 2016, 1}}},         /* Defeat at a Blow: the second half */
    {0xA0, 144, 4608, {{1440, 2592, 1}}},         /* The King of Terrors: the middle */
};
#define RIDE_AHEAD 2                  /* ticks: the switch comes just before the downbeat's stroke */
static const RideMap *ride_map;
static int ride_on, ride_crash;
static struct {
    float lo, hi, ping, bell;         /* envelopes: the wash (body, sizzle), the stick, the bell */
    float ph[6], bph[3];
    float hp1, hp2, lp1, php, nhp, nlp;
    uint32_t rng;
} ride;

static void ride_reset(void) {
    ride_map = NULL;
    for (size_t i = 0; i < sizeof RIDE_MAP / sizeof RIDE_MAP[0]; i++)
        if (RIDE_MAP[i].song && RIDE_MAP[i].song == song) ride_map = &RIDE_MAP[i];
    ride_on = ride_crash = 0;
}

/* each driver tick: in a louder section or not */
static void ride_follow_song(void) {
    int on = 0, crash = 0;
    const RideMap *m = ride_map;
    if (style == STYLE_ROCK && m && song_tick >= 0) {
        int t = song_tick + RIDE_AHEAD;
        if (t >= m->end && m->end > m->loop) t = m->loop + (t - m->loop) % (m->end - m->loop);
        for (int i = 0; i < 6 && m->r[i].to; i++)
            if (t >= m->r[i].from && t < m->r[i].to) { on = 1; crash = m->r[i].crash; }
    }
    if (on && !ride_on) ride_crash = crash;
    ride_on = on;
}

static void ride_hit(float accent) {
    hat_last = now;
    if (log_on >= 2) printf("algol: ride %.3f s (song tick %d)\n", now / SR, song_tick);
    if (!ride.rng) ride.rng = 0x2545F491u;
    float g = (0.75f + 0.35f * accent) * (0.94f + 0.06f * rnd(&ride.rng));
    ride.lo = ride.lo * 0.65f + g;    /* the stick damps the ringing a little, and adds to it */
    ride.hi = ride.hi * 0.5f + g;
    ride.ping = g;
    ride.bell = 0.6f * accent * g;
}

static void rock_crash(void) {
    hat_last = now;
    if (log_on >= 2) printf("algol: crash %.3f s (song tick %d)\n", now / SR, song_tick);
    Drum *d = drum_new();
    d->kind = D_CRASH;
    d->rock = d->added = 1;
    d->decay = KIT[6].decay * 1.25f;
    d->gain = KIT[6].level * 2.6f;                         /* over the ride it leads into */
    d->room = 0.15f;
    drum_pan(d, 0.32f);
    drum_metal(d, 2.3f);
}

static void ride_run(float *l, float *r, float *room) {
    if (ride.lo + ride.hi + ride.ping + ride.bell < 1e-4f) return;
    static float k_lo, k_hi, k_ping, k_bell, mf[6], bf[3], pl, pr;
    if (k_lo == 0.0f) {
        k_lo = expf(-1.0f / (1.5f * SR));
        k_hi = expf(-1.0f / (0.45f * SR));
        k_ping = expf(-1.0f / (0.03f * SR));
        k_bell = expf(-1.0f / (0.22f * SR));
        static const float METAL[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
        for (int i = 0; i < 6; i++) mf[i] = METAL[i] * 1.7f / SR;
        static const float BELL[3] = {742.0f, 1187.0f, 1663.0f};
        for (int i = 0; i < 3; i++) bf[i] = BELL[i] / SR;
        pl = cosf(0.34f * 1.5707963f) * 1.41421356f;
        pr = sinf(0.34f * 1.5707963f) * 1.41421356f;
    }
    float x = 0.0f;                                         /* six square waves: the metal */
    for (int i = 0; i < 6; i++) {
        ride.ph[i] += mf[i];
        if (ride.ph[i] >= 1.0f) ride.ph[i] -= 1.0f;
        x += ride.ph[i] < 0.5f ? 1.0f : -1.0f;
    }
    x /= 6.0f;
    /* the wash: the metal's partials each smeared into a band (times noise
     * low-passed ~1.5 kHz), so it shimmers instead of holding a chord */
    float n = rnd(&ride.rng);
    ride.nlp += (n - ride.nlp) * 0.17f;
    float src = 1.6f * x * ride.nlp + 0.3f * n;
    ride.hp1 += (src - ride.hp1) * 0.26f;                   /* high-pass ~2.5 kHz */
    float h = src - ride.hp1;
    ride.hp2 += (h - ride.hp2) * 0.26f;
    h -= ride.hp2;
    ride.lp1 += (h - ride.lp1) * 0.55f;                     /* split ~7 kHz: body below, sizzle above */
    float s = ride.lp1 * ride.lo * 0.55f + (h - ride.lp1) * ride.hi * 0.45f;
    ride.php += (x - ride.php) * 0.5f;                      /* the ping: the metal's top and a tick */
    ride.nhp += (n - ride.nhp) * 0.6f;
    s += (0.9f * (x - ride.php) + 0.5f * (n - ride.nhp)) * ride.ping;
    float b = 0.0f;                                         /* the bell */
    for (int i = 0; i < 3; i++) {
        ride.bph[i] += bf[i];
        if (ride.bph[i] >= 1.0f) ride.bph[i] -= 1.0f;
        b += sinf(TWO_PI * ride.bph[i]) * (i == 0 ? 1.0f : i == 1 ? 0.6f : 0.4f);
    }
    s += b * ride.bell * 0.07f;
    ride.lo *= k_lo;
    ride.hi *= k_hi;
    ride.ping *= k_ping;
    ride.bell *= k_bell;
    s *= 0.2f * 0.68f;                                      /* with the kit (drum_run's scale), a ride's level */
    *l += s * pl;
    *r += s * pr;
    *room += s * 0.12f;
}

void algol_dac_hit(int sample, int pitch, int volume, int pan) {
    if (!enabled || sample < 1 || sample > 17) return;
    if (fx_fm >> 5 & 1) return;                             /* an effect holds the DAC */
    /* the game's two drum tracks can ask for the same sample in one tick */
    if (now - last_hit[sample] < 0.004 * SR && last_hit[sample] > 0.0) return;
    last_hit[sample] = now;
    hat_last_drum = now;
    if (log_on >= 2) printf("algol: drum %d at %.3f s, song tick %d\n", sample, now / SR, song_tick);
    int rock = style == STYLE_ROCK;
    Drum *d = drum_new();
    d->kind = KIT[sample].kind;
    d->rock = rock && d->kind != D_SAMPLE;
    float semis = (float)(pitch - 0x9A) * (12.0f / 12.6f);  /* the DAC's rate steps, about a semitone */
    d->tone = KIT[sample].tone * exp2f(semis / 12.0f);
    d->decay = KIT[sample].decay;
    d->gain = KIT[sample].level * (volume > 0 && volume <= 16 ? (float)volume / 16.0f : 1.0f);
    float p = (pan & 0xC0) == 0x80 ? 0.25f : (pan & 0xC0) == 0x40 ? 0.75f : 0.5f;
    if (d->kind == D_HAT || d->kind == D_RIDE) p += 0.14f;
    if (d->kind == D_CRASH) p -= 0.18f;
    if (d->kind == D_TOM && d->rock) p -= 0.12f;
    drum_pan(d, p);
    drum_metal(d, 2.6f);
    d->room = 0.15f;
    if (d->rock) {                                          /* the hard rock kit */
        switch (d->kind) {
        case D_KICK:  d->tone *= 1.05f; d->decay *= 0.8f; d->gain *= 1.15f; d->room = 0.10f; break;
        case D_SNARE: d->decay *= 1.15f; d->gain *= 1.10f; d->room = 0.45f; hat_follow_snare(); break;
        case D_TOM:   d->tone *= 0.9f; d->decay *= 1.35f; d->room = 0.40f; break;
        case D_HAT:   d->decay *= 0.8f; d->room = 0.05f; drum_metal(d, 2.9f); break;
        case D_RIDE:  d->decay *= 1.5f; d->room = 0.12f; break;
        case D_CRASH: d->decay *= 1.25f; d->gain *= 1.10f; d->room = 0.15f; drum_metal(d, 2.3f); break;
        }
    }
    const BankSample *bs = bank_on && d->kind < 6 ? snesbank_sample(bank_drum[d->kind]) : NULL;
    if (bs) {                                               /* the bank's drum, at its own rate */
        d->kind = D_BANK;
        d->rock = 0;
        d->fpcm = bs->pcm;
        d->len = bs->len;
        d->step = 32000.0 / SR * exp2((double)semis / 12.0);
        d->gain *= bs->gain * BANK_DRUM_LEVEL;
        return;
    }
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

/* one sample of a drum into the drum bus (l, r) and the room send */
static void drum_run(Drum *d, float *l, float *r, float *room) {
    const float dt = 1.0f / SR;
    float s = 0.0f, t = d->t;
    d->t += dt;
    switch (d->kind) {
    case D_KICK:
        if (d->rock) {                                      /* body, beater knock, click; driven */
            float f = d->tone + 150.0f * expf(-t / 0.024f);
            d->ph += f / SR;
            if (d->ph >= 1.0f) d->ph -= 1.0f;
            float body = sinf(TWO_PI * d->ph) * expf(-t / d->decay);
            float knock = sinf(TWO_PI * 1700.0f * t) * expf(-t / 0.004f) * 0.35f;
            float n = rnd(&d->rng);
            d->hp1 += (n - d->hp1) * 0.4f;
            float click = (n - d->hp1) * expf(-t / 0.0016f) * 0.7f;
            s = tanhf((body + knock + click) * 2.6f) * 0.8f;
        } else {
            float f = d->tone + 120.0f * expf(-t / 0.032f);
            d->ph += f / SR;
            if (d->ph >= 1.0f) d->ph -= 1.0f;
            float body = sinf(TWO_PI * d->ph) * expf(-t / d->decay);
            float click = rnd(&d->rng) * expf(-t / 0.0025f) * 0.5f;
            d->lp1 += (click - d->lp1) * 0.25f;
            s = tanhf((body + d->lp1) * 1.8f) * 0.85f;
        }
        if (t > d->decay * 5.0f) d->on = 0;
        break;
    case D_SNARE: {
        float f = d->rock ? d->tone * (1.0f + 0.3f * expf(-t / 0.008f)) : d->tone;
        d->ph += f / SR;
        if (d->ph >= 1.0f) d->ph -= 1.0f;
        float shell = (sinf(TWO_PI * d->ph) + 0.45f * sinf(TWO_PI * d->ph * 1.78f)) * expf(-t / (d->rock ? 0.09f : 0.06f));
        float n = rnd(&d->rng);
        d->hp1 += (n - d->hp1) * 0.13f;                     /* wires: high-pass ~1.2 kHz */
        float hn = n - d->hp1;
        d->lp1 += (hn - d->lp1) * 0.55f;                    /* low-pass ~7 kHz: a crack, not a hiss */
        /* the wires: a burst, then a quieter tail (d->decay sets the tail) */
        float wires = d->lp1 * ((d->rock ? 0.95f : 0.85f) * expf(-t / (d->rock ? 0.07f : 0.055f))
                              + 0.25f * expf(-t / (d->decay * 0.85f)));
        if (d->rock) {
            float crack = hn * expf(-t / 0.0035f) * 1.0f;
            s = tanhf((0.8f * shell + 1.2f * wires + crack) * 1.7f) * 0.85f;
        } else s = 0.55f * shell + 1.0f * wires;
        if (t > d->decay * 4.5f) d->on = 0;
        break;
    }
    case D_HAT: case D_RIDE: case D_CRASH: {
        float n = rnd(&d->rng), mt = metal(d);
        float x = d->kind == D_RIDE ? 0.6f * mt + 0.4f * n : d->kind == D_CRASH ? 0.45f * mt + 0.55f * n : 0.5f * mt + 0.5f * n;
        float k = d->kind == D_HAT ? (d->rock ? 0.6f : 0.5f) : d->kind == D_RIDE ? 0.3f : (d->rock ? 0.42f : 0.35f);
        d->hp1 += (x - d->hp1) * k;
        float h = x - d->hp1;
        d->hp2 += (h - d->hp2) * k;
        h = h - d->hp2;
        float att = d->kind == D_CRASH ? (t < 0.002f ? t / 0.002f : 1.0f) : 1.0f;
        float ping = d->kind == D_RIDE ? (d->rock ? 0.65f + 0.35f * sinf(TWO_PI * 3150.0f * t) * expf(-t / 0.4f)
                                                  : 0.8f + 0.2f * sinf(TWO_PI * 3200.0f * t)) : 1.0f;
        s = h * att * expf(-t / d->decay) * ping;
        if (t > d->decay * 5.8f) d->on = 0;
        break;
    }
    case D_TOM: {
        float f = d->tone * (1.0f + (d->rock ? 0.9f : 0.6f) * expf(-t / (d->rock ? 0.06f : 0.045f)));
        d->ph += f / SR;
        if (d->ph >= 1.0f) d->ph -= 1.0f;
        float n = rnd(&d->rng) * expf(-t / 0.02f) * (d->rock ? 0.35f : 0.25f);
        s = tanhf((sinf(TWO_PI * d->ph) * expf(-t / d->decay) + n) * (d->rock ? 2.0f : 1.4f)) * 0.9f;
        if (t > d->decay * 6.0f) d->on = 0;
        break;
    }
    case D_BANK: {                                          /* a bank's drum: interpolated */
        int i = (int)d->pos;
        if (i + 1 >= d->len) { d->on = 0; return; }
        float fr = (float)(d->pos - (double)i);
        s = d->fpcm[i] + (d->fpcm[i + 1] - d->fpcm[i]) * fr;
        d->pos += d->step;
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
    *room += s * d->room;
}

/* The drum bus: a small room (four short delay lines, a Hadamard mix) and,
 * in the rock style, a parallel compressor -- the dry kit plus a squashed
 * copy of it -- and a little saturation */
#define RM_N 4
static const int RM_LEN[RM_N] = {293, 421, 557, 683};
static float rm_buf[RM_N][700], rm_lp[RM_N], rm_g[RM_N];
static int rm_pos[RM_N];
static float comp_env;

static void room_init(void) {
    for (int i = 0; i < RM_N; i++) {
        rm_g[i] = powf(10.0f, -3.0f * (float)RM_LEN[i] / (SR * 0.45f));   /* 0.45 s to -60 dB */
        rm_pos[i] = 0;
        rm_lp[i] = 0.0f;
    }
    memset(rm_buf, 0, sizeof rm_buf);
    comp_env = 0.0f;
}

static void drum_bus(float *l, float *r, float room_in, float *rev) {
    float y[RM_N], h[RM_N];
    for (int i = 0; i < RM_N; i++) {
        float v = rm_buf[i][rm_pos[i]];
        rm_lp[i] += (v - rm_lp[i]) * 0.5f;
        y[i] = h[i] = rm_lp[i];
    }
    float a = h[0] + h[1], b = h[0] - h[1], c = h[2] + h[3], e = h[2] - h[3];
    h[0] = (a + c) * 0.5f; h[1] = (b + e) * 0.5f; h[2] = (a - c) * 0.5f; h[3] = (b - e) * 0.5f;
    for (int i = 0; i < RM_N; i++) {
        rm_buf[i][rm_pos[i]] = room_in + h[i] * rm_g[i];
        if (++rm_pos[i] >= RM_LEN[i]) rm_pos[i] = 0;
    }
    *l += (y[0] - y[2]) * 0.6f;
    *r += (y[1] - y[3]) * 0.6f;
    if (style == STYLE_ROCK) {
        /* parallel compression: the dry kit, plus a copy squashed 3:1 (5 ms
         * attack lets the hits through, 100 ms release) a third as loud --
         * fuller tails without pumping -- and a touch of saturation */
        float in = fmaxf(fabsf(*l), fabsf(*r));
        comp_env += (in - comp_env) * (in > comp_env ? 0.0037f : 0.00019f);
        float thr = 0.08f, gr = comp_env > thr ? powf(thr / comp_env, 0.667f) : 1.0f;
        float k = 1.0f + 0.33f * 1.6f * gr;
        *l = tanhf(*l * k * 1.6f) / 1.6f;
        *r = tanhf(*r * k * 1.6f) / 1.6f;
    }
    *rev += (*l + *r) * 0.5f * (style == STYLE_ROCK ? 0.08f : 0.12f);
}

/* ------------------------------------------------------------------ songs */
static uint32_t song_vt[0x34];                         /* each song's voice table: how to tell them apart */

static void song_tables(void) {
    static int done;
    if (done || !m68k_rom) return;
    done = 1;
    for (int i = 0; i < 0x34; i++) {
        uint32_t p = 0xD1C40u + (uint32_t)i * 4u;      /* MusicPtrs */
        if (p + 4 > m68k_rom_size) return;
        uint32_t hdr = (uint32_t)m68k_rom[p] << 24 | (uint32_t)m68k_rom[p + 1] << 16 | (uint32_t)m68k_rom[p + 2] << 8 | m68k_rom[p + 3];
        hdr &= 0xFFFFFF;
        if (hdr + 2 > m68k_rom_size) continue;
        song_vt[i] = hdr + (uint32_t)(m68k_rom[hdr] << 8 | m68k_rom[hdr + 1]);
    }
    for (size_t i = 0; i < sizeof ROCK_SONGS; i++) song_style[ROCK_SONGS[i] - 0x81] = STYLE_ROCK;
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
    room_init();
    hat_running = 0;
    sn_n = 0;
    sn_last = -1e9;
    memset(last_hit, 0, sizeof last_hit);
    memset(&ride, 0, sizeof ride);
    ride_on = ride_crash = 0;
    fresh = 1;
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

static void song_start(int id, int tick) {
    song = id;
    song_tick = tick;
    style = song ? song_style[song - 0x81] : STYLE_STANDARD;
    hat_running = 0;
    sn_n = 0;
    sn_last = -1e9;
    ride_reset();
    if (log_on < 0) { const char *v = getenv("PSIV_ALGOL_LOG"); log_on = v ? atoi(v) : 0; }
    if (log_on) printf("algol: song %02X, %s drums (at %.3f s%s)\n", song, style == STYLE_ROCK ? "rock" : "standard",
                       now / SR, tick < 0 ? ", partway" : "");
}

/* which channels the driver's sound-effect tracks hold: the music has the rest */
void algol_driver_tick(void) {
    /* the song: PlayMusic leaves its voice table's address at $FF5020 */
    song_tables();
    if (!user_loaded) load_user_map();
    uint32_t vt = (uint32_t)m68k_ram[0x5021] << 16 | (uint32_t)m68k_ram[0x5022] << 8 | m68k_ram[0x5023];
    int now_song = 0;
    for (int i = 0; i < 0x34; i++) if (song_vt[i] && song_vt[i] == vt) { now_song = 0x81 + i; break; }
    int playing = 0;                                       /* the ten music tracks, $FF5040 on */
    for (int i = 0; i < 10; i++) if (m68k_ram[0x5040u + (unsigned)i * 0x30u] & 0x80) playing = 1;
    if (fresh) { fresh = 0; song_start(now_song, playing ? -1 : 0); }   /* just switched on */
    else if (now_song != song || (playing && !music_on)) song_start(now_song, 0);   /* a song (re)starts */
    else if (song_tick >= 0) song_tick++;
    music_on = playing;
    ride_follow_song();
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
 * 6-8 PSG 1-3, 9 noise, 10 the game's drums, 11 the cymbals Algol adds (the
 * rock hat, ride and crash); sound.c applies the same to the chips */
static unsigned solo_mask(void) {
    static int read;
    static unsigned mask = 0xFFF;
    if (!read) { const char *v = getenv("PSIV_CHANNELS"); if (v && *v) mask = (unsigned)strtoul(v, NULL, 16); read = 1; }
    return mask;
}

/* PSIV_ALGOL_METER=1: each channel's energy, and how much of it each instrument made, printed when
 * the program ends -- to set the instruments' levels against the game's own mix (the chips' channels
 * soloed with PSIV_CHANNELS, the same numbering) */
static int meter_on = -1;
static double meter_ch[12], meter_inst[12][N_INST];
static void meter_print(void) {
    printf("algol meter: channel energy");
    for (int c = 0; c < 12; c++) printf(" %.6g", meter_ch[c]);
    printf("\n");
    for (int c = 0; c < 10; c++) {
        printf("algol meter: ch %d", c);
        for (int i = 0; i < N_INST; i++)
            if (meter_inst[c][i] > 0.0) printf(" %s=%.6g", INST[i].name, meter_inst[c][i]);
        printf("\n");
    }
}

void algol_render(float *left, float *right) {
    if (!ready) silence();
    if (meter_on < 0) {
        meter_on = getenv("PSIV_ALGOL_METER") != NULL;
        if (meter_on) atexit(meter_print);
    }
    float l = 0.0f, r = 0.0f, rev = 0.0f, dly = 0.0f, cho = 0.0f;
    unsigned solo = solo_mask();
    for (int i = 0; i < NVOICES; i++) {
        Voice *v = &voice[i];
        if (!v->live) continue;
        float vl = 0.0f, vr = 0.0f, vrev = 0.0f, vdly = 0.0f, vcho = 0.0f;
        if (i == V_NOISE) noise_run(v, &vl, &vr, &vrev);
        else voice_run(v, &vl, &vr, &vrev, &vdly, &vcho);
        if (meter_on) {
            double e = (double)vl * vl + (double)vr * vr;
            meter_ch[i] += e;
            if (i != V_NOISE && v->inst >= 0 && v->inst < N_INST) meter_inst[i][v->inst] += e;
        }
        if (!(solo >> i & 1)) continue;
        l += vl; r += vr; rev += vrev; dly += vdly; cho += vcho;
    }
    /* the eighths (rock style): the hat, or in a louder section the ride
     * (a crash on the way in); then the kit through its bus */
    if (style == STYLE_ROCK && hat_running) {
        if (now - hat_last_drum > 8.0 * hat_p8) hat_running = 0;           /* the drums stopped */
        else if (now >= hat_anchor + (double)hat_k * hat_p8) {
            float accent = hat_k % 2 == 0 ? 1.0f : 0.0f;
            if (ride_crash) { rock_crash(); ride_crash = 0; }
            else if (ride_on) ride_hit(accent);
            else rock_hat(accent);
            hat_k++;
        }
    }
    now += 1.0;
    float bl = 0.0f, br = 0.0f, room = 0.0f, brev = 0.0f;
    for (int i = 0; i < NDRUMS; i++) {
        if (!drums[i].on) continue;
        float dl = 0.0f, dr = 0.0f, dm = 0.0f;
        int added = drums[i].added;
        drum_run(&drums[i], &dl, &dr, &dm);
        if (meter_on) meter_ch[added ? 11 : 10] += (double)dl * dl + (double)dr * dr;
        if (solo >> (added ? 11 : 10) & 1) { bl += dl; br += dr; room += dm; }
    }
    {
        float dl = 0.0f, dr = 0.0f, dm = 0.0f;
        ride_run(&dl, &dr, &dm);
        if (solo >> 11 & 1) { bl += dl; br += dr; room += dm; }
    }
    drum_bus(&bl, &br, room, &brev);
    l += bl; r += br; rev += brev;
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
