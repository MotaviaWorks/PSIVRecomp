/*
 * The ROM out of a .zip: see unzip.h.
 */
#include "unzip.h"

#include <stdio.h>
#include <string.h>

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

int unzip_is_zip(const uint8_t *data, size_t n) {
    return n >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
}

/* ------------------------------------------------------------------ inflate (RFC 1951) */
typedef struct {
    const uint8_t *in, *end;
    uint32_t bits;
    int nbits, err;
    uint8_t *out;
    size_t len, cap;
} Inflate;

typedef struct {
    uint16_t count[16];                  /* codes of each length */
    uint16_t symbol[288];                /* symbols in canonical order */
} Huffman;

static uint32_t take(Inflate *s, int n) {
    while (s->nbits < n) {
        if (s->in >= s->end) { s->err = 1; return 0; }
        s->bits |= (uint32_t)*s->in++ << s->nbits;
        s->nbits += 8;
    }
    uint32_t v = s->bits & ((1u << n) - 1);
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

/* a canonical code from code lengths; 0 if it isn't a valid one */
static int build(Huffman *h, const uint8_t *lengths, int n) {
    uint16_t offset[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;                        /* over-subscribed codes are corrupt */
    for (int len = 1; len < 16; len++) {
        left = left * 2 - h->count[len];
        if (left < 0) return 0;
    }
    offset[1] = 0;
    for (int len = 1; len < 15; len++) offset[len + 1] = (uint16_t)(offset[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offset[lengths[i]]++] = (uint16_t)i;
    return 1;
}

/* one symbol, a bit at a time (codes are stored most significant bit first) */
static int decode(Inflate *s, const Huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)take(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                       8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Inflate *s, const Huffman *lit, const Huffman *dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (s->err) return 0;
        if (sym < 256) {
            if (s->len >= s->cap) return 0;
            s->out[s->len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) return 0;
            size_t n = LEN_BASE[sym] + take(s, LEN_EXTRA[sym]);
            int d = decode(s, dist);
            if (s->err || d < 0 || d >= 30) return 0;
            size_t back = DIST_BASE[d] + take(s, DIST_EXTRA[d]);
            if (s->err || back > s->len || n > s->cap - s->len) return 0;
            for (size_t i = 0; i < n; i++, s->len++) s->out[s->len] = s->out[s->len - back];
        }
    }
}

static int inflate_raw(Inflate *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    Huffman lit, dist;
    uint8_t lengths[320];
    int last;
    do {
        last = (int)take(s, 1);
        int type = (int)take(s, 2);
        if (s->err) return 0;
        if (type == 0) {                                 /* stored */
            s->bits = 0;
            s->nbits = 0;
            if (s->end - s->in < 4) return 0;
            size_t n = le16(s->in);
            if ((le16(s->in + 2) ^ 0xFFFF) != n) return 0;
            s->in += 4;
            if ((size_t)(s->end - s->in) < n || n > s->cap - s->len) return 0;
            memcpy(s->out + s->len, s->in, n);
            s->in += n;
            s->len += n;
        } else if (type == 1) {                          /* fixed codes */
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            build(&lit, lengths, 288);
            for (i = 0; i < 30; i++) lengths[i] = 5;
            build(&dist, lengths, 30);
            if (!codes(s, &lit, &dist)) return 0;
        } else if (type == 2) {                          /* codes sent with the block */
            int nlen = (int)take(s, 5) + 257, ndist = (int)take(s, 5) + 1, ncode = (int)take(s, 4) + 4;
            if (s->err || nlen > 286 || ndist > 30) return 0;
            memset(lengths, 0, 19);
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)take(s, 3);
            Huffman cl;
            if (s->err || !build(&cl, lengths, 19)) return 0;
            for (int i = 0; i < nlen + ndist;) {
                int sym = decode(s, &cl);
                if (s->err || sym < 0) return 0;
                if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
                int rep, val = 0;
                if (sym == 16) {
                    if (i == 0) return 0;
                    val = lengths[i - 1];
                    rep = 3 + (int)take(s, 2);
                } else if (sym == 17) rep = 3 + (int)take(s, 3);
                else rep = 11 + (int)take(s, 7);
                if (s->err || i + rep > nlen + ndist) return 0;
                while (rep--) lengths[i++] = (uint8_t)val;
            }
            if (!lengths[256]) return 0;                 /* no end-of-block code */
            if (!build(&lit, lengths, nlen) || !build(&dist, lengths + nlen, ndist)) return 0;
            if (!codes(s, &lit, &dist)) return 0;
        } else {
            return 0;
        }
    } while (!last);
    return 1;
}

/* ------------------------------------------------------------------ the archive */
static int rom_like(const uint8_t *name, size_t n) {
    static const char *ext[] = {".md", ".bin", ".gen", ".smd", ".68k"};
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        size_t e = strlen(ext[i]);
        if (n <= e) continue;
        size_t k = 0;
        while (k < e && (name[n - e + k] | 0x20) == (uint8_t)(ext[i][k] | 0x20)) k++;
        if (k == e) return 1;
    }
    return 0;
}

size_t unzip_rom(const uint8_t *zip, size_t n, uint8_t *out, size_t cap, char *err, size_t errn) {
    /* the end-of-directory record: the last 22 bytes plus up to 64 KB of comment */
    size_t eocd = 0;
    int found = 0;
    for (size_t i = n >= 22 ? n - 22 : 0; n >= 22; i--) {
        if (le32(zip + i) == 0x06054B50) { eocd = i; found = 1; break; }
        if (i == 0 || n - i > 22 + 65535) break;
    }
    if (!found) { snprintf(err, errn, "the .zip is damaged or incomplete"); return 0; }
    size_t entries = le16(zip + eocd + 10), dir = le32(zip + eocd + 16);
    for (size_t e = 0, p = dir; e < entries; e++) {
        if (p + 46 > n || le32(zip + p) != 0x02014B50) break;
        uint32_t method = le16(zip + p + 10), flags = le16(zip + p + 8);
        size_t csize = le32(zip + p + 20), usize = le32(zip + p + 24);
        size_t nlen = le16(zip + p + 28), xlen = le16(zip + p + 30), clen = le16(zip + p + 32);
        size_t local = le32(zip + p + 42);
        const uint8_t *name = zip + p + 46;
        p += 46 + nlen + xlen + clen;
        if (p > n || !rom_like(name, nlen)) continue;
        if (flags & 1) { snprintf(err, errn, "the .zip is encrypted"); return 0; }
        if (usize > cap) { snprintf(err, errn, "the .zip holds a file too big to be this game"); return 0; }
        if (local + 30 > n || le32(zip + local) != 0x04034B50) break;
        size_t data = local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
        if (data > n || csize > n - data) break;
        if (method == 0) {
            if (csize != usize) break;
            memcpy(out, zip + data, usize);
            return usize;
        }
        if (method != 8) {
            snprintf(err, errn, "the .zip uses a compression this port can't read (method %u); "
                     "re-zip it, or extract the ROM first", (unsigned)method);
            return 0;
        }
        Inflate s = {zip + data, zip + data + csize, 0, 0, 0, out, 0, usize};
        if (!inflate_raw(&s) || s.len != usize) { snprintf(err, errn, "the .zip is damaged"); return 0; }
        return usize;
    }
    snprintf(err, errn, "the .zip holds no Mega Drive ROM (.md, .bin, .gen or .smd)");
    return 0;
}
