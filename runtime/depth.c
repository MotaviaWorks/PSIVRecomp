/*
 * Battle depth: see depth.h.
 *
 * Layers, back to front, and what goes in them:
 *   background  plane B (and the backdrop colour)                follows the camera a little
 *   enemies     plane A low priority; plane A high priority with
 *               tiles $280-$67F (the damage numbers over enemies) follows it more
 *   action      low-priority sprites: a character mid-attack, its
 *               weapon and spell effects (drawn over the enemies)  moves with the enemies
 *   party       plane A high priority, tiles $180-$27F           follows it most (nearest)
 *   windows     plane A high priority, other tiles (frames, font,
 *               digits, status icons), the window plane, and
 *               high-priority sprites next to a window (menu cursors)  fixed
 *   pointer     other high-priority sprites (the cursor over an
 *               enemy)                                           moves with the enemies
 * The tile ranges are where GameMode_LoadBattle puts the party's and the
 * windows' graphics; they held across every battle in the recorded sessions.
 *
 * The camera drifts slowly; while a character acts (action sprites on
 * screen) it leans toward them and zooms in a little; a damage number
 * appearing gives a short shake. The party is scaled about its feet and
 * never moves vertically, so it stays planted on the status window; the
 * background is mirrored past its edges, which the camera can expose.
 */
#include "depth.h"
#include "m68k.h"

#include <math.h>
#include <string.h>

#define GAME_MODE_INDEX 0xEF00u
#define MODE_BATTLE     0x14             /* GameMode_Battle */

#define TILE_PARTY_LO   0x180
#define TILE_PARTY_HI   0x280
#define TILE_UI_HI      0x680            /* $680-$7FF: window frames, font, digits */
#define PARTY_FEET_Y    168              /* top of the status window */
#define MAX_SCALE       4

#define F_BG    0.25f                    /* how far each layer follows the camera */
#define F_ENEMY 0.6f
#define F_PARTY 1.0f

typedef struct {
    float drift_x, drift_y;              /* idle drift, pixels at the nearest layer */
    float zoom;                          /* extra zoom while someone acts */
    float pan;                           /* lean toward the action, 0..1 */
    float shake;                         /* hit shake, pixels */
} Params;

static const Params LEVEL_PARAMS[DEPTH_LEVELS] = {
    {0.0f, 0.0f, 0.0f,   0.0f,  0.0f},   /* off */
    {4.0f, 1.2f, 0.035f, 0.15f, 1.5f},   /* subtle */
    {9.0f, 2.5f, 0.08f,  0.30f, 3.5f},   /* strong */
};
static const char *NAMES[DEPTH_LEVELS] = {"off", "subtle", "strong"};

static int level;
static Params cur;                       /* eased toward LEVEL_PARAMS[level] */
static float ramp;                       /* 0..1, eases the effect in at battle start */
static float t;                          /* seconds of battle shown */
static float pan_x, pan_y, zoom_extra;   /* camera, eased */
static float shake_amp, shake_t;
static int last_popups;

void depth_set_level(int l) { level = l < 0 ? 0 : l >= DEPTH_LEVELS ? DEPTH_LEVELS - 1 : l; }
int depth_get_level(void) { return level; }
const char *depth_level_name(int l) { return NAMES[l < 0 || l >= DEPTH_LEVELS ? 0 : l]; }

/* the frame sorted into layers: palette index | priority, 0 = transparent */
enum { L_BG, L_ENEMY, L_ACTION, L_PARTY, L_UI, L_POINTER, NLAYERS };
static uint8_t layer[NLAYERS][VDP_MAX_H][VDP_MAX_W];

static void ease(float *v, float target, float k) { *v += (target - *v) * k; }

/* output pixel -> source coordinate for a layer moved by the camera */
static void build_map(int16_t *map, int n, int scale, int size, float center, float factor,
                      float zoom, float cam, int mirror) {
    float s = 1.0f + factor * (zoom - 1.0f);
    for (int i = 0; i < n; i++) {
        float q = ((float)i + 0.5f) / (float)scale;
        int p = (int)floorf(center + (q - center) / s + factor * cam);
        if (mirror) {
            if (p < 0) p = -p - 1;
            if (p >= size) p = 2 * size - p - 1;
            if (p < 0) p = 0;
            if (p >= size) p = size - 1;
        } else if (p < 0 || p >= size) {
            p = -1;
        }
        map[i] = (int16_t)p;
    }
}

int depth_compose(const VdpLine *lines, int w, int h, uint32_t *out, int out_stride, int scale) {
    const float dt = 1.0f / 59.92f;
    uint16_t mode = (uint16_t)(m68k_ram[GAME_MODE_INDEX] << 8 | m68k_ram[GAME_MODE_INDEX + 1]);

    float k = 1.0f - expf(-dt / 0.3f);                    /* level changes ease over ~0.3 s */
    const Params *tp = &LEVEL_PARAMS[level];
    ease(&cur.drift_x, tp->drift_x, k);
    ease(&cur.drift_y, tp->drift_y, k);
    ease(&cur.zoom, tp->zoom, k);
    ease(&cur.pan, tp->pan, k);
    ease(&cur.shake, tp->shake, k);

    if (mode != MODE_BATTLE) {
        ramp = t = pan_x = pan_y = zoom_extra = shake_amp = 0.0f;
        last_popups = 0;
        return 0;
    }
    if (level == 0 && cur.drift_x < 0.02f && cur.zoom < 0.0005f) return 0;   /* eased out */
    if (scale > MAX_SCALE) scale = MAX_SCALE;
    if (scale < 1) scale = 1;
    ramp = fminf(1.0f, ramp + dt / 0.6f);
    t += dt;

    /* sort the frame into layers */
    static int sx0[128], sy0[128], sx1[128], sy1[128], near_ui[128];
    for (int i = 0; i < 128; i++) { sx0[i] = sy0[i] = 1 << 20; sx1[i] = sy1[i] = -1; }
    long act_n = 0, popups = 0;
    double act_x = 0, act_y = 0;
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        for (int x = 0; x < w; x++) {
            uint8_t b = l->plane_b[x], s = l->sprite[x];
            uint8_t a = l->win_in[x] ? l->window[x] : l->plane_a[x];
            uint16_t tile = l->plane_a_tile[x] & 0x7FF;
            for (int n = 1; n < NLAYERS; n++) layer[n][y][x] = 0;
            layer[L_BG][y][x] = b ? b : (uint8_t)(l->backdrop | 0x40);   /* never transparent */
            if (a) {
                if (l->win_in[x]) layer[L_UI][y][x] = a;
                else if (!(a & 0x80)) layer[L_ENEMY][y][x] = a;
                else if (tile >= TILE_PARTY_LO && tile < TILE_PARTY_HI) layer[L_PARTY][y][x] = a;
                else if (tile >= TILE_PARTY_HI && tile < TILE_UI_HI) { layer[L_ENEMY][y][x] = a; popups++; }
                else layer[L_UI][y][x] = a;
            }
            if (s) {
                if (!(s & 0x80)) {
                    layer[L_ACTION][y][x] = s;
                    act_n++; act_x += x; act_y += y;
                } else {
                    int id = l->sprite_no[x] & 127;
                    if (x < sx0[id]) sx0[id] = x;
                    if (x > sx1[id]) sx1[id] = x;
                    if (y < sy0[id]) sy0[id] = y;
                    if (y > sy1[id]) sy1[id] = y;
                }
            }
        }
    }
    /* high-priority sprites: menu cursors stay with their window, the
     * cursor over an enemy moves with the enemies */
    for (int id = 0; id < 128; id++) {
        near_ui[id] = 0;
        if (sx1[id] < 0) continue;
        for (int y = sy0[id] - 4; y <= sy1[id] + 4 && !near_ui[id]; y++)
            for (int x = sx0[id] - 4; x <= sx1[id] + 4; x++)
                if (y >= 0 && y < h && x >= 0 && x < w && layer[L_UI][y][x]) { near_ui[id] = 1; break; }
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t s = lines[y].sprite[x];
            if (s & 0x80) layer[near_ui[lines[y].sprite_no[x] & 127] ? L_UI : L_POINTER][y][x] = s;
        }

    /* the camera */
    const float two_pi = 6.2831853f;
    float e = ramp * ramp * (3.0f - 2.0f * ramp);
    float dx = cur.drift_x * sinf(two_pi * t / 9.0f);
    float dy = cur.drift_y * sinf(two_pi * t / 6.3f + 1.0f);
    float kc = 1.0f - expf(-dt / 0.35f);
    if (act_n > 40) {                                       /* someone is acting: lean in */
        ease(&pan_x, (float)(act_x / (double)act_n - w / 2.0) * cur.pan, kc);
        ease(&pan_y, (float)(act_y / (double)act_n - h / 2.0) * cur.pan * 0.5f, kc);
        ease(&zoom_extra, cur.zoom, kc);
    } else {
        ease(&pan_x, 0.0f, kc);
        ease(&pan_y, 0.0f, kc);
        ease(&zoom_extra, 0.0f, kc);
    }
    if (popups > 0 && last_popups == 0) { shake_amp = cur.shake; shake_t = 0.0f; }
    last_popups = (int)popups;
    shake_t += dt;
    float sh = shake_amp * expf(-shake_t / 0.12f);
    float cam_x = (dx + pan_x + sh * sinf(shake_t * two_pi * 17.0f)) * e;
    float cam_y = (dy + pan_y + sh * 0.5f * cosf(shake_t * two_pi * 13.0f)) * e;
    float zoom = 1.0f + zoom_extra * e;

    /* source coordinate for every output column and row, per layer */
    int ow = w * scale, oh = h * scale;
    static int16_t col_bg[VDP_MAX_W * MAX_SCALE], col_en[VDP_MAX_W * MAX_SCALE], col_pa[VDP_MAX_W * MAX_SCALE];
    static int16_t row_bg[VDP_MAX_H * MAX_SCALE], row_en[VDP_MAX_H * MAX_SCALE], row_pa[VDP_MAX_H * MAX_SCALE];
    build_map(col_bg, ow, scale, w, w / 2.0f, F_BG, zoom, cam_x, 1);
    build_map(row_bg, oh, scale, h, h * 0.45f, F_BG, zoom, cam_y, 1);
    build_map(col_en, ow, scale, w, w / 2.0f, F_ENEMY, zoom, cam_x, 0);
    build_map(row_en, oh, scale, h, h / 2.0f, F_ENEMY, zoom, cam_y, 0);
    build_map(col_pa, ow, scale, w, w / 2.0f, F_PARTY, zoom, cam_x, 0);
    build_map(row_pa, oh, scale, h, (float)PARTY_FEET_Y, F_PARTY, zoom, 0.0f, 0);   /* no vertical move */

    uint32_t pal[128];
    for (int i = 0; i < 64; i++) pal[i] = pal[i + 64] = vdp_color(i);

    for (int Y = 0; Y < oh; Y++) {
        int vu = Y / scale, ve = row_en[Y], vp = row_pa[Y], vb = row_bg[Y];
        uint32_t *dst = out + (size_t)Y * (size_t)out_stride;
        for (int X = 0; X < ow; X++) {
            int u, c;
            if (ve >= 0 && (u = col_en[X]) >= 0 && (c = layer[L_POINTER][ve][u]) != 0) { dst[X] = pal[c & 0x3F]; continue; }
            if ((c = layer[L_UI][vu][X / scale]) != 0) { dst[X] = pal[c & 0x3F]; continue; }
            if (vp >= 0 && (u = col_pa[X]) >= 0 && (c = layer[L_PARTY][vp][u]) != 0) { dst[X] = pal[c & 0x3F]; continue; }
            if (ve >= 0 && (u = col_en[X]) >= 0) {
                if ((c = layer[L_ACTION][ve][u]) != 0) { dst[X] = pal[c & 0x3F]; continue; }
                if ((c = layer[L_ENEMY][ve][u]) != 0) { dst[X] = pal[c & 0x3F]; continue; }
            }
            dst[X] = pal[layer[L_BG][vb][col_bg[X]] & 0x3F];
        }
    }
    return 1;
}
