/*
 * The port's own sound effects: see sfxsynth.h.
 *
 * Building blocks: white noise, a state-variable filter that can move every
 * sample, two-pole resonators for ringing metal, sine oscillators with pitch
 * envelopes, equal-power panning and Freeverb (Jezar's public-domain design)
 * for the room. Each recipe renders its parts into separate buffers,
 * normalises them and mixes them at set levels, so the balance is explicit.
 */
#include "sfxsynth.h"
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR  ((float)SOUND_RATE)
#define PI  3.14159265358979f
#define TAU 6.28318530717959f

/* ------------------------------------------------------------------ buffers */
typedef struct { float *l, *r; int n; } Buf;

static Buf buf_new(float seconds) {
    Buf b;
    b.n = (int)(seconds * SR);
    b.l = calloc((size_t)b.n, sizeof(float));
    b.r = calloc((size_t)b.n, sizeof(float));
    if (!b.l || !b.r) { free(b.l); free(b.r); b.l = b.r = NULL; b.n = 0; }
    return b;
}

static void buf_free(Buf *b) { free(b->l); free(b->r); b->l = b->r = NULL; b->n = 0; }

static float buf_peak(const Buf *b) {
    float p = 0;
    for (int n = 0; n < b->n; n++) {
        if (fabsf(b->l[n]) > p) p = fabsf(b->l[n]);
        if (fabsf(b->r[n]) > p) p = fabsf(b->r[n]);
    }
    return p;
}

static void normalize(Buf *b) {
    float p = buf_peak(b);
    if (p <= 0) return;
    for (int n = 0; n < b->n; n++) { b->l[n] /= p; b->r[n] /= p; }
}

/* dst += src at a level in dB */
static void mix(Buf *dst, const Buf *src, float db) {
    float g = powf(10.0f, db / 20.0f);
    int n = dst->n < src->n ? dst->n : src->n;
    for (int i = 0; i < n; i++) { dst->l[i] += src->l[i] * g; dst->r[i] += src->r[i] * g; }
}

/* ------------------------------------------------------------------ signals */
typedef struct { uint32_t s; } Rng;

static float rnd(Rng *r) {                       /* white noise in [-1, 1) */
    r->s ^= r->s << 13; r->s ^= r->s >> 17; r->s ^= r->s << 5;
    return (float)(int32_t)r->s * (1.0f / 2147483648.0f);
}
static float urnd(Rng *r) { return rnd(r) * 0.5f + 0.5f; }

static Rng seed(uint32_t a, int variant) {
    Rng r = {a ^ ((uint32_t)variant * 0x9E3779B9u + 0x7F4A7C15u)};
    if (!r.s) r.s = 1;
    for (int i = 0; i < 4; i++) rnd(&r);
    return r;
}

/* TPT state-variable filter (Zavalishin): stays stable while fc moves */
typedef struct { float ic1, ic2; } Svf;
typedef struct { float lp, bp, hp; } Bands;      /* bp at unity gain at fc */

static Bands svf(Svf *f, float x, float fc, float q) {
    if (fc > SR * 0.45f) fc = SR * 0.45f;
    if (fc < 20.0f) fc = 20.0f;
    float g = tanf(PI * fc / SR), k = 1.0f / q;
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - f->ic2;
    float v1 = a1 * f->ic1 + a2 * v3;
    float v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    Bands o = {v2, k * v1, x - k * v1 - v2};
    return o;
}

/* a two-pole resonator: one ringing mode, impulse response amp * e^(-t/tau) * sin */
typedef struct { float y1, y2, a1, a2, g; } Mode;

static void mode_set(Mode *m, float f, float tau, float amp) {
    float r = expf(-1.0f / (tau * SR)), w = TAU * f / SR;
    m->a1 = 2.0f * r * cosf(w);
    m->a2 = -r * r;
    m->g = amp * sinf(w);
    m->y1 = m->y2 = 0;
}

static float mode_step(Mode *m, float x) {
    float y = m->a1 * m->y1 + m->a2 * m->y2 + m->g * x;
    m->y2 = m->y1;
    m->y1 = y;
    return y;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float smooth(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

/* a quick rise (a quarter sine, `a` seconds) and an exponential fall */
static float attack_decay(float t, float a, float tau) {
    if (t < 0) return 0;
    if (t < a) return sinf(0.5f * PI * t / a);
    return expf(-(t - a) / tau);
}

/* equal-power pan: p from -1 (left) to 1 (right) */
static void pan(float p, float *gl, float *gr) {
    float a = (clamp01((p + 1.0f) * 0.5f)) * 0.5f * PI;
    *gl = cosf(a);
    *gr = sinf(a);
}

/* ------------------------------------------------------------------ the room */
static const int COMB_LEN[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int AP_LEN[4] = {556, 441, 341, 225};
typedef struct { float *b; int n, i; float store; } Comb;
typedef struct { float *b; int n, i; } Allpass;

static float comb_step(Comb *c, float x, float fb, float damp) {
    float y = c->b[c->i];
    c->store = y * (1.0f - damp) + c->store * damp;
    c->b[c->i] = x + c->store * fb;
    if (++c->i >= c->n) c->i = 0;
    return y;
}

static float allpass_step(Allpass *a, float x) {
    float bo = a->b[a->i], y = bo - x;
    a->b[a->i] = x + bo * 0.5f;
    if (++a->i >= a->n) a->i = 0;
    return y;
}

/* Freeverb: room size and damping 0-1, wet level 0-1, added to the dry
 * sound; the send is high-passed at `hp` Hz so low thumps don't rumble on */
static void reverb(Buf *b, float room, float damp, float wet, float hp) {
    float scale = SR / 44100.0f, fb = room * 0.28f + 0.7f, dmp = damp * 0.4f;
    Comb cl[8], cr[8];
    Allpass al[4], ar[4];
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        cl[i].n = (int)((float)COMB_LEN[i] * scale);
        cr[i].n = (int)((float)(COMB_LEN[i] + 23) * scale);           /* Freeverb's stereo spread */
        cl[i].b = calloc((size_t)cl[i].n, sizeof(float));
        cr[i].b = calloc((size_t)cr[i].n, sizeof(float));
        cl[i].i = cr[i].i = 0;
        cl[i].store = cr[i].store = 0;
        ok &= cl[i].b && cr[i].b;
    }
    for (int i = 0; i < 4; i++) {
        al[i].n = (int)((float)AP_LEN[i] * scale);
        ar[i].n = (int)((float)(AP_LEN[i] + 23) * scale);
        al[i].b = calloc((size_t)al[i].n, sizeof(float));
        ar[i].b = calloc((size_t)ar[i].n, sizeof(float));
        al[i].i = ar[i].i = 0;
        ok &= al[i].b && ar[i].b;
    }
    Svf send = {0, 0};
    if (ok)
        for (int n = 0; n < b->n; n++) {
            float in = svf(&send, (b->l[n] + b->r[n]) * 0.015f, hp, 0.7f).hp, ol = 0, orr = 0;
            for (int i = 0; i < 8; i++) {
                ol += comb_step(&cl[i], in, fb, dmp);
                orr += comb_step(&cr[i], in, fb, dmp);
            }
            for (int i = 0; i < 4; i++) {
                ol = allpass_step(&al[i], ol);
                orr = allpass_step(&ar[i], orr);
            }
            b->l[n] += ol * wet * 3.0f;
            b->r[n] += orr * wet * 3.0f;
        }
    for (int i = 0; i < 8; i++) { free(cl[i].b); free(cr[i].b); }
    for (int i = 0; i < 4; i++) { free(al[i].b); free(ar[i].b); }
}

/* the end of every recipe: DC removed, `drive` > 0 rounds off the peaks
 * (tanh) so a transient-heavy sound carries more body for its peak, the
 * silent tail trimmed, short fades, the peak set to `peak_db` dBFS, then
 * 16-bit with a little dither */
static int16_t *finish(Buf *b, float peak_db, float drive, int *frames) {
    float R = 1.0f - TAU * 20.0f / SR, xl = 0, yl = 0, xr = 0, yr = 0;
    for (int n = 0; n < b->n; n++) {
        float l = b->l[n] - xl + R * yl, r = b->r[n] - xr + R * yr;
        xl = b->l[n]; yl = l; b->l[n] = l;
        xr = b->r[n]; yr = r; b->r[n] = r;
    }
    if (drive > 0) {
        float p0 = buf_peak(b), k = p0 > 0 ? drive / p0 : 0, norm = tanhf(drive);
        for (int n = 0; n < b->n; n++) { b->l[n] = tanhf(b->l[n] * k) / norm; b->r[n] = tanhf(b->r[n] * k) / norm; }
    }
    float p = buf_peak(b), floor_level = p * 0.0003f;                /* -70 dB */
    int end = b->n;
    while (end > 1 && fabsf(b->l[end - 1]) < floor_level && fabsf(b->r[end - 1]) < floor_level) end--;
    end += (int)(0.004f * SR);
    if (end > b->n) end = b->n;
    int fi = (int)(0.0004f * SR), fo = (int)(0.06f * SR);
    for (int i = 0; i < fi && i < end; i++) { float g = (float)i / (float)fi; b->l[i] *= g; b->r[i] *= g; }
    for (int i = 0; i < fo && i < end; i++) {
        float g = (float)i / (float)fo;
        g *= g;                                                     /* a gentle end to the reverb */
        b->l[end - 1 - i] *= g;
        b->r[end - 1 - i] *= g;
    }
    float gain = p > 0 ? powf(10.0f, peak_db / 20.0f) / p * 32767.0f : 0;
    int16_t *out = malloc((size_t)end * 2 * sizeof(int16_t));
    if (!out) { buf_free(b); return NULL; }
    Rng d = {0xC0FFEE11u};
    for (int n = 0; n < end; n++)
        for (int ch = 0; ch < 2; ch++) {
            float v = (ch ? b->r[n] : b->l[n]) * gain + (urnd(&d) - urnd(&d));
            long s = lrintf(v);
            out[n * 2 + ch] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
    *frames = end;
    buf_free(b);
    return out;
}

/* ------------------------------------------------------------------ recipes */

/* Blade hit (the game's SFXID_Sword, $F5: every sword, dagger and slasher
 * hit). The request comes as the blow lands, so the swing is only a 40-55 ms
 * lead-in: noise through a band-pass rising to ~4 kHz and sweeping to the
 * centre; then a bright crack with a darker tear under it (different noise
 * left and right, for width), the blade's ring -- a free-free bar's bending
 * modes (1 : 2.76 : 5.40 : 8.93 : 13.3), slightly detuned between the
 * channels so it shimmers -- and a short falling sine for weight. */
static int16_t *render_slash(int v, int *frames) {
    Rng vr = seed(0x51A54u, v);
    float ti = 0.040f + 0.015f * urnd(&vr);                   /* the blade connects */
    float dir = (v & 1) ? 1.0f : -1.0f;                       /* swing from the left or the right */
    float f0 = 1050.0f * (0.92f + 0.16f * urnd(&vr));         /* the ring's lowest mode */
    float crack_fc = 2900.0f * (0.88f + 0.24f * urnd(&vr));
    float len = 0.75f;

    /* the swing */
    Buf sw = buf_new(len);
    Svf fa = {0, 0}, fb = {0, 0};
    Rng nz = seed(0xA11CEu, v);
    for (int n = 0; n < sw.n; n++) {
        float t = (float)n / SR, u = t / ti;
        float env = t < ti ? powf(u, 1.6f) : expf(-(t - ti) / 0.022f);
        if (t > ti && env < 1e-4f) break;
        float fc = 600.0f * powf(7.0f, fminf(u, 1.2f));      /* 600 Hz to ~4.2 kHz at the hit */
        float x = rnd(&nz);
        float s = (svf(&fa, x, fc, 1.3f).bp + 0.35f * svf(&fb, x, fc * 1.9f, 0.7f).hp) * env;
        float gl, gr;
        pan(dir * 0.55f * (1.0f - smooth(u)), &gl, &gr);
        sw.l[n] = s * gl;
        sw.r[n] = s * gr;
    }
    normalize(&sw);

    /* the slice: a bright crack over a broadband burst, a darker tear under
     * them, lightly saturated so the body carries over the music */
    Buf sl = buf_new(len);
    Svf c1 = {0, 0}, c2 = {0, 0}, c3 = {0, 0}, c4 = {0, 0}, c5 = {0, 0}, c6 = {0, 0};
    Rng nl = seed(0x5EED1u, v), nr = seed(0x5EED2u, v);
    for (int n = (int)(ti * SR); n < sl.n; n++) {
        float t = (float)n / SR - ti;
        if (t > 0.5f) break;
        float crack = attack_decay(t, 0.0006f, 0.045f), burst = attack_decay(t, 0.0004f, 0.025f);
        float tear = attack_decay(t, 0.003f, 0.11f);
        float xl = rnd(&nl), xr = rnd(&nr);
        float l = svf(&c1, xl, crack_fc, 0.9f).bp * crack + 0.6f * svf(&c5, xl, 1000.0f, 0.7f).hp * burst
                + 0.55f * svf(&c3, xl, 1700.0f, 0.7f).lp * tear;
        float r = svf(&c2, xr, crack_fc, 0.9f).bp * crack + 0.6f * svf(&c6, xr, 1000.0f, 0.7f).hp * burst
                + 0.55f * svf(&c4, xr, 1700.0f, 0.7f).lp * tear;
        sl.l[n] = tanhf(2.2f * l);
        sl.r[n] = tanhf(2.2f * r);
    }
    normalize(&sl);

    /* the ring */
    static const float RATIO[5] = {1.0f, 2.756f, 5.404f, 8.933f, 13.34f};
    static const float DECAY[5] = {0.26f, 0.30f, 0.20f, 0.11f, 0.06f};
    static const float LEVEL[5] = {0.35f, 0.50f, 0.32f, 0.18f, 0.10f};
    Mode ml[5], mr[5];
    int nm = 0;
    for (int i = 0; i < 5; i++) {
        float f = f0 * RATIO[i];
        if (f > SR * 0.42f) break;
        mode_set(&ml[nm], f, DECAY[i], LEVEL[i]);
        mode_set(&mr[nm], f * 1.0035f, DECAY[i] * 0.9f, LEVEL[i]);
        nm++;
    }
    Buf rg = buf_new(len);
    Rng ne = seed(0xB1ADEu, v);
    for (int n = (int)(ti * SR); n < rg.n; n++) {
        float t = (float)n / SR - ti;
        float ex = t < 0.004f ? rnd(&ne) * (1.0f - t / 0.004f) : 0.0f;
        float l = 0, r = 0;
        for (int i = 0; i < nm; i++) { l += mode_step(&ml[i], ex); r += mode_step(&mr[i], ex); }
        rg.l[n] = l;
        rg.r[n] = r;
    }
    normalize(&rg);

    /* weight */
    Buf th = buf_new(len);
    float ph = 0;
    for (int n = (int)(ti * SR); n < th.n; n++) {
        float t = (float)n / SR - ti;
        if (t > 0.3f) break;
        ph += TAU * (55.0f + 95.0f * expf(-t / 0.022f)) / SR;
        float s = tanhf(1.6f * sinf(ph) * attack_decay(t, 0.0015f, 0.04f)) / tanhf(1.6f);
        th.l[n] = th.r[n] = s;
    }
    normalize(&th);

    Buf out = buf_new(len);
    mix(&out, &sw, -4.0f);
    mix(&out, &sl, 0.0f);
    mix(&out, &rg, -12.0f);
    mix(&out, &th, -6.0f);
    buf_free(&sw); buf_free(&sl); buf_free(&rg); buf_free(&th);
    reverb(&out, 0.5f, 0.45f, 0.11f, 350.0f);
    return finish(&out, -3.0f, 1.8f, frames);
}

/* Thrown slasher (SFXID_Slasher, $B7: Alys's attack). One request per
 * throw; the slasher leaves the screen to the right within 0.2 s and comes
 * back through the enemies, the first hit ($F5) 38-41 frames (0.63-0.68 s)
 * later. So: a whip of the throw, then the spin -- band-passed noise pulsed
 * at each pass of an arm (two per turn, ~17 a second), a low body pulsed the
 * same way -- flying off to the right (fading, Doppler down), returning from
 * the right (Doppler up) to pass through the enemies at the hit, and away. */
static int16_t *render_slasher(int v, int *frames) {
    Rng vr = seed(0xB00Eu, v);
    float spin = 16.0f + 3.0f * urnd(&vr);                    /* arm passes a second */
    float t_hit = 0.635f + 0.03f * urnd(&vr);
    float len = 1.15f;

    /* the throw's whip */
    Buf wh = buf_new(len);
    Svf fw = {0, 0};
    Rng nz = seed(0x7A20u, v);
    for (int n = 0; n < wh.n; n++) {
        float t = (float)n / SR;
        if (t > 0.2f) break;
        float env = attack_decay(t, 0.025f, 0.03f);
        float s = svf(&fw, rnd(&nz), 900.0f + 2600.0f * smooth(t / 0.04f), 1.2f).bp * env;
        float gl, gr;
        pan(0.1f + 0.3f * smooth(t / 0.06f), &gl, &gr);
        wh.l[n] = s * gl;
        wh.r[n] = s * gr;
    }
    normalize(&wh);

    /* the whirr: a falling tone under the throw, pulsed by the spin */
    Buf wr = buf_new(len);
    float wph = 0, wsp = 0;
    for (int n = 0; n < wr.n; n++) {
        float t = (float)n / SR;
        if (t > 0.45f) break;
        float f = 170.0f + 480.0f * expf(-t / 0.09f);
        wph += TAU * f / SR;
        wsp += spin * 0.95f / SR;
        float c = 0.5f + 0.5f * cosf(TAU * wsp);
        float s = (sinf(wph) + 0.3f * sinf(2.0f * wph)) * attack_decay(t, 0.01f, 0.1f) * (0.45f + 0.55f * c * c);
        float gl, gr;
        pan(0.2f + 0.6f * smooth(t / 0.25f), &gl, &gr);
        wr.l[n] = s * gl;
        wr.r[n] = s * gr;
    }
    normalize(&wr);

    /* the spin, out and back */
    Buf sp = buf_new(len);
    Svf f1 = {0, 0}, f2 = {0, 0};
    float phase = 0;
    for (int n = 0; n < sp.n; n++) {
        float t = (float)n / SR, amp, dop, p;
        if (t < 0.45f) {                                      /* flying off to the right */
            amp = smooth(t / 0.03f) * expf(-t / 0.12f);
            dop = 0.98f - 0.13f * smooth(t / 0.3f);
            p = 0.2f + 0.75f * smooth(t / 0.25f);
        } else {                                              /* coming back past the enemies */
            float tr = t - t_hit;
            amp = expf(-fabsf(tr) / (tr < 0 ? 0.075f : 0.11f)) * smooth((t - 0.45f) / 0.06f);
            dop = 1.0f + 0.12f * tanhf(-tr / 0.045f);
            p = 0.95f - 1.35f * smooth((tr + 0.12f) / 0.3f);
        }
        if (amp < 1e-5f) { sp.l[n] = sp.r[n] = 0; phase += spin / SR; continue; }
        phase += spin * dop / SR;
        float c = 0.5f + 0.5f * cosf(TAU * phase), pulse = c * c * c * c;
        float arm = ((int)floorf(phase) & 1) ? 0.82f : 1.0f;  /* the two arms differ a little */
        float x = rnd(&nz);
        float air = svf(&f1, x, 1500.0f * dop * (0.7f + 0.6f * pulse), 1.6f).bp * (0.22f + 0.78f * pulse) * arm;
        float body = svf(&f2, x, 380.0f * dop, 0.8f).lp * pulse * 1.1f;
        float s = (air + body) * amp, gl, gr;
        pan(p, &gl, &gr);
        sp.l[n] = s * gl;
        sp.r[n] = s * gr;
    }
    normalize(&sp);

    Buf out = buf_new(len);
    mix(&out, &wh, -2.0f);
    mix(&out, &wr, -9.0f);
    mix(&out, &sp, 0.0f);
    buf_free(&wh); buf_free(&wr); buf_free(&sp);
    reverb(&out, 0.6f, 0.4f, 0.18f, 300.0f);
    return finish(&out, -5.0f, 1.5f, frames);
}

/* Menu cursor (SFXID_MovingCursor, $F2): a soft tick -- a 2.35 kHz sine with
 * a tiny downward chirp, an octave below it a little longer, and a
 * millisecond of high noise for the edge. Variants differ by ~1.5% pitch. */
static int16_t *render_cursor(int v, int *frames) {
    float pitch = v == 1 ? 1.016f : v == 2 ? 0.984f : 1.0f;
    Buf b = buf_new(0.2f);
    Svf hp = {0, 0};
    Rng nz = seed(0xC0C0u, v);
    float p1 = 0, p2 = 0;
    for (int n = 0; n < b.n; n++) {
        float t = (float)n / SR;
        float f = 2350.0f * pitch * (1.0f + 0.22f * expf(-t / 0.006f));
        p1 += TAU * f / SR;
        p2 += TAU * f * 0.5f / SR;
        float s = sinf(p1) * attack_decay(t, 0.0005f, 0.021f) + 0.4f * sinf(p2) * attack_decay(t, 0.0008f, 0.032f)
                + 0.6f * svf(&hp, rnd(&nz), 4500.0f, 0.7f).hp * attack_decay(t, 0.0002f, 0.0012f);
        b.l[n] = b.r[n] = s;
    }
    reverb(&b, 0.2f, 0.6f, 0.05f, 400.0f);
    return finish(&b, -8.0f, 0.0f, frames);
}

/* Menu choice (SFXID_Selection, $F3; the most frequent sound, ~100 times in
 * a short session, so it stays soft): two notes a fifth apart, E6 then B6
 * 48 ms later, each a sine with a glassy FM edge that fades in 12 ms and a
 * touch of second and third partial, a little apart in the stereo field. */
static int16_t *render_select(int v, int *frames) {
    float det = v ? 1.007f : 1.0f;
    static const float NOTE[2] = {1318.5f, 1975.5f}, START[2] = {0.0f, 0.048f};
    static const float DECAY[2] = {0.085f, 0.13f}, LEVEL[2] = {0.75f, 1.0f};
    Buf b = buf_new(0.6f);
    for (int k = 0; k < 2; k++) {
        float ph = 0, pm = 0, f = NOTE[k] * det, gl, gr;
        pan(k ? 0.12f : -0.12f, &gl, &gr);
        for (int n = (int)(START[k] * SR); n < b.n; n++) {
            float t = (float)n / SR - START[k], env = attack_decay(t, 0.0012f, DECAY[k]);
            if (t > 0.01f && env < 1e-4f) break;
            ph += TAU * f / SR;
            pm += TAU * f * 3.5f / SR;
            float s = sinf(ph + 0.8f * expf(-t / 0.012f) * sinf(pm)) + 0.22f * sinf(2.0f * ph) * expf(-t / 0.04f)
                    + 0.08f * sinf(3.01f * ph) * expf(-t / 0.025f);
            s *= env * LEVEL[k];
            b.l[n] += s * gl;
            b.r[n] += s * gr;
        }
    }
    reverb(&b, 0.45f, 0.5f, 0.16f, 400.0f);
    return finish(&b, -10.0f, 0.0f, frames);
}

/* ------------------------------------------------------------------ layers
 * The battle set below is built from these: each returns one layer,
 * normalised, for mix() at a set level. Times in seconds, from the layer's
 * start; `t0` delays a layer inside the effect. */

/* noise through a band-pass whose centre moves from f0 to f1 (exponentially,
 * over `sweep` seconds), shaped by attack_decay(a, tau) and panned from p0 to
 * p1 over the same time; `air` (0-1) adds the high-passed noise for breath */
static Buf noise_sweep(float len, float t0, float a, float tau, float f0, float f1, float sweep, float q,
                       float p0, float p1, float air, uint32_t s, int v) {
    Buf b = buf_new(len);
    Svf f = {0, 0}, h = {0, 0};
    Rng r = seed(s, v);
    for (int n = t0 > 0 ? (int)(t0 * SR) : 0; n < b.n; n++) {
        float t = (float)n / SR - t0, env = attack_decay(t, a, tau);
        if (t > a && env < 1e-4f) break;
        float u = clamp01(t / sweep), fc = f0 * powf(f1 / f0, u), x = rnd(&r), gl, gr;
        float y = (svf(&f, x, fc, q).bp + air * svf(&h, x, fc * 1.6f, 0.7f).hp) * env;
        pan(p0 + (p1 - p0) * smooth(u), &gl, &gr);
        b.l[n] = y * gl;
        b.r[n] = y * gr;
    }
    normalize(&b);
    return b;
}

/* a tone gliding from f0 to f1 (time constant `glide`), its edge from FM
 * (`edge` 0-1, fading over the first 60 ms by half), the right channel
 * detuned by `det` for width; attack_decay(a, tau); at pan p */
static Buf tone_glide(float len, float t0, float a, float tau, float f0, float f1, float glide, float edge,
                      float det, float p) {
    Buf b = buf_new(len);
    float pl = 0, pr = 0, gl, gr;
    pan(p, &gl, &gr);
    for (int n = t0 > 0 ? (int)(t0 * SR) : 0; n < b.n; n++) {
        float t = (float)n / SR - t0, env = attack_decay(t, a, tau);
        if (t > a && env < 1e-4f) break;
        float f = f1 + (f0 - f1) * expf(-t / glide), e = edge * (0.5f + 0.5f * expf(-t / 0.06f));
        pl += TAU * f / SR;
        pr += TAU * f * (1.0f + det) / SR;
        b.l[n] = sinf(pl + e * sinf(2.0f * pl)) * env * gl;
        b.r[n] = sinf(pr + e * sinf(2.0f * pr)) * env * gr;
    }
    normalize(&b);
    return b;
}

/* weight: a sine falling from f0 to f1, saturated by `drive` */
static Buf thump(float len, float t0, float f0, float f1, float glide, float tau, float drive) {
    Buf b = buf_new(len);
    float ph = 0, nd = tanhf(drive > 0 ? drive : 1.0f);
    for (int n = t0 > 0 ? (int)(t0 * SR) : 0; n < b.n; n++) {
        float t = (float)n / SR - t0, env = attack_decay(t, 0.0015f, tau);
        if (t > 0.01f && env < 1e-4f) break;
        ph += TAU * (f1 + (f0 - f1) * expf(-t / glide)) / SR;
        float s = drive > 0 ? tanhf(drive * sinf(ph) * env) / nd : sinf(ph) * env;
        b.l[n] = b.r[n] = s;
    }
    normalize(&b);
    return b;
}

/* crackle: from t0 for `dur`, little bursts at `rate` a second (random), each
 * 1-5 ms of noise ringing through a band near fc (+-spread octaves), at a
 * random place in the stereo field, the density fading over `dur` */
static Buf crackle(float len, float t0, float dur, float rate, float fc, float spread, float q, uint32_t s, int v) {
    Buf b = buf_new(len);
    Rng r = seed(s, v);
    Svf fl = {0, 0}, fr = {0, 0};
    float env = 0, tau = 0.002f, gl = 0.7f, gr = 0.7f, f = fc;
    for (int n = t0 > 0 ? (int)(t0 * SR) : 0; n < b.n; n++) {
        float t = (float)n / SR - t0;
        if (t > dur + 0.05f) break;
        float dens = t < dur ? 1.0f - 0.7f * t / dur : 0.0f;
        if (urnd(&r) < rate * dens / SR) {
            env = 0.35f + 0.65f * urnd(&r);
            tau = 0.001f + 0.004f * urnd(&r);
            f = fc * powf(2.0f, spread * rnd(&r));
            pan(rnd(&r) * 0.8f, &gl, &gr);
        }
        float x = rnd(&r) * env;
        env *= expf(-1.0f / (tau * SR));
        b.l[n] = svf(&fl, x, f, q).bp * gl;
        b.r[n] = svf(&fr, x, f, q).bp * gr;
    }
    normalize(&b);
    return b;
}

/* a struck glass/bell: partials at `ratio` of f, each its own decay, a 2 ms
 * strike; added into b at `amp` and pan p (not normalised: chimes stack) */
static void bell(Buf *b, float t0, float f, float tau, float amp, float p) {
    static const float RATIO[5] = {1.0f, 2.0f, 2.76f, 4.07f, 5.4f};
    static const float LEVEL[5] = {1.0f, 0.42f, 0.24f, 0.12f, 0.06f};
    static const float DECAY[5] = {1.0f, 0.62f, 0.4f, 0.26f, 0.18f};
    float gl, gr;
    pan(p, &gl, &gr);
    for (int k = 0; k < 5; k++) {
        float fk = f * RATIO[k];
        if (fk > SR * 0.42f) break;
        float ph = 0, w = TAU * fk / SR, lv = LEVEL[k] * amp;
        for (int n = t0 > 0 ? (int)(t0 * SR) : 0; n < b->n; n++) {
            float t = (float)n / SR - t0, env = attack_decay(t, 0.002f, tau * DECAY[k]);
            if (t > 0.01f && env < 1e-4f) break;
            ph += w;
            float s = sinf(ph) * env * lv;
            b->l[n] += s * gl;
            b->r[n] += s * gr * (k & 1 ? 0.92f : 1.0f);
        }
    }
}

/* amplitude pulses at `hz` (0 to 1 to 0 each cycle, sharpened by `pow`) over
 * a layer, from t0 */
static void pulse(Buf *b, float t0, float hz, float sharp) {
    float ph = 0;
    for (int n = t0 > 0 ? (int)(t0 * SR) : 0; n < b->n; n++) {
        ph += hz / SR;
        float c = 0.5f - 0.5f * cosf(TAU * ph), g = powf(c, sharp);
        b->l[n] *= g;
        b->r[n] *= g;
    }
}

/* ------------------------------------------------------------------ the battle set
 * Made from the battles the test bot fights across the fan saves
 * (tools/sfx_census.py: which effects come, and what calls them) and the
 * originals' own lengths and shapes (tools/sfx_study.py). Each keeps the
 * original's length, so it fits the animation it was timed to. */

/* Claw (SFXID_Claw, $E8; party claws and Double Slash, the commonest battle
 * sound after the menus): three rakes 22-30 ms apart drawing across the
 * stereo field, a ragged tear under them and a soft thud */
static int16_t *render_claw(int v, int *frames) {
    Rng vr = seed(0xC1A3u, v);
    float len = 0.42f, gap = 0.022f + 0.008f * urnd(&vr), top = 5200.0f * (0.9f + 0.2f * urnd(&vr));
    Buf out = buf_new(len);
    for (int k = 0; k < 3; k++) {
        float p = -0.5f + 0.5f * (float)k;
        Buf rk = noise_sweep(len, (float)k * gap, 0.001f, 0.024f, top * (1.0f - 0.08f * (float)k), 1300.0f, 0.06f, 2.4f,
                             p - 0.15f, p + 0.15f, 0.25f, 0xC1A4u + (uint32_t)k, v);
        mix(&out, &rk, k == 1 ? 0.0f : -2.0f);
        buf_free(&rk);
    }
    Buf tr = noise_sweep(len, 0.004f, 0.004f, 0.06f, 1800.0f, 700.0f, 0.12f, 0.8f, -0.2f, 0.2f, 0.0f, 0xC1A5u, v);
    Buf th = thump(len, 0.012f, 170.0f, 70.0f, 0.02f, 0.045f, 1.4f);
    mix(&out, &tr, -8.0f);
    mix(&out, &th, -9.0f);
    buf_free(&tr); buf_free(&th);
    reverb(&out, 0.35f, 0.5f, 0.08f, 400.0f);
    return finish(&out, -3.0f, 1.4f, frames);
}

/* Enemy killed (SFXID_EnemyKilled, $B9): a falling zap, then the enemy
 * dissolving -- a soft burst darkening as it goes, dust glittering off it --
 * over a low thump; 0.9 s like the original */
static int16_t *render_dissolve(int v, int *frames) {
    Rng vr = seed(0xD1E5u, v);
    float len = 1.1f, k = 0.9f + 0.2f * urnd(&vr);
    Buf zp = tone_glide(len, 0.0f, 0.002f, 0.11f, 2100.0f * k, 140.0f * k, 0.07f, 0.9f, 0.006f, 0.0f);
    Buf bu = noise_sweep(len, 0.02f, 0.006f, 0.24f, 4200.0f, 280.0f, 0.5f, 0.7f, -0.25f, 0.25f, 0.3f, 0xD1E6u, v);
    Buf du = crackle(len, 0.08f, 0.62f, 70.0f, 7000.0f * k, 0.5f, 3.0f, 0xD1E7u, v);
    Buf th = thump(len, 0.01f, 110.0f, 42.0f, 0.03f, 0.18f, 1.2f);
    Buf out = buf_new(len);
    mix(&out, &zp, -5.0f);
    mix(&out, &bu, 0.0f);
    mix(&out, &du, -10.0f);
    mix(&out, &th, -6.0f);
    buf_free(&zp); buf_free(&bu); buf_free(&du); buf_free(&th);
    reverb(&out, 0.62f, 0.45f, 0.2f, 300.0f);
    return finish(&out, -4.0f, 1.2f, frames);
}

/* Grave opening (SFXID_GraveOpening, $DD; in battle an enemy's attack, and
 * heard a lot): a grinding rumble pulsing ~19 times a second for a second --
 * dark noise and a crunchier band under a wavering growl */
static int16_t *render_grind(int v, int *frames) {
    Rng vr = seed(0x6A1Du, v);
    float len = 1.35f, hz = 18.0f + 3.0f * urnd(&vr);
    Buf lo = noise_sweep(len, 0.0f, 0.03f, 0.45f, 520.0f, 260.0f, 1.1f, 0.6f, -0.2f, 0.2f, 0.0f, 0x6A1Eu, v);
    Buf cr = noise_sweep(len, 0.0f, 0.02f, 0.38f, 2600.0f, 1500.0f, 1.0f, 1.4f, 0.3f, -0.3f, 0.2f, 0x6A1Fu, v);
    pulse(&lo, 0.0f, hz, 1.5f);
    pulse(&cr, 0.012f, hz, 2.5f);
    Buf gw = buf_new(len);                                    /* the growl: a rough low tone, wavering */
    float ph = 0;
    for (int n = 0; n < gw.n; n++) {
        float t = (float)n / SR, env = attack_decay(t, 0.05f, 0.45f);
        ph += TAU * (62.0f + 7.0f * sinf(TAU * 3.1f * t)) / SR;
        float s = tanhf(2.5f * sinf(ph)) * env;
        gw.l[n] = s;
        gw.r[n] = s;
    }
    pulse(&gw, 0.006f, hz, 1.0f);
    normalize(&gw);
    Buf out = buf_new(len);
    mix(&out, &lo, 0.0f);
    mix(&out, &cr, -5.0f);
    mix(&out, &gw, -7.0f);
    buf_free(&lo); buf_free(&cr); buf_free(&gw);
    reverb(&out, 0.5f, 0.6f, 0.12f, 250.0f);
    return finish(&out, -2.0f, 1.6f, frames);
}

/* Zan (SFXID_Zan, $C6: Zan, Nazan, the Hewn skill): wind blades -- two
 * whistling gusts sweeping up and away in opposite directions, a thin
 * whistle riding the first, airy noise trailing */
static int16_t *render_wind(int v, int *frames) {
    Rng vr = seed(0x2A11u, v);
    float len = 0.62f, k = 0.92f + 0.16f * urnd(&vr);
    Buf g1 = noise_sweep(len, 0.0f, 0.06f, 0.12f, 650.0f * k, 3600.0f * k, 0.22f, 4.0f, -0.6f, 0.5f, 0.15f, 0x2A12u, v);
    Buf g2 = noise_sweep(len, 0.07f, 0.05f, 0.13f, 900.0f * k, 4300.0f * k, 0.2f, 3.2f, 0.6f, -0.4f, 0.15f, 0x2A13u, v);
    Buf wh = tone_glide(len, 0.03f, 0.05f, 0.1f, 1500.0f * k, 2900.0f * k, 0.12f, 0.15f, 0.004f, -0.1f);
    Buf ai = noise_sweep(len, 0.12f, 0.08f, 0.16f, 6000.0f, 3000.0f, 0.4f, 0.7f, -0.3f, 0.3f, 0.6f, 0x2A14u, v);
    Buf out = buf_new(len);
    mix(&out, &g1, 0.0f);
    mix(&out, &g2, -1.5f);
    mix(&out, &wh, -14.0f);
    mix(&out, &ai, -9.0f);
    buf_free(&g1); buf_free(&g2); buf_free(&wh); buf_free(&ai);
    reverb(&out, 0.55f, 0.4f, 0.16f, 400.0f);
    return finish(&out, -3.0f, 1.3f, frames);
}

/* An enemy's blow (SFXID_EnemyAttack1, $BA): a punch -- a low thump, a
 * crack and a short body of dark noise */
static int16_t *render_blow(int v, int *frames) {
    Rng vr = seed(0xB10Au, v);
    float len = 0.35f, k = 0.9f + 0.2f * urnd(&vr);
    Buf th = thump(len, 0.0f, 150.0f * k, 52.0f * k, 0.025f, 0.06f, 2.0f);
    Buf ck = noise_sweep(len, 0.0f, 0.0005f, 0.018f, 2100.0f * k, 1400.0f, 0.03f, 1.0f, -0.1f, 0.1f, 0.4f, 0xB10Bu, v);
    Buf bo = noise_sweep(len, 0.002f, 0.003f, 0.045f, 900.0f, 400.0f, 0.06f, 0.7f, 0.1f, -0.1f, 0.0f, 0xB10Cu, v);
    Buf out = buf_new(len);
    mix(&out, &th, 0.0f);
    mix(&out, &ck, -3.0f);
    mix(&out, &bo, -6.0f);
    buf_free(&th); buf_free(&ck); buf_free(&bo);
    reverb(&out, 0.35f, 0.55f, 0.07f, 300.0f);
    return finish(&out, -3.0f, 1.8f, frames);
}

/* Mole attack (SFXID_MoleAttack, $D5; an enemy's): six snapping bites ~50 ms
 * apart -- a click and a short chirp each, a little uneven */
static int16_t *render_gnash(int v, int *frames) {
    Rng vr = seed(0x6A55u, v);
    float len = 0.5f;
    Buf out = buf_new(len);
    for (int k = 0; k < 6; k++) {
        float t0 = fmaxf(0.0f, 0.05f * (float)k + 0.006f * rnd(&vr)), f = 1200.0f * (0.85f + 0.3f * urnd(&vr));
        float p = (k & 1) ? 0.25f : -0.25f;
        Buf ck = noise_sweep(len, t0, 0.0004f, 0.006f, 3300.0f, 2600.0f, 0.01f, 1.6f, p, p, 0.3f, 0x6A56u + (uint32_t)k, v);
        Buf ch = tone_glide(len, t0, 0.001f, 0.016f, f * 1.4f, f * 0.7f, 0.012f, 0.6f, 0.0f, p);
        mix(&out, &ck, 0.0f);
        mix(&out, &ch, -5.0f);
        buf_free(&ck); buf_free(&ch);
    }
    reverb(&out, 0.3f, 0.5f, 0.08f, 400.0f);
    return finish(&out, -4.0f, 1.2f, frames);
}

/* Rod (SFXID_Rod, $B5: Rune's and Hahn's staves): a knock of hard wood --
 * the bending modes of a short bar, a click and a little thump */
static int16_t *render_rod(int v, int *frames) {
    Rng vr = seed(0x20D0u, v);
    float len = 0.3f, f0 = 380.0f * (0.9f + 0.2f * urnd(&vr));
    static const float RATIO[4] = {1.0f, 2.756f, 5.404f, 8.933f};
    static const float DECAY[4] = {0.05f, 0.032f, 0.02f, 0.012f};
    static const float LEVEL[4] = {1.0f, 0.55f, 0.3f, 0.15f};
    Mode ml[4], mr[4];
    for (int i = 0; i < 4; i++) {
        mode_set(&ml[i], f0 * RATIO[i], DECAY[i], LEVEL[i]);
        mode_set(&mr[i], f0 * RATIO[i] * 1.004f, DECAY[i], LEVEL[i]);
    }
    Buf wd = buf_new(len);
    Rng ne = seed(0x20D1u, v);
    for (int n = 0; n < wd.n; n++) {
        float t = (float)n / SR, ex = t < 0.002f ? rnd(&ne) * (1.0f - t / 0.002f) : 0.0f, l = 0, r = 0;
        for (int i = 0; i < 4; i++) { l += mode_step(&ml[i], ex); r += mode_step(&mr[i], ex); }
        wd.l[n] = l;
        wd.r[n] = r;
    }
    normalize(&wd);
    Buf ck = noise_sweep(len, 0.0f, 0.0003f, 0.004f, 4000.0f, 3000.0f, 0.01f, 1.2f, 0.0f, 0.0f, 0.5f, 0x20D2u, v);
    Buf th = thump(len, 0.0f, 120.0f, 60.0f, 0.02f, 0.04f, 1.2f);
    Buf out = buf_new(len);
    mix(&out, &wd, 0.0f);
    mix(&out, &ck, -6.0f);
    mix(&out, &th, -8.0f);
    buf_free(&wd); buf_free(&ck); buf_free(&th);
    reverb(&out, 0.3f, 0.5f, 0.08f, 400.0f);
    return finish(&out, -3.0f, 1.0f, frames);
}

/* Foi (SFXID_Foi, $BE: the fire techniques and fire weapons): two puffs of
 * flame 0.12 s apart, like the original -- a whoosh catching, a flickering
 * roar, a low fwoomp -- and crackling between and after */
static int16_t *render_fire(int v, int *frames) {
    float len = 0.62f;
    Buf out = buf_new(len);
    for (int k = 0; k < 2; k++) {
        float t0 = 0.12f * (float)k, p = k ? 0.25f : -0.25f;
        Buf ig = noise_sweep(len, t0, 0.015f, 0.035f, 450.0f, 3200.0f, 0.05f, 1.4f, p, p, 0.2f, 0xF01u + (uint32_t)k, v);
        Buf ro = noise_sweep(len, t0 + 0.025f, 0.015f, k ? 0.13f : 0.07f, 2600.0f, 900.0f, 0.25f, 0.6f, p, -p, 0.1f,
                             0xF02u + (uint32_t)k, v);
        pulse(&ro, t0, 23.0f + 5.0f * (float)k, 0.6f);         /* the flame flickers */
        Buf fw = thump(len, t0 + 0.01f, 140.0f, 55.0f, 0.03f, 0.09f, 1.3f);
        mix(&out, &ig, k ? -3.0f : -1.0f);
        mix(&out, &ro, k ? -1.0f : 0.0f);
        mix(&out, &fw, -8.0f);
        buf_free(&ig); buf_free(&ro); buf_free(&fw);
    }
    Buf cr = crackle(len, 0.03f, 0.45f, 90.0f, 3000.0f, 0.7f, 2.0f, 0xF03u, v);
    mix(&out, &cr, -9.0f);
    buf_free(&cr);
    reverb(&out, 0.5f, 0.45f, 0.13f, 300.0f);
    return finish(&out, -3.0f, 1.4f, frames);
}

/* Res (SFXID_Res, $CC: Res, Gires and the healing items): the original's
 * rising staircase as a run of glass bells up a pentatonic scale, left to
 * right, over a high shimmer that settles */
static int16_t *render_heal(int v, int *frames) {
    static const float NOTE[8] = {1046.5f, 1174.7f, 1318.5f, 1568.0f, 1760.0f, 2093.0f, 2349.3f, 2637.0f};
    float len = 1.3f, det = v ? 1.0f / 1.0595f : 1.0f;        /* variant 2 a semitone lower */
    Buf b = buf_new(len);
    for (int k = 0; k < 8; k++)
        bell(&b, 0.055f * (float)k, NOTE[k] * det, 0.32f, 1.0f - 0.04f * (float)k, -0.6f + 1.2f * (float)k / 7.0f);
    normalize(&b);
    Buf sh = tone_glide(len, 0.15f, 0.25f, 0.35f, 4186.0f * det, 4186.0f * det, 1.0f, 0.1f, 0.003f, 0.0f);
    Buf sp = crackle(len, 0.1f, 0.6f, 40.0f, 9000.0f, 0.4f, 4.0f, 0x7E5u, v);
    Buf out = buf_new(len);
    mix(&out, &b, 0.0f);
    mix(&out, &sh, -16.0f);
    mix(&out, &sp, -16.0f);
    buf_free(&b); buf_free(&sh); buf_free(&sp);
    reverb(&out, 0.7f, 0.35f, 0.3f, 400.0f);
    return finish(&out, -5.0f, 0.0f, frames);
}

/* A technique cast (SFXID_TechCast, $BB): a bright chime, then at 0.2 s a
 * rising rush and three glittering tones falling into it */
static int16_t *render_cast(int v, int *frames) {
    float len = 0.95f, k = v ? 1.06f : 1.0f;
    Buf ch = buf_new(len);
    bell(&ch, 0.0f, 2637.0f * k, 0.14f, 1.0f, -0.2f);
    bell(&ch, 0.006f, 3951.0f * k, 0.11f, 0.6f, 0.2f);
    normalize(&ch);
    Buf ru = noise_sweep(len, 0.2f, 0.18f, 0.12f, 600.0f, 5200.0f, 0.3f, 2.2f, -0.4f, 0.4f, 0.3f, 0xCA5u, v);
    Buf out = buf_new(len);
    mix(&out, &ch, -2.0f);
    mix(&out, &ru, -4.0f);
    for (int i = 0; i < 3; i++) {
        Buf gl = tone_glide(len, 0.22f + 0.05f * (float)i, 0.004f, 0.12f, 3300.0f * k / (1.0f + 0.2f * (float)i),
                            1300.0f * k, 0.09f, 0.5f, 0.005f, -0.3f + 0.3f * (float)i);
        mix(&out, &gl, -7.0f);
        buf_free(&gl);
    }
    buf_free(&ch); buf_free(&ru);
    reverb(&out, 0.6f, 0.4f, 0.22f, 400.0f);
    return finish(&out, -4.0f, 0.0f, frames);
}

/* Shot (SFXID_Shot, $B6: Wren's gun): an energy shot -- a sharp crack, a
 * bright "pew" falling fast, and a low punch */
static int16_t *render_shot(int v, int *frames) {
    Rng vr = seed(0x5407u, v);
    float len = 0.32f, k = 0.9f + 0.2f * urnd(&vr);
    Buf ck = noise_sweep(len, 0.0f, 0.0003f, 0.006f, 6000.0f, 4000.0f, 0.01f, 0.8f, 0.0f, 0.0f, 0.8f, 0x5408u, v);
    Buf pw = tone_glide(len, 0.0f, 0.001f, 0.06f, 2600.0f * k, 260.0f * k, 0.028f, 1.2f, 0.01f, 0.1f);
    Buf pu = thump(len, 0.0f, 130.0f, 48.0f, 0.02f, 0.05f, 2.0f);
    Buf tl = noise_sweep(len, 0.01f, 0.004f, 0.06f, 2400.0f, 600.0f, 0.08f, 0.8f, 0.1f, 0.4f, 0.0f, 0x5409u, v);
    Buf out = buf_new(len);
    mix(&out, &ck, -4.0f);
    mix(&out, &pw, 0.0f);
    mix(&out, &pu, -5.0f);
    mix(&out, &tl, -9.0f);
    buf_free(&ck); buf_free(&pw); buf_free(&pu); buf_free(&tl);
    reverb(&out, 0.4f, 0.5f, 0.1f, 350.0f);
    return finish(&out, -3.0f, 1.5f, frames);
}

/* Moonshade (SFXID_Moonshad, $C4: Anti, Rimpa, Giwat): glitter rising in six
 * quick steps, then a soft fifth that breathes under a sparkle */
static int16_t *render_glimmer(int v, int *frames) {
    static const float NOTE[6] = {2349.3f, 2793.8f, 3135.9f, 3729.3f, 4186.0f, 4698.6f};
    float len = 0.85f, k = v ? 0.94f : 1.0f;
    Buf gl = buf_new(len);
    for (int i = 0; i < 6; i++) bell(&gl, 0.035f * (float)i, NOTE[i] * k, 0.12f, 1.0f, -0.5f + 0.2f * (float)i);
    normalize(&gl);
    Buf p1 = tone_glide(len, 0.2f, 0.06f, 0.3f, 440.0f * k, 440.0f * k, 1.0f, 0.2f, 0.004f, -0.2f);
    Buf p2 = tone_glide(len, 0.2f, 0.06f, 0.3f, 660.0f * k, 660.0f * k, 1.0f, 0.2f, -0.004f, 0.2f);
    pulse(&p1, 0.2f, 7.0f, 0.5f);
    pulse(&p2, 0.24f, 7.0f, 0.5f);
    Buf sp = crackle(len, 0.2f, 0.45f, 50.0f, 8000.0f, 0.5f, 4.0f, 0x400Du, v);
    Buf out = buf_new(len);
    mix(&out, &gl, 0.0f);
    mix(&out, &p1, -10.0f);
    mix(&out, &p2, -12.0f);
    mix(&out, &sp, -14.0f);
    buf_free(&gl); buf_free(&p1); buf_free(&p2); buf_free(&sp);
    reverb(&out, 0.65f, 0.4f, 0.26f, 400.0f);
    return finish(&out, -5.0f, 0.0f, frames);
}

/* An enemy's swipe (SFXID_EnemyAttack3, $D7): a rush of air falling in
 * pitch, a tone falling under it, and a thud */
static int16_t *render_swipe(int v, int *frames) {
    Rng vr = seed(0x5A1Eu, v);
    float len = 0.42f, k = 0.9f + 0.2f * urnd(&vr), dir = (v & 1) ? 1.0f : -1.0f;
    Buf ai = noise_sweep(len, 0.0f, 0.012f, 0.06f, 3300.0f * k, 800.0f, 0.12f, 2.0f, -0.6f * dir, 0.6f * dir, 0.2f, 0x5A1Fu, v);
    Buf tn = tone_glide(len, 0.004f, 0.006f, 0.08f, 1400.0f * k, 380.0f * k, 0.05f, 0.8f, 0.006f, 0.0f);
    Buf th = thump(len, 0.05f, 140.0f, 60.0f, 0.02f, 0.05f, 1.6f);
    Buf out = buf_new(len);
    mix(&out, &ai, 0.0f);
    mix(&out, &tn, -8.0f);
    mix(&out, &th, -6.0f);
    buf_free(&ai); buf_free(&tn); buf_free(&th);
    reverb(&out, 0.4f, 0.5f, 0.1f, 350.0f);
    return finish(&out, -3.0f, 1.5f, frames);
}

/* A healing cast (SFXID_HealTechCast, $BD): a soft rising sweep, as in the
 * original, settling into three chimes of a major chord */
static int16_t *render_healcast(int v, int *frames) {
    float len = 0.95f, k = v ? 1.06f : 1.0f;
    Buf sw = tone_glide(len, 0.0f, 0.12f, 0.12f, 300.0f * k, 1250.0f * k, 0.09f, 0.35f, 0.004f, 0.0f);
    Buf ch = buf_new(len);
    bell(&ch, 0.22f, 1318.5f * k, 0.3f, 1.0f, -0.35f);
    bell(&ch, 0.27f, 1661.2f * k, 0.3f, 0.9f, 0.0f);
    bell(&ch, 0.32f, 1975.5f * k, 0.35f, 0.85f, 0.35f);
    normalize(&ch);
    Buf sp = crackle(len, 0.25f, 0.4f, 35.0f, 9000.0f, 0.4f, 4.0f, 0x4EA1u, v);
    Buf out = buf_new(len);
    mix(&out, &sw, -3.0f);
    mix(&out, &ch, 0.0f);
    mix(&out, &sp, -15.0f);
    buf_free(&sw); buf_free(&ch); buf_free(&sp);
    reverb(&out, 0.65f, 0.4f, 0.26f, 400.0f);
    return finish(&out, -5.0f, 0.0f, frames);
}

/* A miss (SFXID_AttackMiss, $B8): a light whiff of air passing by */
static int16_t *render_miss(int v, int *frames) {
    float len = 0.22f, dir = (v & 1) ? 1.0f : -1.0f;
    Buf ai = noise_sweep(len, 0.0f, 0.02f, 0.035f, 2600.0f, 900.0f, 0.07f, 1.8f, -0.5f * dir, 0.5f * dir, 0.3f, 0x2155u, v);
    reverb(&ai, 0.3f, 0.5f, 0.06f, 500.0f);
    return finish(&ai, -6.0f, 0.0f, frames);
}

/* Gra (SFXID_Gra, $C7). The game asks for it every 8 frames while the
 * gravity wave runs (as it does for enemies' Gra), so each is one deep
 * pulse of the warp, short enough that they chain into a throb: a falling
 * saturated sine, a sub, and dark noise squeezed by the pulse */
static int16_t *render_gravity(int v, int *frames) {
    float len = 0.3f, k = v == 1 ? 0.94f : v == 2 ? 1.06f : 1.0f;
    Buf wp = thump(len, 0.0f, 150.0f * k, 46.0f * k, 0.035f, 0.07f, 2.4f);
    Buf sb = tone_glide(len, 0.0f, 0.01f, 0.09f, 48.0f * k, 40.0f * k, 0.05f, 0.0f, 0.0f, 0.0f);
    Buf dk = noise_sweep(len, 0.0f, 0.01f, 0.06f, 420.0f, 180.0f, 0.1f, 0.9f, -0.3f, 0.3f, 0.0f, 0x6A0u, v);
    Buf zg = noise_sweep(len, 0.0f, 0.001f, 0.016f, 6500.0f * k, 3500.0f * k, 0.03f, 2.5f, -0.2f, 0.2f, 0.3f, 0x6A1u, v);
    Buf out = buf_new(len);
    mix(&out, &wp, 0.0f);
    mix(&out, &sb, -5.0f);
    mix(&out, &dk, -7.0f);
    mix(&out, &zg, -11.0f);
    buf_free(&wp); buf_free(&sb); buf_free(&dk); buf_free(&zg);
    reverb(&out, 0.5f, 0.6f, 0.1f, 200.0f);
    return finish(&out, -3.0f, 1.6f, frames);
}

/* Recovery (SFXID_Recovery, $CD: the recovery skills and items): the
 * original's arpeggio, rising four times, as soft glass over a shimmer */
static int16_t *render_recover(int v, int *frames) {
    static const float NOTE[4] = {1046.5f, 1318.5f, 1568.0f, 2093.0f};
    float len = 1.6f, k = v ? 1.0595f : 1.0f;
    Buf b = buf_new(len);
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++)
            bell(&b, 0.29f * (float)c + 0.05f * (float)i, NOTE[i] * k * (1.0f + 0.06f * (float)c), 0.12f,
                 0.8f + 0.05f * (float)i, -0.4f + 0.27f * (float)i);
    normalize(&b);
    Buf sh = tone_glide(len, 0.0f, 0.3f, 0.6f, 3136.0f * k, 3136.0f * k, 1.0f, 0.1f, 0.003f, 0.0f);
    pulse(&sh, 0.0f, 5.0f, 0.5f);
    Buf out = buf_new(len);
    mix(&out, &b, 0.0f);
    mix(&out, &sh, -17.0f);
    buf_free(&b); buf_free(&sh);
    reverb(&out, 0.7f, 0.35f, 0.28f, 400.0f);
    return finish(&out, -6.0f, 0.0f, frames);
}

/* Fire breath (SFXID_FireBreath, $C2): a roar -- flickering fire, a growl
 * under it, crackling */
static int16_t *render_breath(int v, int *frames) {
    float len = 0.6f;
    Buf ro = noise_sweep(len, 0.0f, 0.02f, 0.2f, 1800.0f, 700.0f, 0.3f, 0.6f, -0.3f, 0.3f, 0.25f, 0xB2EAu, v);
    pulse(&ro, 0.0f, 19.0f, 0.5f);
    Buf gr = buf_new(len);
    float ph = 0;
    for (int n = 0; n < gr.n; n++) {
        float t = (float)n / SR, env = attack_decay(t, 0.02f, 0.2f);
        ph += TAU * (95.0f + 12.0f * sinf(TAU * 7.0f * t)) / SR;
        float s = tanhf(3.0f * sinf(ph)) * env;                     /* a rough, square-ish growl */
        gr.l[n] = gr.r[n] = s;
    }
    normalize(&gr);
    Buf cr = crackle(len, 0.02f, 0.35f, 120.0f, 2500.0f, 0.8f, 2.0f, 0xB2EBu, v);
    Buf out = buf_new(len);
    mix(&out, &ro, 0.0f);
    mix(&out, &gr, -7.0f);
    mix(&out, &cr, -8.0f);
    buf_free(&ro); buf_free(&gr); buf_free(&cr);
    reverb(&out, 0.5f, 0.45f, 0.14f, 300.0f);
    return finish(&out, -2.0f, 1.5f, frames);
}

/* A buff (SFXID_BuffCast, $BC: Saner, Shift, Deban): powering up -- a tone
 * and its fifth climbing two octaves, a rush climbing with them, a sparkle
 * at the top */
static int16_t *render_buff(int v, int *frames) {
    float len = 0.75f, k = v ? 1.06f : 1.0f;
    Buf t1 = tone_glide(len, 0.0f, 0.02f, 0.2f, 220.0f * k, 880.0f * k, 0.12f, 0.6f, 0.005f, -0.2f);
    Buf t2 = tone_glide(len, 0.0f, 0.02f, 0.2f, 330.0f * k, 1320.0f * k, 0.12f, 0.6f, -0.005f, 0.2f);
    Buf ru = noise_sweep(len, 0.0f, 0.25f, 0.1f, 500.0f, 6000.0f, 0.32f, 1.8f, -0.3f, 0.3f, 0.3f, 0xB0FFu, v);
    Buf sp = crackle(len, 0.3f, 0.3f, 60.0f, 8000.0f, 0.4f, 4.0f, 0xB100u, v);
    Buf out = buf_new(len);
    mix(&out, &t1, -2.0f);
    mix(&out, &t2, -6.0f);
    mix(&out, &ru, -6.0f);
    mix(&out, &sp, -12.0f);
    buf_free(&t1); buf_free(&t2); buf_free(&ru); buf_free(&sp);
    reverb(&out, 0.55f, 0.4f, 0.2f, 400.0f);
    return finish(&out, -3.0f, 1.0f, frames);
}

/* Brose (SFXID_Brose, $CB: Brose and the Diem skill): a second of crackling
 * energy pulsing ~28 times a second, a bright line falling through it */
static int16_t *render_surge(int v, int *frames) {
    float len = 1.25f, k = v ? 0.94f : 1.0f;
    Buf bz = noise_sweep(len, 0.0f, 0.03f, 0.5f, 1300.0f * k, 900.0f * k, 1.0f, 2.0f, -0.3f, 0.3f, 0.2f, 0xB205u, v);
    Buf hm = tone_glide(len, 0.0f, 0.03f, 0.5f, 180.0f * k, 170.0f * k, 1.0f, 1.2f, 0.01f, 0.0f);
    pulse(&bz, 0.0f, 28.0f, 2.0f);
    pulse(&hm, 0.0f, 28.0f, 1.0f);
    Buf ln = tone_glide(len, 0.02f, 0.05f, 0.45f, 6200.0f * k, 1500.0f * k, 0.35f, 0.4f, 0.004f, 0.0f);
    Buf cr = crackle(len, 0.0f, 0.9f, 140.0f, 4500.0f, 0.6f, 3.0f, 0xB206u, v);
    Buf out = buf_new(len);
    mix(&out, &bz, 0.0f);
    mix(&out, &hm, -6.0f);
    mix(&out, &ln, -10.0f);
    mix(&out, &cr, -9.0f);
    buf_free(&bz); buf_free(&hm); buf_free(&ln); buf_free(&cr);
    reverb(&out, 0.5f, 0.45f, 0.14f, 300.0f);
    return finish(&out, -3.0f, 1.4f, frames);
}

/* Laser / lightning (SFXID_LaserAttack, $C5: Tsu and the Flare skill): a
 * crack, three zaps falling from high, crackling electricity and a rumble */
static int16_t *render_zap(int v, int *frames) {
    Rng vr = seed(0x2A9u, v);
    float len = 0.8f;
    Buf out = buf_new(len);
    Buf ck = noise_sweep(len, 0.0f, 0.0004f, 0.012f, 5000.0f, 3000.0f, 0.02f, 0.8f, 0.0f, 0.0f, 0.8f, 0x2AAu, v);
    mix(&out, &ck, -2.0f);
    buf_free(&ck);
    for (int i = 0; i < 3; i++) {
        float t0 = 0.12f * (float)i + 0.02f * urnd(&vr);
        Buf zp = tone_glide(len, t0, 0.001f, 0.07f, 8000.0f / (1.0f + 0.3f * (float)i), 600.0f, 0.03f, 1.5f, 0.01f,
                            -0.4f + 0.4f * (float)i);
        mix(&out, &zp, -4.0f);
        buf_free(&zp);
    }
    Buf el = crackle(len, 0.0f, 0.45f, 320.0f, 4500.0f, 0.8f, 2.5f, 0x2ABu, v);
    Buf ru = noise_sweep(len, 0.02f, 0.03f, 0.3f, 260.0f, 120.0f, 0.4f, 0.7f, -0.2f, 0.2f, 0.0f, 0x2ACu, v);
    mix(&out, &el, -3.0f);
    mix(&out, &ru, -5.0f);
    buf_free(&el); buf_free(&ru);
    reverb(&out, 0.6f, 0.4f, 0.18f, 300.0f);
    return finish(&out, -3.0f, 1.5f, frames);
}

/* ------------------------------------------------------------------ the combination set
 * What the combination attacks ask for that the battle set hadn't made:
 * Shooting Star's rockets and explosions, Black Hole's sparks, Tri-Blaster's
 * Legeon, Conducted Thunder's and Circuit Break's Tandle, Grand Cross's
 * Efess, Circuit Break's Phonon (the test bot's macros, tools/battle_test.py). */

/* Burstroc's rocket (SFXID_Rifle, $D0): a crack of ignition, the rocket
 * screaming away (a falling whistle over a rush), a sputtering rumble */
static int16_t *render_rocket(int v, int *frames) {
    Rng vr = seed(0x20C7u, v);
    float len = 0.95f, k = 0.92f + 0.16f * urnd(&vr);
    Buf ig = noise_sweep(len, 0.0f, 0.0005f, 0.02f, 7000.0f, 3500.0f, 0.03f, 0.8f, 0.0f, 0.0f, 0.8f, 0x20C8u, v);
    Buf ru = noise_sweep(len, 0.0f, 0.01f, 0.09f, 6000.0f, 1500.0f, 0.18f, 1.2f, -0.3f, 0.6f, 0.3f, 0x20C9u, v);
    Buf wh = tone_glide(len, 0.005f, 0.004f, 0.12f, 3400.0f * k, 900.0f * k, 0.09f, 0.5f, 0.006f, 0.3f);
    Buf rb = noise_sweep(len, 0.05f, 0.04f, 0.35f, 320.0f, 140.0f, 0.6f, 0.7f, 0.2f, -0.2f, 0.0f, 0x20CAu, v);
    pulse(&rb, 0.05f, 16.0f, 0.7f);                           /* it sputters */
    Buf th = thump(len, 0.0f, 120.0f, 45.0f, 0.03f, 0.1f, 2.0f);
    Buf out = buf_new(len);
    mix(&out, &ig, -3.0f);
    mix(&out, &ru, 0.0f);
    mix(&out, &wh, -8.0f);
    mix(&out, &rb, -4.0f);
    mix(&out, &th, -6.0f);
    buf_free(&ig); buf_free(&ru); buf_free(&wh); buf_free(&rb); buf_free(&th);
    reverb(&out, 0.55f, 0.45f, 0.14f, 300.0f);
    return finish(&out, -3.0f, 1.5f, frames);
}

/* Megid's blast (SFXID_Megid, $C0; also Shooting Star's meteors): the
 * original's explosion -- bright, collapsing to a rumble that rolls on -- as
 * a deep boom, a burst darkening fast, debris crackling, and the rumble */
static int16_t *render_blast(int v, int *frames) {
    Rng vr = seed(0xB1A5u, v);
    float len = 1.6f, k = 0.92f + 0.16f * urnd(&vr);
    Buf bm = thump(len, 0.0f, 95.0f * k, 34.0f * k, 0.06f, 0.3f, 2.4f);
    Buf bu = noise_sweep(len, 0.0f, 0.003f, 0.16f, 8000.0f, 380.0f, 0.28f, 0.6f, -0.2f, 0.2f, 0.06f, 0xB1A6u, v);
    Buf de = crackle(len, 0.05f, 0.4f, 70.0f, 2200.0f, 1.0f, 2.0f, 0xB1A7u, v);
    Buf ru = noise_sweep(len, 0.1f, 0.1f, 0.5f, 240.0f, 110.0f, 1.0f, 0.7f, 0.3f, -0.3f, 0.0f, 0xB1A8u, v);
    pulse(&ru, 0.1f, 14.0f, 0.6f);
    Buf out = buf_new(len);
    mix(&out, &bm, 0.0f);
    mix(&out, &bu, -1.0f);
    mix(&out, &de, -9.0f);
    mix(&out, &ru, -5.0f);
    buf_free(&bm); buf_free(&bu); buf_free(&de); buf_free(&ru);
    reverb(&out, 0.75f, 0.4f, 0.22f, 200.0f);
    return finish(&out, -2.0f, 1.8f, frames);
}

/* Spark (SFXID_Spark, $D3; Black Hole asks seven times): a crisp electric
 * snap -- a crack, a tiny falling zap, a spit of crackle */
static int16_t *render_spark(int v, int *frames) {
    Rng vr = seed(0x5A4Cu, v);
    float len = 0.2f, k = 0.85f + 0.3f * urnd(&vr), p = 0.6f * rnd(&vr);
    Buf ck = noise_sweep(len, 0.0f, 0.0003f, 0.006f, 7500.0f * k, 5000.0f, 0.01f, 1.0f, p, p, 0.9f, 0x5A4Du, v);
    Buf zp = tone_glide(len, 0.0f, 0.0008f, 0.025f, 5200.0f * k, 900.0f, 0.015f, 1.4f, 0.01f, p);
    Buf cr = crackle(len, 0.002f, 0.06f, 400.0f, 5500.0f, 0.6f, 2.5f, 0x5A4Eu, v);
    Buf out = buf_new(len);
    mix(&out, &ck, 0.0f);
    mix(&out, &zp, -6.0f);
    mix(&out, &cr, -5.0f);
    buf_free(&ck); buf_free(&zp); buf_free(&cr);
    reverb(&out, 0.35f, 0.5f, 0.08f, 600.0f);
    return finish(&out, -4.0f, 1.2f, frames);
}

/* Legeon (SFXID_Legeon, $BF: the Legeon skill, Tri-Blaster): a burst of
 * holy light -- a hard bright strike, a glassy cluster ringing over air */
static int16_t *render_radiance(int v, int *frames) {
    float len = 0.75f, k = v ? 1.06f : 1.0f;
    Buf st = noise_sweep(len, 0.0f, 0.0006f, 0.03f, 9000.0f, 4500.0f, 0.05f, 0.9f, -0.2f, 0.2f, 0.8f, 0x1E60u, v);
    Buf gl = buf_new(len);
    bell(&gl, 0.0f, 1567.98f * k, 0.28f, 1.0f, -0.4f);
    bell(&gl, 0.004f, 2349.3f * k, 0.25f, 0.8f, 0.4f);
    bell(&gl, 0.008f, 3135.96f * k, 0.22f, 0.6f, 0.0f);
    normalize(&gl);
    Buf ai = noise_sweep(len, 0.0f, 0.02f, 0.2f, 7000.0f, 9000.0f, 0.3f, 0.7f, 0.3f, -0.3f, 0.6f, 0x1E61u, v);
    Buf out = buf_new(len);
    mix(&out, &st, -2.0f);
    mix(&out, &gl, 0.0f);
    mix(&out, &ai, -10.0f);
    buf_free(&st); buf_free(&gl); buf_free(&ai);
    reverb(&out, 0.65f, 0.4f, 0.24f, 400.0f);
    return finish(&out, -3.0f, 1.0f, frames);
}

/* Tandle (SFXID_Tandle, $CE: Tandle, Conducted Thunder asks nine times): an
 * electric current -- bright noise buzzing ~30 times a second, crackling,
 * a hum under it */
static int16_t *render_current(int v, int *frames) {
    float len = 1.0f, k = v ? 0.94f : 1.0f;
    Buf bz = noise_sweep(len, 0.0f, 0.01f, 0.32f, 5200.0f * k, 3800.0f * k, 0.8f, 1.6f, -0.3f, 0.3f, 0.5f, 0x7A9Du, v);
    pulse(&bz, 0.0f, 31.0f * k, 2.0f);
    Buf cr = crackle(len, 0.0f, 0.7f, 260.0f, 6000.0f, 0.6f, 2.5f, 0x7A9Eu, v);
    Buf hm = tone_glide(len, 0.0f, 0.02f, 0.3f, 120.0f * k, 116.0f * k, 1.0f, 1.0f, 0.01f, 0.0f);
    pulse(&hm, 0.0f, 31.0f * k, 1.0f);
    Buf out = buf_new(len);
    mix(&out, &bz, 0.0f);
    mix(&out, &cr, -5.0f);
    mix(&out, &hm, -9.0f);
    buf_free(&bz); buf_free(&cr); buf_free(&hm);
    reverb(&out, 0.45f, 0.45f, 0.12f, 300.0f);
    return finish(&out, -3.0f, 1.4f, frames);
}

/* Efess (SFXID_Efess, $C3: the Efess skill, Grand Cross): a flash of light,
 * then a rising swell of shimmer, as the original's rising sweep */
static int16_t *render_holy(int v, int *frames) {
    float len = 1.05f, k = v ? 1.06f : 1.0f;
    Buf fl = buf_new(len);
    bell(&fl, 0.0f, 2093.0f * k, 0.18f, 1.0f, -0.3f);
    bell(&fl, 0.003f, 2637.0f * k, 0.18f, 0.9f, 0.3f);
    bell(&fl, 0.006f, 3136.0f * k, 0.16f, 0.8f, 0.0f);
    normalize(&fl);
    Buf ck = noise_sweep(len, 0.0f, 0.0005f, 0.02f, 8000.0f, 5000.0f, 0.03f, 0.8f, 0.0f, 0.0f, 0.8f, 0xEF55u, v);
    Buf sw = noise_sweep(len, 0.15f, 0.45f, 0.15f, 900.0f, 8500.0f, 0.6f, 2.5f, -0.5f, 0.5f, 0.4f, 0xEF56u, v);
    Buf tn = tone_glide(len, 0.15f, 0.45f, 0.15f, 700.0f * k, 2800.0f * k, 0.35f, 0.3f, 0.005f, 0.0f);
    Buf out = buf_new(len);
    mix(&out, &fl, 0.0f);
    mix(&out, &ck, -6.0f);
    mix(&out, &sw, -4.0f);
    mix(&out, &tn, -12.0f);
    buf_free(&fl); buf_free(&ck); buf_free(&sw); buf_free(&tn);
    reverb(&out, 0.7f, 0.35f, 0.26f, 400.0f);
    return finish(&out, -3.0f, 0.0f, frames);
}

/* Phonon (SFXID_Phonon, $C1: the Phonon skill, Circuit Break; also an
 * enemy's): the original's two steady tones as a sonic beam -- a low and a
 * high tone wavering against each other, swelling in, a ring at the end */
static int16_t *render_sonic(int v, int *frames) {
    float len = 0.7f, k = v ? 1.05f : 1.0f;
    Buf lo = tone_glide(len, 0.0f, 0.03f, 0.25f, 420.0f * k, 400.0f * k, 0.2f, 0.6f, 0.012f, -0.25f);
    Buf hi = tone_glide(len, 0.0f, 0.03f, 0.22f, 2500.0f * k, 2450.0f * k, 0.2f, 0.3f, -0.008f, 0.25f);
    pulse(&lo, 0.0f, 11.0f, 0.4f);
    pulse(&hi, 0.0f, 11.0f, 0.4f);
    Buf rg = buf_new(len);
    bell(&rg, 0.32f, 1250.0f * k, 0.2f, 1.0f, 0.0f);
    normalize(&rg);
    Buf out = buf_new(len);
    mix(&out, &lo, 0.0f);
    mix(&out, &hi, -5.0f);
    mix(&out, &rg, -9.0f);
    buf_free(&lo); buf_free(&hi); buf_free(&rg);
    reverb(&out, 0.5f, 0.45f, 0.16f, 300.0f);
    return finish(&out, -4.0f, 0.6f, frames);
}

/* ------------------------------------------------------------------ the list */
static const SfxRecipe RECIPES[] = {
    {"slash", "blade hit: a short swing, a slice, a ringing edge and a thump", 4, 3, 0.000f},
    {"slasher", "thrown slasher: the throw, the spin flying off and coming back", 3, 1, 0.000f},
    {"cursor", "menu cursor: a soft tick", 3, 1, 0.000f},
    {"select", "menu choice: a two-note chime", 2, 2, 0.000f},
    {"claw", "claws: three rakes across, a tear and a thud", 3, 3, 0.045f},
    {"dissolve", "an enemy destroyed: a falling zap, a soft burst, glittering dust", 3, 3, 0.100f},
    {"grind", "a grinding rumble pulsing under a growl (an enemy's attack)", 2, 1, 0.300f},
    {"wind", "Zan: wind blades, two whistling gusts and the air after", 3, 2, 0.400f},
    {"blow", "an enemy's blow: a punch, a crack, a body", 3, 2, 0.050f},
    {"gnash", "an enemy's bites: six snapping clicks", 2, 1, 0.200f},
    {"rod", "a staff's knock: hard wood, a click, a thump", 3, 2, 0.050f},
    {"fire", "Foi: two puffs of flame, crackling", 2, 2, 0.100f},
    {"heal", "Res: glass bells rising over a shimmer", 2, 1, 0.300f},
    {"cast", "a technique cast: a chime, a rising rush, falling glitter", 2, 1, 0.200f},
    {"shot", "Wren's gun: a crack, a falling pew, a punch", 3, 2, 0.050f},
    {"glimmer", "Moonshade: rising glitter and a breathing fifth", 2, 1, 0.250f},
    {"swipe", "an enemy's swipe: falling air, a tone, a thud", 3, 2, 0.050f},
    {"healcast", "a healing cast: a soft rise into a major chord", 2, 1, 0.200f},
    {"miss", "a miss: a whiff of air", 2, 1, 0.050f},
    {"gravity", "Gra: one deep pulse of the warp (chained, they throb)", 3, 3, 0.000f},
    {"recover", "recovery: an arpeggio rising four times", 2, 1, 0.500f},
    {"breath", "fire breath: a flickering roar and a growl", 2, 1, 0.250f},
    {"buff", "a buff: a tone and its fifth climbing, a sparkle at the top", 2, 1, 0.250f},
    {"surge", "Brose: crackling energy pulsing, a bright line falling", 2, 1, 0.400f},
    {"zap", "Tsu: a crack, falling zaps, crackling electricity, a rumble", 2, 2, 0.150f},
    {"rocket", "Burstroc: ignition, a rocket screaming away, a sputtering rumble", 3, 2, 0.100f},
    {"blast", "Megid: a deep boom, a burst darkening, debris, a rolling rumble", 3, 2, 0.150f},
    {"spark", "Spark: a crisp electric snap", 3, 3, 0.030f},
    {"radiance", "Legeon: a strike of holy light, a glassy cluster ringing", 2, 1, 0.200f},
    {"current", "Tandle: an electric current buzzing, crackling, humming", 2, 1, 0.350f},
    {"holy", "Efess: a flash of light, a rising swell of shimmer", 2, 1, 0.300f},
    {"sonic", "Phonon: two tones wavering into a sonic beam", 2, 1, 0.250f},
};

typedef int16_t *(*RenderFn)(int, int *);
static const RenderFn RENDER[] = {render_slash, render_slasher, render_cursor, render_select,
                                  render_claw, render_dissolve, render_grind, render_wind, render_blow,
                                  render_gnash, render_rod, render_fire, render_heal, render_cast, render_shot,
                                  render_glimmer, render_swipe, render_healcast, render_miss, render_gravity,
                                  render_recover, render_breath, render_buff, render_surge, render_zap,
                                  render_rocket, render_blast, render_spark, render_radiance, render_current,
                                  render_holy, render_sonic};

int sfxsynth_count(void) { return (int)(sizeof RECIPES / sizeof RECIPES[0]); }

const SfxRecipe *sfxsynth_recipe(int i) { return i >= 0 && i < sfxsynth_count() ? &RECIPES[i] : NULL; }

const SfxRecipe *sfxsynth_find(const char *name) {
    for (int i = 0; i < sfxsynth_count(); i++)
        if (!strcmp(RECIPES[i].name, name)) return &RECIPES[i];
    return NULL;
}

int16_t *sfxsynth_render(const char *name, int variant, int *frames) {
    for (int i = 0; i < sfxsynth_count(); i++)
        if (!strcmp(RECIPES[i].name, name)) return RENDER[i](variant, frames);
    return NULL;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

int sfxsynth_write_wav(const char *path, const int16_t *pcm, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint8_t h[44];
    uint32_t data = (uint32_t)frames * 4;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, SOUND_RATE);
    put32(h + 28, SOUND_RATE * 4); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    int ok = fwrite(h, 1, 44, f) == 44;
    for (int i = 0; i < frames * 2 && ok; i++) {                 /* little-endian samples */
        uint8_t s[2];
        put16(s, (uint16_t)pcm[i]);
        ok = fwrite(s, 1, 2, f) == 2;
    }
    fclose(f);
    return ok;
}
