/*
 * Short clips of the picture: see clip.h.
 */
#include "clip.h"
#include "unzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ deflate (RFC 1951), fixed codes */
typedef struct {
    uint8_t *out;
    size_t len, cap;
    uint64_t bits;
    int nbits, full;
} Bits;

static void put_bits(Bits *b, uint32_t v, int n) {       /* least significant bit first */
    b->bits |= (uint64_t)v << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) {
        if (b->len >= b->cap) { b->full = 1; b->nbits = 0; b->bits = 0; return; }
        b->out[b->len++] = (uint8_t)b->bits;
        b->bits >>= 8;
        b->nbits -= 8;
    }
}

static uint16_t lit_code[288];                            /* the fixed codes, bit-reversed to go out first bit first */
static uint8_t lit_bits[288];
static uint8_t dist_code[30];
static uint8_t len_sym[259], len_xbits[259];              /* a match's length: its symbol (less 257), extra bits */
static uint16_t len_xval[259];
static int tables_ready;

static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                       8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static uint32_t reverse(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++, v >>= 1) r = r << 1 | (v & 1);
    return r;
}

static void make_tables(void) {
    for (int s = 0; s < 288; s++) {
        uint32_t code;
        int n;
        if (s < 144) { code = 0x30u + (uint32_t)s; n = 8; }
        else if (s < 256) { code = 0x190u + (uint32_t)(s - 144); n = 9; }
        else if (s < 280) { code = (uint32_t)(s - 256); n = 7; }
        else { code = 0xC0u + (uint32_t)(s - 280); n = 8; }
        lit_code[s] = (uint16_t)reverse(code, n);
        lit_bits[s] = (uint8_t)n;
    }
    for (int d = 0; d < 30; d++) dist_code[d] = (uint8_t)reverse((uint32_t)d, 5);
    for (int len = 3; len <= 258; len++) {
        int i = 28;
        while (LEN_BASE[i] > len) i--;
        if (len == 258) i = 28;
        else if (i == 28) i = 27;                         /* (258 has a code of its own) */
        len_sym[len] = (uint8_t)i;
        len_xbits[len] = LEN_EXTRA[i];
        len_xval[len] = (uint16_t)(len - LEN_BASE[i]);
    }
    tables_ready = 1;
}

static int dist_index(unsigned d) {
    int i = 29;
    while (DIST_BASE[i] > d) i--;
    return i;
}

#define WBITS 15
#define WSIZE (1u << WBITS)
#define HBITS 15
#define CHAIN 32

size_t clip_deflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    if (!tables_ready) make_tables();
    static int32_t head[1u << HBITS], prev[WSIZE];
    for (size_t i = 0; i < (1u << HBITS); i++) head[i] = -1;
    Bits b = {out, 0, cap, 0, 0, 0};
    put_bits(&b, 1, 1);                                   /* the last block */
    put_bits(&b, 1, 2);                                   /* fixed codes */
#define HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (p)[2]) & ((1u << HBITS) - 1))
#define INSERT(pos) do { uint32_t h_ = HASH(in + (pos)); prev[(pos) & (WSIZE - 1)] = head[h_]; head[h_] = (int32_t)(pos); } while (0)
    size_t i = 0;
    while (i < n && !b.full) {
        int best = 0;
        size_t best_d = 0;
        if (i + 3 <= n) {
            int32_t c = head[HASH(in + i)];
            size_t maxlen = n - i < 258 ? n - i : 258;
            for (int k = 0; k < CHAIN && c >= 0 && i - (size_t)c <= WSIZE; k++) {
                const uint8_t *a = in + c, *p = in + i;
                if (a[best] == p[best]) {
                    size_t l = 0;
                    while (l < maxlen && a[l] == p[l]) l++;
                    if ((int)l > best) {
                        best = (int)l;
                        best_d = i - (size_t)c;
                        if (l == maxlen) break;
                    }
                }
                int32_t nx = prev[c & (WSIZE - 1)];
                if (nx >= c) break;                       /* (a slot reused: older positions only) */
                c = nx;
            }
        }
        if (best >= 3) {
            int s = len_sym[best];
            put_bits(&b, lit_code[257 + s], lit_bits[257 + s]);
            if (len_xbits[best]) put_bits(&b, len_xval[best], len_xbits[best]);
            int d = dist_index((unsigned)best_d);
            put_bits(&b, dist_code[d], 5);
            if (DIST_EXTRA[d]) put_bits(&b, (uint32_t)(best_d - DIST_BASE[d]), DIST_EXTRA[d]);
            for (int k = 0; k < best; k++, i++)
                if (i + 3 <= n) INSERT(i);
        } else {
            put_bits(&b, lit_code[in[i]], lit_bits[in[i]]);
            if (i + 3 <= n) INSERT(i);
            i++;
        }
    }
    put_bits(&b, lit_code[256], lit_bits[256]);           /* the block's end */
    if (b.nbits) put_bits(&b, 0, 8 - b.nbits);
#undef HASH
#undef INSERT
    return b.full ? 0 : b.len;
}

/* ------------------------------------------------------------------ recording */
static struct {
    int on, w, h, fps, n;
    uint16_t *last, *cur;
    uint8_t *data;                                        /* the frames so far, as they go in the file */
    size_t len, cap;
} rec;

static void rec_drop(void) {
    free(rec.last);
    free(rec.cur);
    free(rec.data);
    memset(&rec, 0, sizeof rec);
}

int clip_rec_begin(int w, int h, int fps) {
    rec_drop();
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    rec.w = w;
    rec.h = h;
    rec.fps = fps;
    rec.last = calloc((size_t)w * (size_t)h, 2);
    rec.cur = malloc((size_t)w * (size_t)h * 2);
    if (!rec.last || !rec.cur) { rec_drop(); return 0; }
    rec.on = 1;
    return 1;
}

int clip_rec_frames(void) { return rec.n; }

void clip_rec_frame(const uint32_t *argb, int stride) {
    if (!rec.on || rec.n >= 65535) return;
    size_t px = (size_t)rec.w * (size_t)rec.h;
    for (int y = 0; y < rec.h; y++)
        for (int x = 0; x < rec.w; x++) {
            uint32_t c = argb[(size_t)y * (size_t)stride + (size_t)x];
            uint16_t v = (uint16_t)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F));
            size_t k = (size_t)y * (size_t)rec.w + (size_t)x;
            rec.cur[k] = v ^ rec.last[k];                 /* against the frame before (the first: against black) */
            rec.last[k] = v;
        }
    uint8_t *raw = (uint8_t *)rec.cur;                    /* big-endian, as it's read back */
    for (size_t k = 0; k < px; k++) {
        uint16_t v = rec.cur[k];
        raw[2 * k] = (uint8_t)(v >> 8);
        raw[2 * k + 1] = (uint8_t)v;
    }
    size_t need = px * 2 + px / 4 + 64;
    if (rec.len + need + 4 > rec.cap) {
        size_t cap = rec.cap ? rec.cap * 2 : need * 8;
        while (cap < rec.len + need + 4) cap *= 2;
        uint8_t *d = realloc(rec.data, cap);
        if (!d) { rec_drop(); return; }
        rec.data = d;
        rec.cap = cap;
    }
    size_t z = clip_deflate(raw, px * 2, rec.data + rec.len + 4, rec.cap - rec.len - 4);
    if (!z) { rec_drop(); return; }
    uint8_t *p = rec.data + rec.len;
    p[0] = (uint8_t)(z >> 24); p[1] = (uint8_t)(z >> 16); p[2] = (uint8_t)(z >> 8); p[3] = (uint8_t)z;
    rec.len += 4 + z;
    rec.n++;
}

int clip_rec_save(const char *path) {
    if (!rec.on || !rec.n) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint8_t hdr[17] = "PSIVCLIP1";
    int v[4] = {rec.w, rec.h, rec.n, rec.fps};
    for (int i = 0; i < 4; i++) { hdr[9 + 2 * i] = (uint8_t)(v[i] >> 8); hdr[10 + 2 * i] = (uint8_t)v[i]; }
    int ok = fwrite(hdr, 1, 17, f) == 17 && fwrite(rec.data, 1, rec.len, f) == rec.len;
    ok = fclose(f) == 0 && ok;
    rec_drop();
    return ok;
}

/* ------------------------------------------------------------------ playing */
void clip_free(Clip *c) {
    free(c->px);
    memset(c, 0, sizeof *c);
}

int clip_unpack(const uint8_t *d, size_t len, Clip *c) {
    memset(c, 0, sizeof *c);
    if (len < 17 || memcmp(d, "PSIVCLIP1", 9)) return 0;
    int w = d[9] << 8 | d[10], h = d[11] << 8 | d[12], n = d[13] << 8 | d[14], fps = d[15] << 8 | d[16];
    if (w <= 0 || h <= 0 || n <= 0 || w > 4096 || h > 4096) return 0;
    size_t px = (size_t)w * (size_t)h;
    uint16_t *all = malloc(px * 2 * (size_t)n);
    uint8_t *raw = malloc(px * 2);
    if (!all || !raw) { free(all); free(raw); return 0; }
    size_t o = 17;
    for (int k = 0; k < n; k++) {
        if (o + 4 > len) goto bad;
        size_t z = (size_t)d[o] << 24 | (size_t)d[o + 1] << 16 | (size_t)d[o + 2] << 8 | d[o + 3];
        o += 4;
        if (z > len - o || unzip_inflate(d + o, z, raw, px * 2) != px * 2) goto bad;
        o += z;
        uint16_t *cur = all + px * (size_t)k, *before = k ? cur - px : NULL;
        for (size_t i = 0; i < px; i++) {
            uint16_t v = (uint16_t)(raw[2 * i] << 8 | raw[2 * i + 1]);
            cur[i] = before ? (uint16_t)(v ^ before[i]) : v;
        }
    }
    free(raw);
    c->w = w; c->h = h; c->n = n; c->fps = fps > 0 ? fps : 15;
    c->px = all;
    return 1;
bad:
    free(all);
    free(raw);
    return 0;
}

int clip_load(const char *path, Clip *c) {
    memset(c, 0, sizeof *c);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    int ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    ok = ok && clip_unpack(d, (size_t)n, c);
    free(d);
    return ok;
}
