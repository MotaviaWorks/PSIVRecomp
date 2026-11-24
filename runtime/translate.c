/*
 * A translation's dialogue: see translate.h.
 */
#include "translate.h"
#include "m68k.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ the pack */
typedef struct { uint32_t id; char *text; } Entry;           /* id: bank << 12 | line, or the ROM address with the top bit */
#define ROM_TEXT 0x80000000u
static Entry *entries;
static int nentries;
static char language[64];

static int by_id(const void *a, const void *b) {
    uint32_t x = ((const Entry *)a)->id, y = ((const Entry *)b)->id;
    return x < y ? -1 : x > y;
}

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* [D07.01A] or [T2AC0DE], alone on its line: its id, or 0 */
static uint32_t heading(const char *s, size_t n) {
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    if (n == 9 && s[0] == '[' && s[1] == 'D' && s[4] == '.' && s[8] == ']') {
        int a = hex(s[2]), b = hex(s[3]), x = hex(s[5]), y = hex(s[6]), z = hex(s[7]);
        if (a < 0 || a > 9 || b < 0 || b > 9 || x < 0 || y < 0 || z < 0) return 0;
        return (uint32_t)(a * 10 + b) << 12 | (uint32_t)(x << 8 | y << 4 | z);
    }
    if (n == 9 && s[0] == '[' && s[1] == 'T' && s[8] == ']') {
        uint32_t v = 0;
        for (int i = 2; i < 8; i++) {
            int d = hex(s[i]);
            if (d < 0) return 0;
            v = v << 4 | (uint32_t)d;
        }
        return v | ROM_TEXT;
    }
    return 0;
}

/* each line of the pack's text in turn */
static const char *line_of(const char **p, const char *end, size_t *n) {
    if (*p >= end) return NULL;
    const char *s = *p, *e = memchr(s, '\n', (size_t)(end - s));
    *n = (size_t)((e ? e : end) - s);
    *p = e ? e + 1 : end;
    return s;
}

static size_t bom(const char *t, size_t n) { return n >= 3 && !memcmp(t, "\xEF\xBB\xBF", 3) ? 3 : 0; }

int translate_is_pack(const char *text, size_t len, char *lang, size_t ln, int *lines) {
    static const char MARK[] = "# PSIV Recomp translation pack";
    const char *p = text + bom(text, len), *end = text + len, *s;
    size_t n;
    int count = 0, marked = 0;
    if (lang && ln) lang[0] = 0;
    while ((s = line_of(&p, end, &n))) {
        if (heading(s, n)) { count++; continue; }
        if (count) continue;
        while (n && s[n - 1] == '\r') n--;
        if (n >= sizeof MARK - 1 && !memcmp(s, MARK, sizeof MARK - 1)) marked = 1;
        if (n > 8 && !memcmp(s, "language", 8) && lang && ln) {
            const char *v = memchr(s, '=', n);
            if (!v) continue;
            v++;
            while (v < s + n && (*v == ' ' || *v == '\t')) v++;
            size_t k = (size_t)(s + n - v);
            if (k >= ln) {
                k = ln - 1;
                while (k && ((unsigned char)v[k] & 0xC0) == 0x80) k--;
            }
            memcpy(lang, v, k);
            lang[k] = 0;
        }
    }
    if (lines) *lines = count;
    return marked || count > 0;
}

static void forget(void);
static char tried[256];

int translate_load(const char *pack, size_t len) {
    for (int i = 0; i < nentries; i++) free(entries[i].text);
    free(entries);
    entries = NULL;
    nentries = 0;
    language[0] = 0;
    forget();
    tried[0] = 0;
    if (!pack) return 0;
    int count = 0;
    translate_is_pack(pack, len, language, sizeof language, &count);
    if (!count || !(entries = calloc((size_t)count, sizeof *entries))) return 0;
    const char *p = pack + bom(pack, len), *end = pack + len, *from = NULL;
    uint32_t id = 0;
    for (;;) {
        size_t n = 0;
        const char *s = line_of(&p, end, &n);
        uint32_t h = s ? heading(s, n) : 0;
        if (id && (h || !s)) {                               /* the one before is whole: its words, no \r, no blank lines about them */
            const char *e = s ? s : end;
            char *t = malloc((size_t)(e - from) + 1);
            if (t) {
                size_t k = 0;
                for (const char *c = from; c < e; c++)
                    if (*c != '\r') t[k++] = *c;
                while (k && (t[k - 1] == '\n' || t[k - 1] == ' ')) k--;
                t[k] = 0;
                size_t lead = strspn(t, "\n");
                memmove(t, t + lead, k - lead + 1);
                if (t[0] && nentries < count) {
                    entries[nentries].id = id;
                    entries[nentries++].text = t;
                } else free(t);
            }
            id = 0;
        }
        if (!s) break;
        if (h) { id = h; from = p; }
    }
    qsort(entries, (size_t)nentries, sizeof *entries, by_id);
    return nentries;
}

int translate_lines(void) { return nentries; }
const char *translate_language(void) { return language; }

static const char *entry(uint32_t id) {
    int lo = 0, hi = nentries - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (entries[mid].id == id) return entries[mid].text;
        if (entries[mid].id < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ the game's text */
#define BANKS 43
#define TREES 0x3000                                         /* Dialogue_Trees */
#define TREES_MAX 0x4000
static const uint32_t BANK_AT[BANKS] = {                     /* DialogueTree1 .. 43 (USA), Kosinski-packed */
    0x1DF600, 0x1E0190, 0x1E0BA0, 0x1E1980, 0x1E2680, 0x1E32A0, 0x1E3F40, 0x1E4E20, 0x1E5740, 0x1E6380, 0x1E6FE0,
    0x1E7D50, 0x1E89E0, 0x1E99F0, 0x1EA8C0, 0x1EB1A0, 0x1EBA90, 0x1EC870, 0x1ED360, 0x1EDCA0, 0x1EE9B0, 0x1EF130,
    0x1EFA80, 0x1F0620, 0x1F10C0, 0x1F1AD0, 0x1F2400, 0x1F2DF0, 0x1F3A90, 0x1F4570, 0x1F4F70, 0x1F5A70, 0x1F6320,
    0x1F7190, 0x1F81B0, 0x1F9020, 0x1F9580, 0x1FA480, 0x1FAAC0, 0x1FB610, 0x1FC3B0, 0x1FC920, 0x1FD7A0};

static uint8_t rom(uint32_t a) { return a < m68k_rom_size ? m68k_rom[a] : 0xFF; }

typedef struct { uint32_t pos; unsigned desc; int bits; } Kos;
static int kos_bit(Kos *q) {
    int b = (int)(q->desc & 1);
    q->desc >>= 1;
    if (!--q->bits) {
        q->desc = rom(q->pos) | (unsigned)rom(q->pos + 1) << 8;
        q->bits = 16;
        q->pos += 2;
    }
    return b;
}

static int kosinski(uint32_t at, uint8_t *out, int max) {
    Kos q = {at + 2, rom(at) | (unsigned)rom(at + 1) << 8, 16};
    int n = 0;
    while (q.pos < m68k_rom_size) {
        int off, count;
        if (kos_bit(&q)) {
            if (n >= max) return n;
            out[n++] = rom(q.pos++);
            continue;
        }
        if (kos_bit(&q)) {
            int lo = rom(q.pos), hi = rom(q.pos + 1);
            q.pos += 2;
            off = ((hi & 0xF8) << 5 | lo) - 0x2000;
            count = hi & 7;
            if (count) count += 2;
            else {
                count = rom(q.pos++);
                if (!count) return n;
                if (count == 1) continue;
                count++;
            }
        } else {
            count = kos_bit(&q) << 1;
            count = (count | kos_bit(&q)) + 2;
            off = (int)rom(q.pos++) - 0x100;
        }
        if (n + off < 0) return 0;
        for (; count > 0; count--, n++) {
            if (n >= max) return n;
            out[n] = out[n + off];
        }
    }
    return 0;
}

/* a letter or a digit (lower case) of the dialogue's characters; 0 for the others */
static char letter(int code) {
    code &= 0x7F;
    if (code >= 1 && code <= 26) return (char)('a' + code - 1);
    if (code >= 27 && code <= 52) return (char)('a' + code - 27);
    if (code >= 64 && code <= 73) return (char)('0' + code - 64);
    return 0;
}

/* One filling of the box: from the byte it starts at to the one that ends it. `part`: how many marks (a page's
 * end, a cut, a choice ...) of its line stand before it, which is the piece of the translation it shows; `sub`
 * of `subs`: a piece the game itself breaks over more than one box (at its 64th letter) */
typedef struct { int start, end, line, part, sub, subs, total; } Box;

/* the bytes a code takes after it (as tools/translate counts them) */
static int arguments(const uint8_t *b, int i, int n, int code, int two) {
    switch (code) {
    case 0xF2: {
        int act = i < n ? b[i] : 0;
        return 1 + (act == 0 ? 2 : act == 3 || act == 4 || act == 0xB ? 1 : 0);
    }
    case 0xF4: return two ? 2 : 1;
    case 0xF5: case 0xF6: case 0xFA: return 2;
    case 0xF9: return 1;
    default: return 0;
    }
}

typedef struct { Box *out; int max, count, start, line, part, total; } Filling;
static void box_end(Filling *f, int at) {
    if (f->count < f->max) {
        Box *q = &f->out[f->count++];
        q->start = f->start;
        q->end = at;
        q->line = f->line;
        q->part = f->part;
        q->sub = 0;
        q->subs = 1;
        q->total = f->total;
    }
    f->total = 0;
}

/* one line, from byte `i`, into its boxes, as RunText fills them. *next: the byte after its $FF */
static int boxes_of(const uint8_t *b, int n, int i, int line, int two, Box *out, int max, int *next) {
    Filling f = {out, max, 0, i, line, 0, 0};
    int col = 0, row = 0, done = 0;
    while (i < n && !done) {
        int c = b[i++];
        if (c < 0xF0) {
            if (c >= 0xE0 && i < n) i++;
            f.total++;
            col = (col + 1) & 31;
            if (col || (i < n && b[i] == 0xF5)) continue;
            row++;
            if (i < n && b[i] == 0xFC) i++;
            if (row & 1) continue;
            box_end(&f, i);                                  /* the box is full: the arrow */
            col = row = 0;
            if (i < n && b[i] == 0xFF) { i++; done = 1; }
            else if (i < n && (b[i] == 0xFD || b[i] == 0xF7)) { i++; f.part++; }
            f.start = i;
            continue;
        }
        int at = i - 1;
        switch (c) {
        case 0xFF:
            box_end(&f, at);
            done = 1;
            break;
        case 0xFC:
            col = 0;
            row++;
            break;
        case 0xFD:
            box_end(&f, at);
            col = row = 0;
            f.part++;
            if (i < n && b[i] == 0xFF) { i++; done = 1; }
            else if (i < n && (b[i] == 0xFD || b[i] == 0xF7)) { i++; f.part++; }
            f.start = i;
            break;
        case 0xF0: case 0xF1: case 0xF3: case 0xF5: case 0xF7: case 0xF8: case 0xFB: case 0xFE:
            box_end(&f, at);
            col = row = 0;
            i += arguments(b, i, n, c, two);
            f.part++;
            f.start = i;
            break;
        default:
            i += arguments(b, i, n, c, two);
        }
    }
    if (!done && f.total) box_end(&f, i);
    for (int a = 0; a < f.count;) {                          /* the boxes of one piece */
        int e = a + 1, full = 0, k = 0;
        while (e < f.count && out[e].part == out[a].part) e++;
        for (int j = a; j < e; j++) full += out[j].total > 0;
        for (int j = a; j < e; j++) {
            out[j].subs = full ? full : 1;
            out[j].sub = k < out[j].subs ? k : out[j].subs - 1;
            k += out[j].total > 0;
        }
        a = e;
    }
    *next = i;
    return f.count;
}

typedef struct { uint8_t *bytes; int n; Box *box; int nbox; } Bank;
static Bank bank[BANKS];
static int two_bytes(int k) { return k + 1 == 31 || k + 1 == 32; }   /* (the camp's Talk: a portrait's place with it) */

static void banks_ready(void) {
    static uint32_t made_for;
    if (made_for && made_for == m68k_rom_size) return;
    made_for = m68k_rom_size;
    static uint8_t tmp[0x4000];
    static Box box[4096];
    for (int k = 0; k < BANKS; k++) {
        Bank *q = &bank[k];
        free(q->bytes);
        free(q->box);
        memset(q, 0, sizeof *q);
        int n = kosinski(BANK_AT[k], tmp, (int)sizeof tmp), nb = 0, line = 0;
        for (int i = 0; i < n && nb < 4000; line++) {
            int next;
            nb += boxes_of(tmp, n, i, line, two_bytes(k), box + nb, 4096 - nb, &next);
            i = next;
        }
        q->bytes = n > 0 ? malloc((size_t)n) : NULL;
        q->box = malloc((size_t)(nb ? nb : 1) * sizeof *box);
        if (!q->bytes || !q->box) continue;
        memcpy(q->bytes, tmp, (size_t)n);
        memcpy(q->box, box, (size_t)nb * sizeof *box);
        q->n = n;
        q->nbox = nb;
    }
}

/* the bank in RAM: the one whose bytes stand round byte `at`. Not all of them need to: the game changes a few itself
 * (a portrait's number, for someone who has left the party), and what lies behind the text may be used otherwise */
static int bank_now(int at) {
    int best = -1, least = 9;
    for (int k = 0; k < BANKS; k++) {
        const Bank *q = &bank[k];
        int from = at > 256 ? at - 256 : 0, to = at + 64 < q->n ? at + 64 : q->n, off = 0;
        if (at >= q->n || to - from < 16 || to > TREES_MAX) continue;
        for (int i = from; i < to && off < least; i++) off += m68k_ram[TREES + i] != q->bytes[i];
        if (off < least) { least = off; best = k; }
    }
    return best;
}

/* the letters from byte a to byte e; *chars: how many characters that is */
static int letters(const uint8_t *b, int n, int a, int e, int two, char *out, int max, int *chars) {
    int k = 0, c = 0;
    for (int i = a; i < e && i < n;) {
        int v = b[i++];
        if (v >= 0xF0) { i += arguments(b, i, n, v, two); continue; }
        if (v >= 0xE0 && i < n) v = b[i++];
        c++;
        char l = letter(v);
        if (l && k < max - 1) out[k++] = l;
    }
    out[k] = 0;
    if (chars) *chars = c;
    return k;
}

/* ------------------------------------------------------------------ the piece of a translation a box shows */
#define PIECES 40
static int starts(const char *s, const char *end, const char *with) {
    size_t n = strlen(with);
    return (size_t)(end - s) >= n && !memcmp(s, with, n);
}

/* the translation's words cut at its marks, the codes out of them */
static int pieces_of(const char *t, char *buf, size_t max, char **piece) {
    const char *end = t + strlen(t);
    size_t k = 0;
    int n = 0;
    piece[n++] = buf;
    while (t < end && k + 2 < max) {
        const char *e = *t == '{' ? memchr(t, '}', (size_t)(end - t)) : NULL;
        if (!e) { buf[k++] = *t++; continue; }
        int mark = starts(t, end, "{page}") || starts(t, end, "{cut}");
        if (!mark && e - t >= 3 && hex(t[1]) >= 0 && hex(t[2]) >= 0) {
            int c = hex(t[1]) * 16 + hex(t[2]);
            mark = c == 0xF0 || c == 0xF1 || c == 0xF3 || c == 0xF5 || c == 0xF8 || c == 0xFB || c == 0xFE;
        }
        t = e + 1;
        if (mark && n < PIECES) { buf[k++] = 0; piece[n++] = buf + k; }
    }
    buf[k] = 0;
    for (int i = 0; i < n; i++) {                            /* no blank lines or spaces round a piece */
        char *s = piece[i];
        while (*s == '\n' || *s == ' ') s++;
        for (char *e = s + strlen(s); e > s && (e[-1] == '\n' || e[-1] == ' ');) *--e = 0;
        piece[i] = s;
    }
    while (n > 1 && !piece[n - 1][0]) n--;
    return n;
}

static int utf8_count(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

/* the words for box `at` of a line's boxes; 0: none */
static int words_for(const char *t, const Box *first, int count, int at, char *out, size_t max) {
    static char buf[8192];
    char *piece[PIECES];
    int nt = pieces_of(t, buf, sizeof buf, piece), ng = 1, a, e;
    const Box *q = &first[at];
    for (int i = 0; i < count; i++)                          /* the game's pieces */
        if (first[i].total && first[i].part + 1 > ng) ng = first[i].part + 1;
    if (nt == ng) { a = q->part; e = a + 1; }
    else if (nt > ng) {                                      /* more than the game has: shared out among its boxes */
        a = q->part * nt / ng;
        e = (q->part + 1) * nt / ng;
    } else {                                                 /* fewer: the last one stays */
        a = q->part < nt ? q->part : nt - 1;
        e = a + 1;
    }
    size_t k = 0;
    out[0] = 0;
    for (int i = a; i < e && i < nt; i++) {
        size_t l = strlen(piece[i]);
        if (!l || k + l + 2 > max) continue;
        if (k) out[k++] = '\n';
        memcpy(out + k, piece[i], l + 1);
        k += l;
    }
    if (q->subs > 1) {                                       /* a piece the game breaks over boxes of its own: its lines shared out */
        int lines = 1, l = 0;
        for (const char *c = out; *c; c++) lines += *c == '\n';
        int from = q->sub * lines / q->subs, to = (q->sub + 1) * lines / q->subs;
        char *w = out;
        for (char *c = out; *c; c++) {
            int here = l;
            l += *c == '\n';
            if (here >= from && here < to) *w++ = *c;
        }
        *w = 0;
        if (w > out && w[-1] == '\n') w[-1] = 0;
    }
    return out[0] != 0;
}

/* ------------------------------------------------------------------ what is on the screen */
#define RUNTEXT_LO  0x06A090u                                /* RunText .. the end of its codes */
#define RUNTEXT_HI  0x06A7A8u
#define RUNTEXT3_LO 0x06AD00u                                /* RunText3 (it turns its own pages) */
#define RUNTEXT3_HI 0x06B000u
#define AFTER_VINT  0x041688u                                /* DMAPlane_A_VInt, after VInt_Prepare */

static char said[4096];                                      /* the box's words */
static int up, typed;                                        /* a box is up; its characters typed */
static char last_key[256];                                   /* the game's letters in that box, as last seen */
static uint32_t last_id;

static char tried[256];                                      /* a box looked for by its letters */

static void forget(void) {
    up = typed = 0;
    last_key[0] = said[0] = 0;
    last_id = 0;
}

int translate_showing(void) { return up && nentries > 0; }
const char *translate_text(void) { return said; }
int translate_shown(void) { return typed; }

/* RunText's place in its text, while it waits for a frame; 0 when it is not it that waits */
static uint32_t text_at(void) {
    uint32_t sp = A(7) & 0xFFFFFF;                           /* VInt_Prepare: d0-d7, a0-a4, the return, the caller's */
    if (sp < 0xFF0000 || sp + 60 > 0xFFFFFF) return 0;
    if (rd32(sp + 52) != AFTER_VINT) return 0;
    uint32_t r = rd32(sp + 56);
    if (!((r > RUNTEXT_LO && r < RUNTEXT_HI) || (r > RUNTEXT3_LO && r < RUNTEXT3_HI))) return 0;
    return rd32(sp + 32) & 0xFFFFFF;
}

static int log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("PSIV_TRANSLATE_LOG");
        on = v && *v == '1';
    }
    return on;
}

/* box `at` of its line's boxes, with the game's text read up to byte p: shown, if the screen agrees */
static int show(const uint8_t *b, int n, const Box *first, int count, int at, int p, int two, const char *t, uint32_t id,
                const char *key) {
    const Box *q = &first[at];
    char now[256], all[256];
    int chars, k = letters(b, n, q->start, p < q->end ? p : q->end, two, now, (int)sizeof now, &chars);
    int seen = (int)strlen(key), was = last_id == id;
    /* (the letter typed in this frame may not be in the picture yet) */
    if (seen > k || seen < k - 2 || memcmp(now, key, (size_t)seen)) {
        if (letters(b, n, q->start, q->end, two, all, (int)sizeof all, NULL) != seen || strcmp(all, key)) return 0;
        chars = q->total;                                    /* the whole of it */
    }
    if (!was) {
        up = t && words_for(t, first, count, at, said, sizeof said);
        if (log_on())
            printf("translate: %08X box %d of %d (%d characters): %s\n", id, at + 1, count, q->total,
                   up ? said : t ? "(nothing for it)" : "(not in the pack)");
    }
    if (up) {
        int full = utf8_count(said);
        typed = chars >= q->total ? full : chars * full / q->total;
    }
    last_id = id;
    snprintf(last_key, sizeof last_key, "%s", key);
    return 1;
}

void translate_frame(const char *key) {
    if (!nentries) return;
    banks_ready();
    uint32_t p = text_at();
    int k = p >= 0xFF0000u + TREES && p < 0xFF0000u + TREES + TREES_MAX ? bank_now((int)(p - 0xFF0000u - TREES)) : -1;
    if (k >= 0) {
        const Bank *q = &bank[k];
        int at = (int)(p - 0xFF0000u - TREES);
        for (int i = 0; i < q->nbox; i++) {
            if (at < q->box[i].start || at > q->box[i].end + 2) continue;      /* (+ 2: past the mark that ends it) */
            int f = i, e = i + 1;
            while (f > 0 && q->box[f - 1].line == q->box[i].line) f--;
            while (e < q->nbox && q->box[e].line == q->box[i].line) e++;
            uint32_t id = (uint32_t)(k + 1) << 12 | (uint32_t)q->box[i].line;
            if (show(q->bytes, q->n, q->box + f, e - f, i - f, at, two_bytes(k), entry(id), id | (uint32_t)(i - f + 1) << 20, key)) return;
        }
    } else if (p && p < m68k_rom_size) {                     /* a short text, straight from the ROM */
        int lo = 0, hi = nentries - 1, best = -1;            /* the pack's last one that starts at or before p */
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (entries[mid].id <= (p | ROM_TEXT)) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        for (int j = best; j >= 0 && j > best - 2; j--) {    /* (or the one before it: at a text's end the place is the next one's start) */
            if (!(entries[j].id & ROM_TEXT) || p - (entries[j].id & ~ROM_TEXT) >= 400) break;
            uint32_t a = entries[j].id & ~ROM_TEXT;
            Box box[16];
            int next, n = m68k_rom_size - a < 600 ? (int)(m68k_rom_size - a) : 600, at = (int)(p - a);
            int c = boxes_of(m68k_rom + a, n, 0, 0, 0, box, 16, &next);
            for (int i = 0; i < c; i++)
                if (at >= box[i].start && at <= box[i].end + 2
                    && show(m68k_rom + a, n, box, c, i, at, 0, entries[j].text, entries[j].id ^ (uint32_t)i << 26, key)) return;
        }
    }
    /* not typing: the box stays as it was while its letters do (the game waits for a button, or asks yes or no) */
    if (*key && !strcmp(key, last_key)) return;
    size_t l = strlen(last_key);
    if (*key && up && l && strlen(key) <= l + 2 && !strncmp(key, last_key, l)) {   /* (its last letter came into the picture late) */
        typed = utf8_count(said);
        snprintf(last_key, sizeof last_key, "%s", key);
        return;
    }
    if (up || l) forget();
    /* a box that is simply there (the pack was read again, a rewind): by its letters, if the map has one such */
    if (strlen(key) < 6) tried[0] = 0;
    else if (strcmp(key, tried) && !p) {
        int b = bank_now(0), found = -1, other = 0;
        char all[256];
        snprintf(tried, sizeof tried, "%s", key);
        for (int i = 0; b >= 0 && i < bank[b].nbox; i++) {
            const Box *x = &bank[b].box[i];
            if (x->total < (int)strlen(key)) continue;
            letters(bank[b].bytes, bank[b].n, x->start, x->end, two_bytes(b), all, (int)sizeof all, NULL);
            if (!strcmp(all, key)) { other += found >= 0; found = i; }
        }
        if (found >= 0 && !other) {
            const Box *x = bank[b].box;
            int f = found, e = found + 1;
            while (f > 0 && x[f - 1].line == x[found].line) f--;
            while (e < bank[b].nbox && x[e].line == x[found].line) e++;
            uint32_t id = (uint32_t)(b + 1) << 12 | (uint32_t)x[found].line;
            show(bank[b].bytes, bank[b].n, x + f, e - f, found - f, x[found].end, two_bytes(b), entry(id),
                 id | (uint32_t)(found - f + 1) << 20, key);
        }
    }
}
