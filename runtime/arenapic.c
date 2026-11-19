/*
 * The Boss Arena's pictures: see arenapic.h.
 */
#include "arenapic.h"
#include "frame.h"
#include "vdp.h"

#include <stdlib.h>
#include <string.h>

#define REC       360             /* frames recorded: six seconds of idling */
#define MAX_UNIQ  160             /* distinct pictures kept while recording */
#define NO_LOOP   120             /* nothing repeats (a raster wave): the last two seconds, as they come */
#define PIC_H     192             /* the battle's picture down to its floor; the windows below */

static struct {
    int on, n, w, h, x0, nuniq;
    uint64_t hash[MAX_UNIQ];
    uint8_t *px[MAX_UNIQ];
    uint16_t pal[MAX_UNIQ][64];
    int seq[REC];
} rec;

static void rec_clear(void) {
    for (int i = 0; i < rec.nuniq; i++) free(rec.px[i]);
    memset(&rec, 0, sizeof rec);
}

void arenapic_capture_begin(void) {
    rec_clear();
    rec.on = 1;
    rec.w = vdp_width();                         /* the battle's own picture: widescreen's margins are
                                                  * drawn later (side pictures), not in the layers */
    rec.x0 = vdp_view_x0 < 0 ? -vdp_view_x0 : 0;
    if (rec.x0 + rec.w > VDP_MAX_W) rec.x0 = 0;
    rec.h = vdp_height() < PIC_H ? vdp_height() : PIC_H;
}

/* the last frame drawn, as the arena shows a boss: backdrop, plane B, plane A's low-priority cells
 * (and pictures the port put back behind the game's windows), sprites */
static void compose(uint8_t *out) {
    for (int y = 0; y < rec.h; y++) {
        const VdpLine *l = &frame_lines[y];
        for (int x = 0; x < rec.w; x++) {
            int lx = rec.x0 + x;
            uint8_t c = l->backdrop, b = l->plane_b[lx], a = l->plane_a[lx], s = l->sprite[lx];
            if (b) c = b;
            if (a && !l->win_in[lx] && (!(a & 0x80) || l->plane_a_under[lx])) c = a;
            if (s) c = s;
            out[y * rec.w + x] = c & 0x3F;
        }
    }
}

int arenapic_capture_frame(void) {
    if (!rec.on) return 1;
    size_t sz = (size_t)rec.w * (size_t)rec.h;
    uint8_t *img = malloc(sz);
    if (!img) { rec.on = 0; return 1; }
    compose(img);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sz; i++) h = (h ^ img[i]) * 1099511628211ull;
    for (int i = 0; i < 64; i++) h = (h ^ vdp.cram[i]) * 1099511628211ull;
    int id = -1;
    for (int i = 0; i < rec.nuniq && id < 0; i++)
        if (rec.hash[i] == h) id = i;
    if (id < 0 && rec.nuniq < MAX_UNIQ) {
        id = rec.nuniq++;
        rec.hash[id] = h;
        rec.px[id] = img;
        memcpy(rec.pal[id], vdp.cram, sizeof rec.pal[id]);
        img = NULL;
    }
    free(img);
    if (id < 0) id = rec.n ? rec.seq[rec.n - 1] : 0;   /* (full: the last again) */
    rec.seq[rec.n++] = id;
    if (rec.n >= REC) { rec.on = 0; return 1; }
    return 0;
}

/* the earliest start, then the shortest period, from which the rest repeats at least twice over */
static void find_loop(int *start, int *period) {
    int n = rec.n, bs = -1, bp = 0;
    for (int p = 1; p <= n / 2; p++) {
        int bad = 0;
        for (int i = 0; i + p < n; i++)
            if (rec.seq[i] != rec.seq[i + p]) bad = i + 1;
        if (n - bad >= 2 * p && (bs < 0 || bad + p < bs + bp)) { bs = bad; bp = p; }
    }
    if (bs < 0) { bs = n > NO_LOOP ? n - NO_LOOP : 0; bp = n - bs; }
    *start = bs;
    *period = bp;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* a picture against the one before it (XOR), as runs: c < $80, c + 1 bytes as they are;
 * c >= $80, c - $7F zeros */
static size_t pack_frame(const uint8_t *cur, const uint8_t *prev, size_t n, uint8_t *out) {
    size_t o = 0, i = 0;
    while (i < n) {
        uint8_t d = cur[i] ^ (prev ? prev[i] : 0);
        if (!d) {
            size_t k = 0;
            while (i + k < n && k < 128 && !(cur[i + k] ^ (prev ? prev[i + k] : 0))) k++;
            out[o++] = (uint8_t)(0x7F + k);
            i += k;
        } else {
            size_t k = 0, at = o++;
            while (i + k < n && k < 128) {
                uint8_t e = cur[i + k] ^ (prev ? prev[i + k] : 0);
                if (!e && i + k + 1 < n && !(cur[i + k + 1] ^ (prev ? prev[i + k + 1] : 0))) break;   /* zeros ahead */
                out[o++] = e;
                k++;
            }
            out[at] = (uint8_t)(k - 1);
            i += k;
        }
    }
    return o;
}

#define MAGIC "PSIVARENA1"

int arenapic_capture_pack(uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    if (!rec.n || !rec.nuniq) return 0;
    int start, period;
    find_loop(&start, &period);
    /* the loop's runs, and the pictures they use, renumbered in order of use */
    int map[MAX_UNIQ], used[MAX_UNIQ], nused = 0, nruns = 0;
    uint8_t rf[REC], rl[REC];
    for (int i = 0; i < MAX_UNIQ; i++) map[i] = -1;
    for (int i = start; i < start + period; i++) {
        int u = rec.seq[i];
        if (map[u] < 0) { map[u] = nused; used[nused++] = u; }
        if (nruns && rf[nruns - 1] == map[u] && rl[nruns - 1] < 255) rl[nruns - 1]++;
        else { rf[nruns] = (uint8_t)map[u]; rl[nruns] = 1; nruns++; }
    }
    size_t sz = (size_t)rec.w * (size_t)rec.h;
    size_t cap = 64 + (size_t)nruns * 2 + (size_t)nused * (128 + 4 + sz + sz / 128 + 8);
    uint8_t *b = malloc(cap);
    if (!b) return 0;
    size_t o = 0;
    memcpy(b, MAGIC, 10); o = 10;
    put32(b + o, (uint32_t)rec.w); o += 4;
    put32(b + o, (uint32_t)rec.h); o += 4;
    put32(b + o, (uint32_t)nused); o += 4;
    put32(b + o, (uint32_t)nruns); o += 4;
    for (int r = 0; r < nruns; r++) { b[o++] = rf[r]; b[o++] = rl[r]; }
    for (int k = 0; k < nused; k++) {
        for (int c = 0; c < 64; c++) { b[o++] = (uint8_t)(rec.pal[used[k]][c] >> 8); b[o++] = (uint8_t)rec.pal[used[k]][c]; }
        size_t at = o;
        o += 4;
        size_t n = pack_frame(rec.px[used[k]], k ? rec.px[used[k - 1]] : NULL, sz, b + o);
        put32(b + at, (uint32_t)n);
        o += n;
    }
    *out = b;
    *len = o;
    rec_clear();
    return 1;
}

void arenapic_free(ArenaPic *pic) {
    free(pic->px);
    free(pic->pal);
    free(pic->run_frame);
    free(pic->run_len);
    memset(pic, 0, sizeof *pic);
}

int arenapic_unpack(const uint8_t *d, size_t len, ArenaPic *pic) {
    memset(pic, 0, sizeof *pic);
    if (len < 26 || memcmp(d, MAGIC, 10)) return 0;
    size_t o = 10;
    int w = (int)get32(d + o), h = (int)get32(d + o + 4), nf = (int)get32(d + o + 8), nr = (int)get32(d + o + 12);
    o += 16;
    if (w <= 0 || w > VDP_MAX_W || h <= 0 || h > VDP_MAX_H || nf <= 0 || nf > MAX_UNIQ || nr <= 0 || nr > REC) return 0;
    if (o + (size_t)nr * 2 > len) return 0;
    size_t sz = (size_t)w * (size_t)h;
    pic->w = w; pic->h = h; pic->nframes = nf; pic->nruns = nr;
    pic->px = malloc(sz * (size_t)nf);
    pic->pal = malloc(sizeof(uint16_t) * 64 * (size_t)nf);
    pic->run_frame = malloc((size_t)nr);
    pic->run_len = malloc((size_t)nr);
    if (!pic->px || !pic->pal || !pic->run_frame || !pic->run_len) { arenapic_free(pic); return 0; }
    for (int r = 0; r < nr; r++) {
        pic->run_frame[r] = d[o++];
        pic->run_len[r] = d[o++];
        if (pic->run_frame[r] >= nf || !pic->run_len[r]) { arenapic_free(pic); return 0; }
        pic->total += pic->run_len[r];
    }
    for (int k = 0; k < nf; k++) {
        if (o + 132 > len) { arenapic_free(pic); return 0; }
        for (int c = 0; c < 64; c++, o += 2) pic->pal[k * 64 + c] = (uint16_t)(d[o] << 8 | d[o + 1]);
        size_t n = get32(d + o), end;
        o += 4;
        end = o + n;
        if (end > len) { arenapic_free(pic); return 0; }
        uint8_t *cur = pic->px + sz * (size_t)k, *prev = k ? cur - sz : NULL;
        size_t i = 0;
        while (o < end && i < sz) {
            uint8_t c = d[o++];
            if (c >= 0x80) {
                size_t z = (size_t)c - 0x7F;
                for (size_t j = 0; j < z && i < sz; j++, i++) cur[i] = prev ? prev[i] : 0;
            } else {
                size_t m = (size_t)c + 1;
                for (size_t j = 0; j < m && i < sz && o < end; j++, i++) cur[i] = (uint8_t)((prev ? prev[i] : 0) ^ d[o++]);
            }
        }
        if (i != sz) { arenapic_free(pic); return 0; }
        o = end;
    }
    return 1;
}

int arenapic_frame_at(const ArenaPic *pic, uint64_t t) {
    if (!pic->nruns || pic->total <= 0) return 0;
    int k = (int)(t % (uint64_t)pic->total);
    for (int r = 0; r < pic->nruns; r++) {
        if (k < pic->run_len[r]) return pic->run_frame[r];
        k -= pic->run_len[r];
    }
    return pic->run_frame[0];
}
