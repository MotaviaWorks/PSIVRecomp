/*
 * Smoothing for the port's own picture (smooth.h): a box blur one game pixel
 * wide and high, in two passes (along the rows into a spare picture, then
 * down the columns back), each in pieces on several cores (par.h).
 *
 * The window is centred on the pixel: `scale` taps for an odd scale; for an
 * even one scale + 1 taps with the two end ones at half weight, so the
 * picture does not shift by half a pixel. Weights are whole numbers (1 or 2)
 * and their sum is divided out by a multiplication.
 *
 * Each row is summed a tap at a time into two plain arrays (red and blue
 * together in one word, sixteen bits each; green in the other), in loops
 * simple enough for the compiler to do several pixels at a time.
 */
#include "smooth.h"
#include "par.h"

#include <stdlib.h>
#include <string.h>

#define MAX_TAPS 9                    /* scale up to 8 */
#define MAX_W 4096                    /* (the widest picture is 424 x 4) */

typedef struct {
    const uint32_t *src;
    uint32_t *dst;
    int src_stride, dst_stride, w, h;
    int taps, wt[MAX_TAPS], half;     /* the window: taps weights, reaching `half` pixels either way */
    uint32_t round, recip;            /* (sum + round) * recip >> 16 = sum / total */
} Pass;

static uint32_t *spare;               /* the picture between the two passes */
static size_t spare_size;

static inline void clear(uint32_t *rb, uint32_t *g, int w) {
    memset(rb, 0, (size_t)w * sizeof *rb);
    memset(g, 0, (size_t)w * sizeof *g);
}

/* one tap: w pixels from p, at weight 1 (sh 0) or 2 (sh 1), onto the sums */
static inline void add_tap(uint32_t *rb, uint32_t *g, const uint32_t *p, int w, int sh) {
    for (int x = 0; x < w; x++) {
        uint32_t c = p[x];
        rb[x] += (c & 0x00FF00FFu) << sh;
        g[x] += (c >> 8 & 0xFFu) << sh;
    }
}

/* the sums divided out, as pixels */
static inline void put(const Pass *q, const uint32_t *rb, const uint32_t *g, uint32_t *d, int w) {
    uint32_t round = q->round, recip = q->recip;
    for (int x = 0; x < w; x++) {
        uint32_t r = ((rb[x] >> 16) + round) * recip >> 16;
        uint32_t b = ((rb[x] & 0xFFFFu) + round) * recip >> 16;
        uint32_t gg = (g[x] + round) * recip >> 16;
        d[x] = 0xFF000000u | r << 16 | gg << 8 | b;
    }
}

/* along each row: src -> dst */
static void across_rows(void *ctx, int y0, int y1) {
    const Pass *q = ctx;
    int w = q->w, half = q->half;
    uint32_t pad[MAX_W + 2 * MAX_TAPS], rb[MAX_W], g[MAX_W];
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = q->src + (size_t)y * (size_t)q->src_stride;
        for (int k = 0; k < half; k++) {                 /* the row with its end pixels again, either side */
            pad[k] = s[0];
            pad[half + w + k] = s[w - 1];
        }
        memcpy(pad + half, s, (size_t)w * sizeof *s);
        clear(rb, g, w);
        for (int k = 0; k < q->taps; k++) add_tap(rb, g, pad + k, w, q->wt[k] - 1);
        put(q, rb, g, q->dst + (size_t)y * (size_t)q->dst_stride, w);
    }
}

/* down each column: src -> dst */
static void down_rows(void *ctx, int y0, int y1) {
    const Pass *q = ctx;
    int w = q->w, h = q->h, half = q->half;
    uint32_t rb[MAX_W], g[MAX_W];
    for (int y = y0; y < y1; y++) {
        clear(rb, g, w);
        for (int k = 0; k < q->taps; k++) {
            int v = y - half + k;
            add_tap(rb, g, q->src + (size_t)(v < 0 ? 0 : v >= h ? h - 1 : v) * (size_t)q->src_stride, w, q->wt[k] - 1);
        }
        put(q, rb, g, q->dst + (size_t)y * (size_t)q->dst_stride, w);
    }
}

void smooth_soften(uint32_t *buf, int stride, int w, int h, int scale) {
    if (scale < 2 || w < 1 || h < 1 || w > MAX_W) return;
    if (scale > MAX_TAPS - 1) scale = MAX_TAPS - 1;
    size_t need = (size_t)w * (size_t)h;
    if (need > spare_size) {
        uint32_t *p = realloc(spare, need * sizeof *p);
        if (!p) return;                                  /* (no room: the picture stays as it is) */
        spare = p;
        spare_size = need;
    }
    Pass q = {0};
    q.w = w;
    q.h = h;
    uint32_t total;
    if (scale & 1) {
        q.taps = scale;
        for (int k = 0; k < q.taps; k++) q.wt[k] = 1;
        total = (uint32_t)scale;
    } else {
        q.taps = scale + 1;
        for (int k = 0; k < q.taps; k++) q.wt[k] = k == 0 || k == scale ? 1 : 2;
        total = 2u * (uint32_t)scale;
    }
    q.half = q.taps / 2;
    q.round = total / 2;
    q.recip = (65536u + total - 1) / total;              /* rounded up: a full sum comes out 255, not 254 */
    q.src = buf; q.src_stride = stride;
    q.dst = spare; q.dst_stride = w;
    par_rows(across_rows, &q, h);
    q.src = spare; q.src_stride = w;
    q.dst = buf; q.dst_stride = stride;
    par_rows(down_rows, &q, h);
}
