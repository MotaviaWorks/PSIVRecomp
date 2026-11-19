/*
 * Algol: the game's own instruments, clean. See algol2.h.
 *
 *   FM       the YM2612 in floating point: six channels of four operators
 *            (phase with detune and multiple, feedback on the first, the eight
 *            algorithms), the envelope generator on the chip's own clock and
 *            rate tables, key scaling, the LFO, channel 3's special mode
 *   PSG      three band-limited squares; the noise from its shift register
 *   drums    the ROM's samples, as the chip's DAC plays them
 *   polish   per note, from its patch: a detuned double spread in stereo, a sub
 *            under a bass, the reverb send; then the reverb
 *   effects  the channels a sound effect holds, the same way, with a polish of
 *            their own by the effect's kind and two reverbs of their own (an
 *            option: Settings > Sound > Sound effects > Algol)
 *
 * Levels are the chips': a carrier at full level here is one at full level in
 * the chips' mix (sound.c), so the two can be measured against each other.
 */
#include "algol2.h"
#include "m68k.h"
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR        ((double)SOUND_RATE)
#define FM_SCALE  12098.0f      /* a carrier (or the DAC) at full level: 8191 >> 5, x 128 * 64 / (6 * 65), x 9 / 4 */
#define PSG_SCALE 5850.0f       /* a PSG channel at full level: 2600 x 9 / 4 */
#define TWO_PI    6.283185307179586
#define MOD_DEPTH 3.99951f      /* a full-level modulator moves the phase this many cycles (8191 / 2048) */

static int dry = -1;            /* PSIV_ALGOL2_DRY: no polish */

/* ------------------------------------------------------------------ tables */
#define SINE_N 4096
static float sine_tab[SINE_N + 2];
static float att_gain[2400];    /* attenuation in the envelope's steps (0.09375 dB) -> gain */
static int tables_ready;

/* detune: added to the phase step, by key code (OPN) */
static const uint8_t DT[4][32] = {
    {0},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8},
    {1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16},
    {2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22},
};
/* the envelope's steps: eight in turn, by rate */
static const uint8_t EG_INC[19][8] = {
    {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1},   /* rates 0-47, by rate & 3 */
    {1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2},   /* 48-51 */
    {2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 4, 2, 2, 2, 4}, {2, 4, 2, 4, 2, 4, 2, 4}, {2, 4, 4, 4, 2, 4, 4, 4},   /* 52-55 */
    {4, 4, 4, 4, 4, 4, 4, 4}, {4, 4, 4, 8, 4, 4, 4, 8}, {4, 8, 4, 8, 4, 8, 4, 8}, {4, 8, 8, 8, 4, 8, 8, 8},   /* 56-59 */
    {8, 8, 8, 8, 8, 8, 8, 8},                                                                                 /* 60-63 */
    {16, 16, 16, 16, 16, 16, 16, 16}, {0, 0, 0, 0, 0, 0, 0, 0},
};
static const float LFO_HZ[8] = {3.98f, 5.56f, 6.02f, 6.37f, 6.88f, 9.63f, 48.1f, 72.2f};
static const float PMS_CENTS[8] = {0.0f, 3.4f, 6.7f, 10.0f, 14.0f, 20.0f, 40.0f, 80.0f};
static const int AMS_SHIFT[4] = {8, 3, 1, 0};
static const uint8_t CARRIER[8] = {0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF};   /* bit s = operator S(s+1) is heard */

static void tables(void) {
    if (tables_ready) return;
    for (int i = 0; i < SINE_N + 2; i++) sine_tab[i] = (float)sin(TWO_PI * (double)i / SINE_N);
    for (int i = 0; i < 2400; i++) att_gain[i] = i >= 1023 ? 0.0f : (float)pow(10.0, -(double)i * 0.09375 / 20.0);
    tables_ready = 1;
}

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

/* ------------------------------------------------------------------ FM */
enum { EG_OFF, EG_ATT, EG_DEC, EG_SUS, EG_REL };
typedef struct {
    uint8_t mul, dt, tl, rs, ar, d1r, d2r, rr, sl, am, ssg;
    uint8_t key, state, ksr;
    int vol;                      /* the envelope: 0 (full) .. 1023 (silent) */
    double ph[2], inc;            /* phase, cycles (the voice and its double); cycles a sample */
} Op;

/* the polish a note gets, by its patch's kind */
enum { F_BASS, F_LEAD, F_BRASS, F_STRINGS, F_PLUCK, F_BELL, F_ORGAN, F_KEYS, NFAM };
static const struct { const char *name; float cents, spread, rev, sub; } POLISH[NFAM] = {
    /*            the double's   stereo   reverb   sub
     *            detune, cents  spread   send          */
    {"bass",      0.0f,          0.00f,   0.03f,   0.22f},
    {"lead",      7.0f,          0.36f,   0.16f,   0.0f},
    {"brass",     8.0f,          0.42f,   0.18f,   0.0f},
    {"strings",   12.0f,         0.60f,   0.26f,   0.0f},
    {"pluck",     5.0f,          0.32f,   0.20f,   0.0f},
    {"bell",      4.0f,          0.46f,   0.30f,   0.0f},
    {"organ",     8.0f,          0.50f,   0.20f,   0.0f},
    {"keys",      5.0f,          0.40f,   0.22f,   0.0f},
};

/* A sound effect's voice gets its polish from the effect's kind instead: a wider double, a sine on
 * its fundamental under the heavy ones (only while that is low: body, not a whistle), and sends to
 * the effects' own two reverbs, a small room and a hall.
 * PSIV_SFX_POLISH="cents,spread,room,hall,sub" gives every kind the same (for trying things). */
enum { X_MENU, X_HIT, X_SPELL, X_BLAST, X_WORLD, NXK };
typedef struct { const char *name; float cents, spread, room, hall, sub; } XPolish;
static const XPolish XPOLISH[NXK] = {
    /*           the double's   stereo   room    hall    sub
     *           detune, cents  spread   send    send         */
    {"menu",     4.0f,          0.30f,   0.10f,  0.00f,  0.00f},
    {"hit",      7.0f,          0.55f,   0.24f,  0.05f,  0.14f},
    {"spell",    10.0f,         0.75f,   0.10f,  0.30f,  0.00f},
    {"blast",    8.0f,          0.65f,   0.12f,  0.34f,  0.22f},
    {"world",    6.0f,          0.50f,   0.26f,  0.08f,  0.10f},
};

static const XPolish *xpolish(int kind) {
    static XPolish all;
    static int read;
    if (!read) {
        const char *v = getenv("PSIV_SFX_POLISH");
        read = v && sscanf(v, "%f,%f,%f,%f,%f", &all.cents, &all.spread, &all.room, &all.hall, &all.sub) == 5 ? 2 : 1;
    }
    return read == 2 ? &all : &XPOLISH[kind < 0 || kind >= NXK ? X_HIT : kind];
}

/* the kind of each of the game's sound effects ($B5-$F7, SFXID_* in ps4.constants.asm) */
static int kind_of(int id) {
    switch (id) {
    case 0xF1: case 0xF2: case 0xF3: case 0xF4:                                     /* Souvenir, the cursor, Selection, Surprise */
        return X_MENU;
    case 0xBB: case 0xBC: case 0xBD: case 0xC4: case 0xC9: case 0xCA: case 0xCC:    /* the casts, the buffs, the healing */
    case 0xCD: case 0xCF: case 0xD1: case 0xD4: case 0xD9: case 0xDC: case 0xDE:
    case 0xEB: case 0xF0:
        return X_SPELL;
    case 0xB9: case 0xBE: case 0xBF: case 0xC0: case 0xC1: case 0xC2: case 0xC3:    /* fire, wind, light, thunder, things breaking */
    case 0xC5: case 0xC6: case 0xC7: case 0xC8: case 0xCB: case 0xCE: case 0xD3:
    case 0xDD: case 0xE6: case 0xEA: case 0xEC: case 0xEE:
        return X_BLAST;
    case 0xD6: case 0xDB: case 0xDF: case 0xE0: case 0xE1: case 0xE2: case 0xE3:    /* doors, stairs, lifts, machines, alarms */
    case 0xE4: case 0xE5: case 0xE7: case 0xED: case 0xEF:
        return X_WORLD;
    default:                                                                        /* weapons, claws, the enemies' blows */
        return X_HIT;
    }
}

typedef struct {
    Op op[4];                     /* S1..S4 */
    int fnum, block;
    uint8_t alg, fb, pl, pr, ams, pms, hi;   /* hi: $A4's latch, taken with $A0 */
    float fbm[2][2];              /* S1's last two outputs, the voice's and its double's */
    int fam, copies;
    double ratio[2];              /* each copy's tuning */
    float gl[2], gr[2];           /* ... and where it sits */
    float rev, sub;
    double sub_ph, sub_inc;
    int fxk;                      /* a sound effect's voice: its kind + 1 (0: the song's) */
    float room, hall;             /* ... and its sends to the effects' two reverbs */
} Chan;
static Chan chan[6];
static uint8_t R[2][256];
static int fnum3[3], block3[3], hi3[3], ch3_mode, dac_on, lfo_on, lfo_rate;
static unsigned eg_cnt = 1;
static int eg_div;
static double lfo_ph;

static const int SLOT_OF_REG[4] = {0, 2, 1, 3};         /* the registers go S1, S3, S2, S4 */

static int keycode(int fnum, int block) {
    int f11 = fnum >> 10 & 1, f10 = fnum >> 9 & 1, f9 = fnum >> 8 & 1, f8 = fnum >> 7 & 1;
    return block << 2 | f11 << 1 | ((f11 & (f10 | f9 | f8)) | ((f11 ^ 1) & f10 & f9 & f8));
}

static void op_freq(Op *o, int fnum, int block) {
    int kc = keycode(fnum, block), step = (fnum << block) >> 1, dt = DT[o->dt & 3][kc];
    step += o->dt & 4 ? -dt : dt;
    if (step < 0) step = 0;
    step = o->mul ? step * o->mul : step >> 1;
    o->inc = (double)(step & 0xFFFFF) / 1048576.0;
    o->ksr = (uint8_t)(kc >> (3 - o->rs));
}

/* channel 3's first three operators have their own frequencies in its special mode ($A9, $AA, $A8) */
static void chan_freq(int c) {
    Chan *ch = &chan[c];
    static const int OWN[3] = {1, 2, 0};
    for (int s = 0; s < 4; s++) {
        if (c == 2 && ch3_mode && s < 3) op_freq(&ch->op[s], fnum3[OWN[s]], block3[OWN[s]]);
        else op_freq(&ch->op[s], ch->fnum, ch->block);
    }
}

static int rate_of(int r, int ksr) {
    if (!r) return 0;
    int x = (r << 1) + ksr;
    return x > 63 ? 63 : x;
}

/* what kind of instrument the patch is, from its own sound (as Algol tells them apart) */
static int classify(const Chan *ch, float note) {
    int alg = ch->alg, main = 3;
    for (int s = 0; s < 4; s++)
        if ((CARRIER[alg] >> s & 1) && ch->op[s].tl <= ch->op[main].tl) main = s;
    const Op *m = &ch->op[main];
    int sustained = m->d1r <= 4 || (m->sl <= 3 && m->d2r <= 9);
    int fast_decay = m->d2r >= 24 || (m->d1r >= 20 && m->sl >= 4);
    float cmul = m->mul ? (float)m->mul : 0.5f, bright = (float)ch->fb / 7.0f * 0.4f;
    int nonint = 0, high = 0;
    for (int s = 0; s < 4; s++) {
        if (CARRIER[alg] >> s & 1) continue;
        const Op *o = &ch->op[s];
        float ratio = (o->mul ? (float)o->mul : 0.5f) / cmul;
        if (o->tl < 50 && fabsf(ratio - roundf(ratio)) > 0.2f) nonint = 1;
        if (o->tl < 50 && ratio >= 7.0f) high = 1;
        if (o->tl < 40) bright += (40.0f - (float)o->tl) / 40.0f;
    }
    if (alg == 7) return sustained ? F_ORGAN : F_KEYS;
    if (note < 47.0f) return F_BASS;
    if (m->ar < 18) return sustained ? F_STRINGS : F_BRASS;
    if (!sustained) return nonint || (high && note >= 72.0f) ? F_BELL : fast_decay ? F_PLUCK : F_KEYS;
    if (alg >= 4) return bright > 1.2f ? F_BRASS : F_LEAD;
    return F_LEAD;
}

/* where a channel sits, and how wide: at a note's start, and when its pan changes */
static void chan_place(Chan *ch) {
    if (dry < 0) { const char *v = getenv("PSIV_ALGOL2_DRY"); dry = v && *v == '1'; }
    int both = ch->pl && ch->pr;
    if (dry) {                                           /* the chip's own: all of it on a side, or both */
        ch->copies = 1;
        ch->ratio[0] = 1.0;
        ch->gl[0] = ch->pl ? 1.0f : 0.0f;
        ch->gr[0] = ch->pr ? 1.0f : 0.0f;
        ch->rev = ch->sub = 0.0f;
        return;
    }
    const XPolish *x = ch->fxk ? xpolish(ch->fxk - 1) : NULL;
    float cents = x ? x->cents : POLISH[ch->fam].cents, spread = x ? x->spread : POLISH[ch->fam].spread;
    float p = both ? 0.5f : ch->pl ? 0.2f : ch->pr ? 0.8f : 0.5f;
    if (x) {                                             /* an effect's low voice: one copy (two would only beat) */
        int main = 3;
        for (int s = 0; s < 4; s++)
            if ((CARRIER[ch->alg] >> s & 1) && ch->op[s].tl <= ch->op[main].tl) main = s;
        if (ch->op[main].inc * SR < 180.0) cents = 0.0f;
    }
    ch->copies = cents > 0.0f ? 2 : 1;
    /* (an effect's two copies a little lower: at a short sound's start they are still in step) */
    float level = !ch->pl && !ch->pr ? 0.0f : ch->copies == 2 ? (x ? 0.88f : 1.0f) : 1.41421356f;
    for (int k = 0; k < ch->copies; k++) {
        float side = ch->copies == 2 ? (k ? 0.5f : -0.5f) : 0.0f, q = clampf(p + side * spread, 0.0f, 1.0f);
        ch->ratio[k] = exp2((double)(side * cents) / 1200.0);
        ch->gl[k] = cosf(q * 1.5707963f) * level;
        ch->gr[k] = sinf(q * 1.5707963f) * level;
    }
    ch->rev = x ? 1.0f : POLISH[ch->fam].rev;            /* (an effect's send: the whole voice, shared out below) */
    ch->sub = x ? x->sub : POLISH[ch->fam].sub;
    ch->room = x ? x->room : 0.0f;
    ch->hall = x ? x->hall : 0.0f;
}

static void note_start(Chan *ch) {
    int main = 3;
    for (int s = 0; s < 4; s++)
        if ((CARRIER[ch->alg] >> s & 1) && ch->op[s].tl <= ch->op[main].tl) main = s;
    double hz = ch->op[main].inc * SR;
    float note = hz > 8.0 ? 69.0f + 12.0f * log2f((float)hz / 440.0f) : 0.0f;
    ch->fam = classify(ch, note);
    ch->sub_inc = ch->op[main].inc;
    chan_place(ch);
}

static void op_key(Chan *ch, int s, int on) {
    Op *o = &ch->op[s];
    if (on && !o->key) {
        o->ph[0] = o->ph[1] = 0.0;
        o->state = EG_ATT;
        if (rate_of(o->ar, o->ksr) >= 62) { o->vol = 0; o->state = EG_DEC; }
    } else if (!on && o->key) {
        if (o->state != EG_OFF) o->state = EG_REL;
    }
    o->key = (uint8_t)on;
}

/* PSIV_ALGOL2_LOG=1: each of the chip's rarer features, the first time a song uses it */
static void note_use(const char *what) {
    static int log_on = -1;
    static const char *seen[8];
    static int nseen;
    if (log_on < 0) { const char *v = getenv("PSIV_ALGOL2_LOG"); log_on = v && *v == '1'; }
    if (!log_on) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == what) return;
    if (nseen < 8) seen[nseen++] = what;
    printf("algol: the song uses %s\n", what);
}

void algol2_ym_write(int part, uint8_t reg, uint8_t v) {
    part &= 1;
    R[part][reg] = v;
    if (reg < 0x30) {
        if (part) return;
        if (reg == 0x22) { lfo_on = v >> 3 & 1; lfo_rate = v & 7; if (lfo_on) note_use("the LFO"); }
        else if (reg == 0x27) { ch3_mode = v >> 6 != 0; chan_freq(2); if (ch3_mode) note_use("channel 3's special mode"); }
        else if (reg == 0x2B) dac_on = v >> 7;
        else if (reg == 0x28) {
            int c = v & 3;
            if (c == 3) return;
            Chan *ch = &chan[c + (v & 4 ? 3 : 0)];
            int was = ch->op[0].key | ch->op[1].key | ch->op[2].key | ch->op[3].key;
            if (!was && (v & 0xF0)) note_start(ch);
            for (int s = 0; s < 4; s++) op_key(ch, s, v >> (4 + s) & 1);
        }
        return;
    }
    int c = reg & 3;
    if (c == 3) return;
    Chan *ch = &chan[c + part * 3];
    if (reg < 0xA0) {
        Op *o = &ch->op[SLOT_OF_REG[reg >> 2 & 3]];
        switch (reg & 0xF0) {
        case 0x30: o->dt = v >> 4 & 7; o->mul = v & 15; chan_freq(c + part * 3); break;
        case 0x40: o->tl = v & 127; break;
        case 0x50: o->rs = v >> 6; o->ar = v & 31; chan_freq(c + part * 3); break;
        case 0x60: o->am = v >> 7; o->d1r = v & 31; break;
        case 0x70: o->d2r = v & 31; break;
        case 0x80: o->sl = v >> 4; o->rr = v & 15; break;
        case 0x90: o->ssg = v & 15; if (v & 8) note_use("SSG-EG (not played: the plain envelope)"); break;
        }
        return;
    }
    switch (reg & 0xFC) {
    case 0xA0: ch->fnum = (ch->hi & 7) << 8 | v; ch->block = ch->hi >> 3 & 7; chan_freq(c + part * 3); break;
    case 0xA4: ch->hi = v; break;
    case 0xA8: if (!part) { fnum3[c] = (hi3[c] & 7) << 8 | v; block3[c] = hi3[c] >> 3 & 7; chan_freq(2); } break;
    case 0xAC: if (!part) hi3[c] = v; break;
    case 0xB0: ch->alg = v & 7; ch->fb = v >> 3 & 7; break;
    case 0xB4: ch->pl = v >> 7; ch->pr = v >> 6 & 1; ch->ams = v >> 4 & 3; ch->pms = v & 7; chan_place(ch); break;
    }
}

/* one step of an operator's envelope (every third sample, as the chip's) */
static void eg_step(Op *o) {
    int rate;
    switch (o->state) {
    case EG_ATT: rate = rate_of(o->ar, o->ksr); break;
    case EG_DEC: rate = rate_of(o->d1r, o->ksr); break;
    case EG_SUS: rate = rate_of(o->d2r, o->ksr); break;
    case EG_REL: rate = rate_of(o->rr << 1 | 1, o->ksr); break;
    default: return;
    }
    if (o->state == EG_ATT && rate >= 62) { o->vol = 0; o->state = EG_DEC; return; }
    int shift = rate < 48 ? 11 - (rate >> 2) : 0;
    if (eg_cnt & ((1u << shift) - 1)) return;
    int row = rate < 2 ? 18 : rate < 48 ? (rate & 3) : rate < 60 ? 4 + (rate - 48) : 16;
    int inc = EG_INC[row][eg_cnt >> shift & 7];
    if (o->state == EG_ATT) {
        o->vol += (~o->vol * inc) >> 4;
        if (o->vol <= 0) { o->vol = 0; o->state = EG_DEC; }
        return;
    }
    o->vol += inc;
    if (o->state == EG_DEC) {
        int sl = o->sl == 15 ? 31 << 5 : o->sl << 5;
        if (o->vol >= sl) o->state = EG_SUS;
    }
    if (o->vol >= 1023) { o->vol = 1023; o->state = EG_OFF; }
}

static inline float op_sin(double ph, float mod) {
    float p = (float)ph + mod;
    p -= floorf(p);
    float x = p * (float)SINE_N;
    int i = (int)x;
    return sine_tab[i] + (sine_tab[i + 1] - sine_tab[i]) * (x - (float)i);
}

/* a channel past the chip's own limit (four carriers at full level): bent, not clipped */
static inline float soft(float x) {
    if (x > 0.9f) return 0.9f + 0.1f * tanhf((x - 0.9f) * 10.0f);
    if (x < -0.9f) return -0.9f + 0.1f * tanhf((x + 0.9f) * 10.0f);
    return x;
}

static const float FB_DEPTH[8] = {0.0f, 1.0f / 64, 1.0f / 32, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1.0f};

/* one sample of a channel into the mix (the carriers' sum, 1 = one at full level) and its reverb send */
static void chan_run(Chan *ch, float am, float pm, float *l, float *r, float *rev) {
    Op *o = ch->op;
    if (o[0].state == EG_OFF && o[1].state == EG_OFF && o[2].state == EG_OFF && o[3].state == EG_OFF) return;
    float g[4];
    int amv = lfo_on ? (int)am >> AMS_SHIFT[ch->ams] : 0;
    for (int s = 0; s < 4; s++) {
        int att = o[s].vol + (o[s].tl << 3) + (o[s].am ? amv : 0);
        g[s] = att_gain[att];
    }
    double bend = lfo_on && ch->pms ? exp2((double)(pm * PMS_CENTS[ch->pms]) / 1200.0) : 1.0;
    float sum = 0.0f;
    for (int k = 0; k < ch->copies; k++) {
        double rt = ch->ratio[k] * bend;
        float fb = ch->fb ? (ch->fbm[k][0] + ch->fbm[k][1]) * FB_DEPTH[ch->fb] : 0.0f;
        float s1 = op_sin(o[0].ph[k], fb) * g[0], s2, s3, y;
        ch->fbm[k][1] = ch->fbm[k][0];
        ch->fbm[k][0] = s1;
        float m1 = s1 * MOD_DEPTH;
        switch (ch->alg) {
        case 0:
            s2 = op_sin(o[1].ph[k], m1) * g[1];
            s3 = op_sin(o[2].ph[k], s2 * MOD_DEPTH) * g[2];
            y = op_sin(o[3].ph[k], s3 * MOD_DEPTH) * g[3];
            break;
        case 1:
            s2 = op_sin(o[1].ph[k], 0.0f) * g[1];
            s3 = op_sin(o[2].ph[k], m1 + s2 * MOD_DEPTH) * g[2];
            y = op_sin(o[3].ph[k], s3 * MOD_DEPTH) * g[3];
            break;
        case 2:
            s2 = op_sin(o[1].ph[k], 0.0f) * g[1];
            s3 = op_sin(o[2].ph[k], s2 * MOD_DEPTH) * g[2];
            y = op_sin(o[3].ph[k], m1 + s3 * MOD_DEPTH) * g[3];
            break;
        case 3:
            s2 = op_sin(o[1].ph[k], m1) * g[1];
            s3 = op_sin(o[2].ph[k], 0.0f) * g[2];
            y = op_sin(o[3].ph[k], (s2 + s3) * MOD_DEPTH) * g[3];
            break;
        case 4:
            s2 = op_sin(o[1].ph[k], m1) * g[1];
            s3 = op_sin(o[2].ph[k], 0.0f) * g[2];
            y = s2 + op_sin(o[3].ph[k], s3 * MOD_DEPTH) * g[3];
            break;
        case 5:
            y = op_sin(o[1].ph[k], m1) * g[1] + op_sin(o[2].ph[k], m1) * g[2] + op_sin(o[3].ph[k], m1) * g[3];
            break;
        case 6:
            y = op_sin(o[1].ph[k], m1) * g[1] + op_sin(o[2].ph[k], 0.0f) * g[2] + op_sin(o[3].ph[k], 0.0f) * g[3];
            break;
        default:
            y = s1 + op_sin(o[1].ph[k], 0.0f) * g[1] + op_sin(o[2].ph[k], 0.0f) * g[2] + op_sin(o[3].ph[k], 0.0f) * g[3];
            break;
        }
        for (int s = 0; s < 4; s++) {
            o[s].ph[k] += o[s].inc * rt;
            if (o[s].ph[k] >= 1.0) o[s].ph[k] -= floor(o[s].ph[k]);
        }
        y = soft(y);
        *l += y * ch->gl[k];
        *r += y * ch->gr[k];
        sum += y;
    }
    if (ch->sub > 0.0f) {                                /* a bass: a sine on its fundamental, under it */
        int main = 3;
        for (int s = 0; s < 4; s++)
            if ((CARRIER[ch->alg] >> s & 1) && o[s].tl <= o[main].tl) main = s;
        ch->sub_ph += ch->sub_inc * bend;
        if (ch->sub_ph >= 1.0) ch->sub_ph -= floor(ch->sub_ph);
        float s = op_sin(ch->sub_ph, 0.0f) * g[main] * ch->sub;
        *l += s * (ch->gl[0] + (ch->copies == 2 ? ch->gl[1] : 0.0f));
        *r += s * (ch->gr[0] + (ch->copies == 2 ? ch->gr[1] : 0.0f));
    }
    *rev += sum * ch->rev / (float)ch->copies;
}

/* ------------------------------------------------------------------ the PSG */
static struct {
    int latch, noise, period[3], att[4];
    double ph[3], nph;
    uint16_t lfsr;
    float nval;
} psg;
static float psg_gain[16];

void algol2_psg_write(uint8_t v) {
    if (v & 0x80) {
        psg.latch = v >> 4 & 7;
        int c = psg.latch >> 1;
        if (psg.latch & 1) psg.att[c] = v & 15;
        else if (c < 3) psg.period[c] = (psg.period[c] & 0x3F0) | (v & 15);
        else { psg.noise = v & 7; psg.lfsr = 0x8000; }
    } else {
        int c = psg.latch >> 1;
        if (psg.latch & 1) psg.att[c] = v & 15;
        else if (c < 3) psg.period[c] = (psg.period[c] & 0x0F) | (v & 0x3F) << 4;
        else { psg.noise = v & 7; psg.lfsr = 0x8000; }
    }
}

static inline float blep(double t, double dt) {          /* a step's corner, rounded over two samples */
    if (t < dt) { double x = t / dt; return (float)(x + x - x * x - 1.0); }
    if (t > 1.0 - dt) { double x = (t - 1.0) / dt; return (float)(x * x + x + x + 1.0); }
    return 0.0f;
}

static void psg_run(unsigned fx_psg, unsigned solo, float *l, float *r, float *rev) {
    static const float PAN[4] = {0.40f, 0.60f, 0.50f, 0.56f};
    for (int c = 0; c < 3; c++) {
        if (psg.att[c] == 15 || psg.period[c] <= 1 || (fx_psg >> c & 1) || !(solo >> (6 + c) & 1)) continue;
        double dt = 3579545.0 / (32.0 * (double)psg.period[c]) / SR;
        if (dt > 0.45) continue;                         /* above hearing: the chip's rest */
        psg.ph[c] += dt;
        if (psg.ph[c] >= 1.0) psg.ph[c] -= 1.0;
        double t = psg.ph[c], t2 = t + 0.5;
        if (t2 >= 1.0) t2 -= 1.0;
        float s = (t < 0.5 ? 1.0f : -1.0f) + blep(t, dt) - blep(t2, dt);
        s *= psg_gain[psg.att[c]];
        float p = dry ? 0.5f : PAN[c];
        *l += s * cosf(p * 1.5707963f) * 1.41421356f;
        *r += s * sinf(p * 1.5707963f) * 1.41421356f;
        if (!dry) *rev += s * 0.10f * (PSG_SCALE / FM_SCALE);
    }
    /* the noise: the shift register at its own rate (or the third square's), each sample its mean */
    if (psg.att[3] != 15 && !(fx_psg >> 3 & 1) && (solo >> 9 & 1)) {
        static const double RATE[3] = {16.0, 32.0, 64.0};
        double div = (psg.noise & 3) == 3 ? (double)(psg.period[2] ? psg.period[2] : 1) : RATE[psg.noise & 3];
        double step = 3579545.0 / 16.0 / (2.0 * div) / SR;
        float before = psg.lfsr & 1 ? 1.0f : -1.0f;
        psg.nph += step;
        int n = (int)psg.nph;
        psg.nph -= (double)n;
        for (int i = 0; i < n && i < 64; i++) {
            unsigned fb = psg.noise & 4 ? ((psg.lfsr ^ psg.lfsr >> 3) & 1) : (psg.lfsr & 1);
            psg.lfsr = (uint16_t)(psg.lfsr >> 1 | fb << 15);
        }
        float after = psg.lfsr & 1 ? 1.0f : -1.0f, s = after;
        if (n) { float fa = step > 0.0 ? (float)(psg.nph / step) : 1.0f; s = before + (after - before) * clampf(fa, 0.0f, 1.0f); }
        s *= psg_gain[psg.att[3]];
        float p = dry ? 0.5f : PAN[3];
        *l += s * cosf(p * 1.5707963f) * 1.41421356f;
        *r += s * sinf(p * 1.5707963f) * 1.41421356f;
    }
}

/* ------------------------------------------------------------------ the drums */
/* The game's seventeen samples (the Z80 driver's tables in the ROM: a bank, a start and a
 * length each; 8 bits, unsigned), played as the chip plays them. Which are hi-hats and which
 * the ride: a new stroke of one ends the last, as a drummer's would. */
static const uint8_t BANK_TBL[17][2] = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {1, 0}, {0, 7}, {2, 0},
                                        {1, 1}, {0, 8}, {2, 1}, {0, 9}, {2, 2}, {0, 6}, {3, 0}, {2, 3}};
enum { K_OTHER, K_HAT, K_RIDE };
static const uint8_t CYMBAL_OF[18] = {0, 0, 0, K_RIDE, K_HAT, 0, 0, 0, 0, K_HAT, 0, 0, 0, 0, 0, 0, 0, 0};

typedef struct {
    int on, sample, role;
    const uint8_t *rom;
    int len;
    double pos, step;             /* (step below 0: played backwards) */
    float gl, gr, gain, lp1, k, fade;
    const uint8_t *loop;          /* the driver's loop: where it goes on from at its end, and how long */
    int loop_len;
} Hit;
#define NHITS 16
static Hit hits[NHITS];
static double now_s, last_hit[18];

void algol2_dac_hit(int sample, int pitch, int volume, int pan) {
    if (sample < 1 || sample > 17) return;
    /* The driver has two loops. The plain one waits between samples by the pitch (and cancels a
     * pitch below $81). The one with volume control ($1F01 bit 7) has no wait at all: about 465
     * Z80 cycles a sample whatever the pitch, 7.7 kHz. */
    int volctl = sound_68k_read(0x1F01) & 0x80;
    if (!volctl && pitch < 0x81) return;
    /* the game's two drum tracks can ask for the same sample in one tick */
    if (last_hit[sample] > 0.0 && now_s - last_hit[sample] < 0.004 * SR) return;
    last_hit[sample] = now_s;
    {   /* PSIV_ALGOL2_LOG=2: each drum as it is asked for */
        static int log_on = -1;
        if (log_on < 0) { const char *v = getenv("PSIV_ALGOL2_LOG"); log_on = v && *v == '2'; }
        if (log_on) printf("algol: drum %d pitch %02X volume %d pan %02X loop %d reverse %d at %.3f s\n", sample, pitch, volume,
                           pan, sound_68k_read(0x1F05), sound_68k_read(0x1F06), now_s / SR);
    }
    static const uint32_t BASE[4] = {0xE0000, 0xE8000, 0xF0000, 0xF8000};
    uint32_t base = BASE[BANK_TBL[sample - 1][0]], e = base + BANK_TBL[sample - 1][1] * 8u;
    if (!m68k_rom || e + 8 > m68k_rom_size) return;
    uint32_t start = (uint32_t)(m68k_rom[e] | m68k_rom[e + 1] << 8), len = (uint32_t)(m68k_rom[e + 2] | m68k_rom[e + 3] << 8);
    uint32_t off = base + start - 0x8000;
    if (start < 0x8000 || off + len > m68k_rom_size || len < 4) return;
    int role = CYMBAL_OF[sample];
    Hit *h = NULL;
    if (dry) memset(hits, 0, sizeof hits);               /* the chip's one voice: a new sample ends the last */
    if (role)                                            /* a hi-hat (closed or open), or the ride: the last stroke ends */
        for (int i = 0; i < NHITS; i++)
            if (hits[i].on && hits[i].role == role && hits[i].fade >= 1.0f) hits[i].fade = 0.998f;
    for (int i = 0; i < NHITS; i++) if (!hits[i].on) { h = &hits[i]; break; }
    if (!h) {                                            /* all in use: the one furthest along */
        h = &hits[0];
        for (int i = 1; i < NHITS; i++) if (hits[i].pos > h->pos) h = &hits[i];
    }
    memset(h, 0, sizeof *h);
    h->sample = sample;
    h->role = role;
    h->fade = 1.0f;
    h->rom = m68k_rom + off;
    h->len = (int)len;
    h->step = (volctl ? 3579545.0 / 465.0 : 2000.0 * pow(10300.0 / 2000.0, (double)(pitch - 0x81) / 28.0)) / SR;
    if (sound_68k_read(0x1F05)) {                        /* the driver's loop flag: on from the table's loop point */
        uint32_t ls = (uint32_t)(m68k_rom[e + 4] | m68k_rom[e + 5] << 8), ll = (uint32_t)(m68k_rom[e + 6] | m68k_rom[e + 7] << 8);
        if (ls >= 0x8000 && ll >= 4 && base + ls - 0x8000 + ll <= m68k_rom_size) { h->loop = m68k_rom + base + ls - 0x8000; h->loop_len = (int)ll; }
    }
    if (sound_68k_read(0x1F06)) { h->pos = (double)(len - 3); h->step = -h->step; }   /* ... and its reverse flag */
    h->gain = volume >= 16 ? 1.0f : volume > 0 ? (float)volume / 16.0f : 0.0f;       /* (the driver's: sixteenths) */
    h->k = 0.70f;                                        /* (the polish: a gentle low-pass, about 10 kHz) */
    float p = (pan & 0xC0) == 0x80 ? 0.25f : (pan & 0xC0) == 0x40 ? 0.75f : 0.5f;
    if (dry) { h->gl = (pan & 0xC0) == 0x40 ? 0.0f : 1.0f; h->gr = (pan & 0xC0) == 0x80 ? 0.0f : 1.0f; }
    else { h->gl = cosf(p * 1.5707963f) * 1.41421356f; h->gr = sinf(p * 1.5707963f) * 1.41421356f; }
    h->on = 1;
}

/* A rest in the drum track stops the sample on the chip (the composer's gating): the samples follow
 * it, over a few milliseconds */
void algol2_dac_stop(void) {
    for (int i = 0; i < NHITS; i++) {
        Hit *h = &hits[i];
        if (!h->on) continue;
        if (dry) h->on = 0;
        else if (h->fade >= 1.0f) h->fade = 0.9965f;
    }
}

static void hits_run(float *l, float *r, float *rev) {
    for (int i = 0; i < NHITS; i++) {
        Hit *h = &hits[i];
        if (!h->on) continue;
        int n = (int)h->pos;
        if (n + 2 >= h->len || h->pos < 0.0) {           /* its end: the loop, if the driver has one on */
            if (!h->loop || h->step < 0.0) { h->on = 0; continue; }
            h->rom = h->loop;
            h->len = h->loop_len;
            h->pos = 0.0;
            n = 0;
        }
        /* each value held until the next, as the chip's DAC holds it -- at two or three thousand a
         * second that step is much of a sample's sound; with the polish, its very top taken off */
        float s = ((float)h->rom[n] - 128.0f) / 128.0f;
        if (!dry) { h->lp1 += (s - h->lp1) * h->k; s = h->lp1; }
        h->pos += h->step;
        if (h->fade < 1.0f) { h->gain *= h->fade; if (h->gain < 0.0005f) h->on = 0; }
        s *= h->gain;
        *l += s * h->gl;
        *r += s * h->gr;
        if (!dry) *rev += s * 0.10f;
    }
}

/* ------------------------------------------------------------------ the reverb */
/* eight delay lines fed back through a Householder mix, each damped; a short pre-delay */
#define RV_N 8
#define RV_MAX 4800
#define RV_PRE 800                                       /* 15 ms */
static const int RV_LEN[RV_N] = {1447, 1777, 2129, 2543, 2903, 3347, 3761, 4177};
static float rv_buf[RV_N][RV_MAX], rv_lp[RV_N], rv_gain[RV_N], rv_pre[RV_PRE];
static int rv_pos[RV_N], rv_pp;

static void reverb_init(void) {
    memset(rv_buf, 0, sizeof rv_buf);
    memset(rv_lp, 0, sizeof rv_lp);
    memset(rv_pre, 0, sizeof rv_pre);
    memset(rv_pos, 0, sizeof rv_pos);
    rv_pp = 0;
    for (int i = 0; i < RV_N; i++) rv_gain[i] = (float)pow(10.0, -3.0 * (double)RV_LEN[i] / (1.5 * SR));   /* 1.5 s */
}

static void reverb(float in, float *l, float *r) {
    float x = rv_pre[rv_pp];
    rv_pre[rv_pp] = in;
    if (++rv_pp >= RV_PRE) rv_pp = 0;
    float o[RV_N], sum = 0.0f;
    for (int i = 0; i < RV_N; i++) {
        o[i] = rv_buf[i][rv_pos[i]];
        rv_lp[i] += (o[i] - rv_lp[i]) * 0.45f;           /* the highs die first */
        o[i] = rv_lp[i];
        sum += o[i];
    }
    sum *= 2.0f / RV_N;
    for (int i = 0; i < RV_N; i++) {
        rv_buf[i][rv_pos[i]] = x + (o[i] - sum) * rv_gain[i];
        if (++rv_pos[i] >= RV_LEN[i]) rv_pos[i] = 0;
    }
    *l += (o[0] - o[2] + o[4] - o[6]) * 0.5f;
    *r += (o[1] - o[3] + o[5] - o[7]) * 0.5f;
}

/* ------------------------------------------------------------------ the effects */
/* The channels a sound effect holds (the driver's effect tracks), when the effects are Algol's
 * too: the same FM and PSG, each voice with its effect's polish, into a mix of their own with two
 * reverbs of their own -- a small room (weapons, doors) and a hall (spells, thunder). */
typedef struct {
    int len[RV_N], pos[RV_N], npre, pp;
    float gain[RV_N], lp[RV_N], damp;
    float buf[RV_N][RV_MAX], pre[2048];
} Verb;
static Verb x_room, x_hall;
static int x_live;                                       /* samples until the effects' reverbs have rung out */
static float x_low[2];                                   /* the sends' lows, kept out of the reverbs */
#define X_SEND_HP 0.0256f                                /* 220 Hz: a held low tone would sit on a resonance and build */
static uint8_t psg_fxk[4];                               /* the PSG's channels: the effect's kind + 1 */
#define XN_N 256
static float xn_buf[XN_N];                               /* the noise a moment ago: its other side */
static int xn_pos;

static void verb_init(Verb *v, const int *len, double seconds, double pre_ms, float damp) {
    memset(v, 0, sizeof *v);
    for (int i = 0; i < RV_N; i++) {
        v->len[i] = len[i];
        v->gain[i] = (float)pow(10.0, -3.0 * (double)len[i] / (seconds * SR));
    }
    v->npre = (int)(pre_ms * SR / 1000.0);
    if (v->npre < 1) v->npre = 1;
    if (v->npre > 2048) v->npre = 2048;
    v->damp = damp;
}

static void verb_run(Verb *v, float in, float *l, float *r) {
    float x = v->pre[v->pp];
    v->pre[v->pp] = in;
    if (++v->pp >= v->npre) v->pp = 0;
    float o[RV_N], sum = 0.0f;
    for (int i = 0; i < RV_N; i++) {
        o[i] = v->buf[i][v->pos[i]];
        v->lp[i] += (o[i] - v->lp[i]) * v->damp;
        o[i] = v->lp[i];
        sum += o[i];
    }
    sum *= 2.0f / RV_N;
    for (int i = 0; i < RV_N; i++) {
        v->buf[i][v->pos[i]] = x + (o[i] - sum) * v->gain[i];
        if (++v->pos[i] >= v->len[i]) v->pos[i] = 0;
    }
    *l += (o[0] - o[2] + o[4] - o[6]) * 0.5f;
    *r += (o[1] - o[3] + o[5] - o[7]) * 0.5f;
}

static void effects_init(void) {
    static const int ROOM[RV_N] = {631, 773, 929, 1109, 1259, 1447, 1637, 1811};
    static const int HALL[RV_N] = {1663, 2039, 2447, 2927, 3343, 3851, 4327, 4793};
    verb_init(&x_room, ROOM, 0.45, 4.0, 0.55f);
    verb_init(&x_hall, HALL, 1.9, 22.0, 0.40f);
    memset(xn_buf, 0, sizeof xn_buf);
    xn_pos = x_live = 0;
    x_low[0] = x_low[1] = 0.0f;
}

/* which effect holds each channel (its sound ID; 0: none; $FF: one not known), every driver tick */
void algol2_fx_tracks(const uint8_t *fm_id, const uint8_t *psg_id) {
    for (int c = 0; c < 6; c++) {
        int k = fm_id[c] ? kind_of(fm_id[c]) + 1 : 0;
        if (k == chan[c].fxk) continue;
        chan[c].fxk = k;
        chan_place(&chan[c]);
    }
    for (int c = 0; c < 4; c++) psg_fxk[c] = (uint8_t)(psg_id[c] ? kind_of(psg_id[c]) + 1 : 0);
}

/* the PSG channels an effect holds: the squares apart in the stereo field, the noise wide (its
 * other side a moment later), each with its effect's reverb sends */
static void psg_run_fx(unsigned fx_psg, unsigned solo, float *l, float *r, float *room, float *hall) {
    static const float SIDE[3] = {-0.5f, 0.5f, 0.0f};
    for (int c = 0; c < 3; c++) {
        if (psg.att[c] == 15 || psg.period[c] <= 1 || !(fx_psg >> c & 1) || !(solo >> (6 + c) & 1)) continue;
        double dt = 3579545.0 / (32.0 * (double)psg.period[c]) / SR;
        if (dt > 0.45) continue;
        psg.ph[c] += dt;
        if (psg.ph[c] >= 1.0) psg.ph[c] -= 1.0;
        double t = psg.ph[c], t2 = t + 0.5;
        if (t2 >= 1.0) t2 -= 1.0;
        float s = (t < 0.5 ? 1.0f : -1.0f) + blep(t, dt) - blep(t2, dt);
        s *= psg_gain[psg.att[c]];
        const XPolish *x = xpolish(psg_fxk[c] - 1);
        float p = dry ? 0.5f : 0.5f + SIDE[c] * x->spread * 0.6f;
        *l += s * cosf(p * 1.5707963f) * 1.41421356f;
        *r += s * sinf(p * 1.5707963f) * 1.41421356f;
        if (!dry) { *room += s * x->room; *hall += s * x->hall; }
    }
    if (psg.att[3] != 15 && (fx_psg >> 3 & 1) && (solo >> 9 & 1)) {
        static const double RATE[3] = {16.0, 32.0, 64.0};
        double div = (psg.noise & 3) == 3 ? (double)(psg.period[2] ? psg.period[2] : 1) : RATE[psg.noise & 3];
        double step = 3579545.0 / 16.0 / (2.0 * div) / SR;
        float before = psg.lfsr & 1 ? 1.0f : -1.0f;
        psg.nph += step;
        int n = (int)psg.nph;
        psg.nph -= (double)n;
        for (int i = 0; i < n && i < 64; i++) {
            unsigned fb = psg.noise & 4 ? ((psg.lfsr ^ psg.lfsr >> 3) & 1) : (psg.lfsr & 1);
            psg.lfsr = (uint16_t)(psg.lfsr >> 1 | fb << 15);
        }
        float after = psg.lfsr & 1 ? 1.0f : -1.0f, s = after;
        if (n) { float fa = step > 0.0 ? (float)(psg.nph / step) : 1.0f; s = before + (after - before) * clampf(fa, 0.0f, 1.0f); }
        s *= psg_gain[psg.att[3]];
        const XPolish *x = xpolish(psg_fxk[3] - 1);
        float w = dry ? 0.0f : clampf(x->spread, 0.0f, 0.9f);
        float late = xn_buf[(xn_pos + XN_N - 127) & (XN_N - 1)];      /* 2.4 ms ago */
        xn_buf[xn_pos] = s;
        xn_pos = (xn_pos + 1) & (XN_N - 1);
        *l += s;
        *r += s * (1.0f - w) + late * w;
        if (!dry) { *room += s * x->room; *hall += s * x->hall; }
    }
}

/* ------------------------------------------------------------------ the frame */
void algol2_silence(void) {
    tables();
    for (int c = 0; c < 6; c++) {
        Chan *ch = &chan[c];
        for (int s = 0; s < 4; s++) { ch->op[s].state = EG_OFF; ch->op[s].vol = 1023; ch->op[s].key = 0; }
        memset(ch->fbm, 0, sizeof ch->fbm);
        if (!ch->copies) chan_place(ch);
    }
    memset(hits, 0, sizeof hits);
    memset(last_hit, 0, sizeof last_hit);
    reverb_init();
    effects_init();
    for (int i = 0; i < 15; i++) psg_gain[i] = (float)(pow(10.0, -0.1 * i) * (double)(PSG_SCALE / FM_SCALE));
    psg_gain[15] = 0.0f;
}

void algol2_reset(void) {
    memset(chan, 0, sizeof chan);
    memset(R, 0, sizeof R);
    memset(fnum3, 0, sizeof fnum3);
    memset(block3, 0, sizeof block3);
    memset(hi3, 0, sizeof hi3);
    ch3_mode = dac_on = lfo_on = lfo_rate = 0;
    eg_cnt = 1;
    eg_div = 0;
    lfo_ph = 0.0;
    memset(&psg, 0, sizeof psg);
    for (int i = 0; i < 4; i++) psg.att[i] = 15;
    psg.lfsr = 0x8000;
    memset(psg_fxk, 0, sizeof psg_fxk);
    for (int c = 0; c < 6; c++) { chan[c].pl = chan[c].pr = 1; }
    algol2_silence();
}

/* PSIV_CHANNELS=hex (for measuring): only these are heard -- bits 0-5 FM 1-6, 6-8 PSG 1-3, 9 the
 * noise, 10 the drums (sound.c applies the same to the chips) */
static unsigned solo_mask(void) {
    static int read;
    static unsigned mask = 0xFFF;
    if (!read) { const char *v = getenv("PSIV_CHANNELS"); if (v && *v) mask = (unsigned)strtoul(v, NULL, 16); read = 1; }
    return mask;
}

void algol2_render(float *left, float *right, unsigned fx_fm, unsigned fx_psg) {
    algol2_render_split(left, right, NULL, NULL, fx_fm, fx_psg, 1, 0);
}

/* One sample: the song (music) into ml / mr, the channels the effects hold (effects) into el / er.
 * The song alone is the render it always was; without effects those channels are the chips'. */
void algol2_render_split(float *ml, float *mr, float *el, float *er, unsigned fx_fm, unsigned fx_psg, int music, int effects) {
    if (!tables_ready) algol2_reset();
    unsigned solo = solo_mask();
    /* the envelopes, every third sample; the LFO */
    if (++eg_div >= 3) {
        eg_div = 0;
        if (++eg_cnt >= 4096) eg_cnt = 1;
        for (int c = 0; c < 6; c++)
            for (int s = 0; s < 4; s++) eg_step(&chan[c].op[s]);
    }
    float am = 0.0f, pm = 0.0f;
    if (lfo_on) {
        lfo_ph += (double)LFO_HZ[lfo_rate] / SR;
        if (lfo_ph >= 1.0) lfo_ph -= 1.0;
        am = (float)(lfo_ph < 0.5 ? lfo_ph * 2.0 : 2.0 - lfo_ph * 2.0) * 126.0f;
        pm = (float)sin(TWO_PI * lfo_ph);
    }
    float l = 0.0f, r = 0.0f, rev = 0.0f, xl = 0.0f, xr = 0.0f, xroom = 0.0f, xhall = 0.0f;
    for (int c = 0; c < 6; c++) {
        if ((c == 5 && dac_on) || !(solo >> c & 1)) continue;
        Chan *ch = &chan[c];
        if (!(fx_fm >> c & 1)) {
            if (music) chan_run(ch, am, pm, &l, &r, &rev);
            continue;
        }
        if (!effects) continue;
        /* an effect's voice: its sub follows the voice's pitch, and is there only while that is low */
        float sub = ch->sub, send = 0.0f;
        if (sub > 0.0f) {
            int main = 3;
            for (int s = 0; s < 4; s++)
                if ((CARRIER[ch->alg] >> s & 1) && ch->op[s].tl <= ch->op[main].tl) main = s;
            ch->sub_inc = ch->op[main].inc;
            ch->sub = sub * clampf((260.0f - (float)(ch->sub_inc * SR)) / 120.0f, 0.0f, 1.0f);
        }
        chan_run(ch, am, pm, &xl, &xr, &send);
        ch->sub = sub;
        xroom += send * ch->room;
        xhall += send * ch->hall;
    }
    if (music) psg_run(fx_psg, solo, &l, &r, &rev);
    if (effects) psg_run_fx(fx_psg, solo, &xl, &xr, &xroom, &xhall);
    now_s += 1.0;
    if (music && !(fx_fm >> 5 & 1) && (solo >> 10 & 1)) hits_run(&l, &r, &rev);
    if (music && !dry) reverb(rev, &l, &r);
    if (effects && !dry) {                               /* the effects' reverbs, until they have rung out */
        if (xroom != 0.0f || xhall != 0.0f) x_live = (int)(3.0 * SR);
        if (x_live > 0) {
            x_live--;
            x_low[0] += (xroom - x_low[0]) * X_SEND_HP;
            x_low[1] += (xhall - x_low[1]) * X_SEND_HP;
            verb_run(&x_room, xroom - x_low[0], &xl, &xr);
            verb_run(&x_hall, xhall - x_low[1], &xl, &xr);
        }
    }
    if (ml) { *ml = l * FM_SCALE; *mr = r * FM_SCALE; }
    if (el) { *el = xl * FM_SCALE; *er = xr * FM_SCALE; }
}
