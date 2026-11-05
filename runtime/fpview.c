/*
 * First-person dungeons: see fpview.h.
 *
 * The map, from the game's RAM (names from the disassembly):
 *   Map_Layout_FG $FFA000 / _BG $FFB000   a byte per 32x32 chunk; rows of
 *                                          Map_Row_Size + 1 ($FFEC64 / $EC66)
 *   Chunk_Table   $FF6000                  32 bytes a chunk: 4x4 name-table
 *                                          words, bit 14 taken for collision
 *   $FFEC24                                set: the BG layer is the one walked on
 *   Character_1   $FFC000                  the leader: x (+$30), y (+$34),
 *                                          facing (+6: 0 down 4 up 8 right $C left)
 * A 16x16 cell's collision is GetChunkAndCollision's: bit 14 of its four
 * words (top-left 1, top-right 2, bottom-left 4, bottom-right 8); 8 = solid.
 * The leader stands on the cell at (x, y + 16).
 */
#include "fpview.h"
#include "bus.h"
#include "m68k.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE   0xEF00
#define MODE_FIELD  0x0C
#define FIELD_MAP   0xEC28
#define USE_BG      0xEC24
#define VEHICLE     0xF43C
#define LEADER      0xC000

static int enabled, shown;
static int last_ui;                       /* the last frame had a window open */

void fpview_set_enabled(int on) { enabled = on; }
int  fpview_enabled(void) { return enabled; }

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }

/* the dungeons, by field map (FieldMapPtrs): caves, towers, forts, plants,
 * mazes, the Air Castle, Kuran, Garuberk... towns, houses, shops and the
 * planets' surfaces keep the game's own view */
static const uint16_t DUNGEONS[][2] = {
    {0x015, 0x017}, {0x02B, 0x02D}, {0x047, 0x04A}, {0x081, 0x098}, {0x09A, 0x0BE}, {0x0C0, 0x0D3},
    {0x0D5, 0x0D7}, {0x0DA, 0x0DB}, {0x0EC, 0x0ED}, {0x0F1, 0x0FE}, {0x100, 0x108}, {0x158, 0x15E},
    {0x160, 0x165}, {0x16E, 0x16F}, {0x170, 0x18B}, {0x18D, 0x18E}, {0x190, 0x19F},
};

static int is_dungeon(unsigned map) {
    for (size_t i = 0; i < sizeof DUNGEONS / sizeof DUNGEONS[0]; i++)
        if (map >= DUNGEONS[i][0] && map <= DUNGEONS[i][1]) return 1;
    return 0;
}

int fpview_active(void) {
    static int anywhere = -1;                             /* PSIV_FPVIEW_ANYWHERE: towns too (for testing) */
    if (anywhere < 0) anywhere = getenv("PSIV_FPVIEW_ANYWHERE") != NULL;
    unsigned map = r16(FIELD_MAP);
    return enabled && r16(GAME_MODE) == MODE_FIELD && !r16(VEHICLE) && (is_dungeon(map) || (anywhere && map > 2));
}

/* ------------------------------------------------------------------ the map */
typedef struct { unsigned layout; int w, h; int pal; } Layer;   /* w, h in chunks; pal: palette line added */
static Layer layer[2];                    /* 0 = FG (plane A), 1 = BG (plane B) */
static int walk;                          /* the layer walked on (collision) */
/* The chunk table, kept from when no window is open: a window saves the
 * plane mappings it covers over the table's start ($FFEC98, the end of what
 * is saved, is $6040 when nothing is), and the game puts the table back when
 * it closes */
static uint8_t chunks[0x2000];            /* 256 chunks: a big map's run on past $7000 */

static uint16_t chunk_word(const Layer *L, int x, int y) {         /* x, y: pixels, wrapped */
    int W = L->w * 32, H = L->h * 32;
    x %= W; if (x < 0) x += W;
    y %= H; if (y < 0) y += H;
    unsigned c = r8(L->layout + (unsigned)((y >> 5) * L->w + (x >> 5)));
    unsigned a = (c * 32 + (unsigned)(((y >> 3) & 3) * 8 + ((x >> 3) & 3) * 2)) & 0x1FFF;
    return (uint16_t)(chunks[a] << 8 | chunks[a + 1]);
}

static int tile_pixel(uint16_t e, int x, int y) {                  /* 0 = transparent, else 1-15 */
    int px = x & 7, py = y & 7;
    if (e & 0x800) px = 7 - px;
    if (e & 0x1000) py = 7 - py;
    uint8_t b = vdp.vram[((unsigned)(e & 0x7FF) * 32 + (unsigned)(py * 4 + px / 2)) & 0xFFFF];
    return px & 1 ? b & 15 : b >> 4;
}

static uint32_t rgb_of(int index) {
    uint16_t c = vdp.cram[index & 63];
    uint32_t r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
    return 0xFF000000u | (r * 36) << 16 | (g * 36) << 8 | (b * 36);
}

/* the map's colour at world pixel (x, y): plane A over plane B, as the game draws them */
static uint32_t map_rgb(int x, int y) {
    for (int k = 0; k < 2; k++) {
        uint16_t e = chunk_word(&layer[k], x, y);
        int p = tile_pixel(e, x, y);
        if (p) return rgb_of((((e >> 13) & 1) + layer[k].pal) * 16 + p);
    }
    return rgb_of(vdp.reg[7] & 63);
}

static int cell_code(int cx, int cy) {
    const Layer *L = &layer[walk];
    int x = cx * 16, y = cy * 16, code = 0;
    if (chunk_word(L, x, y) & 0x4000) code |= 1;
    if (chunk_word(L, x + 8, y) & 0x4000) code |= 2;
    if (chunk_word(L, x, y + 8) & 0x4000) code |= 4;
    if (chunk_word(L, x + 8, y + 8) & 0x4000) code |= 8;
    return code;
}

static int solid(int cx, int cy) {
    int c = cell_code(cx, cy);
    return c == 8 || c == 0xB || c == 0xC;            /* solid, an ice block, a shop counter */
}

/* the map changed: its layers, their palette lines (from what the game drew),
 * and its usual wall front -- for the faces the game never draws */
static unsigned map_key;
static int gen_x, gen_y;                              /* a cell whose 2-cell front stands for the rest */

static void map_update(void) {
    if (r16(0xEC98) <= 0x6040) memcpy(chunks, &m68k_ram[0x6000], sizeof chunks);   /* no window open */
    layer[0].layout = 0xA000; layer[0].w = (int)r8(0xEC64) + 1; layer[0].h = (int)r8(0xEC65) + 1;
    layer[1].layout = 0xB000; layer[1].w = (int)r8(0xEC66) + 1; layer[1].h = (int)r8(0xEC67) + 1;
    walk = r8(USE_BG) ? 1 : 0;
    unsigned key = r16(FIELD_MAP) << 16 ^ (unsigned)layer[walk].w << 8 ^ (unsigned)layer[walk].h ^ r8(USE_BG) << 24;
    /* palette lines: the visible name tables against the chunks under them */
    unsigned nt[2] = {(unsigned)(vdp.reg[2] & 0x38) << 10, (unsigned)(vdp.reg[4] & 7) << 13};
    unsigned cam[2][2] = {{r16(0xEF94), r16(0xEF90)}, {r16(0xEF9C), r16(0xEF98)}};
    for (int k = 0; k < 2; k++) {
        int votes[4] = {0};
        for (int ty = 0; ty < 28; ty += 3)
            for (int tx = 0; tx < 40; tx += 3) {
                int px = (int)cam[k][0] + tx * 8, py = (int)cam[k][1] + ty * 8;
                uint16_t c = chunk_word(&layer[k], px, py);
                if (!(c & 0x7FF)) continue;
                unsigned a = nt[k] + (unsigned)(((py >> 3) & 31) * 64 + ((px >> 3) & 63)) * 2;
                uint16_t v = (uint16_t)(vdp.vram[a & 0xFFFF] << 8 | vdp.vram[(a + 1) & 0xFFFF]);
                if ((v & 0x1FFF) != (c & 0x1FFF)) continue;
                int d = (int)((v >> 13) & 3) - (int)((c >> 13) & 1);
                if (d >= 0 && d < 4) votes[d]++;
            }
        int best = 0;
        for (int d = 1; d < 4; d++) if (votes[d] > votes[best]) best = d;
        layer[k].pal = best;
    }
    if (key == map_key) return;
    map_key = key;
    /* the usual front: the commonest 16x32 block over a floor (a cheap hash) */
    int W = layer[walk].w * 2, H = layer[walk].h * 2;
    static uint32_t hashes[128 * 128];
    static uint16_t cells[128 * 128];
    int n = 0;
    for (int cy = 1; cy < H - 1 && n < 128 * 128; cy++)
        for (int cx = 0; cx < W && n < 128 * 128; cx++) {
            if (!solid(cx, cy) || solid(cx, cy + 1) || !solid(cx, cy - 1)) continue;
            uint32_t h = 2166136261u;
            for (int y = 0; y < 32; y += 2)
                for (int x = 0; x < 16; x += 2) {
                    int px = cx * 16 + x, py = (cy - 1) * 16 + y;
                    uint16_t e = chunk_word(&layer[walk], px, py);
                    h = (h ^ (uint32_t)(e & 0x1FFF)) * 16777619u;
                }
            hashes[n] = h;
            cells[n] = (uint16_t)(cy << 8 | cx);
            n++;
        }
    int best = -1, best_count = 0;
    for (int i = 0; i < n; i++) {
        int c = 0;
        for (int j = 0; j < n; j++) if (hashes[j] == hashes[i]) c++;
        if (c > best_count) { best_count = c; best = i; }
        if (best_count > n / 2) break;
    }
    if (best >= 0) { gen_x = cells[best] & 0xFF; gen_y = cells[best] >> 8; }
    else { gen_x = 0; gen_y = 1; }
}

/* ------------------------------------------------------------------ the camera */
static float cam_ang;
static unsigned cam_map = 0xFFFFFFFFu;

static float facing_angle(unsigned f) {
    return f == 0 ? 1.5707963f : f == 4 ? -1.5707963f : f == 8 ? 0.0f : 3.1415927f;
}

/* ------------------------------------------------------------------ drawing */
#define FOV    1.15f                       /* radians, ~66 degrees */
#define WALL_H 2.0f                        /* cells: the usual 2-cell front */
#define EYE    0.9f

static uint32_t shade(uint32_t c, float k) {
    if (k >= 1.0f) return c;
    uint32_t r = (uint32_t)((float)(c >> 16 & 255) * k), g = (uint32_t)((float)(c >> 8 & 255) * k), b = (uint32_t)((float)(c & 255) * k);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

/* ------------------------------------------------------------------ the field's objects */
/* Field_Objects_Memory ($FFC000): 64 objects of $40 bytes, the party in the
 * first five (left out: it is their view), then the NPCs, chests and the rest.
 * Each draws its current frame -- mappings (+$C): a count less one, a byte,
 * then 6-byte pieces (y, size, a tile word, x, x when mirrored) -- from its
 * position (+$30, +$34), as Field_FillSpriteAttributes builds it: its tiles
 * from art_tile (+$16) in VRAM, or (render flag bit 5) streamed from ROM at
 * art_ptr (+$18), read from there here -- an object off the screen hasn't had
 * its art sent. The game builds sprites only for what is on screen; the view
 * shows every object, near or far. */
#define OBJ_FIRST 5
#define SPR_SCALE (1.0f / 24.0f)                /* world units (cells) per sprite pixel */
#define OBJ_MAX   128                           /* the largest picture drawn, px */

typedef struct { int x, y, w, h; uint16_t attr; uint32_t src; } Piece;   /* src: its tiles in ROM, or 0: VRAM */
typedef struct { float px, py, dx, dy, plx, ply, horizon, proj; } View;
static float zbuf[VDP_MAX_W * 2];                /* each column's wall distance */

static unsigned rom8(uint32_t a) { return a < m68k_rom_size ? m68k_rom[a] : 0; }
static uint32_t rom32(uint32_t a) { return (uint32_t)rom8(a) << 24 | rom8(a + 1) << 16 | rom8(a + 2) << 8 | rom8(a + 3); }

/* The frame FieldObj_Animate would show for facing f (0 down, 4 up, 8 right,
 * $C left): mappings_addr (+8) holds a script for each facing -- a count (bit
 * 7: a duration for each frame follows), a duration, then the frames -- and
 * mappings_idx (+$10) is the frame it is on. 0: none. */
static uint32_t anim_frame(unsigned a, unsigned f) {
    uint32_t tab = (uint32_t)r16(a + 8) << 16 | r16(a + 0xA);
    if ((tab & 1) || tab + 16 > m68k_rom_size || f > 0xC || (f & 3)) return 0;
    uint32_t s = rom32(tab + f);
    unsigned n = rom8(s) & 0x7F, i = r8(a + 0x10);
    if ((s & 1) || s + 8 > m68k_rom_size || !n) return 0;
    if (i >= n) i = 0;
    if (rom8(s) & 0x80) { unsigned d = i * 4 + n; return rom32(s + 1 + (d | 1)); }
    return rom32(s + 2 + i * 4);
}

/* People, by their routine (FieldObjectsJmpTbl, the object ID & $7FFC): only
 * they turn with the camera. Others keep the game's frame -- a chest's
 * "facing" is whether it is open */
static const uint16_t PEOPLE[][2] = {
    {0x004, 0x02C},                                         /* the party's own, as NPCs in scenes */
    {0x038, 0x070}, {0x088, 0x08C}, {0x09C, 0x09C},         /* NPC types 1-14, Alys, Hahn, Rune; penguins */
    {0x0C0, 0x0CC}, {0x0D4, 0x0D4}, {0x0DC, 0x0DC},         /* NPC types 16-36, less those lying down */
    {0x0E4, 0x0F8}, {0x100, 0x11C},
    {0x128, 0x14C},                                         /* Aiedo's townsfolk */
    {0x190, 0x194}, {0x1A4, 0x1A4}, {0x1F0, 0x1F0}, {0x214, 0x214},   /* scripted: Rune, Alys, Wren */
};

static int is_person(unsigned a) {
    unsigned r = r16(a) & 0x7FFC;
    for (size_t i = 0; i < sizeof PEOPLE / sizeof PEOPLE[0]; i++)
        if (r >= PEOPLE[i][0] && r <= PEOPLE[i][1]) return 1;
    return 0;
}

/* The frame to show from where the camera stands: the game draws everyone as
 * seen from the south, so a person is turned by the angle from the camera to
 * them -- whoever faces the camera shows their front */
static uint32_t obj_frame(unsigned a, float rx, float ry) {
    uint32_t m = (uint32_t)r16(a + 0xC) << 16 | r16(a + 0xE);
    unsigned f = r16(a + 6);
    if (!is_person(a) || !anim_frame(a, f) || anim_frame(a, f) != m) return m;   /* not one of those */
    static const int CW[4] = {2, 0, 1, 3};                      /* facing/4 -> clockwise from north */
    static const unsigned FACING[4] = {4, 8, 0, 0xC};           /* and back */
    int look = ((int)lroundf(atan2f(ry, rx) / 1.5707963f) + 1 + 4) & 3;   /* the camera looks at it this way */
    uint32_t t = anim_frame(a, FACING[(CW[f >> 2] - look + 4) & 3]);
    return t ? t : m;
}

static int obj_pieces(unsigned a, uint32_t m, Piece *p, int max) {
    unsigned rf = r8(a + 2);
    if ((rf & 3) || m < 0x200 || m + 8 > m68k_rom_size) return 0;   /* hidden, fixed to the screen, or no frame */
    int n = (int)rom8(m) + 1;
    if (n > max) n = max;
    unsigned art = r16(a + 0x16), hi0 = (art >> 8 | r8(a + 0x13) | (rf & 0x10 ? 0x80 : 0)) & 0xFF;
    uint32_t artp = (uint32_t)r16(a + 0x18) << 16 | r16(a + 0x1A);
    unsigned streamed = 0;
    m += 2;
    for (int i = 0; i < n; i++, m += 6) {
        unsigned size = rom8(m + 1), mh = rom8(m + 2), ml = rom8(m + 3);
        p[i].y = (int8_t)rom8(m);
        p[i].x = (int8_t)rom8(m + 4);
        p[i].w = (int)((size >> 2) & 3) + 1;
        p[i].h = (int)(size & 3) + 1;
        if (rf & 0x20) {                                        /* streamed: its art from ROM */
            unsigned hi = (hi0 & ~8u) | (mh & 8);
            p[i].attr = (uint16_t)((hi << 8 | (art & 0xFF)) + streamed);
            p[i].src = artp + ((mh << 8 | ml) & 0x7FF) * 32;
            streamed += (unsigned)(p[i].w * p[i].h);
            if (rf & 0x80) { p[i].attr ^= 0x800; p[i].x = (int8_t)rom8(m + 5); }
        } else {                                                 /* art_tile + the piece's tile word */
            unsigned hi = (((art >> 8) + mh) & 0xFF) | r8(a + 0x13), lo = (art & 0xFF) + ml;
            if (rf & 0x10) hi |= 0x80;
            if (lo > 0xFF) hi = (hi + 1) & 0xFF;
            p[i].attr = (uint16_t)(hi << 8 | (lo & 0xFF));
            p[i].src = 0;
        }
    }
    return n;
}

/* a piece's pixel at (x, y) as drawn (flips applied): 0 = clear */
static int piece_pixel(const Piece *p, int x, int y) {
    if (p->attr & 0x800) x = p->w * 8 - 1 - x;
    if (p->attr & 0x1000) y = p->h * 8 - 1 - y;
    unsigned t = (unsigned)((x >> 3) * p->h + (y >> 3)), o = (unsigned)((y & 7) * 4 + (x & 7) / 2);
    unsigned b = p->src ? rom8(p->src + t * 32 + o) : vdp.vram[(((p->attr & 0x7FFu) + t) * 32 + o) & 0xFFFF];
    return (int)(x & 1 ? b & 15 : b >> 4);
}

static uint32_t r32(unsigned a) { return (uint32_t)r16(a) << 16 | r16(a + 2); }

/* an object placed in the world: its sprite where FieldObj_CalcSpritePos puts
 * it, its position less the camera (the walked layer's, and its step). A
 * window's arrow and the like are placed on the screen itself, and fail this */
static int in_world(unsigned a) {
    unsigned bg = r8(USE_BG) ? 8u : 0u;
    uint32_t ex = (r32(a + 0x30) + 0x800000u - r32(0xEF94 + bg) - r32(0xEC50 + bg)) >> 16;
    uint32_t ey = (r32(a + 0x34) + 0x800000u - r32(0xEF90 + bg) - r32(0xEC54 + bg)) >> 16;
    return abs((int16_t)(uint16_t)(r16(a + 0x2C) - ex)) <= 4 && abs((int16_t)(uint16_t)(r16(a + 0x2E) - ey)) <= 4;
}

/* an object's picture: its pieces composed, the first on top, clipped to max
 * x max; colours, 0 = clear. *x0, *y0: its top-left from the object's position */
static int obj_image(const Piece *p, int n, uint32_t *img, int max, int *x0, int *y0, int *w, int *h) {
    int ax = 1 << 20, ay = 1 << 20, bx = -(1 << 20), by = -(1 << 20);
    for (int k = 0; k < n; k++) {
        if (p[k].x < ax) ax = p[k].x;
        if (p[k].y < ay) ay = p[k].y;
        if (p[k].x + p[k].w * 8 > bx) bx = p[k].x + p[k].w * 8;
        if (p[k].y + p[k].h * 8 > by) by = p[k].y + p[k].h * 8;
    }
    if (bx - ax > max) bx = ax + max;
    if (by - ay > max) ay = by - max;                    /* keep the feet */
    int iw = bx - ax, ih = by - ay;
    if (iw <= 0 || ih <= 0) return 0;
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            uint32_t c = 0;
            for (int k = 0; k < n && !c; k++) {
                int qx = x + ax - p[k].x, qy = y + ay - p[k].y;
                if (qx < 0 || qy < 0 || qx >= p[k].w * 8 || qy >= p[k].h * 8) continue;
                int pix = piece_pixel(&p[k], qx, qy);
                if (pix) c = rgb_of((int)((p[k].attr >> 13) & 3) * 16 + pix);
            }
            img[y * iw + x] = c;
        }
    *x0 = ax; *y0 = ay; *w = iw; *h = ih;
    return 1;
}

/* Objects come in two kinds here. One on a solid cell is part of a wall --
 * the Machine Center's elevator doors, say: the game draws it over the wall's
 * front, so it is painted there, onto the face the view takes from that art
 * (decals). The rest stand on the floor, pictures turned to the camera
 * (billboards), drawn after the walls, far to near, each column behind the
 * nearest wall left alone. */
#define DECAL_MAX 96
typedef struct { int x, y, w, h; uint32_t px[DECAL_MAX * DECAL_MAX]; } Decal;   /* x, y: world pixels */
static Decal decal[8];
static int ndecal;
typedef struct { unsigned a; float tx, ty, rx, ry; } Vis;      /* a billboard: tx across, ty ahead */
static Vis vis[64];
static int nvis;

static void objects_prepare(const View *v) {
    nvis = ndecal = 0;
    float inv = 1.0f / (v->plx * v->dy - v->dx * v->ply);
    float mw = (float)(layer[walk].w * 2), mh = (float)(layer[walk].h * 2);
    for (int s = OBJ_FIRST; s < 64; s++) {
        unsigned a = LEADER + (unsigned)s * 0x40;
        if (!r16(a) || (r8(a + 2) & 3) || !in_world(a)) continue;
        int ox = (int)r16(a + 0x30), oy = (int)r16(a + 0x34);
        if (solid((ox + 8) >> 4, (oy + 24) >> 4)) {             /* on a wall */
            Piece p[16];
            Decal *d = &decal[ndecal];
            int n = ndecal < 8 ? obj_pieces(a, r32(a + 0xC), p, 16) : 0, x0, y0;
            if (n && obj_image(p, n, d->px, DECAL_MAX, &x0, &y0, &d->w, &d->h)) {
                d->x = ox + x0;
                d->y = oy + y0;
                ndecal++;
            }
            continue;
        }
        float rx = ((float)ox + 8.0f) / 16.0f - v->px, ry = ((float)oy + 24.0f) / 16.0f - v->py;
        if (rx > mw * 0.5f) rx -= mw; else if (rx < -mw * 0.5f) rx += mw;     /* the map wraps, as the walls do */
        if (ry > mh * 0.5f) ry -= mh; else if (ry < -mh * 0.5f) ry += mh;
        float ty = inv * (-v->ply * rx + v->plx * ry);
        if (ty < 0.3f || ty > 48.0f) continue;
        vis[nvis].a = a;
        vis[nvis].rx = rx;
        vis[nvis].ry = ry;
        vis[nvis].tx = inv * (v->dy * rx - v->dx * ry);
        vis[nvis].ty = ty;
        nvis++;
    }
    for (int i = 1; i < nvis; i++)                                /* far to near */
        for (int j = i; j > 0 && vis[j].ty > vis[j - 1].ty; j--) { Vis t = vis[j]; vis[j] = vis[j - 1]; vis[j - 1] = t; }
}

/* an object painted over the wall art at world pixel (x, y), or 0 */
static uint32_t decal_rgb(int x, int y) {
    int W = layer[walk].w * 32, H = layer[walk].h * 32;
    for (int i = 0; i < ndecal; i++) {
        const Decal *d = &decal[i];
        int u = ((x - d->x) % W + W) % W, v = ((y - d->y) % H + H) % H;
        if (u < d->w && v < d->h && d->px[v * d->w + u]) return d->px[v * d->w + u];
    }
    return 0;
}

static void draw_billboards(uint32_t *out, int stride, int RW, int RH, const View *v) {
    static uint32_t img[OBJ_MAX * OBJ_MAX];
    for (int i = 0; i < nvis; i++) {
        Piece p[16];
        int n = obj_pieces(vis[i].a, obj_frame(vis[i].a, vis[i].rx, vis[i].ry), p, 16), x0, y0, iw, ih;
        if (!n || !obj_image(p, n, img, OBJ_MAX, &x0, &y0, &iw, &ih)) continue;
        float ty = vis[i].ty, ppx = SPR_SCALE * v->proj / ty;    /* screen pixels per sprite pixel */
        float fog = 1.0f / (1.0f + ty * 0.12f);
        /* standing on the floor: the picture's foot on it, its x as the game places it */
        float cx = (float)RW * 0.5f * (1.0f + vis[i].tx / ty);
        float left = cx + (float)(x0 - 8) * ppx, bot = v->horizon + EYE * v->proj / ty, top = bot - (float)ih * ppx;
        int c0 = left < 0 ? 0 : (int)left, c1 = (int)ceilf(left + (float)iw * ppx), r0 = top < 0 ? 0 : (int)top, r1 = (int)ceilf(bot);
        if (c1 > RW) c1 = RW;
        if (r1 > RH) r1 = RH;
        for (int c = c0; c < c1; c++) {
            if (ty >= zbuf[c]) continue;                         /* behind a wall */
            int sx = (int)(((float)c + 0.5f - left) / ppx);
            if (sx < 0 || sx >= iw) continue;
            for (int r = r0; r < r1; r++) {
                int sy = (int)(((float)r + 0.5f - top) / ppx);
                if (sy < 0 || sy >= ih) continue;
                uint32_t col = img[sy * iw + sx];
                if (col) out[(size_t)r * (size_t)stride + (size_t)c] = shade(col, fog);
            }
        }
    }
}

static void render(uint32_t *out, int stride, int RW, int RH) {
    float px = ((float)r16(LEADER + 0x30) + 8.0f) / 16.0f, py = ((float)r16(LEADER + 0x34) + 24.0f) / 16.0f;
    float target = facing_angle(r16(LEADER + 6));
    unsigned mapnow = r16(FIELD_MAP);
    if (mapnow != cam_map) { cam_map = mapnow; cam_ang = target; }
    float d = target - cam_ang;
    while (d > 3.1415927f) d -= 6.2831853f;
    while (d < -3.1415927f) d += 6.2831853f;
    cam_ang += d * 0.22f;                           /* a quarter turn in about 0.2 s */
    float dx = cosf(cam_ang), dy = sinf(cam_ang), k = tanf(FOV / 2.0f);
    float plx = -dy * k, ply = dx * k;
    float horizon = (float)RH * 0.5f, proj = ((float)RW * 0.5f) / k;
    if (RW > (int)(sizeof zbuf / sizeof zbuf[0])) RW = (int)(sizeof zbuf / sizeof zbuf[0]);
    View view = {px, py, dx, dy, plx, ply, horizon, proj};
    objects_prepare(&view);
    for (int x = 0; x < RW; x++) {
        float camx = 2.0f * ((float)x + 0.5f) / (float)RW - 1.0f;
        float rdx = dx + plx * camx, rdy = dy + ply * camx;
        int mx = (int)floorf(px), my = (int)floorf(py);
        float ddx = rdx != 0.0f ? fabsf(1.0f / rdx) : 1e30f, ddy = rdy != 0.0f ? fabsf(1.0f / rdy) : 1e30f;
        int sx = rdx < 0 ? -1 : 1, sy = rdy < 0 ? -1 : 1;
        float stx = rdx < 0 ? (px - (float)mx) * ddx : ((float)mx + 1.0f - px) * ddx;
        float sty = rdy < 0 ? (py - (float)my) * ddy : ((float)my + 1.0f - py) * ddy;
        int side = 0, hit = 0;
        for (int i = 0; i < 64; i++) {
            if (stx < sty) { stx += ddx; mx += sx; side = 0; }
            else { sty += ddy; my += sy; side = 1; }
            if (solid(mx, my)) { hit = 1; break; }
        }
        float dist = side == 0 ? stx - ddx : sty - ddy;
        if (dist < 0.05f) dist = 0.05f;
        zbuf[x] = hit ? dist : 1e30f;
        float top = horizon - (WALL_H - EYE) / dist * proj, bot = horizon + EYE / dist * proj;
        int y0 = top < 0 ? 0 : (int)top, y1 = bot > (float)RH ? RH : (int)bot;
        if (!hit) y0 = y1 = (int)horizon;
        float u = side == 0 ? py + dist * rdy : px + dist * rdx;
        u -= floorf(u);
        if ((side == 0 && rdx < 0) || (side == 1 && rdy > 0)) u = 1.0f - u;
        int tx = (int)(u * 16.0f);
        if (tx > 15) tx = 15;
        float fog = 1.0f / (1.0f + dist * 0.12f) * (side == 0 ? 0.82f : 1.0f);
        /* the face the game draws (a wall over a floor, seen from the south);
         * a face it never draws takes the front of its own wall, found below
         * it, or else the map's usual front */
        int fx = mx, fy = my, own = 0;
        if (hit) {
            for (int k2 = 0; k2 < 4; k2++) {
                if (!solid(mx, my + k2)) break;
                if (!solid(mx, my + k2 + 1)) { fy = my + k2; own = 1; break; }
            }
        }
        int fc = 1;
        if (own) while (fc < 2 && solid(fx, fy - fc)) fc++;
        for (int y = y0; y < y1; y++) {
            float v = ((float)y + 0.5f - top) / (bot - top);
            uint32_t c;
            if (own) {
                int wx = fx * 16 + tx, wy = (fy + 1 - fc) * 16 + (int)(v * (float)fc * 16.0f);
                c = ndecal ? decal_rgb(wx, wy) : 0;
                if (!c) c = map_rgb(wx, wy);
            } else c = map_rgb(gen_x * 16 + tx, (gen_y - 1) * 16 + (int)(v * 32.0f));
            out[(size_t)y * (size_t)stride + (size_t)x] = shade(c, fog);
        }
        for (int y = y1 > (int)horizon + 1 ? y1 : (int)horizon + 1; y < RH; y++) {   /* the floor: the map */
            float rd = EYE * proj / ((float)y + 0.5f - horizon);
            int wx = (int)floorf((px + rd * rdx) * 16.0f), wy = (int)floorf((py + rd * rdy) * 16.0f);
            out[(size_t)y * (size_t)stride + (size_t)x] = shade(map_rgb(wx, wy), 1.0f / (1.0f + rd * 0.12f));
        }
        for (int y = 0; y < y0; y++) {                  /* the ceiling: dark */
            uint32_t g = 10u + (uint32_t)(10 * y) / (uint32_t)(horizon > 1 ? horizon : 1);
            out[(size_t)y * (size_t)stride + (size_t)x] = 0xFF000000u | g << 16 | (g - 2) << 8 | (g + 4);
        }
    }
    draw_billboards(out, stride, RW, RH, &view);
}

/* A battle that starts in a first-person dungeon is fought there: the view the
 * field last showed stands in for the battle's own backdrop (plane B), the
 * enemies and every effect in front of it, the party's own picture left out
 * (it is their view; the battle menus show them). The dungeon's tiles are gone
 * from VRAM by then, so the view is the one kept from the field. */
static uint32_t snap[VDP_MAX_W * 2 * VDP_MAX_H * 2];
static int snap_w, snap_h, from_dungeon;

int fpview_in_battle(void) {
    unsigned m = r16(GAME_MODE);
    return enabled && from_dungeon && snap_w && m == 0x14;
}

static int party_cell(uint16_t e) {                  /* the party's art in battle (battleui.c) */
    int t = e & 0x7FF;
    return t >= 0x180 && t < 0x280;
}

/* which layer the VDP shows at a pixel: 0 backdrop, 1 B, 2 A, 3 sprite */
static int top_layer(const VdpLine *l, int x) {
    int s = l->sprite[x], a = l->plane_a[x], b = l->plane_b[x];
    if (s && (s & 0x80)) return 3;
    if (a && (a & 0x80)) return 2;
    if (b && (b & 0x80)) return 1;
    if (s) return 3;
    if (a) return 2;
    if (b) return 1;
    return 0;
}

static void battle_compose(const VdpLine *lines, const uint32_t *game, int w, int h, uint32_t *out, int stride, int scale) {
    for (int y = 0; y < h * scale; y++) {                /* the field's view, scaled */
        const uint32_t *src = snap + (size_t)(y * snap_h / (h * scale)) * (size_t)snap_w;
        uint32_t *dst = out + (size_t)y * (size_t)stride;
        for (int x = 0; x < w * scale; x++) dst[x] = src[x * snap_w / (w * scale)];
    }
    for (int y = 0; y < h; y++) {                        /* the battle over it */
        const VdpLine *l = &lines[y];
        for (int x = 0; x < w; x++) {
            int t = top_layer(l, x);
            if (t <= 1 || (t == 2 && party_cell(l->plane_a_tile[x]))) continue;
            uint32_t c = game[(size_t)y * VDP_MAX_W + (size_t)x];
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    out[(size_t)(y * scale + sy) * (size_t)stride + (size_t)(x * scale + sx)] = c;
        }
    }
}

int fpview_compose(const VdpLine *lines, const uint32_t *game, int w, int h, uint32_t *out, int stride, int scale) {
    unsigned mode = r16(GAME_MODE);
    if (mode == MODE_FIELD) from_dungeon = fpview_active();
    else if (mode == 0x14 && enabled && from_dungeon && snap_w) {
        shown = 0;
        last_ui = 1;
        battle_compose(lines, game, w, h, out, stride, scale);
        return 1;
    }
    shown = fpview_active();
    if (!shown) { last_ui = 0; return 0; }
    map_update();
    /* drawn at up to twice the game's size, then scaled up */
    int s2 = scale < 2 ? scale : 2;
    static uint32_t buf[VDP_MAX_W * 2 * VDP_MAX_H * 2];
    int RW = w * s2, RH = h * s2;
    render(buf, RW, RW, RH);
    memcpy(snap, buf, sizeof(uint32_t) * (size_t)RW * (size_t)RH);   /* for a battle here */
    snap_w = RW;
    snap_h = RH;
    for (int y = 0; y < h * scale; y++) {
        const uint32_t *src = buf + (size_t)(y * s2 / scale) * (size_t)RW;
        uint32_t *dst = out + (size_t)y * (size_t)stride;
        for (int x = 0; x < w * scale; x++) dst[x] = src[x * s2 / scale];
    }
    /* the game's windows -- whatever it has drawn on plane A that isn't the
     * map there (windows, text, portraits, menus; its cursors with them) --
     * on top. Plane A is a 64 x 32-cell ring of the map around the camera:
     * each cell against the map it holds, then each pixel's cell from the
     * VDP's own scroll (full-screen scroll, as the field uses) */
    static uint8_t ui_cell[32][64], art[32][64];
    int camx = (int)r16(0xEF94) + 160, camy = (int)r16(0xEF90) + 112;
    unsigned nta = (unsigned)(vdp.reg[2] & 0x38) << 10;
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 64; c++) {
            int mx = c * 8 + (int)lroundf((float)(camx - c * 8) / 512.0f) * 512;   /* the nearest map x in this ring cell */
            int my = r * 8 + (int)lroundf((float)(camy - r * 8) / 256.0f) * 256;
            unsigned a = nta + (unsigned)(r * 64 + c) * 2;
            uint16_t e = (uint16_t)(vdp.vram[a & 0xFFFF] << 8 | vdp.vram[(a + 1) & 0xFFFF]);
            ui_cell[r][c] = ((e ^ chunk_word(&layer[0], mx, my)) & 0x7FF) != 0;
            art[r][c] = (e & 0x7FF) >= 0x55C;
        }
    /* a window is a group of cells with window art in it -- its frame, text
     * or portrait, all from tile $55C up -- while a platform or a door the
     * game draws on plane A itself is map art (4-connected; the ring wraps) */
    static uint8_t seen[32][64];
    static int16_t stack[32 * 64][2];
    memset(seen, 0, sizeof seen);
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 64; c++) {
            if (!ui_cell[r][c] || seen[r][c]) continue;
            int n = 0, top = 0, window = 0;
            static int16_t members[32 * 64][2];
            stack[top][0] = (int16_t)r; stack[top][1] = (int16_t)c; top++;
            seen[r][c] = 1;
            while (top) {
                top--;
                int rr = stack[top][0], cc = stack[top][1];
                members[n][0] = (int16_t)rr; members[n][1] = (int16_t)cc; n++;
                window |= art[rr][cc];
                static const int DR[4] = {1, -1, 0, 0}, DC[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; k++) {
                    int r2 = (rr + DR[k]) & 31, c2 = (cc + DC[k]) & 63;
                    if (ui_cell[r2][c2] && !seen[r2][c2]) { seen[r2][c2] = 1; stack[top][0] = (int16_t)r2; stack[top][1] = (int16_t)c2; top++; }
                }
            }
            if (!window) for (int i = 0; i < n; i++) ui_cell[members[i][0]][members[i][1]] = 0;
        }
    unsigned hsa = (unsigned)(vdp.reg[13] & 0x3F) << 10;
    int hs = (int16_t)(vdp.vram[hsa & 0xFFFF] << 8 | vdp.vram[(hsa + 1) & 0xFFFF]);
    int vs = vdp.vsram[0] & 0x3FF;
    int ui = 0;
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        int row = ((y + vs) >> 3) & 31;
        for (int x = 0; x < w; x++) {
            if (!l->win_in[x] && !ui_cell[row][((x - hs) >> 3) & 63]) continue;
            ui = 1;
            uint32_t c = game[(size_t)y * VDP_MAX_W + (size_t)x];
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    out[(size_t)(y * scale + sy) * (size_t)stride + (size_t)(x * scale + sx)] = c;
        }
    }
    last_ui = ui;
    return 1;
}

/* ------------------------------------------------------------------ the d-pad */
uint8_t fpview_remap_pad(uint8_t pad) {
    static uint8_t held_rel, held_abs;
    uint8_t dirs = pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT), rest = pad & (uint8_t)~(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    if (!shown || last_ui) { held_rel = 0; return pad; }     /* menus and dialogue: the pad as it is */
    if (!dirs) { held_rel = 0; return rest; }
    uint8_t rel = dirs & PAD_UP ? PAD_UP : dirs & PAD_LEFT ? PAD_LEFT : dirs & PAD_RIGHT ? PAD_RIGHT : PAD_DOWN;
    if (rel != held_rel) {
        static const uint8_t ABS[4] = {PAD_DOWN, PAD_UP, PAD_RIGHT, PAD_LEFT};     /* facing 0, 4, 8, $C */
        static const int CW[4] = {3, 2, 0, 1};                  /* turning right: down->left, up->right, right->down, left->up */
        static const int CCW[4] = {2, 3, 1, 0};                 /* turning left */
        static const int BACK[4] = {1, 0, 3, 2};
        int f = (int)(r16(LEADER + 6) >> 2) & 3;
        int a = rel == PAD_UP ? f : rel == PAD_DOWN ? BACK[f] : rel == PAD_LEFT ? CCW[f] : CW[f];
        held_rel = rel;
        held_abs = ABS[a];
    }
    return rest | held_abs;
}
