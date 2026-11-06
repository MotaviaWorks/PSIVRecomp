/*
 * Widescreen margins in the field: see widemap.h.
 *
 * What the game keeps (names from the disassembly):
 *   Map_Layout_FG $FFA000 / _BG $FFB000   a byte per 32x32 chunk, rows of
 *                                          Map_Row_Size + 1 ($FFEC64 / $EC66;
 *                                          Map_Column_Size + 1 rows, $EC65 / $EC67)
 *   Chunk_Table   $FF6000                  16 name-table words a chunk (4x4
 *                                          cells), bit 14 = collision
 *   Camera_X/Y_Pos_FG $FFEF94 / $EF90, _BG $EF9C / $EF98 (16.16)
 *   BG_Alternate_Color_Flag $FFEC4F        plane B drawn in palette line 0, low priority
 *   Field_Map_Index $FFEC28                0 and 1: the world maps
 * and what it draws (RefreshPlaneFG/BG, RefreshTilesAsYouMoveFG/BG): plane A
 * from the FG layer and plane B from the BG layer, each 64 x 32 cells; world
 * chunk (cx, cy) goes to the plane's chunk slot (cx & 15, cy & 7), its words
 * ANDed with $BFFF (plane B with the flag: $1FFF). On a world map the chunk
 * numbers wrap at 128 x 32 (the layout holds a band of 32 rows that the game
 * refills as the camera moves; Map_Row_Size is the stride); elsewhere they
 * wrap at the map's size.
 *
 * The VDP shows the screen's top-left at the camera: plane pixel
 * (x + cam_x) & 511, (y + cam_y) & 255. A margin pixel's place in the map is
 * the one that falls on the same plane pixel nearest the camera (the RAM
 * camera may already be a frame ahead of the scroll the VDP shows).
 *
 * A window saves the plane mappings it covers over the chunk table's start
 * ($FFEC98, the end of what is saved, is $6040 when nothing is), so the table
 * is kept from frames without one. Each frame the planes on screen are
 * checked against the map; the margins come from the map only while most of
 * the screen agrees (a window covers some of it; a scene that uses the planes
 * for something else agrees nowhere).
 */
#include "widemap.h"
#include "m68k.h"
#include "vdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE   0xEF00
#define FIELD_MAP   0xEC28
#define ALT_COLOR   0xEC4F
#define SAVE_END    0xEC98
#define CHUNKS      0x6000

#define OUTSIDE     (-3)                  /* beyond the map's edge */
/* the field's windows draw tiles from here up (portraits $55C, text $580,
 * frames $6E9); the maps' art lies below */
#define WINDOW_TILES 0x55C

typedef struct {
    int on;                               /* margins from the map this frame */
    unsigned layout;                      /* the layer's layout */
    int stride;                           /* layout bytes a row */
    int wrap_w, wrap_h;                   /* chunks: where the game's chunk numbers wrap */
    int endless;                          /* a world map: always wrap */
    uint16_t mask;                        /* bits kept from a chunk word */
    int cam_x, cam_y;                     /* the camera, from RAM */
} Layer;

static Layer layer[2];                    /* 0 = plane A (FG), 1 = plane B (BG) */
static uint8_t chunks[0x2000];            /* the chunk table as last seen whole (see widemap_frame) */
static int chunks_ok;
static const uint8_t *table = chunks;     /* the table map_entry reads */
static int edge_mode = 1;                 /* widemap_set_edge */
static int enabled = 1;                   /* widemap_set_enabled */
static int logging = -1;                  /* PSIV_WIDEMAP_LOG=1: per frame, how the planes and margins compare */
static unsigned long long frames;

void widemap_set_edge(int mode) { edge_mode = mode; }
void widemap_set_enabled(int on) { enabled = on; }

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }

static int wrapi(int v, int n) { v %= n; return v < 0 ? v + n : v; }

/* PSIV_WIDEMAP_LOG: objects (Field_Objects_Memory $FFC000, 64 x $40; +0 the
 * routine, +$2C/$2E the sprite position + 128) that start up on screen */
static void log_objects(void) {
    static uint16_t was[64];
    for (int i = 5; i < 64; i++) {
        unsigned o = 0xC000 + (unsigned)i * 0x40, r = r16(o);
        int sx = (int)(int16_t)r16(o + 0x2C) - 128, sy = (int)(int16_t)r16(o + 0x2E) - 128;
        if (r && !was[i])
            printf("widemap %llu: object %d starts (routine %04X) at %d,%d%s\n", frames, i, r, sx, sy,
                   sx < 0 || sx >= 320 ? " -- outside the 320 view" : "");
        was[i] = (uint16_t)r;
    }
}

/* the name-table word the game draws at map pixel (X, Y), or OUTSIDE */
static int map_entry(const Layer *L, int X, int Y) {
    if (L->endless) {
        X = wrapi(X, L->wrap_w * 32);
        Y = wrapi(Y, L->wrap_h * 32);
    } else if (X < 0 || Y < 0 || X >= L->wrap_w * 32 || Y >= L->wrap_h * 32) {
        if (!edge_mode) return OUTSIDE;
        /* the edge chunk repeated outwards */
        if (X < 0) X = wrapi(X, 32);
        else if (X >= L->wrap_w * 32) X = (L->wrap_w - 1) * 32 + wrapi(X, 32);
        if (Y < 0) Y = wrapi(Y, 32);
        else if (Y >= L->wrap_h * 32) Y = (L->wrap_h - 1) * 32 + wrapi(Y, 32);
    }
    int cx = X >> 5, cy = Y >> 5;
    unsigned c = r8(L->layout + (unsigned)(cy * L->stride + cx));
    unsigned a = (c * 32 + (unsigned)(((Y >> 3) & 3) * 8 + ((X >> 3) & 3) * 2)) & 0x1FFF;
    return (int)((unsigned)(table[a] << 8 | table[a + 1]) & L->mask);
}

/* the map pixel shown at plane pixel (px, py) for screen pixel (x, y) */
static void map_xy(const Layer *L, int x, int y, int px, int py, int *X, int *Y) {
    int nx = L->cam_x + x, ny = L->cam_y + y;
    int dx = (px - nx) & 511, dy = (py - ny) & 255;
    if (dx >= 256) dx -= 512;
    if (dy >= 128) dy -= 256;
    *X = nx + dx;
    *Y = ny + dy;
}

/* plane A rows the game has filled across the whole 4:3 view with one entry
 * that isn't the map's -- the white bars closing in as a battle starts
 * (FieldRoutine_Battle2 paints only the 40 columns it shows): -1 for none.
 * The margins carry the fill on, so the bars cross the whole picture. */
static int fill_a[32];

static int margin_entry(int plane, int x, int y, int px, int py) {
    if (plane == 0 && fill_a[(py >> 3) & 31] >= 0) return fill_a[(py >> 3) & 31];
    const Layer *L = &layer[plane ? 1 : 0];
    if (!L->on) return -1;
    int X, Y;
    map_xy(L, x, y, px, py, &X, &Y);
    int e = map_entry(L, X, Y);
    return e == OUTSIDE ? -2 : e;
}

/* how much of the screen (cells, at their centres) agrees with the map:
 * n cells compared, `match` of them the same; windows' cells left out */
static void compare(int plane, const Layer *L, int *n, int *match) {
    *n = *match = 0;
    int mw = vdp_width(), h = vdp_height();
    for (int y = 4; y < h; y += 8)
        for (int x = 4; x < mw; x += 8) {
            int px, py, X, Y;
            vdp_plane_xy(plane, x, y, &px, &py);
            map_xy(L, x, y, px, py, &X, &Y);
            int m = map_entry(L, X, Y);
            if (m == OUTSIDE) continue;
            uint16_t v = vdp_plane_entry(plane, px, py);
            if (plane == 0 && (v & 0x7FF) >= WINDOW_TILES) continue;   /* a window's cell */
            (*n)++;
            if (v == (uint16_t)m) (*match)++;
        }
}

static int agrees(int n, int match) { return n >= 200 && match * 5 >= n * 4; }   /* 80%: doors, animated tiles */

/* PSIV_WIDEMAP_LOG: the margins as the planes have them, against the map */
static void margin_diffs(int plane, const Layer *L, int *ldiff, int *rdiff, int *count) {
    *ldiff = *rdiff = *count = 0;
    int mw = vdp_width(), h = vdp_height();
    for (int y = 4; y < h; y += 8)
        for (int s = 0; s < 2; s++)
            for (int k = 0; k < 7; k++) {
                int x = s ? mw + 4 + k * 8 : -4 - k * 8, px, py, X, Y;
                vdp_plane_xy(plane, x, y, &px, &py);
                map_xy(L, x, y, px, py, &X, &Y);
                int m = map_entry(L, X, Y);
                if (m == OUTSIDE) continue;
                (*count)++;
                if (vdp_plane_entry(plane, px, py) != (uint16_t)m) (*(s ? rdiff : ldiff))++;
            }
}

/* The chunk table to trust: a window saves the plane mappings it covers
 * over the table's start ($FFEC98, the end of what is saved, is $6040 when
 * nothing is), and the game puts the table back a few frames after the
 * pointer drops. So the table in RAM is only taken (copied) when there is no
 * window and it agrees with the screen at least as well as the copy kept. */
void widemap_frame(int wide) {
    if (logging < 0) logging = getenv("PSIV_WIDEMAP_LOG") != NULL;
    frames++;
    unsigned mode = r16(GAME_MODE) / 4;
    int field = mode == 2 || mode == 3;
    if (logging && field) log_objects();
    layer[0].on = layer[1].on = 0;
    vdp_margin_entry = NULL;
    int wcells, hcells;
    vdp_plane_cells(&wcells, &hcells);
    if (!wide || !field || wcells != 64 || hcells != 32 || (!enabled && !logging)) return;

    int world = (r16(FIELD_MAP) & 0xFFFE) == 0;
    for (int k = 0; k < 2; k++) {
        Layer *L = &layer[k];
        L->layout = k ? 0xB000 : 0xA000;
        L->stride = (int)r8(k ? 0xEC66 : 0xEC64) + 1;
        L->endless = world;
        L->wrap_w = world ? 128 : L->stride;
        L->wrap_h = world ? 32 : (int)r8(k ? 0xEC67 : 0xEC65) + 1;
        L->mask = k && r8(ALT_COLOR) ? 0x1FFF : 0xBFFF;
        L->cam_x = (int)r16(k ? 0xEF9C : 0xEF94);
        L->cam_y = (int)r16(k ? 0xEF98 : 0xEF90);
    }
    int usable = layer[0].wrap_h * layer[0].stride <= 0x1000 && layer[1].wrap_h * layer[1].stride <= 0x1000;
    if (!usable) return;                                  /* not layouts that fit their RAM */

    int n[2] = {0, 0}, m[2] = {0, 0};
    if (chunks_ok) {
        table = chunks;
        for (int k = 0; k < 2; k++) compare(k, &layer[k], &n[k], &m[k]);
    }
    if (r16(SAVE_END) <= 0x6040) {                        /* no window: try the table in RAM */
        int rn[2], rm[2];
        table = &m68k_ram[CHUNKS];
        for (int k = 0; k < 2; k++) compare(k, &layer[k], &rn[k], &rm[k]);
        if ((agrees(rn[0], rm[0]) || agrees(rn[1], rm[1])) && (!chunks_ok || rm[0] + rm[1] >= m[0] + m[1])) {
            memcpy(chunks, &m68k_ram[CHUNKS], sizeof chunks);
            chunks_ok = 1;
            for (int k = 0; k < 2; k++) { n[k] = rn[k]; m[k] = rm[k]; }
        }
    }
    table = chunks;
    /* rows of plane A filled with one entry, not the map's (see fill_a) */
    int fills = 0, mw = vdp_width(), h = vdp_height();
    for (int r = 0; r < 32; r++) fill_a[r] = -1;
    for (int y = 4; y < h; y += 8) {
        int px0, py0;
        vdp_plane_xy(0, 4, y, &px0, &py0);
        uint16_t e0 = vdp_plane_entry(0, px0, py0);
        if (!(e0 & 0x7FF)) continue;                      /* a blank row isn't a fill */
        int same = 1, mapped = 0, cells = 0;
        for (int x = 4; x < mw && same; x += 8) {
            int px, py, X, Y;
            vdp_plane_xy(0, x, y, &px, &py);
            uint16_t e = vdp_plane_entry(0, px, py);
            same = e == e0;
            cells++;
            if (chunks_ok) {
                map_xy(&layer[0], x, y, px, py, &X, &Y);
                mapped += map_entry(&layer[0], X, Y) == (int)e;
            }
        }
        if (same && mapped * 2 < cells) {
            fill_a[(py0 >> 3) & 31] = e0;
            fills++;
        }
    }
    for (int k = 0; k < 2; k++) {
        Layer *L = &layer[k];
        L->on = chunks_ok && agrees(n[k], m[k]);
        if (logging) {
            int ld, rd, mc;
            margin_diffs(k, L, &ld, &rd, &mc);
            printf("widemap %llu: plane %c map %03X (%dx%d chunks) camera %d,%d %s %d/%d agree (%d%%); "
                   "margins differ L %d R %d of %d; filled rows %d\n",
                   frames, k ? 'B' : 'A', r16(FIELD_MAP), L->wrap_w, L->wrap_h, L->cam_x, L->cam_y,
                   L->on ? "on " : "off", m[k], n[k], n[k] ? m[k] * 100 / n[k] : 0, ld, rd, mc, fills);
        }
    }
    if (enabled && (layer[0].on || layer[1].on || fills)) vdp_margin_entry = margin_entry;
}
