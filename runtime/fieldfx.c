/*
 * The field's living water: see fieldfx.h.
 *
 * fieldfx_prepare, once a frame, at the game's resolution:
 *   1. the walked layer's collision under the view, cell by cell (16 x 16):
 *      water (9), water all round, and beside water
 *   2. which colours are water: those drawn (by a background plane, or the
 *      backdrop) in the cells that are water all round, each at least 2% of
 *      them -- the game animates its water by changing tiles and colours, so
 *      this is found again every frame
 *   3. the mask: background pixels of those colours in cells at or beside
 *      water (a shore cell's sand and rock stay out), and each one's distance
 *      to the shore
 * fieldfx_draw:
 *   4. the water's colour with the game's dither smoothed away, spread a
 *      little past the shore so nothing samples the land
 *   5. the water worked out once per game pixel: waves (a height field of
 *      four crossing wave trains, swells to chop, in map coordinates so they
 *      stay with the map as the view scrolls) bend what lies under them and
 *      catch the light; faint caustics; deep water darker; foam washing along
 *      the shore
 *   6. that, interpolated smoothly up to the display's resolution (the waves
 *      are many pixels long, so nothing is lost), then the glints
 */
#include "fieldfx.h"
#include "frame.h"
#include "walkto.h"
#include "m68k.h"
#include "scenevox.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int water_on = 1;
void fieldfx_set_water(int on) { water_on = on; }
int  fieldfx_water(void) { return water_on; }

/* ------------------------------------------------------------------ helpers */
#define TAB 4096
static float sin_tab[TAB];
static int tab_ready;
static void tab_init(void) {
    for (int i = 0; i < TAB; i++) sin_tab[i] = sinf((float)i * (6.2831853f / TAB));
    tab_ready = 1;
}
static inline float fsin(float x) { return sin_tab[(int)(x * (TAB / 6.2831853f)) & (TAB - 1)]; }
static inline float fcos(float x) { return sin_tab[((int)(x * (TAB / 6.2831853f)) + TAB / 4) & (TAB - 1)]; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* smoothstep from e0, over 1/inv */
static inline float smooth(float e0, float inv, float x) {
    float t = clampf((x - e0) * inv, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* the layer that won a pixel, as vdp_compose_line decides it: 0 backdrop,
 * 1 plane B, 2 plane A, 3 the window, 4 a sprite; *idx its colour */
static int winner(const VdpLine *l, int x, uint8_t *idx) {
    uint8_t b = l->plane_b[x], a = l->win_in[x] ? l->window[x] : l->plane_a[x], s = l->sprite[x];
    uint8_t c = l->backdrop;
    int who = 0, aw = l->win_in[x] ? 3 : 2;
    if (b && !(b & 0x80)) { c = b; who = 1; }
    if (a && !(a & 0x80)) { c = a; who = aw; }
    if (s && !(s & 0x80)) { c = s; who = 4; }
    if (b & 0x80) { c = b; who = 1; }
    if ((a & 0x80) && !(!l->win_in[x] && l->plane_a_under[x] && s && !(s & 0x80))) { c = a; who = aw; }
    if (s & 0x80) { c = s; who = 4; }
    *idx = c & 0x3F;
    return who;
}

/* ------------------------------------------------------------------ the frame's water */
#define GW 40                                    /* cells across the view, with a margin */
#define GH 24
#define DCAP 8.0f
static int ready, fw, fh, cam_x, cam_y, found;   /* cam: the map pixel at column 0, row 0 */
static float t_now;
static uint8_t mask[VDP_MAX_H][VDP_MAX_W];
static uint8_t bgidx[VDP_MAX_H][VDP_MAX_W];      /* a background pixel's colour, 0xFF: not background */
static float dist[VDP_MAX_H][VDP_MAX_W];         /* to the nearest pixel that isn't water, capped */

int fieldfx_pixels(void) { return ready ? found : 0; }

/* What the game puts over the map -- a dialogue box, a portrait, a
 * cutscene's picture -- is drawn into plane A where the map would be. So a
 * plane A pixel whose tile (and flips) isn't the one the map has at that
 * spot is not the map: no water or trees there. The map: Map_Layout_FG
 * ($FFA000, a byte per 32x32 chunk; the planets keep a 32-row window of it)
 * and the Chunk_Table ($FF6000, 4x4 name-table words a chunk), kept from a
 * frame with no window open (a window saves the plane under it over the
 * table's start: $FFEC98 is the end of what it saved, $6040 = none). */
static uint8_t chunks[0x2000];
static int chunks_ok;
static uint64_t overlay_frame = (uint64_t)-1;
static uint8_t overlay[VDP_MAX_H][VDP_MAX_W];
static unsigned rb(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned rw(unsigned a) { return rb(a) << 8 | rb(a + 1); }
static void find_overlays(const VdpLine *lines, int w, int h) {
    if (overlay_frame == frame_count) return;            /* once a frame, for both */
    overlay_frame = frame_count;
    if (rw(0xEC98) <= 0x6040) { memcpy(chunks, &m68k_ram[0x6000], sizeof chunks); chunks_ok = 1; }
    memset(overlay, 0, sizeof overlay);
    if (!chunks_ok) return;
    int world = (rw(0xEC28) & 0xFFFE) == 0;
    /* plane A against the FG layout, plane B against the BG layout, each
     * by its own camera (a cutscene's picture can use both: figures on A,
     * its backdrop on B) */
    for (int plane = 0; plane < 2; plane++) {
        unsigned layout = plane ? 0xB000u : 0xA000u;
        int lw = (int)rb(plane ? 0xEC66 : 0xEC64) + 1, lh = (int)rb(plane ? 0xEC67 : 0xEC65) + 1;   /* in chunks */
        int cx = (int)rw(plane ? 0xEF9C : 0xEF94) + vdp_view_x0, cy = (int)rw(plane ? 0xEF98 : 0xEF90);
        int W = lw * 32, H = lh * 32, pwc, phc;
        vdp_plane_cells(&pwc, &phc);
        for (int y = 0; y < h; y++) {
            const VdpLine *l = &lines[y];
            for (int x = 0; x < w; x++) {
                uint16_t e;
                int sx = x + vdp_view_x0;
                if (sx < 0 || sx >= vdp_width()) continue;          /* the widescreen margins: the port's own, from the map */
                if (plane == 0 && (!l->plane_a[x] || l->win_in[x])) continue;   /* what plane A draws */
                if (plane == 1 && !l->plane_b[x]) continue;
                int px, py;
                vdp_plane_xy(plane, sx, y, &px, &py);
                e = plane == 0 ? l->plane_a_tile[x] : vdp_plane_entry(1, px, py);
                if (!(e & 0x7FF)) continue;
                /* the map pixel shown here. The camera in RAM is already the
                 * next frame's while the party walks (the VDP takes it at the
                 * next VInt), a step ahead of the picture; the game draws the
                 * map into the plane at the map's own coordinates, so the
                 * plane's scroll says where the picture really is (as
                 * widemap.c's map_xy) */
                int dx = (px - (cx + x)) & (pwc * 8 - 1), dy = (py - (cy + y)) & (phc * 8 - 1);
                if (dx >= pwc * 4) dx -= pwc * 8;
                if (dy >= phc * 4) dy -= phc * 8;
                int wx = (cx + x + dx) % W, wy = (cy + y + dy) % H;
                if (wx < 0) wx += W;
                if (wy < 0) wy += H;
                unsigned row = (unsigned)wy >> 5;
                if (world) row &= 0x1F;
                unsigned c = rb(layout + row * (unsigned)lw + ((unsigned)wx >> 5));
                unsigned ca = (c * 32 + (unsigned)(((wy >> 3) & 3) * 8 + ((wx >> 3) & 3) * 2)) & 0x1FFF;
                uint16_t m = (uint16_t)(chunks[ca] << 8 | chunks[ca + 1]);
                if ((e & 0x1FFF) != (m & 0x1FFF)) overlay[y][x] = 1;
            }
        }
    }
}
static inline int over(int x, int y) { return overlay[y][x]; }

/* The void sea: a cutscene's picture on black (the new game's panorama, a
 * story told in pictures) -- the black round it becomes a dark sea, the
 * water's waves, sheen, caustic light and glints with nothing under them.
 * A frame is such a scene when black covers a good part of it, the picture
 * (what isn't the map, see find_overlays) a part, and the map next to
 * nothing. The sea: black background that isn't the picture's own, and any
 * 8 x 8 block that is wholly black. It follows the picture's palette in and
 * out, so the game's fades take it with them. */
static int void_mode;                            /* this frame's water is the void sea */
static float void_amt;                           /* how much of it shows, 0..1 */
static uint8_t void_black[VDP_MAX_H][VDP_MAX_W];
static inline int black_idx(uint8_t idx) { return (vdp.cram[idx & 0x3F] & 0x0EEE) == 0; }
static int void_prepare(const VdpLine *lines, int w, int h) {
    int black = 0, picture = 0, map = 0, bright = 0;
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        for (int x = 0; x < w; x++) {
            uint8_t idx;
            int who = winner(l, x, &idx), blk = who != 4 && black_idx(idx);
            void_black[y][x] = (uint8_t)blk;
            if (blk) { black++; continue; }
            if (who == 4 || who == 3 || over(x, y)) {
                picture++;
                uint16_t c = vdp.cram[idx];
                int m = (c >> 1) & 7;
                if (((c >> 5) & 7) > m) m = (c >> 5) & 7;
                if (((c >> 9) & 7) > m) m = (c >> 9) & 7;
                if (m > bright) bright = m;
            } else map++;
        }
    }
    int n = w * h;
    int scene = black * 5 >= n && picture * 16 >= n && map * 50 <= n;
    float target = scene ? clampf((float)bright / 5.0f, 0.0f, 1.0f) : 0.0f;
    void_amt += (target - void_amt) * (target > void_amt ? 0.08f : 0.15f);
    if (void_amt < 0.01f && target == 0.0f) { void_amt = 0.0f; return 0; }
    if (!scene) {                                        /* going: what stays black, as it goes */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) mask[y][x] = void_black[y][x] && !over(x, y);
        return 1;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) mask[y][x] = void_black[y][x] && !over(x, y);
    for (int by = 0; by < h; by += 8)                    /* the picture's own black tiles, whole */
        for (int bx = 0; bx < w; bx += 8) {
            int all = 1;
            for (int y = by; y < by + 8 && y < h && all; y++)
                for (int x = bx; x < bx + 8 && x < w; x++)
                    if (!void_black[y][x]) { all = 0; break; }
            if (!all) continue;
            for (int y = by; y < by + 8 && y < h; y++)
                for (int x = bx; x < bx + 8 && x < w; x++) mask[y][x] = 1;
        }
    return 1;
}

static int water_prepare(const VdpLine *lines, int w, int h) {
    ready = 0;
    void_mode = 0;
    if (!water_on || w > VDP_MAX_W || h > VDP_MAX_H) return 0;
    int mw, mh, cx, cy;
    if (!walkto_field_map(&mw, &mh, &cx, &cy)) { void_amt = 0.0f; return 0; }
    if (!tab_ready) tab_init();
    cam_x = cx + vdp_view_x0;
    cam_y = cy;
    find_overlays(lines, w, h);
    int count = 0;
    if (void_prepare(lines, w, h)) {
        void_mode = 1;
        cam_x = cam_y = 0;                               /* the sea stays put: no map moves under it */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                dist[y][x] = mask[y][x] ? DCAP : 0.0f;
                count += mask[y][x];
            }
        if (count < 64) return 0;
        goto shore;
    }

    /* 1. the cells */
    static uint8_t wet[GH][GW], inner[GH][GW], near[GH][GW];
    int gx0 = (cam_x >> 4) - 1, gy0 = (cam_y >> 4) - 1;
    int gc = ((cam_x + w - 1) >> 4) - gx0 + 2, gr = ((cam_y + h - 1) >> 4) - gy0 + 2;
    if (gc > GW || gr > GH) return 0;
    int any = 0;
    for (int r = 0; r < gr; r++)
        for (int c = 0; c < gc; c++) any |= wet[r][c] = walkto_cell_code(gx0 + c, gy0 + r) == 9;
    if (!any) return 0;
    for (int r = 0; r < gr; r++)
        for (int c = 0; c < gc; c++) {
            int all = wet[r][c], one = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int rr = r + dy, cc = c + dx;
                    int v = rr >= 0 && cc >= 0 && rr < gr && cc < gc && wet[rr][cc];
                    one |= v;
                    if (!dx || !dy) all &= v;
                }
            inner[r][c] = (uint8_t)all;
            near[r][c] = (uint8_t)one;
        }

    /* 2. the water's colours */
    unsigned hist[64] = {0}, total = 0;
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        int r = ((cam_y + y) >> 4) - gy0;
        for (int x = 0; x < w; x++) {
            uint8_t idx;
            bgidx[y][x] = winner(l, x, &idx) <= 2 && !over(x, y) ? idx : 0xFF;
            if (bgidx[y][x] != 0xFF && inner[r][((cam_x + x) >> 4) - gx0]) { hist[idx]++; total++; }
        }
    }
    if (total < 192) return 0;
    uint8_t is_water[65];
    for (int i = 0; i < 64; i++) is_water[i] = hist[i] * 50 >= total;
    is_water[64] = 0;

    /* 3. the mask and the distance to the shore */
    for (int y = 0; y < h; y++) {
        int r = ((cam_y + y) >> 4) - gy0;
        for (int x = 0; x < w; x++) {
            int c = ((cam_x + x) >> 4) - gx0;
            int m = near[r][c] && is_water[bgidx[y][x] < 64 ? bgidx[y][x] : 64];
            mask[y][x] = (uint8_t)m;
            dist[y][x] = m ? DCAP : 0.0f;
            count += m;
        }
    }
    if (count < 64) return 0;
shore:;
    const float D = 1.41421356f;
    for (int y = 0; y < h; y++)                          /* chamfer, forward then back; the picture's */
        for (int x = 0; x < w; x++) {                    /* edges count as more water */
            if (!mask[y][x]) continue;
            float d = dist[y][x];
            if (x > 0) d = fminf(d, dist[y][x - 1] + 1);
            if (y > 0) {
                d = fminf(d, dist[y - 1][x] + 1);
                if (x > 0) d = fminf(d, dist[y - 1][x - 1] + D);
                if (x + 1 < w) d = fminf(d, dist[y - 1][x + 1] + D);
            }
            dist[y][x] = d;
        }
    for (int y = h - 1; y >= 0; y--)
        for (int x = w - 1; x >= 0; x--) {
            if (!mask[y][x]) continue;
            float d = dist[y][x];
            if (x + 1 < w) d = fminf(d, dist[y][x + 1] + 1);
            if (y + 1 < h) {
                d = fminf(d, dist[y + 1][x] + 1);
                if (x + 1 < w) d = fminf(d, dist[y + 1][x + 1] + D);
                if (x > 0) d = fminf(d, dist[y + 1][x - 1] + D);
            }
            dist[y][x] = d;
        }
    t_now = (float)frame_count * (1.0f / 60.0f);
    found = count;
    fw = w;
    fh = h;
    ready = 1;
    return 1;
}

/* ------------------------------------------------------------------ drawing */
/* the waves: swells to chop, crossing (map pixels; a in pixels of height) */
typedef struct { float kx, ky, w, a, p; } Wave;
#define WAVE(dx, dy, len, speed, amp, ph) {6.2831853f / (len) * (dx), 6.2831853f / (len) * (dy), speed, amp, ph}
static const Wave WAVES[] = {
    WAVE(0.80f, 0.60f, 40.0f, 1.10f, 1.00f, 0.0f),
    WAVE(-0.55f, 0.84f, 25.0f, 1.50f, 0.62f, 1.3f),
    WAVE(0.20f, -0.98f, 15.0f, 2.00f, 0.36f, 2.1f),
    WAVE(0.96f, -0.28f, 9.0f, 2.60f, 0.20f, 4.0f),
};
#define NWAVES (int)(sizeof WAVES / sizeof WAVES[0])

static float col[VDP_MAX_H][VDP_MAX_W][3];       /* what lies under the water (step 4) */
static uint32_t res[VDP_MAX_H][VDP_MAX_W];       /* the water, per game pixel (step 5), 0xRRGGBB */

/* 4. the water's colours averaged over 5 x 5 water pixels (the game's dither
 * becomes the smooth colour it stands for) with a little of its texture
 * kept, then spread two pixels past the edge */
static void water_colour(const uint32_t *pixels, int src_stride) {
    static float tmp[VDP_MAX_H][VDP_MAX_W][4];
    static uint8_t have[VDP_MAX_H][VDP_MAX_W], next[VDP_MAX_H][VDP_MAX_W];
    static const int8_t DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            float s4[4] = {0, 0, 0, 0};
            for (int k = -2; k <= 2; k++) {
                int xx = x + k;
                if (xx < 0 || xx >= fw || !mask[y][xx]) continue;
                uint32_t p = pixels[(size_t)y * src_stride + xx];
                s4[0] += (float)((p >> 16) & 255); s4[1] += (float)((p >> 8) & 255); s4[2] += (float)(p & 255); s4[3] += 1;
            }
            memcpy(tmp[y][x], s4, sizeof s4);
        }
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            have[y][x] = mask[y][x];
            if (!mask[y][x]) continue;
            float s4[4] = {0, 0, 0, 0};
            for (int k = -2; k <= 2; k++) {
                int yy = y + k;
                if (yy < 0 || yy >= fh) continue;
                for (int c = 0; c < 4; c++) s4[c] += tmp[yy][x][c];
            }
            float n = s4[3] > 0 ? 0.78f / s4[3] : 0;
            uint32_t p = pixels[(size_t)y * src_stride + x];
            col[y][x][0] = s4[0] * n + (float)((p >> 16) & 255) * 0.22f;
            col[y][x][1] = s4[1] * n + (float)((p >> 8) & 255) * 0.22f;
            col[y][x][2] = s4[2] * n + (float)(p & 255) * 0.22f;
        }
    for (int ring = 0; ring < 2; ring++) {
        memcpy(next, have, sizeof next);
        for (int y = 0; y < fh; y++)
            for (int x = 0; x < fw; x++) {
                if (have[y][x]) continue;
                float s3[3] = {0, 0, 0};
                int n = 0;
                for (int k = 0; k < 8; k++) {
                    int xx = x + DX[k], yy = y + DY[k];
                    if (xx < 0 || yy < 0 || xx >= fw || yy >= fh || !have[yy][xx]) continue;
                    for (int c = 0; c < 3; c++) s3[c] += col[yy][xx][c];
                    n++;
                }
                if (!n) continue;
                for (int c = 0; c < 3; c++) col[y][x][c] = s3[c] / (float)n;
                next[y][x] = 1;
            }
        memcpy(have, next, sizeof have);
    }
}

/* the waves' slope and the caustics' wave, every second map pixel (they are
 * 9 to 40 pixels long): interpolated for each pixel */
#define FW (VDP_MAX_W / 2 + 2)
#define FH (VDP_MAX_H / 2 + 2)
static float f_gx[FH][FW], f_gy[FH][FW], f_cv[FH][FW];
static uint8_t f_need[FH][FW];
static void wave_fields(void) {
    float t = t_now;
    memset(f_need, 0, sizeof f_need);
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++)
            if (mask[y][x]) {
                int i = x >> 1, j = y >> 1;
                f_need[j][i] = f_need[j][i + 1] = f_need[j + 1][i] = f_need[j + 1][i + 1] = 1;
            }
    for (int j = 0; j < FH; j++)
        for (int i = 0; i < FW; i++) {
            if (!f_need[j][i]) continue;
            float X = (float)(cam_x + 2 * i), Y = (float)(cam_y + 2 * j);
            float gx = 0, gy = 0;
            for (int k = 0; k < NWAVES; k++) {
                const Wave *wv = &WAVES[k];
                float c = fcos(wv->kx * X + wv->ky * Y - wv->w * t + wv->p) * wv->a;
                gx += c * wv->kx;
                gy += c * wv->ky;
            }
            float ca = fsin(0.27f * X + 0.6f * t + 1.2f * fsin(0.16f * Y - 0.35f * t));
            float cb = fsin(0.24f * Y - 0.5f * t + 1.1f * fsin(0.15f * X + 0.4f * t));
            float cc = fsin(0.17f * (X + Y) + 0.3f * t);
            f_gx[j][i] = gx;
            f_gy[j][i] = gy;
            f_cv[j][i] = (ca + cb + cc) * (1.0f / 3.0f);
        }
}

/* 5. one game pixel of water, at its centre */
static void water_pixel(int x, int y) {
    const float LX = -0.6f, LY = -0.8f;                  /* the light: from the top left */
    float X = (float)(cam_x + x) + 0.5f, Y = (float)(cam_y + y) + 0.5f, t = t_now;
    int i = x >> 1, j = y >> 1;
    float fx = ((float)(x & 1) + 0.5f) * 0.5f, fy = ((float)(y & 1) + 0.5f) * 0.5f;
    float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    float gx = f_gx[j][i] * w00 + f_gx[j][i + 1] * w10 + f_gx[j + 1][i] * w01 + f_gx[j + 1][i + 1] * w11;
    float gy = f_gy[j][i] * w00 + f_gy[j][i + 1] * w10 + f_gy[j + 1][i] * w01 + f_gy[j + 1][i + 1] * w11;
    float cv = f_cv[j][i] * w00 + f_cv[j][i + 1] * w10 + f_cv[j + 1][i] * w01 + f_cv[j + 1][i + 1] * w11;
    float d = dist[y][x];
    /* what lies under the water, bent along the waves' slope */
    float su = (float)x - gx * 1.3f, sv = (float)y - gy * 1.3f;
    int sx = (int)(su + 64.0f) - 64, sy = (int)(sv + 64.0f) - 64;          /* floor, without a call */
    float ax = su - (float)sx, ay = sv - (float)sy;
    if (sx < 0) { sx = 0; ax = 0; } else if (sx > fw - 2) { sx = fw - 2; ax = 1; }
    if (sy < 0) { sy = 0; ay = 0; } else if (sy > fh - 2) { sy = fh - 2; ay = 1; }
    const float *p00 = col[sy][sx], *p10 = col[sy][sx + 1], *p01 = col[sy + 1][sx], *p11 = col[sy + 1][sx + 1];
    float b00 = (1 - ax) * (1 - ay), b10 = ax * (1 - ay), b01 = (1 - ax) * ay, b11 = ax * ay;
    float r = p00[0] * b00 + p10[0] * b10 + p01[0] * b01 + p11[0] * b11;
    float g = p00[1] * b00 + p10[1] * b10 + p01[1] * b01 + p11[1] * b11;
    float b = p00[2] * b00 + p10[2] * b10 + p01[2] * b01 + p11[2] * b11;
    /* the light it catches fades with the water itself (the game's fades to
     * black); the void sea has no water under it, and its own fade */
    float lit = void_mode ? 1.0f : clampf((r + g + b) * (1.0f / 120.0f), 0.0f, 1.0f);
    float deep = smooth(2.0f, 1.0f / 7.0f, d);           /* deep water a little darker */
    r *= 1 - 0.09f * deep; g *= 1 - 0.05f * deep; b *= 1 - 0.01f * deep;
    /* the waves lit: faces toward the light brighter, away darker, and a
     * sheen where they catch it */
    float sl = (gx * LX + gy * LY) * 1.6f;
    float shade = 1.04f + 0.36f * clampf(sl, -1, 1);
    r *= shade; g *= shade; b *= shade;
    float spec = clampf((sl - 0.2f) * 1.5f, 0, 1);
    spec = spec * spec * (3 - 2 * spec) * 0.55f * lit;
    r += (225 - r) * spec; g += (240 - g) * spec; b += (255 - b) * spec;
    /* caustic light, faint, where three slower waves cross */
    float k = 1 - fabsf(cv);
    k *= k; k *= k; k *= k;
    k *= 0.14f * smooth(1.0f, 1.0f / 3.0f, d) * lit;
    r += k * 170; g += k * 215; b += k * 240;
    if (d < 2.4f) {                                      /* foam along the shore, washing in and out */
        float f = (1 - smooth(0.4f, 0.5f, d)) * (0.5f + 0.5f * fsin(t * 2.0f - d * 2.6f + 0.9f * fsin(0.21f * X + 0.17f * Y + 0.6f * t)));
        f *= void_mode ? 0.16f : 0.42f * lit;                  /* the void: a faint glow round the picture */
        r += (232 - r) * f; g += (242 - g) * f; b += (248 - b) * f;
    }
    if (void_mode) { r *= void_amt; g *= void_amt; b *= void_amt; }
    res[y][x] = (uint32_t)clampf(r, 0, 255) << 16 | (uint32_t)clampf(g, 0, 255) << 8 | (uint32_t)clampf(b, 0, 255);
}

/* the glints: a soft point with a short cross, its shape made once per scale */
static void glints(uint32_t *out, int stride, int ds) {
    enum { KR = 8 };
    static float kern[2 * KR + 1][2 * KR + 1];
    static int kern_ds;
    if (kern_ds != ds) {
        float rad = 1.6f * (float)ds;
        for (int y = -KR; y <= KR; y++)
            for (int x = -KR; x <= KR; x++) {
                float r2 = (float)(x * x + y * y) / (rad * rad);
                float a = expf(-r2 * 4.0f);
                if (abs(x) == 0 || abs(y) == 0) a += expf(-sqrtf(r2) * 2.2f) * 0.6f;
                kern[y + KR][x + KR] = clampf(a * 0.85f, 0, 1);
            }
        kern_ds = ds;
    }
    int R = (int)ceilf(1.6f * (float)ds) + 1;
    if (R > KR) R = KR;
    const int G = 9;                                     /* one possible glint per 9 x 9 map pixels */
    int gx0 = (cam_x >> 0) / G - 1, gy0 = cam_y / G - 1;
    int gx1 = (cam_x + fw) / G + 1, gy1 = (cam_y + fh) / G + 1;
    for (int gy = gy0; gy <= gy1; gy++)
        for (int gx = gx0; gx <= gx1; gx++) {
            uint32_t hsh = (uint32_t)gx * 73856093u ^ (uint32_t)gy * 19349663u;
            hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
            float phase = (float)(hsh & 1023) * (6.2831853f / 1024.0f);
            float speed = 0.7f + (float)((hsh >> 10) & 63) * (1.0f / 64.0f);
            float b = fsin(t_now * speed + phase);
            if (b < 0.965f) continue;
            int px = gx * G + (int)((hsh >> 16) & 7) - cam_x, py = gy * G + (int)((hsh >> 19) & 7) - cam_y;
            if (px < 1 || py < 1 || px >= fw - 1 || py >= fh - 1 || !mask[py][px] || dist[py][px] < 2.5f) continue;
            const float *u = col[py][px];                /* fading with the water, or the void's own fade */
            float k = (b - 0.965f) * (1.0f / 0.035f) *
                      (void_mode ? void_amt : clampf((u[0] + u[1] + u[2]) * (1.0f / 120.0f), 0.0f, 1.0f));
            if (k <= 0.0f) continue;
            int cx = px * ds + ds / 2, cy = py * ds + ds / 2;
            for (int y = -R; y <= R; y++) {
                if (cy + y < 0 || cy + y >= fh * ds) continue;
                uint32_t *row = out + (size_t)(cy + y) * stride;
                for (int x = -R; x <= R; x++) {
                    if (cx + x < 0 || cx + x >= fw * ds) continue;
                    float a = kern[y + KR][x + KR] * k;
                    if (a <= 0.01f) continue;
                    uint32_t p = row[cx + x];
                    float rr = (float)((p >> 16) & 255), gg = (float)((p >> 8) & 255), bb = (float)(p & 255);
                    rr += (255 - rr) * a; gg += (255 - gg) * a; bb += (255 - bb) * a;
                    row[cx + x] = 0xFF000000u | (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
                }
            }
        }
}

static void water_draw(const uint32_t *pixels, int src_stride, int w, int h, uint32_t *out, int stride, int ds) {
    if (!ready || w != fw || h != fh || ds < 1 || w < 2 || h < 2) return;
    water_colour(pixels, src_stride);
    wave_fields();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (mask[y][x]) water_pixel(x, y);
    /* 6. up to the display: each screen pixel of water blended from the four
     * game pixels round it, two channels to a multiply -- down the column
     * first, then along the row. A neighbour that isn't water lends the
     * average of the water beside it. */
    static uint8_t lent[VDP_MAX_H][VDP_MAX_W];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            lent[y][x] = 0;
            if (mask[y][x]) continue;
            unsigned s4[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                int xx = x + (k == 0 ? -1 : k == 1 ? 1 : 0), yy = y + (k == 2 ? -1 : k == 3 ? 1 : 0);
                if (xx < 0 || yy < 0 || xx >= w || yy >= h || !mask[yy][xx]) continue;
                uint32_t c = res[yy][xx];
                s4[0] += c >> 16 & 255; s4[1] += c >> 8 & 255; s4[2] += c & 255; s4[3]++;
            }
            if (s4[3]) {
                res[y][x] = (s4[0] / s4[3]) << 16 | (s4[1] / s4[3]) << 8 | (s4[2] / s4[3]);
                lent[y][x] = 1;
            }
        }
    int wt[4], side[4];                                  /* per sub-pixel: the neighbour's weight (of 256), which way */
    for (int k = 0; k < ds; k++) {
        float v = ((float)k + 0.5f) / (float)ds - 0.5f;
        wt[k] = (int)(fabsf(v) * 256.0f + 0.5f);
        side[k] = v < 0 ? -1 : 1;
    }
    static uint32_t col_v[VDP_MAX_W];
    for (int y = 0; y < h; y++) {
        int any = 0;
        for (int x = 0; x < w && !any; x++) any = mask[y][x];
        if (!any) continue;
        for (int j = 0; j < ds; j++) {
            int ny = y + side[j];
            unsigned ay = (unsigned)wt[j];
            for (int x = 0; x < w; x++) {
                uint32_t a = res[y][x];
                uint32_t b = (ny >= 0 && ny < h && (mask[ny][x] || lent[ny][x])) ? res[ny][x] : a;
                uint32_t rb = (((a & 0xFF00FFu) * (256 - ay) + (b & 0xFF00FFu) * ay) >> 8) & 0xFF00FFu;
                uint32_t g = (((a & 0x00FF00u) * (256 - ay) + (b & 0x00FF00u) * ay) >> 8) & 0x00FF00u;
                col_v[x] = rb | g;
            }
            uint32_t *dst = out + (size_t)(y * ds + j) * stride;
            for (int x = 0; x < w; x++) {
                if (!mask[y][x]) continue;
                uint32_t a = col_v[x];
                for (int i = 0; i < ds; i++) {
                    int nx = x + side[i];
                    uint32_t b = (nx >= 0 && nx < w && (mask[y][nx] || lent[y][nx])) ? col_v[nx] : a;
                    unsigned ax = (unsigned)wt[i];
                    uint32_t rb = (((a & 0xFF00FFu) * (256 - ax) + (b & 0xFF00FFu) * ax) >> 8) & 0xFF00FFu;
                    uint32_t g = (((a & 0x00FF00u) * (256 - ax) + (b & 0x00FF00u) * ax) >> 8) & 0x00FF00u;
                    dst[x * ds + i] = 0xFF000000u | rb | g;
                }
            }
        }
    }
    glints(out, stride, ds);
}

/* ================================================================== trees
 * Trees that move in the breeze. Their crowns are what the game draws with
 * priority (the party passes behind them) and their trunks stand on solid
 * cells; both are drawn over the ground's own colours. So, per pixel: a
 * background pixel on a priority tile or a solid cell, greenish, in a colour
 * the walkable ground doesn't use, is foliage. Joined (8 ways) they make
 * each tree; its lowest pixel is where it stands. Drawn at the display's
 * resolution: each crown leans with the wind, more the higher it is (the
 * trunk's foot never moves), each tree on its own phase from where it
 * stands; light flickers through the leaves. Where a crown leans away from
 * where it was, the ground beside it shows through. */
static int trees_on = 1, tready, tw_, th_, tcam_x, tcam_y;
void fieldfx_set_trees(int on) { trees_on = on; }
int  fieldfx_trees(void) { return trees_on; }

#define TMAXC 96                                  /* trees in view at most */
typedef struct { int x0, y0, x1, y1, n; float base, x_mid; } Tree;
static Tree trees[TMAXC];
static int ntrees;
static uint8_t tid[VDP_MAX_H][VDP_MAX_W];         /* the tree a pixel belongs to (1-based), 0 none */
static uint8_t tbg[VDP_MAX_H][VDP_MAX_W];         /* background pixel colour (0xFF: a sprite or the window) */

static int greenish(int idx) {
    uint32_t c = vdp_color(idx);
    int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
    return g > r + 12 && g > b + 12;
}

static int trees_prepare(const VdpLine *lines, int w, int h) {
    tready = 0;
    ntrees = 0;
    if (!trees_on || w > VDP_MAX_W || h > VDP_MAX_H) return 0;
    int mw, mh, cx, cy;
    if (!walkto_field_map(&mw, &mh, &cx, &cy)) return 0;
    if (!tab_ready) tab_init();
    tcam_x = cx + vdp_view_x0;
    find_overlays(lines, w, h);
    t_now = (float)frame_count * (1.0f / 60.0f);
    tcam_y = cy;
    /* the cells: solid or not */
    static uint8_t solidc[GH][GW];
    int gx0 = (tcam_x >> 4) - 1, gy0 = (tcam_y >> 4) - 1;
    int gc = ((tcam_x + w - 1) >> 4) - gx0 + 2, gr = ((tcam_y + h - 1) >> 4) - gy0 + 2;
    if (gc > GW || gr > GH) return 0;
    for (int r = 0; r < gr; r++)
        for (int c = 0; c < gc; c++) solidc[r][c] = walkto_cell_code(gx0 + c, gy0 + r) == 8;
    /* the ground's colours: background pixels on walkable cells, without priority */
    static uint8_t pri[VDP_MAX_H][VDP_MAX_W];
    unsigned hist[64] = {0}, total = 0;
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        int r = ((tcam_y + y) >> 4) - gy0;
        for (int x = 0; x < w; x++) {
            uint8_t idx;
            int who = winner(l, x, &idx);
            tbg[y][x] = who <= 2 && !over(x, y) ? idx : 0xFF;
            uint8_t raw = who == 1 ? l->plane_b[x] : who == 2 ? l->plane_a[x] : 0;
            pri[y][x] = (raw & 0x80) != 0;
            if (who <= 2 && !pri[y][x] && !solidc[r][((tcam_x + x) >> 4) - gx0]) { hist[idx]++; total++; }
        }
    }
    uint8_t ground[64];
    for (int i = 0; i < 64; i++) ground[i] = total && hist[i] * 50 >= total;
    uint8_t green[64];
    for (int i = 0; i < 64; i++) green[i] = (uint8_t)greenish(i);
    /* foliage, and each tree */
    memset(tid, 0, sizeof tid);
    static uint8_t cand[VDP_MAX_H][VDP_MAX_W];
    for (int y = 0; y < h; y++) {
        int r = ((tcam_y + y) >> 4) - gy0;
        for (int x = 0; x < w; x++) {
            uint8_t idx = tbg[y][x];
            /* a crown (priority) in any green; on a solid cell, a green the ground doesn't use */
            cand[y][x] = idx != 0xFF && green[idx]
                      && (pri[y][x] || (solidc[r][((tcam_x + x) >> 4) - gx0] && !ground[idx]));
        }
    }
    static int32_t stack[VDP_MAX_H * VDP_MAX_W];
    for (int y0 = 0; y0 < h; y0++)
        for (int x0 = 0; x0 < w; x0++) {
            if (!cand[y0][x0] || tid[y0][x0]) continue;
            if (ntrees >= TMAXC) break;
            Tree *T = &trees[ntrees];
            int id = ntrees + 1, sp = 0;
            T->x0 = T->x1 = x0; T->y0 = T->y1 = y0; T->n = 0;
            stack[sp++] = y0 * VDP_MAX_W + x0;
            tid[y0][x0] = (uint8_t)id;
            while (sp) {
                int i = stack[--sp], x = i % VDP_MAX_W, y = i / VDP_MAX_W;
                T->n++;
                if (x < T->x0) T->x0 = x;
                if (x > T->x1) T->x1 = x;
                if (y < T->y0) T->y0 = y;
                if (y > T->y1) T->y1 = y;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h || !cand[yy][xx] || tid[yy][xx]) continue;
                        tid[yy][xx] = (uint8_t)id;
                        stack[sp++] = yy * VDP_MAX_W + xx;
                    }
            }
            /* a tree: big enough, and taller than about a third of its width
             * (a hedge or a field of tall grass is long and low) */
            int tw = T->x1 - T->x0 + 1, th = T->y1 - T->y0 + 1;
            if (T->n < 24 || th < 10 || th * 3 < tw) {
                for (int y = T->y0; y <= T->y1; y++)
                    for (int x = T->x0; x <= T->x1; x++)
                        if (tid[y][x] == id) tid[y][x] = 0xFF;          /* seen, not a tree */
                continue;
            }
            T->base = (float)T->y1 + 1.0f;
            T->x_mid = (float)(tcam_x + (T->x0 + T->x1) / 2);
            ntrees++;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (tid[y][x] == 0xFF) tid[y][x] = 0;
    tw_ = w;
    th_ = h;
    tready = ntrees > 0;
    return tready;
}

/* how far a tree leans at height y (game pixels, + = right) */
static float lean(const Tree *T, float y, float t) {
    float hgt = T->base - (float)T->y0;
    float k = clampf((T->base - y) / hgt, 0, 1);
    k = k * sqrtf(k);                                    /* more at the tips */
    float X = T->x_mid;
    float wind = 0.62f * fsin(t * 1.15f + X * 0.021f) + 0.28f * fsin(t * 2.7f + X * 0.083f + 1.3f)
               + 0.10f * fsin(t * 5.3f + X * 0.19f);
    float gust = 0.6f + 0.4f * fsin(t * 0.23f + X * 0.004f);   /* the breeze rises and falls */
    /* and the leaves rustle: a quick flutter that differs up the crown */
    float flutter = 0.32f * fsin(t * 7.3f + y * 0.85f + X * 0.37f) + 0.18f * fsin(t * 11.1f - y * 1.3f + X * 0.11f);
    return ((0.5f + 1.5f * gust) * wind + flutter * (0.4f + 0.6f * gust)) * k;
}

static void trees_draw(const uint32_t *pixels, int src_stride, uint32_t *out, int stride, int ds) {
    float t = t_now, inv = 1.0f / (float)ds;
    for (int k = 0; k < ntrees; k++) {
        const Tree *T = &trees[k];
        int id = k + 1;
        int xa = T->x0 - 3 < 0 ? 0 : T->x0 - 3, xb = T->x1 + 3 >= tw_ ? tw_ - 1 : T->x1 + 3;
        for (int Y = T->y0 * ds; Y <= T->y1 * ds + ds - 1; Y++) {
            float v = ((float)Y + 0.5f) * inv;
            int sy = (int)v;
            float dx = lean(T, v, t);
            uint32_t *dst = out + (size_t)Y * stride;
            for (int X = xa * ds; X <= xb * ds + ds - 1; X++) {
                float u = ((float)X + 0.5f) * inv;
                int sx = (int)(u - dx + 64.0f) - 64, ox = (int)u;
                int from = sx >= 0 && sx < tw_ && tid[sy][sx] == id;
                int here = tid[sy][ox] == id;
                if (!from && !here) continue;
                uint32_t p;
                if (from) {
                    p = pixels[(size_t)sy * src_stride + sx];
                    /* light through the leaves: a slow flicker, by pixel */
                    float fl = fsin(0.9f * (float)(tcam_x + sx) + 0.7f * (float)(tcam_y + sy) + t * 2.6f
                                    + 0.8f * fsin(0.31f * (float)(tcam_x + sx) - t * 1.4f));
                    float m = 1.0f + 0.07f * fl * clampf((T->base - v) / (T->base - (float)T->y0), 0, 1);
                    uint32_t r = (uint32_t)clampf((float)(p >> 16 & 255) * m, 0, 255);
                    uint32_t g = (uint32_t)clampf((float)(p >> 8 & 255) * m, 0, 255);
                    uint32_t b = (uint32_t)clampf((float)(p & 255) * m, 0, 255);
                    p = 0xFF000000u | r << 16 | g << 8 | b;
                } else {
                    /* the crown leant away: the ground beside it, nearest first */
                    p = 0;
                    for (int d = 1; d <= 4 && !p; d++) {
                        int l = ox - d, r = ox + d;
                        if (l >= 0 && !tid[sy][l] && tbg[sy][l] != 0xFF) p = pixels[(size_t)sy * src_stride + l];
                        else if (r < tw_ && !tid[sy][r] && tbg[sy][r] != 0xFF) p = pixels[(size_t)sy * src_stride + r];
                    }
                    if (!p) continue;
                }
                dst[X] = p;
            }
        }
    }
}

/* ================================================================== a memory
 * A flashback (scenevox_memory): the scene -- not the boxes, portraits and
 * pictures over it, nor the menus -- turns to warm sepia, darkens toward
 * the edges, and a fine grain flickers over it, like an old film. */
static float mem_amt;
static int mem_ready, mem_w, mem_h;
static uint8_t mem_box[VDP_MAX_H][VDP_MAX_W];       /* 1: a box or window, left as it is */

static int memory_prepare(const VdpLine *lines, int w, int h) {
    mem_ready = 0;
    mem_amt = scenevox_memory();
    if (mem_amt < 0.004f || w > VDP_MAX_W || h > VDP_MAX_H) return 0;
    int mw, mh, cx, cy;
    if (!walkto_field_map(&mw, &mh, &cx, &cy)) return 0;
    find_overlays(lines, w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) mem_box[y][x] = overlay[y][x] || (lines[y].win_in[x] && lines[y].window[x]);
    mem_w = w;
    mem_h = h;
    mem_ready = 1;
    return 1;
}

static void memory_draw(uint32_t *out, int stride, int ds) {
    float m = mem_amt;
    int ow = mem_w * ds, oh = mem_h * ds;
    float cxo = (float)ow * 0.5f, cyo = (float)oh * 0.5f, ir = 1.0f / ((float)oh * 0.62f);
    float flick = 1.0f + 0.025f * m * sinf((float)frame_count * 0.37f) * sinf((float)frame_count * 0.13f + 1.0f);
    uint32_t seed = (uint32_t)frame_count * 0x9E3779B1u;
    for (int Y = 0; Y < oh; Y++) {
        const uint8_t *box = mem_box[Y / ds];
        uint32_t *row = out + (size_t)Y * stride;
        float dy = ((float)Y - cyo) * ir;
        for (int X = 0; X < ow; X++) {
            if (box[X / ds]) continue;
            uint32_t p = row[X];
            float r = (float)(p >> 16 & 255), g = (float)(p >> 8 & 255), b = (float)(p & 255);
            float L = 0.30f * r + 0.59f * g + 0.11f * b;
            float sr = L * 1.08f + 18.0f, sg = L * 0.93f + 6.0f, sb = L * 0.72f - 4.0f;   /* sepia */
            float k = 0.78f * m;
            r += (sr - r) * k; g += (sg - g) * k; b += (sb - b) * k;
            float dx = ((float)X - cxo) * ir, d = sqrtf(dx * dx * 0.55f + dy * dy);   /* vignette */
            float v = clampf((d - 0.55f) * 1.6f, 0, 1);
            v = 1.0f - 0.62f * m * v * v * (3 - 2 * v);
            uint32_t hsh = ((uint32_t)X * 73856093u) ^ ((uint32_t)Y * 19349663u) ^ seed;    /* grain */
            hsh ^= hsh >> 15; hsh *= 0x2C1B3C6Du; hsh ^= hsh >> 12;
            float gr = ((float)(hsh & 255) - 127.5f) * 0.05f * m;
            float f = v * flick;
            r = r * f + gr; g = g * f + gr; b = b * f + gr;
            row[X] = 0xFF000000u | (uint32_t)clampf(r, 0, 255) << 16 | (uint32_t)clampf(g, 0, 255) << 8 | (uint32_t)clampf(b, 0, 255);
        }
    }
}

/* ================================================================== all three */
int fieldfx_tree_count(void) { return tready ? ntrees : 0; }

int fieldfx_prepare(const VdpLine *lines, int w, int h) {
    int a = water_prepare(lines, w, h);
    int b = trees_prepare(lines, w, h);
    int c = memory_prepare(lines, w, h);
    return a || b || c;
}

void fieldfx_draw(const uint32_t *pixels, int src_stride, int w, int h, uint32_t *out, int stride, int ds) {
    if (ready) water_draw(pixels, src_stride, w, h, out, stride, ds);
    if (tready && w == tw_ && h == th_ && ds >= 1) trees_draw(pixels, src_stride, out, stride, ds);
    if (mem_ready && w == mem_w && h == mem_h && ds >= 1) memory_draw(out, stride, ds);
}
