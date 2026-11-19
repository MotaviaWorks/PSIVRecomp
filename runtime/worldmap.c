/*
 * The world map: see worldmap.h.
 *
 * What the game keeps (names from the disassembly; widemap.c has more):
 *   Field_Map_Index $FFEC28        0 Motavia, 1 Dezolis, 2 Rykros
 *   Map_Layout_FG $FFA000 / _BG $FFB000   a byte per 32x32 chunk
 *   Chunk_Table $FF6000            16 name-table words a chunk; bit 14 = collision
 * A planet is 128 x 128 chunks (4096 pixels each way, and it goes round; Dezolis'
 * lower half is one slab eight times over, so its map is the upper 128 x 64). The
 * ROM holds each layer of it as sixteen slabs of eight rows (pointer tables at
 * loc_107DC2 / loc_10BE02 for Motavia, loc_115584 / loc_1175C4 for Dezolis);
 * the game keeps four in RAM, slab s at (s & 3) * $400, the camera's and the
 * three below (loc_53A26, RefreshMapLayout), and patches what the story has
 * changed as it copies a slab (loc_53BB6: Zio's Fort, the spaceports, the
 * Edge ...). So the map here is the ROM's slabs with the slabs in RAM laid
 * over them whenever the party is there: a change shows once the party has
 * been near it. Rykros is an ordinary map, whole in RAM.
 *
 * The picture: each chunk drawn once from the chunk table and VRAM (as
 * palette indices, with the priority bit), plane A over plane B the way the
 * VDP mixes them, the colours from CRAM. Kept at 1/2, 1/4, 1/8 and 1/16 for
 * the views from afar (a block per pair of chunks, so the sea costs nothing);
 * a view at full size or larger is drawn from the chunks as it is asked for.
 * It is made again, a slab's rows a frame, when the table or the palette
 * changes and a moment after the field settles (its tiles are all in then).
 */
#include "worldmap.h"
#include "bus.h"
#include "m68k.h"
#include "vdp.h"
#include "walkto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE    0xEF00
#define MODE_FIELD   0x0C
#define GAME_ROUTINE 0xEC20            /* $C: an event is running */
#define FIELD_MAP    0xEC28
#define USE_BG       0xEC24
#define ALT_COLOR    0xEC4F
#define VEHICLE      0xF43C
#define WIN_END      0xEC98            /* the end of the plane mappings a window saved: $6040 = none */
#define LEADER       0xC000
#define CHUNKS       0x6000
#define EVENT_FLAGS  0xF100

#define MAXC 128                       /* chunks across a world, at most */
#define FOG  0xFF0C1430u               /* not explored */
#define FOG2 0xFF141E46u               /* ... its hatching */
#define DARK 0xFF060A18u               /* beyond the map */

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }
static int wrapi(int v, int n) { v %= n; return v < 0 ? v + n : v; }
static uint32_t rom32(uint32_t a) {
    return a + 3 < m68k_rom_size ? (uint32_t)m68k_rom[a] << 24 | (uint32_t)m68k_rom[a + 1] << 16
                                   | (uint32_t)m68k_rom[a + 2] << 8 | m68k_rom[a + 3] : 0xFFFFFFFFu;
}

static const uint32_t SLABS[2][2] = {{0x107DC2, 0x10BE02}, {0x115584, 0x1175C4}};   /* [planet][FG, BG] */
static const char *const NAMES[3] = {"Motavia", "Dezolis", "Rykros"};

static int mode = -1;
static int here = -1;                  /* the world the party is on now (the field shows it), or -1 */
static int cur = -1;                   /* the world whose layout and picture are held */
static int lw, lh;                     /* its size in chunks */
static int vh;                         /* ... and how many rows of them are the map (Dezolis: the upper half) */
static uint8_t lay[2][MAXC * MAXC];    /* its layout: FG, BG */
static uint8_t table[0x2000];          /* the chunk table, as last seen whole */
static int alt_color;                  /* plane B in palette line 0, low priority (BG_Alternate_Color_Flag) */
static uint32_t pal[64], backdrop;
static uint32_t pal_now, pal_used;     /* the palette's hash: this frame's, the picture's */
static int steady, pal_steady;         /* frames the field has shown the world with no window; ... with one palette */
static int built, build_row = -1;      /* the picture is whole; the next chunk row to make (-1: none) */
static int logging = -1;               /* PSIV_WORLDMAP_LOG=1: what holds the picture up; the story's flags in Piata */
static unsigned rev;

void worldmap_set_mode(int m) {
    if (m < 1 && mode >= 1) worldmap_stop();
    mode = m;
}
int worldmap_mode(void) { return mode; }
unsigned worldmap_rev(void) { return rev; }
int worldmap_ready(void) { return mode >= 1 && here >= 0 && here == cur && built; }
int worldmap_shown(void) {
    return worldmap_ready() && r16(GAME_MODE) == MODE_FIELD && r16(WIN_END) <= 0x6040 && r16(GAME_ROUTINE) != 0x0C;
}
const char *worldmap_name(void) { return cur >= 0 ? NAMES[cur] : ""; }
void worldmap_size(int *w, int *h) { *w = lw * 32; *h = vh * 32; }
int worldmap_wraps(void) { return cur == 0 || cur == 1; }
int worldmap_wraps_y(void) { return worldmap_wraps() && vh == lh; }

int worldmap_party(int *x, int *y) {
    if (here < 0) return 0;
    *x = (int)r16(LEADER + 0x30) + 8;
    *y = (int)r16(LEADER + 0x34) + 24;
    if (here < 2) { *x &= 4095; *y &= 4095; }
    return 1;
}

/* ------------------------------------------------------------------ explored */
static uint8_t explored[3][256 * 256];  /* a byte a 16x16 cell: 1 = seen */
static int ex_dirty;

/* what the screen shows round the party: a little more than the 4:3 view, its corners rounded */
static void explore(int w) {
    int cw = w < 2 ? 256 : ((int)r8(0xEC64) + 1) * 2, ch = w < 2 ? 256 : ((int)r8(0xEC65) + 1) * 2;
    if (cw <= 0 || ch <= 0 || cw > 256 || ch > 256) return;
    int px = (int)r16(LEADER + 0x30), py = (int)r16(LEADER + 0x34) + 16;
    if (w < 2) {    /* the leader where the camera has them (the screen's middle): not a place left over from another map */
        int W = cw * 16, H = ch * 16;
        int dx = wrapi(px - (int)r16(r8(USE_BG) ? 0xEF9C : 0xEF94) - 152, W), dy = wrapi(py - (int)r16(r8(USE_BG) ? 0xEF98 : 0xEF90) - 104, H);
        if (dx > W / 2) dx -= W;
        if (dy > H / 2) dy -= H;
        if (abs(dx) > 48 || abs(dy) > 48) return;
    }
    int pcx = px >> 4, pcy = py >> 4, changed = 0;
    for (int j = -8; j <= 8; j++)
        for (int i = -12; i <= 12; i++) {
            if (i * i * 64 + j * j * 144 > 64 * 144 * 27 / 20) continue;      /* (an ellipse and a third) */
            int x = pcx + i, y = pcy + j;
            if (w < 2) { x &= 255; y &= 255; }
            else if (x < 0 || y < 0 || x >= cw || y >= ch) continue;
            uint8_t *e = &explored[w][y * 256 + x];
            if (!*e) { *e = 1; changed = 1; }
        }
    if (changed) { ex_dirty = 1; rev++; }
}

int worldmap_explored_dirty(void) { return ex_dirty; }
void worldmap_explored_pack(uint8_t *out) {
    memcpy(out, "PSIVEXP1", 8);
    memset(out + 8, 0, 3 * 8192);
    for (int w = 0; w < 3; w++)
        for (int i = 0; i < 65536; i++)
            if (explored[w][i]) out[8 + w * 8192 + (i >> 3)] |= (uint8_t)(1 << (i & 7));
    ex_dirty = 0;
}
int worldmap_explored_unpack(const uint8_t *in, size_t n) {
    if (n < WORLDMAP_FILE_SIZE || memcmp(in, "PSIVEXP1", 8)) return 0;
    for (int w = 0; w < 3; w++)
        for (int i = 0; i < 65536; i++) explored[w][i] = in[8 + w * 8192 + (i >> 3)] >> (i & 7) & 1;
    ex_dirty = 0;
    rev++;
    return 1;
}

/* A new game: the story's flags as the game sets them out (eleven are set from the start; the
 * twelfth comes with the first words spoken) while the field shows Piata, where it opens. What an
 * earlier game explored goes. */
static void new_game_check(void) {
    static int cleared;
    if (r16(GAME_MODE) != MODE_FIELD || r16(FIELD_MAP) < 0x10 || r16(FIELD_MAP) > 0x1C) return;
    int bits = 0, any = 0;
    for (unsigned a = EVENT_FLAGS; a < EVENT_FLAGS + 0x40; a++)
        for (unsigned b = r8(a); b; b &= b - 1) bits++;
    if (logging > 0) {
        static int last = -1;
        if (bits != last) printf("worldmap: map %03X, %d story flags set\n", r16(FIELD_MAP), bits);
        last = bits;
    }
    if (bits < 1 || bits > 11) { cleared = 0; return; }
    if (cleared) return;                                /* (once for this one) */
    cleared = 1;
    for (int w = 0; w < 3 && !any; w++)
        for (int i = 0; i < 65536 && !any; i++) any = explored[w][i];
    if (!any) return;
    memset(explored, 0, sizeof explored);
    ex_dirty = 1;
    rev++;
}

/* ------------------------------------------------------------------ the picture */
static uint8_t cimg[2][256][1024];      /* each chunk as plane A draws it, and as plane B does: 0 = nothing,
                                         * else the palette index | $80 with priority */
static uint16_t *mip[5];                /* [1..4]: the world at 1/2 .. 1/16, RGB565 */
static uint16_t *pool;                  /* a 16x16 block of the 1/2 picture for each pair of chunks met */
static int npool, cap_pool;
static uint16_t pair_at[65536];         /* FG << 8 | BG: its block + 1, 0 = not made */

static inline uint32_t mix(uint8_t a, uint8_t b) {
    uint8_t p = (a & 0x80) ? a : (b & 0x80) ? b : a ? a : b;
    return p ? pal[p & 63] : backdrop;
}
static inline uint16_t to565(unsigned r, unsigned g, unsigned b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
static inline uint32_t from565(uint16_t p) {
    unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
static inline uint16_t avg565(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    unsigned r = (a >> 11) + (b >> 11) + (c >> 11) + (d >> 11) + 2;
    unsigned g = ((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63) + 2;
    unsigned bl = (a & 31) + (b & 31) + (c & 31) + (d & 31) + 2;
    return (uint16_t)((r >> 2) << 11 | (g >> 2) << 5 | (bl >> 2));
}

static void draw_chunks(void) {
    for (int v = 0; v < 2; v++) {
        unsigned mask = v && alt_color ? 0x1FFF : 0xBFFF;
        for (int c = 0; c < 256; c++)
            for (int t = 0; t < 16; t++) {
                unsigned word = (unsigned)(table[c * 32 + t * 2] << 8 | table[c * 32 + t * 2 + 1]) & mask;
                const uint8_t *tile = &vdp.vram[(word & 0x7FF) * 32];
                uint8_t base = (uint8_t)(((word >> 13) & 3) << 4 | (word & 0x8000 ? 0x80 : 0));
                uint8_t *o = &cimg[v][c][(t >> 2) * 8 * 32 + (t & 3) * 8];
                for (int y = 0; y < 8; y++) {
                    const uint8_t *row = tile + (word & 0x1000 ? 7 - y : y) * 4;
                    for (int x = 0; x < 8; x++) {
                        int sx = word & 0x800 ? 7 - x : x;
                        unsigned nib = (row[sx >> 1] >> (sx & 1 ? 0 : 4)) & 15;
                        o[y * 32 + x] = nib ? (uint8_t)(base | nib) : 0;
                    }
                }
            }
    }
    memset(pair_at, 0, sizeof pair_at);
    npool = 0;
}

static const uint16_t *pair_block(int fg, int bg) {
    static uint16_t one[256];
    int key = fg << 8 | bg;
    if (pair_at[key]) return pool + (size_t)(pair_at[key] - 1) * 256;
    uint16_t *o = one;
    if (npool < 65535) {
        if (npool == cap_pool) {
            int cap = cap_pool ? cap_pool * 2 : 1024;
            uint16_t *p = realloc(pool, (size_t)cap * 256 * sizeof *pool);
            if (p) { pool = p; cap_pool = cap; }
        }
        if (npool < cap_pool) { o = pool + (size_t)npool * 256; pair_at[key] = (uint16_t)++npool; }
    }
    const uint8_t *a = cimg[0][fg], *b = cimg[1][bg];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int p = y * 64 + x * 2;
            uint32_t c0 = mix(a[p], b[p]), c1 = mix(a[p + 1], b[p + 1]), c2 = mix(a[p + 32], b[p + 32]), c3 = mix(a[p + 33], b[p + 33]);
            unsigned r = ((c0 >> 16) & 255) + ((c1 >> 16) & 255) + ((c2 >> 16) & 255) + ((c3 >> 16) & 255) + 2;
            unsigned g = ((c0 >> 8) & 255) + ((c1 >> 8) & 255) + ((c2 >> 8) & 255) + ((c3 >> 8) & 255) + 2;
            unsigned bl = (c0 & 255) + (c1 & 255) + (c2 & 255) + (c3 & 255) + 2;
            o[y * 16 + x] = to565(r >> 2, g >> 2, bl >> 2);
        }
    return o;
}

/* a chunk of the world into the kept pictures */
static void blit(int cx, int cy) {
    const uint16_t *b = pair_block(lay[0][cy * MAXC + cx], lay[1][cy * MAXC + cx]);
    int w1 = MAXC * 16;
    for (int y = 0; y < 16; y++) memcpy(&mip[1][(size_t)(cy * 16 + y) * w1 + cx * 16], b + y * 16, 16 * sizeof *b);
    for (int l = 2; l <= 4; l++) {
        int n = 32 >> l, ws = MAXC * 32 >> (l - 1), wd = MAXC * 32 >> l;
        const uint16_t *s = &mip[l - 1][(size_t)(cy * n * 2) * ws + cx * n * 2];
        uint16_t *d = &mip[l][(size_t)(cy * n) * wd + cx * n];
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                d[y * wd + x] = avg565(s[y * 2 * ws + x * 2], s[y * 2 * ws + x * 2 + 1], s[(y * 2 + 1) * ws + x * 2], s[(y * 2 + 1) * ws + x * 2 + 1]);
    }
}

static int alloc_mips(void) {
    for (int l = 1; l <= 4; l++) {
        if (mip[l]) continue;
        size_t n = (size_t)(MAXC * 32 >> l);
        mip[l] = calloc(n * n, sizeof *mip[l]);
        if (!mip[l]) return 0;
    }
    return 1;
}

/* a planet's layout from the ROM */
static int load_planet(int w) {
    for (int k = 0; k < 2; k++)
        for (int s = 0; s < 16; s++) {
            uint32_t p = rom32(SLABS[w][k] + (uint32_t)s * 4);
            if (p >= m68k_rom_size || m68k_rom_size - p < 0x400) return 0;
            memcpy(lay[k] + s * 0x400, m68k_rom + p, 0x400);
        }
    lw = lh = MAXC;
    int n = 16;                                          /* the slabs at the end that are the one before them again */
    while (n > 1 && rom32(SLABS[w][0] + (uint32_t)(n - 1) * 4) == rom32(SLABS[w][0] + (uint32_t)(n - 2) * 4)
           && rom32(SLABS[w][1] + (uint32_t)(n - 1) * 4) == rom32(SLABS[w][1] + (uint32_t)(n - 2) * 4)) n--;
    vh = n < 16 ? n * 8 : MAXC;
    return 1;
}

/* the slabs the game has in RAM, over the ROM's: what the story changed. Returns how many chunks differed. */
static int overlay_planet(int w, int draw) {
    int n = 0;
    for (int k = 0; k < 2; k++) {
        int cs = (int)(r16(k ? 0xEF98 : 0xEF90) >> 8);
        for (int j = 0; j < 4; j++) {
            int s = (cs + j) & 15;
            const uint8_t *ram = &m68k_ram[(k ? 0xB000 : 0xA000) + (s & 3) * 0x400];
            uint32_t p = rom32(SLABS[w][k] + (uint32_t)s * 4);
            int diff = 0;
            for (int i = 0; i < 0x400; i++) diff += ram[i] != m68k_rom[p + (uint32_t)i];
            if (diff > 96) continue;                     /* another slab's place still (the game is about to load this one) */
            uint8_t *mine = lay[k] + s * 0x400;
            for (int i = 0; i < 0x400; i++)
                if (mine[i] != ram[i]) {
                    mine[i] = ram[i];
                    n++;
                    if (draw) blit(i & 127, s * 8 + (i >> 7));
                }
        }
    }
    return n;
}

/* Rykros: the layout in RAM, whole. Returns how many chunks differed. */
static int load_ram(int draw) {
    int w = (int)r8(0xEC64) + 1, h = (int)r8(0xEC65) + 1, wb = (int)r8(0xEC66) + 1, hb = (int)r8(0xEC67) + 1, n = 0;
    if (w > MAXC || h > MAXC || w * h > 0x1000 || wb * hb > 0x1000) return 0;
    if (w != lw || h != lh) { lw = w; lh = vh = h; memset(lay, 0, sizeof lay); n++; draw = 0; }
    for (int k = 0; k < 2; k++) {
        int sw = k ? wb : w, sh = k ? hb : h;
        for (int y = 0; y < h && y < sh; y++)
            for (int x = 0; x < w && x < sw; x++) {
                uint8_t c = (uint8_t)r8((k ? 0xB000u : 0xA000u) + (unsigned)(y * sw + x));
                if (lay[k][y * MAXC + x] == c) continue;
                lay[k][y * MAXC + x] = c;
                n++;
                if (draw) blit(x, y);
            }
    }
    return n;
}

void worldmap_frame(void) {
    here = -1;
    if (mode < 0 || !m68k_rom) return;
    if (logging < 0) logging = getenv("PSIV_WORLDMAP_LOG") != NULL;
    new_game_check();
    if (r16(GAME_MODE) != MODE_FIELD || r16(FIELD_MAP) > 2) { steady = 0; return; }
    int w = (int)r16(FIELD_MAP);
    if (r16(WIN_END) > 0x6040) { here = w == cur ? w : -1; steady = 0; return; }   /* a window: the chunk table isn't whole */
    steady++;
    if (steady >= 12) explore(w);
    if (mode < 1) return;
    here = w;

    /* the palette's colours whatever their order: the sea is animated by turning a few of them round,
     * which is no change; a fade is */
    uint32_t ph = 0;
    int flat = 1;
    for (int i = 0; i < 64; i++) {
        ph += (vdp.cram[i] + 1u) * 2654435761u ^ (uint32_t)(vdp.cram[i] * vdp.cram[i]);
        if (i < 32 && vdp.cram[i] != vdp.cram[0]) flat = 0;
    }
    ph = (ph ^ (vdp.reg[7] & 63u) ^ (r8(ALT_COLOR) ? 0x100u : 0u)) * 16777619u;
    if (ph != pal_now) { pal_now = ph; pal_steady = 0; } else pal_steady++;

    if (w != cur) {                                      /* another world: its layout; its picture to make */
        cur = -1;
        built = 0;
        build_row = -1;
        worldmap_stop();
        if (w < 2) { if (!load_planet(w)) return; }
        else lw = lh = vh = 0;
        cur = w;
    }
    if (logging > 0 && !(steady % 60)) {
        printf("worldmap: world %d steady %d palette steady %d flat %d built %d row %d; cram", w, steady, pal_steady, flat, built, build_row);
        for (int i = 0; i < 32; i++) printf(" %03X", vdp.cram[i]);
        printf("\n");
    }
    if (steady < 20 || pal_steady < 20 || flat) return;  /* (a fade; a window just closed) */
    int again = !built && build_row < 0;
    if (memcmp(table, &m68k_ram[CHUNKS], sizeof table)) { memcpy(table, &m68k_ram[CHUNKS], sizeof table); again = 1; }
    if (ph != pal_used) {
        for (int i = 0; i < 64; i++) pal[i] = vdp_color(i);
        backdrop = vdp_color(vdp.reg[7] & 63);
        alt_color = r8(ALT_COLOR) != 0;
        pal_used = ph;
        again = 1;
    }
    if (steady == 90) again = 1;                         /* the field has settled: every tile is in VRAM now */
    if (w == 2) { if (load_ram(!again && built)) rev++; }
    else if (!(steady & 15) && overlay_planet(w, !again && built)) rev++;
    if (lw <= 0 || lh <= 0) return;
    if (again) {
        if (!alloc_mips()) return;
        draw_chunks();
        build_row = 0;
    }
    if (build_row >= 0) {                                /* a slab's rows a frame */
        for (int n = 0; n < 8 && build_row < lh; n++, build_row++)
            for (int x = 0; x < lw; x++) blit(x, build_row);
        if (build_row >= lh) { build_row = -1; built = 1; }
        rev++;
    }
}

/* ------------------------------------------------------------------ a view */
static int fog_wrap, fog_cw, fog_ch;
static const uint8_t *fog_ex;

static inline int seen_at(int c, int r) {
    if (fog_wrap) { c &= 255; r &= 255; }
    else {
        if (c < 0) c = 0; else if (c >= fog_cw) c = fog_cw - 1;
        if (r < 0) r = 0; else if (r >= fog_ch) r = fog_ch - 1;
    }
    return fog_ex[r * 256 + c];
}
/* how much of map pixel (X, Y) shows, 0-256: explored cells, soft at their edges */
static inline int shows(int X, int Y) {
    int u = X - 8, v = Y - 8, c = u >> 4, r = v >> 4;
    int a = seen_at(c, r), b = seen_at(c + 1, r), d = seen_at(c, r + 1), e = seen_at(c + 1, r + 1);
    int sum = a + b + d + e;
    if (sum == 0) return 0;
    if (sum == 4) return 256;
    int fx = u & 15, fy = v & 15;
    return (a * (16 - fx) + b * fx) * (16 - fy) + (d * (16 - fx) + e * fx) * fy;
}
static inline uint32_t fogged(uint32_t c, int f) {
    unsigned r = (FOG >> 16) & 255, g = (FOG >> 8) & 255, b = FOG & 255;
    r += (unsigned)(((int)((c >> 16) & 255) - (int)r) * f >> 8);
    g += (unsigned)(((int)((c >> 8) & 255) - (int)g) * f >> 8);
    b += (unsigned)(((int)(c & 255) - (int)b) * f >> 8);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

void worldmap_render(uint32_t *out, int stride, int w, int h, int ox, int oy, int lvl, int mag, int wrap) {
    int W = lw * 32, H = vh * 32, fog = mode == 1;
    if (cur < 0 || W <= 0 || H <= 0 || !mip[1]) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) out[j * stride + i] = DARK;
        return;
    }
    int wrap_x = (wrap & 1) && worldmap_wraps(), wrap_y = (wrap & 2) && worldmap_wraps_y();
    fog_wrap = worldmap_wraps();
    fog_cw = lw * 2;
    fog_ch = lh * 2;
    fog_ex = explored[cur];
    if (lvl > 0) {                                       /* from afar: the kept pictures */
        if (lvl > 4) lvl = 4;
        int Wl = W >> lvl, Hl = H >> lvl, ws = MAXC * 32 >> lvl, half = 1 << lvl >> 1;
        const uint16_t *m = mip[lvl];
        for (int j = 0; j < h; j++) {
            uint32_t *o = out + (size_t)j * stride;
            int Y = oy + j;
            if (wrap_y) Y = wrapi(Y, Hl);
            else if (Y < 0 || Y >= Hl) { for (int i = 0; i < w; i++) o[i] = DARK; continue; }
            int my = (Y << lvl) + half;
            for (int i = 0; i < w; i++) {
                int X = ox + i;
                if (wrap_x) X = wrapi(X, Wl);
                else if (X < 0 || X >= Wl) { o[i] = DARK; continue; }
                int f = fog ? shows((X << lvl) + half, my) : 256;
                o[i] = !f ? ((i + j) & 7 ? FOG : FOG2) : f == 256 ? from565(m[(size_t)Y * ws + X])
                     : fogged(from565(m[(size_t)Y * ws + X]), f);
            }
        }
        return;
    }
    if (mag < 1) mag = 1;
    static uint32_t *line;
    static int line_n;
    int cols = (w + mag - 1) / mag;
    if (cols > line_n) {
        free(line);
        line = malloc((size_t)cols * sizeof *line);
        line_n = line ? cols : 0;
        if (!line) return;
    }
    for (int j = 0; j < h; j += mag) {                   /* a row of map pixels, then stretched */
        int Y = oy + j / mag, void_row = 0;
        if (wrap_y) Y = wrapi(Y, H);
        else if (Y < 0 || Y >= H) void_row = 1;
        const uint8_t *la = lay[0] + (void_row ? 0 : Y >> 5) * MAXC, *lb = lay[1] + (void_row ? 0 : Y >> 5) * MAXC;
        for (int c = 0; c < cols; c++) {
            int X = ox + c;
            if (void_row) { line[c] = DARK; continue; }
            if (wrap_x) X = wrapi(X, W);
            else if (X < 0 || X >= W) { line[c] = DARK; continue; }
            int f = fog ? shows(X, Y) : 256;
            if (!f) { line[c] = (c + j / mag) % (mag >= 4 ? 2 : mag == 3 ? 3 : mag == 2 ? 4 : 8) ? FOG : FOG2; continue; }
            int p = (Y & 31) * 32 + (X & 31);
            uint32_t col = mix(cimg[0][la[X >> 5]][p], cimg[1][lb[X >> 5]][p]);
            line[c] = f == 256 ? col : fogged(col, f);
        }
        uint32_t *o = out + (size_t)j * stride;
        if (mag == 1) memcpy(o, line, (size_t)w * sizeof *o);
        else
            for (int i = 0; i < w; i++) o[i] = line[i / mag];
        for (int k = 1; k < mag && j + k < h; k++) memcpy(o + (size_t)k * stride, o, (size_t)w * sizeof *o);
    }
}

/* ------------------------------------------------------------------ collision, places */
/* a 16x16 cell's code on the walked layer (walkto.c: 8 solid, 9 water ...; 1 another map) */
static int cell_code(int cx, int cy, int use_bg) {
    if (worldmap_wraps()) { cx &= 255; cy &= 255; }
    else if (cx < 0 || cy < 0 || cx >= lw * 2 || cy >= lh * 2) return 8;
    unsigned c = lay[use_bg ? 1 : 0][(cy >> 1) * MAXC + (cx >> 1)];
    unsigned a = c * 32 + (cx & 1 ? 4u : 0u) + (cy & 1 ? 16u : 0u);
    return (table[a] & 0x40 ? 1 : 0) | (table[a + 2] & 0x40 ? 2 : 0) | (table[a + 8] & 0x40 ? 4 : 0) | (table[a + 10] & 0x40 ? 8 : 0);
}

/* The ways in from each world (its "map transition data 2"), by the names the game gives the
 * places. An entry there is the cell (16x16) where the leader's object stands at the way in's top
 * left, and a kind of range; the cells that lead in (collision code 1) are a block from the cell
 * below it: up to 4x4 for a city (range 1), 2x2 for a town (2), two side by side for a cave or a
 * tower (9), one (3). The Edge has three entries, side by side. The hidden cave on Motavia is
 * left out. */
typedef struct { uint8_t world, x, y, w, h; const char *name; } Place;
#define CITY(x, y) (x), (y) + 1, 4, 4
#define TOWN(x, y) (x), (y) + 1, 2, 2
#define CAVE(x, y) (x), (y) + 1, 2, 1
#define HOLE(x, y) (x), (y) + 1, 1, 1
static const Place PLACES[] = {
    {0, CITY(0x2C, 0x8B), "Piata"}, {0, TOWN(0x3C, 0x63), "Mile"}, {0, TOWN(0x62, 0x51), "Zema"},
    {0, TOWN(0x6C, 0x97), "Krup"}, {0, TOWN(0x52, 0x9D), "Molcum"}, {0, TOWN(0x96, 0x57), "Tonoe"},
    {0, TOWN(0x50, 0x29), "Nalya"}, {0, CITY(0x22, 0x39), "Aiedo"}, {0, TOWN(0x30, 0x51), "Kadary"},
    {0, TOWN(0xB4, 0x65), "Monsen"}, {0, TOWN(0xE0, 0x75), "Termi"}, {0, TOWN(0x0E, 0xB5), "Uzo"},
    {0, TOWN(0xC0, 0xB1), "Torinco"}, {0, TOWN(0x38, 0x5D), "Zio's Fort"}, {0, CITY(0xCE, 0x8A), "Ladea Tower"},
    {0, CAVE(0x20, 0x48), "Passageway"}, {0, CAVE(0x24, 0x34), "Passageway"}, {0, HOLE(0x77, 0x9A), "Island Cave"},
    {0, CAVE(0x80, 0x78), "Valley Maze"}, {0, CAVE(0x94, 0x5C), "Valley Maze"}, {0, TOWN(0x4A, 0x23), "Wreckage"},
    {0, TOWN(0x72, 0xB3), "Machine Center"}, {0, TOWN(0x34, 0x59), "Spaceport"}, {0, TOWN(0xC4, 0x51), "Plate System"},
    {0, TOWN(0xA2, 0x27), "Vahal Fort"}, {0, 0x36, 0x70, 4, 3, "The Edge"},
    {1, TOWN(0x7E, 0x2B), "Reshel"}, {1, TOWN(0x24, 0x51), "Ryuon"}, {1, TOWN(0x40, 0x1F), "Zosa"},
    {1, TOWN(0x92, 0x33), "Meese"}, {1, TOWN(0x0E, 0x43), "Tyler"}, {1, CITY(0x78, 0x49), "Jut"},
    {1, CAVE(0x22, 0x02), "Myst Vale"}, {1, TOWN(0x54, 0x5F), "Weapon Plant"}, {1, TOWN(0x7E, 0x0D), "Climate Center"},
    {1, TOWN(0x0C, 0x47), "Spaceport"}, {1, CITY(0xE6, 0x35), "Esper Mansion"}, {1, CITY(0x7C, 0x43), "Gumbious"},
    {1, TOWN(0x24, 0x5D), "Raja's Temple"}, {1, TOWN(0xBA, 0x05), "Garuberk Tower"},
    {2, CAVE(0x40, 0x40), "Silence Temple"}, {2, CAVE(0x20, 0x20), "Strength Tower"},
    {2, CAVE(0x60, 0x60), "Courage Tower"}, {2, CAVE(0x22, 0x60), "Anger Tower"},
};
#define NPLACES ((int)(sizeof PLACES / sizeof PLACES[0]))

int worldmap_cell_code(int cx, int cy) { return cur < 0 ? -1 : cell_code(cx, cy, r8(USE_BG) != 0); }

/* a cell of a place's way in: one of its block that leads to another map */
static int place_cell(const Place *p, int cx, int cy, int ub) {
    int dx = cx - p->x, dy = cy - p->y;
    if (worldmap_wraps()) { dx = (int)(int8_t)dx; dy = (int)(int8_t)dy; }
    return dx >= 0 && dx < p->w && dy >= 0 && dy < p->h && cell_code(cx, cy, ub) == 1;
}
/* The way in is there (as the party last saw that part of the world; the story adds and takes
 * some): how many cells, and their middle, a map pixel. */
static int place_there(const Place *p, int *mx, int *my) {
    int ub = r8(USE_BG) != 0, n = 0, sx = 0, sy = 0;
    for (int j = 0; j < p->h; j++)
        for (int i = 0; i < p->w; i++)
            if (place_cell(p, p->x + i, p->y + j, ub)) { n++; sx += i; sy += j; }
    if (n) {
        *mx = p->x * 16 + 8 + sx * 16 / n;
        *my = p->y * 16 + 8 + sy * 16 / n;
    }
    return n;
}

int worldmap_places(WorldmapPlace *out, int max) {
    int n = 0;
    if (cur < 0 || !built) return 0;
    for (int i = 0; i < NPLACES && n < max; i++) {
        const Place *p = &PLACES[i];
        int x, y;
        if (p->world != cur || !place_there(p, &x, &y)) continue;
        if (mode == 1 && !explored[cur][((y >> 4) & 255) * 256 + ((x >> 4) & 255)]) continue;
        out[n].x = x;
        out[n].y = y;
        out[n].name = p->name;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ the journey */
static int j_on, j_world, j_cx, j_cy;           /* walking to that cell of that world */
static const Place *j_place;                    /* ... or into that place, by whichever cell of its way in comes first */
static int j_wx, j_wy;                          /* the cell walkto.h is walking to now */
static int j_last_x, j_last_y, j_still, j_held, j_free;
static int j_n;                                 /* the way: cells from the leader's */
static uint16_t j_path[65536];                  /* y << 8 | x */
static uint8_t  q_seen[65536];                  /* 0, or 1 + the step's direction into the cell */
static uint16_t q_steps[65536], q_cells[65536];

static const int DX[4] = {0, 0, -1, 1}, DY[4] = {-1, 1, 0, 0};

/* the field, ready to set out: on foot, or in a vehicle */
static int field_free(void) {
    return r16(GAME_MODE) == MODE_FIELD && r16(GAME_ROUTINE) != 0x0C && (!r16(VEHICLE) || walkto_riding())
           && r16(WIN_END) <= 0x6040;
}
int worldmap_can_go(void) { return worldmap_ready() && field_free(); }
int worldmap_riding(void) { return walkto_riding(); }
static int j_drive;                             /* the journey is a vehicle's: which (walkto_riding) */

/* a vehicle's block (walkto.c): its feet cell, the one right of it, the two above; their highest code */
static int block_code(int cx, int cy, int ub) {
    int m = 0;
    for (int dy = -1; dy <= 0; dy++)
        for (int dx = 0; dx <= 1; dx++) {
            int c = cell_code(cx + dx, cy + dy, ub);
            if (c > m) m = c;
        }
    return m;
}

/* a way from cell (sx, sy) to the journey's, or to the cell nearest it the party can reach */
static int route(int sx, int sy) {
    int cw = lw * 2, ch = lh * 2, wraps = worldmap_wraps(), ub = r8(USE_BG) != 0;
    if (cw > 256 || ch > 256 || sx < 0 || sy < 0 || sx >= cw || sy >= ch) return 0;
    memset(q_seen, 0, sizeof q_seen);
    int head = 0, tail = 0, start = sy << 8 | sx;
    q_seen[start] = 5;
    q_steps[start] = 0;
    q_cells[tail++] = (uint16_t)start;
    int best = -1, best_score = 1 << 30;
    int st = j_drive ? 2 : 1;                           /* a vehicle: two cells a step, a block at a time */
    while (head < tail) {
        int i = q_cells[head++], x = i & 255, y = i >> 8;
        int dx = abs(j_cx - x), dy = abs(j_cy - y);
        if (j_drive) {                                   /* (the block's nearest cell to it) */
            dx = j_cx < x ? x - j_cx : j_cx > x + 1 ? j_cx - x - 1 : 0;
            dy = j_cy < y - 1 ? y - 1 - j_cy : j_cy > y ? j_cy - y : 0;
        }
        if (wraps) { if (dx > cw / 2) dx = cw - dx; if (dy > ch / 2) dy = ch - dy; }
        int at = j_drive ? dx + dy == 0 : j_place ? head > 1 && place_cell(j_place, x, y, ub) : dx + dy == 0;
        int score = at ? q_steps[i] : (dx + dy + 1) * 65536 + q_steps[i];
        if (score < best_score) { best_score = score; best = i; }
        if (at) break;
        if (q_steps[i] == 65535) continue;
        for (int d = 0; d < 4; d++) {
            int nx = x + st * DX[d], ny = y + st * DY[d];
            if (wraps) { nx &= 255; ny &= 255; }
            else if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
            int j = ny << 8 | nx;
            if (q_seen[j]) continue;
            if (mode == 1 && !explored[cur][j]) continue;   /* (only over what the map shows) */
            if (j_drive) {                               /* the vehicle's ground (another map: never) */
                if (!walkto_vehicle_ok(j_drive, block_code(nx, ny, ub))) continue;
            } else {
                int code = cell_code(nx, ny, ub);
                if (code >= 8 && code <= 0xC) continue;  /* solid, water, sand, ice, a counter */
                if (code == 1 && !(j_place ? place_cell(j_place, nx, ny, ub) : nx == j_cx && ny == j_cy))
                    continue;                            /* another map: only if it's where to */
            }
            q_seen[j] = (uint8_t)(d + 1);
            q_steps[j] = (uint16_t)(q_steps[i] + 1);
            q_cells[tail++] = (uint16_t)j;
        }
    }
    if (best < 0) return 0;
    if (j_place && !j_drive && best_score < 65536) { j_cx = best & 255; j_cy = best >> 8; }   /* the way in's nearest cell */
    int n = q_steps[best] + 1;
    for (int i = best, k = n - 1; k >= 0; k--) {
        j_path[k] = (uint16_t)i;
        if (k) {
            int d = q_seen[i] - 1, x = (i & 255) - st * DX[d], y = (i >> 8) - st * DY[d];
            if (wraps) { x &= 255; y &= 255; }
            i = y << 8 | x;
        }
    }
    j_n = n;
    return 1;
}

void worldmap_stop(void) {
    if (!j_on) return;
    j_on = 0;
    j_n = 0;
    walkto_force(0);
    rev++;
}

int worldmap_going(int *mx, int *my) {
    if (!j_on) return 0;
    *mx = j_cx * 16 + 8;
    *my = j_cy * 16 + 8;
    return 1;
}
int worldmap_route(int i, int *mx, int *my) {
    if (!j_on || i < 0 || i >= j_n) return 0;
    *mx = (j_path[i] & 255) * 16 + 8;
    *my = (j_path[i] >> 8) * 16 + 8;
    return 1;
}

/* the next few steps of the way: walkto's own walk, to a cell the screen shows */
static int step_on(int pcx, int pcy) {
    if (!route(pcx, pcy) || j_n <= 1) return 0;         /* there, or as near as the party gets */
    int lead = j_drive ? 3 : 6, k = j_n - 1 < lead ? j_n - 1 : lead;
    j_wx = j_path[k] & 255;
    j_wy = j_path[k] >> 8;
    walkto_force(1);
    int ok = walkto_go(j_wx, j_wy, -1);
    if (getenv("PSIV_WALK_LOG"))
        fprintf(stderr, "journey: leg from %d,%d to %d,%d (%d of %d cells on the way to %d,%d)%s\n", pcx, pcy, j_wx, j_wy, k,
                j_n - 1, j_cx, j_cy, ok ? "" : ": walkto says no");
    return ok;
}

int worldmap_go(int mx, int my) {
    worldmap_stop();
    if (!worldmap_can_go()) return 0;
    int W = lw * 32, H = lh * 32, pcx, pcy;
    if (worldmap_wraps()) mx = wrapi(mx, W);
    if (worldmap_wraps_y()) my = wrapi(my, H);
    if (mx < 0 || my < 0 || mx >= W || my >= vh * 32) return 0;
    j_cx = mx >> 4;
    j_cy = my >> 4;
    j_place = NULL;
    for (int i = 0; i < NPLACES && !j_place; i++) {      /* by a way in: into the place */
        const Place *p = &PLACES[i];
        int x, y;
        if (p->world != cur || !place_there(p, &x, &y) || abs(x - mx) > 40 || abs(y - my) > 40) continue;
        j_place = p;
        j_cx = x >> 4;
        j_cy = y >> 4;
    }
    walkto_leader_cell(&pcx, &pcy);
    j_drive = walkto_riding();
    if (j_drive) j_place = NULL;                         /* (a vehicle stops beside a place's way in) */
    j_world = cur;
    j_on = 1;
    j_still = j_held = j_free = 0;
    j_last_x = pcx;
    j_last_y = pcy;
    if (!step_on(pcx, pcy)) { worldmap_stop(); return 0; }
    rev++;
    return 1;
}

void worldmap_journey(uint8_t user) {
    if (!j_on) return;
    if (mode < 1 || cur != j_world || (r16(GAME_MODE) == MODE_FIELD && (int)r16(FIELD_MAP) != j_world)) {
        worldmap_stop();                                 /* another map: there, or elsewhere */
        return;
    }
    if (r16(GAME_MODE) != MODE_FIELD) { j_free = 0; return; }   /* a battle: on after it */
    if (walkto_riding() != j_drive) { worldmap_stop(); return; }   /* out of the vehicle, or into one */
    if (!field_free()) {                                 /* a window, an event */
        j_free = 0;
        if (++j_held > 3600) worldmap_stop();
        return;
    }
    j_held = 0;
    j_free++;
    if ((user & (uint8_t)~PAD_C) && j_free > 20) { worldmap_stop(); return; }   /* the player takes over */
    int pcx, pcy;
    walkto_leader_cell(&pcx, &pcy);
    if (j_drive ? j_cx >= pcx && j_cx <= pcx + 1 && j_cy >= pcy - 1 && j_cy <= pcy : pcx == j_cx && pcy == j_cy) {
        worldmap_stop();
        return;
    }
    if (pcx != j_last_x || pcy != j_last_y) { j_last_x = pcx; j_last_y = pcy; j_still = 0; rev++; }
    else if (++j_still > 240) {                          /* nowhere to go on */
        if (getenv("PSIV_WALK_LOG")) fprintf(stderr, "journey: still at %d,%d for 4 s: stopped\n", pcx, pcy);
        worldmap_stop();
        return;
    }
    int dx = abs(j_wx - pcx), dy = abs(j_wy - pcy);
    if (worldmap_wraps()) { if (dx > 128) dx = 256 - dx; if (dy > 128) dy = 256 - dy; }
    if ((!walkto_active() || dx + dy <= 3) && j_free > 2 && !step_on(pcx, pcy) && !walkto_active()) worldmap_stop();
}
