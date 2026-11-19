/*
 * Scale2x / Scale3x (scalex.h). The neighbours of a pixel E are named
 *
 *      A B C
 *      D E F
 *      G H I
 *
 * and past the picture's edge a neighbour is the edge pixel itself. The rows
 * go out in pieces on several cores (par.h): each piece writes only its own
 * output rows.
 *
 * The rule only asks whether two pixels are the same, so it is written once
 * (SCALEX_ROWS) and made twice: for pictures of colours (32 bits a pixel)
 * and for pictures of palette numbers (8 bits: a battle's layers, depth.c,
 * where 0 is "nothing here" and is simply one more value).
 */
#include "scalex.h"
#include "par.h"

#include <stdlib.h>
#include <string.h>

#define SCALEX_ROWS(T, SUFFIX)                                                                                  \
    typedef struct {                                                                                            \
        const T *src;                                                                                           \
        T *out;                                                                                                 \
        int src_stride, w, h, out_stride, scale;                                                                \
    } Job##SUFFIX;                                                                                              \
                                                                                                                \
    static void rows2##SUFFIX(void *ctx, int y0, int y1) {                                                      \
        const Job##SUFFIX *q = ctx;                                                                             \
        int w = q->w, h = q->h;                                                                                 \
        for (int y = y0; y < y1; y++) {                                                                         \
            const T *m = q->src + (size_t)y * (size_t)q->src_stride;                                            \
            const T *u = y > 0 ? m - q->src_stride : m, *d = y < h - 1 ? m + q->src_stride : m;                 \
            T *o0 = q->out + (size_t)y * 2u * (size_t)q->out_stride, *o1 = o0 + q->out_stride;                  \
            for (int x = 0; x < w; x++) {                                                                       \
                T E = m[x], B = u[x], H = d[x], D = m[x > 0 ? x - 1 : x], F = m[x < w - 1 ? x + 1 : x];         \
                if (B != H && D != F) {                                                                         \
                    o0[0] = D == B ? D : E;                                                                     \
                    o0[1] = B == F ? F : E;                                                                     \
                    o1[0] = D == H ? D : E;                                                                     \
                    o1[1] = H == F ? F : E;                                                                     \
                } else {                                                                                        \
                    o0[0] = o0[1] = o1[0] = o1[1] = E;                                                          \
                }                                                                                               \
                o0 += 2;                                                                                        \
                o1 += 2;                                                                                        \
            }                                                                                                   \
        }                                                                                                       \
    }                                                                                                           \
                                                                                                                \
    static void rows3##SUFFIX(void *ctx, int y0, int y1) {                                                      \
        const Job##SUFFIX *q = ctx;                                                                             \
        int w = q->w, h = q->h;                                                                                 \
        for (int y = y0; y < y1; y++) {                                                                         \
            const T *m = q->src + (size_t)y * (size_t)q->src_stride;                                            \
            const T *u = y > 0 ? m - q->src_stride : m, *d = y < h - 1 ? m + q->src_stride : m;                 \
            T *o0 = q->out + (size_t)y * 3u * (size_t)q->out_stride, *o1 = o0 + q->out_stride,                  \
              *o2 = o1 + q->out_stride;                                                                         \
            for (int x = 0; x < w; x++) {                                                                       \
                int xl = x > 0 ? x - 1 : x, xr = x < w - 1 ? x + 1 : x;                                         \
                T A = u[xl], B = u[x], C = u[xr], D = m[xl], E = m[x], F = m[xr], G = d[xl], H = d[x],          \
                  I = d[xr];                                                                                    \
                if (B != H && D != F) {                                                                         \
                    o0[0] = D == B ? D : E;                                                                     \
                    o0[1] = (D == B && E != C) || (B == F && E != A) ? B : E;                                   \
                    o0[2] = B == F ? F : E;                                                                     \
                    o1[0] = (D == B && E != G) || (D == H && E != A) ? D : E;                                   \
                    o1[1] = E;                                                                                  \
                    o1[2] = (B == F && E != I) || (H == F && E != C) ? F : E;                                   \
                    o2[0] = D == H ? D : E;                                                                     \
                    o2[1] = (D == H && E != I) || (H == F && E != G) ? H : E;                                   \
                    o2[2] = H == F ? F : E;                                                                     \
                } else {                                                                                        \
                    o0[0] = o0[1] = o0[2] = o1[0] = o1[1] = o1[2] = o2[0] = o2[1] = o2[2] = E;                  \
                }                                                                                               \
                o0 += 3;                                                                                        \
                o1 += 3;                                                                                        \
                o2 += 3;                                                                                        \
            }                                                                                                   \
        }                                                                                                       \
    }                                                                                                           \
                                                                                                                \
    /* plain blocks, any scale */                                                                               \
    static void rows_plain##SUFFIX(void *ctx, int y0, int y1) {                                                 \
        const Job##SUFFIX *q = ctx;                                                                             \
        int w = q->w, scale = q->scale;                                                                         \
        for (int y = y0; y < y1; y++) {                                                                         \
            const T *s = q->src + (size_t)y * (size_t)q->src_stride;                                            \
            T *d = q->out + (size_t)y * (size_t)scale * (size_t)q->out_stride;                                  \
            for (int x = 0; x < w; x++)                                                                         \
                for (int k = 0; k < scale; k++) d[x * scale + k] = s[x];                                        \
            for (int k = 1; k < scale; k++)                                                                     \
                memcpy(d + (size_t)k * (size_t)q->out_stride, d, (size_t)(w * scale) * sizeof(T));              \
        }                                                                                                       \
    }                                                                                                           \
                                                                                                                \
    static T *spare##SUFFIX;          /* four times: the picture at twice, between the two passes */            \
    static size_t spare_size##SUFFIX;                                                                           \
                                                                                                                \
    static void upscale##SUFFIX(const T *src, int src_stride, int w, int h, T *out, int out_stride,             \
                                int scale) {                                                                    \
        if (w < 1 || h < 1 || scale < 1) return;                                                                \
        Job##SUFFIX q = {src, out, src_stride, w, h, out_stride, scale};                                        \
        if (scale == 2) {                                                                                       \
            par_rows(rows2##SUFFIX, &q, h);                                                                     \
        } else if (scale == 3) {                                                                                \
            par_rows(rows3##SUFFIX, &q, h);                                                                     \
        } else if (scale == 4) {                                                                                \
            size_t need = (size_t)w * 2u * (size_t)h * 2u;                                                      \
            if (need > spare_size##SUFFIX) {                                                                    \
                T *p = realloc(spare##SUFFIX, need * sizeof *p);                                                \
                if (!p) { par_rows(rows_plain##SUFFIX, &q, h); return; }   /* (no room: plain blocks) */        \
                spare##SUFFIX = p;                                                                              \
                spare_size##SUFFIX = need;                                                                      \
            }                                                                                                   \
            Job##SUFFIX a = {src, spare##SUFFIX, src_stride, w, h, w * 2, 2};                                   \
            par_rows(rows2##SUFFIX, &a, h);                                                                     \
            Job##SUFFIX b = {spare##SUFFIX, out, w * 2, w * 2, h * 2, out_stride, 2};                           \
            par_rows(rows2##SUFFIX, &b, h * 2);                                                                 \
        } else {                                                                                                \
            par_rows(rows_plain##SUFFIX, &q, h);                                                                \
        }                                                                                                       \
    }

SCALEX_ROWS(uint32_t, 32)
SCALEX_ROWS(uint8_t, 8)

void scalex_upscale(const uint32_t *src, int src_stride, int w, int h, uint32_t *out, int out_stride, int scale) {
    upscale32(src, src_stride, w, h, out, out_stride, scale);
}

void scalex_upscale8(const uint8_t *src, int src_stride, int w, int h, uint8_t *out, int out_stride, int scale) {
    upscale8(src, src_stride, w, h, out, out_stride, scale);
}
