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

/* ------------------------------------------------------------------ the list */
static const SfxRecipe RECIPES[] = {
    {"slash", "blade hit: a short swing, a slice, a ringing edge and a thump", 4, 3},
    {"slasher", "thrown slasher: the throw, the spin flying off and coming back", 3, 1},
    {"cursor", "menu cursor: a soft tick", 3, 1},
    {"select", "menu choice: a two-note chime", 2, 2},
};

typedef int16_t *(*RenderFn)(int, int *);
static const RenderFn RENDER[] = {render_slash, render_slasher, render_cursor, render_select};

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
