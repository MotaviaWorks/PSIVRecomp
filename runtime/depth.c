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
#include "battleui.h"
#include "frame.h"
#include "m68k.h"
#include "weather.h"
#include "par.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE_INDEX 0xEF00u
#define MODE_BATTLE     0x14             /* GameMode_Battle */

#define TILE_PARTY_LO   0x180
#define TILE_PARTY_HI   0x280
#define TILE_UI_HI      0x680            /* $680-$7FF: window frames, font, digits */
#define PARTY_FEET_Y    168              /* top of the status window */
#define VEHICLE_INDEX   0xF43Cu          /* Vehicle_Index: 0 on foot */
#define TILE_DASH_LO    0x19B            /* a vehicle's dashboard: $19B-$27F ($196-$19A: its windows' frames) */
#define MAX_SCALE       4

#define F_BG    0.25f                    /* how far each layer follows the camera */
#define F_ENEMY 0.6f
#define F_PARTY 1.0f
/* the camera pan's turn: how far each layer drifts against the party's slide */
#define ORBIT_ENEMY 0.12f
#define ORBIT_BG    0.35f
#define ORBIT_FOG   0.25f

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

/* fog: maximum opacity per level, eased like the camera parameters */
static const float FOG_ALPHA[FOG_LEVELS] = {0.0f, 0.42f, 0.68f};
static const char *FOG_NAMES[FOG_LEVELS] = {"off", "light", "heavy"};
#define F_FOG   0.8f                     /* the fog sheet sits between enemies and party */
static int fog_level = 1;
static float fog_cur;
static float fog_tint[3];                /* eased toward the background's colour */
static int fog_tint_valid;

void depth_set_fog(int l) { fog_level = l < 0 ? 0 : l >= FOG_LEVELS ? FOG_LEVELS - 1 : l; }
int depth_get_fog(void) { return fog_level; }
const char *depth_fog_name(int l) { return FOG_NAMES[l < 0 || l >= FOG_LEVELS ? 0 : l]; }

/* ------------------------------------------------------------------ effects */
static const float FX_STRENGTH[FX_LEVELS] = {0.0f, 0.6f, 1.0f};
static const char *FX_NAMES[FX_LEVELS] = {"off", "subtle", "full"};
static int fx_level = 1;
static float fx_cur;
void depth_set_fx(int l) { fx_level = l < 0 ? 0 : l >= FX_LEVELS ? FX_LEVELS - 1 : l; }
int depth_get_fx(void) { return fx_level; }
const char *depth_fx_name(int l) { return FX_NAMES[l < 0 || l >= FX_LEVELS ? 0 : l]; }

/* light from bright action sprites, on a 4x4-pixel grid, smoothed over frames */
#define GRID 4
#define GW (VDP_MAX_W / GRID)
#define GH (VDP_MAX_H / GRID)
static float light_grid[3][GH][GW];
static uint8_t light_px[VDP_MAX_H][VDP_MAX_W][3];
static float light_amount, light_cx, light_cy, light_rgb[3];   /* smoothed totals */
static int light_on;

typedef struct { float x, y, vx, vy, life, max_life, size; uint8_t r, g, b, kind; } Particle;
enum { P_SPARK, P_EMBER, P_TWINKLE, P_FLASH };
#define MAX_PARTICLES 600
static Particle parts[MAX_PARTICLES];
static int nparts;
static uint32_t rng = 0x9E3779B9u;
static float frand(void) {                                 /* xorshift: 0..1 */
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFFFF) / 16777216.0f;
}
static void spawn(int kind, float x, float y, float vx, float vy, float life, float size,
                  int r, int g, int b) {
    if (nparts >= MAX_PARTICLES) return;
    Particle *p = &parts[nparts++];
    p->x = x; p->y = y; p->vx = vx; p->vy = vy; p->life = p->max_life = life; p->size = size;
    p->r = (uint8_t)r; p->g = (uint8_t)g; p->b = (uint8_t)b; p->kind = (uint8_t)kind;
}

/* damage numbers on screen, grouped by column: where hits land */
#define MAX_HITS 8
static float hit_x[MAX_HITS], hit_y[MAX_HITS];
static int nhits;

/* tileable 256x256 value noise, 3 octaves, 0..255 */
#define NOISE 256
static uint8_t noise[NOISE][NOISE];
static int noise_ready;

static float hash01(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float value_noise(float x, float y, int period, int seed) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = ((x0 % period) + period) % period; x1 = ((x1 % period) + period) % period;
    y0 = ((y0 % period) + period) % period; y1 = ((y1 % period) + period) % period;
    float a = hash01(x0, y0, seed), b = hash01(x1, y0, seed), c = hash01(x0, y1, seed), d = hash01(x1, y1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static void make_noise(void) {
    for (int y = 0; y < NOISE; y++)
        for (int x = 0; x < NOISE; x++) {
            float n = 0.55f * value_noise(x / 64.0f, y / 64.0f, 4, 1)
                    + 0.30f * value_noise(x / 32.0f, y / 32.0f, 8, 2)
                    + 0.15f * value_noise(x / 16.0f, y / 16.0f, 16, 3);
            noise[y][x] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, n)) * 255.0f);
        }
    noise_ready = 1;
}
static float ramp;                       /* 0..1, eases the effect in at battle start */
static float t;                          /* seconds of battle shown */
static float pan_x, pan_y, zoom_extra;   /* camera, eased */
static float shake_amp, shake_t;
static float pan_a;                      /* the camera pan (battleui_pan): pixels, eased */
static int last_popups;

void depth_set_level(int l) { level = l < 0 ? 0 : l >= DEPTH_LEVELS ? DEPTH_LEVELS - 1 : l; }
int depth_get_level(void) { return level; }
const char *depth_level_name(int l) { return NAMES[l < 0 || l >= DEPTH_LEVELS ? 0 : l]; }

/* the frame sorted into layers: palette index | priority, 0 = transparent */
enum { L_BG, L_ENEMY, L_ACTION, L_PARTY, L_UI, L_POINTER, NLAYERS };
static uint8_t layer[NLAYERS][VDP_MAX_H][VDP_MAX_W];
/* party pixels the port put back (battleui.c): the action sprites pass in front */
static uint8_t party_under[VDP_MAX_H][VDP_MAX_W];
/* Sprites on the party (a blow landing on someone, a glow in a caster's hand)
 * follow the party's camera instead of the enemies', or the camera's lean
 * would slide them off the one they are on. Sprites that touch make one
 * object, moved as one; one coming down on someone eases over to the party's
 * camera on the way. Here: those pixels, as 1 + object, and their colours. */
static uint8_t near_obj[VDP_MAX_H][VDP_MAX_W];
static uint8_t near_px[VDP_MAX_H][VDP_MAX_W];
/* what the output already shows in front: 0 nothing, 1 the party, 2 a party
 * picture put back (a low sprite passes in front), 3 a window or pointer */
static uint8_t front[VDP_MAX_H * MAX_SCALE][VDP_MAX_W * MAX_SCALE];
static int sprite_root(int *parent, int i) {
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
}

static void ease(float *v, float target, float k) { *v += (target - *v) * k; }

/* a colour darkened (m/256 a channel) and brightened (+a) */
static inline uint32_t tinted(uint32_t s, int mr, int mg, int mb, int ar, int ag, int ab) {
    int r = ((int)(s >> 16 & 255) * mr >> 8) + ar, g = ((int)(s >> 8 & 255) * mg >> 8) + ag, b = ((int)(s & 255) * mb >> 8) + ab;
    return 0xFF000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
}

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

/* --------------------------------------------------------------- effect helpers */
/* Bright pixels of the action sprites (weapons, technique effects) are light
 * sources: summed on a coarse grid, blurred, smoothed over frames and spread
 * back to every pixel, so flames light the scene orange, and so on. */
static void fx_build_light(const uint32_t *pal, int w, int h, float strength) {
    static float acc[3][GH][GW], tmp[GH][GW];
    int gw = w / GRID, gh = h / GRID;
    memset(acc, 0, sizeof acc);
    float amount = 0, cx = 0, cy = 0, rgb[3] = {0, 0, 0};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t c = layer[L_ACTION][y][x];
            if (!c) continue;
            uint32_t col = pal[c & 0x3F];
            float r = (float)(col >> 16 & 255), g = (float)(col >> 8 & 255), b = (float)(col & 255);
            float mx = fmaxf(r, fmaxf(g, b));
            if (mx < 170.0f) continue;
            float wgt = (mx - 150.0f) / 105.0f;
            wgt *= wgt;
            acc[0][y / GRID][x / GRID] += r * wgt;
            acc[1][y / GRID][x / GRID] += g * wgt;
            acc[2][y / GRID][x / GRID] += b * wgt;
            amount += wgt; cx += (float)x * wgt; cy += (float)y * wgt;
            rgb[0] += r * wgt; rgb[1] += g * wgt; rgb[2] += b * wgt;
        }
    /* box blur, radius 4 cells (16 px), twice each way */
    for (int ch = 0; ch < 3; ch++)
        for (int pass = 0; pass < 2; pass++) {
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    float s = 0;
                    for (int d = -4; d <= 4; d++) { int xx = x + d; if (xx >= 0 && xx < gw) s += acc[ch][y][xx]; }
                    tmp[y][x] = s / 9.0f;
                }
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    float s = 0;
                    for (int d = -4; d <= 4; d++) { int yy = y + d; if (yy >= 0 && yy < gh) s += tmp[yy][x]; }
                    acc[ch][y][x] = s / 9.0f;
                }
        }
    float peak = 0;
    for (int ch = 0; ch < 3; ch++)
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++) {
                light_grid[ch][y][x] = light_grid[ch][y][x] * 0.55f + acc[ch][y][x] * 0.45f;
                if (light_grid[ch][y][x] > peak) peak = light_grid[ch][y][x];
            }
    light_amount = light_amount * 0.7f + amount * 0.3f;
    if (amount > 0) {
        light_cx = light_cx * 0.7f + cx / amount * 0.3f;
        light_cy = light_cy * 0.7f + cy / amount * 0.3f;
        for (int i = 0; i < 3; i++) light_rgb[i] = light_rgb[i] * 0.7f + rgb[i] / amount * 0.3f;
    }
    light_on = peak > 4.0f;
    if (!light_on) return;
    /* spread to pixels (bilinear); gain sets how strongly the scene is lit */
    float gain = 0.075f * strength;
    for (int y = 0; y < h; y++) {
        float gy = ((float)y + 0.5f) / GRID - 0.5f;
        int y0 = (int)floorf(gy), y1 = y0 + 1;
        float fy = gy - (float)y0;
        if (y0 < 0) y0 = 0;
        if (y1 >= gh) y1 = gh - 1;
        if (y0 >= gh) y0 = gh - 1;
        for (int x = 0; x < w; x++) {
            float gx = ((float)x + 0.5f) / GRID - 0.5f;
            int x0 = (int)floorf(gx), x1 = x0 + 1;
            float fx = gx - (float)x0;
            if (x0 < 0) x0 = 0;
            if (x1 >= gw) x1 = gw - 1;
            if (x0 >= gw) x0 = gw - 1;
            for (int ch = 0; ch < 3; ch++) {
                float a = light_grid[ch][y0][x0] + (light_grid[ch][y0][x1] - light_grid[ch][y0][x0]) * fx;
                float b = light_grid[ch][y1][x0] + (light_grid[ch][y1][x1] - light_grid[ch][y1][x0]) * fx;
                float v = (a + (b - a) * fy) * gain;
                light_px[y][x][ch] = (uint8_t)(v > 160.0f ? 160.0f : v);
            }
        }
    }
}

/* where hits land: each damage number is a small window above its target;
 * a new one (not there last frame) means a hit just landed below it */
static void fx_hits(const int *pop_cnt, const long *pop_ysum, int w, float strength) {
    float nx[MAX_HITS], ny[MAX_HITS];
    int n = 0;
    for (int x = 0; x < w && n < MAX_HITS;) {
        if (!pop_cnt[x]) { x++; continue; }
        long cnt = 0, xs = 0, ys = 0;
        int gap = 0, x0 = x;
        for (; x < w && gap <= 8; x++) {
            if (pop_cnt[x]) { cnt += pop_cnt[x]; xs += (long)x * pop_cnt[x]; ys += pop_ysum[x]; gap = 0; }
            else gap++;
        }
        (void)x0;
        if (cnt < 24) continue;
        nx[n] = (float)xs / (float)cnt;
        ny[n] = (float)ys / (float)cnt;
        n++;
    }
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < nhits; j++)
            if (fabsf(nx[i] - hit_x[j]) < 14.0f && fabsf(ny[i] - hit_y[j]) < 14.0f) seen = 1;
        if (seen) continue;
        float ix = nx[i], iy = ny[i] + 26.0f;                 /* the target, below its number */
        spawn(P_FLASH, ix, iy, 0, 0, 0.18f, 30.0f * (0.7f + 0.3f * strength), 255, 236, 200);
        int sparks = (int)(8.0f + 14.0f * strength);
        for (int k = 0; k < sparks; k++) {
            float ang = frand() * 6.2831853f, sp = 70.0f + frand() * 140.0f;
            spawn(P_SPARK, ix, iy, cosf(ang) * sp, sinf(ang) * sp - 50.0f, 0.25f + frand() * 0.3f,
                  0.9f + frand() * 0.6f, 255, 245, 190);
        }
    }
    nhits = n;
    memcpy(hit_x, nx, sizeof nx);
    memcpy(hit_y, ny, sizeof ny);
}

/* embers rise from warm light (fire), twinkles sparkle on cool bright light */
static void fx_spawn_from_light(int w, int h, const uint32_t *pal, float dt, float strength) {
    if (light_amount < 3.0f) return;
    int warm = light_rgb[0] > light_rgb[1] * 1.15f && light_rgb[0] > light_rgb[2] * 1.5f;
    float rate = fminf(90.0f, light_amount * 0.5f) * strength;       /* per second */
    static float carry;
    carry += rate * dt;
    while (carry >= 1.0f) {
        carry -= 1.0f;
        for (int tries = 0; tries < 12; tries++) {                    /* a random bright pixel */
            int x = (int)(light_cx + (frand() - 0.5f) * 80.0f), y = (int)(light_cy + (frand() - 0.5f) * 80.0f);
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            uint8_t c = layer[L_ACTION][y][x];
            if (!c) continue;
            uint32_t col = pal[c & 0x3F];
            if (((col >> 16 & 255) < 170) && ((col >> 8 & 255) < 170) && ((col & 255) < 170)) continue;
            if (warm)
                spawn(P_EMBER, (float)x, (float)y, (frand() - 0.5f) * 30.0f, -25.0f - frand() * 40.0f,
                      0.7f + frand() * 0.9f, 1.1f + frand() * 0.7f, 255, 150 + (int)(frand() * 70), 60);
            else
                spawn(P_TWINKLE, (float)x, (float)y, 0, 0, 0.25f + frand() * 0.2f, 1.2f + frand() * 0.8f,
                      (int)(col >> 16 & 255) / 2 + 128, (int)(col >> 8 & 255) / 2 + 128, (int)(col & 255) / 2 + 128);
            break;
        }
    }
}

static void fx_update(float dt) {
    int j = 0;
    for (int i = 0; i < nparts; i++) {
        Particle *p = &parts[i];
        p->life -= dt;
        if (p->life <= 0) continue;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if (p->kind == P_SPARK) { p->vy += 320.0f * dt; p->vx *= 0.96f; }
        if (p->kind == P_EMBER) { p->vx += (frand() - 0.5f) * 60.0f * dt; p->vy *= 0.995f; }
        parts[j++] = *p;
    }
    nparts = j;
}

/* additive soft dot at output resolution, kept off the windows */
static void add_dot(uint32_t *out, int stride, int ow, int oh, int scale, float X, float Y, float R,
                    float r, float g, float b) {
    int x0 = (int)floorf(X - R), x1 = (int)ceilf(X + R), y0 = (int)floorf(Y - R), y1 = (int)ceilf(Y + R);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= ow) x1 = ow - 1;
    if (y1 >= oh) y1 = oh - 1;
    float inv = 1.0f / (R * R);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (layer[L_UI][y / scale][x / scale]) continue;
            float dx = (float)x + 0.5f - X, dy = (float)y + 0.5f - Y, d2 = (dx * dx + dy * dy) * inv;
            if (d2 >= 1.0f) continue;
            float a = (1.0f - d2) * (1.0f - d2);
            uint32_t c = out[(size_t)y * (size_t)stride + (size_t)x];
            int cr = (int)(c >> 16 & 255) + (int)(r * a), cg = (int)(c >> 8 & 255) + (int)(g * a),
                cb = (int)(c & 255) + (int)(b * a);
            out[(size_t)y * (size_t)stride + (size_t)x] = 0xFF000000u | (uint32_t)(cr > 255 ? 255 : cr) << 16 |
                                                          (uint32_t)(cg > 255 ? 255 : cg) << 8 |
                                                          (uint32_t)(cb > 255 ? 255 : cb);
        }
    }
}

/* particles, hit flashes and the lens flare, drawn over the composed frame */
static void fx_draw(uint32_t *out, int stride, int w, int h, int scale, float cam_x, float cam_y, float zoom,
                    float strength) {
    int ow = w * scale, oh = h * scale;
    float s = 1.0f + F_ENEMY * (zoom - 1.0f), cxs = w / 2.0f, cys = h / 2.0f;
    for (int i = 0; i < nparts; i++) {
        const Particle *p = &parts[i];
        float qx = cxs + (p->x - cxs - F_ENEMY * cam_x) * s, qy = cys + (p->y - cys - F_ENEMY * cam_y) * s;
        float age = 1.0f - p->life / p->max_life;            /* 0 new .. 1 gone */
        float X = qx * (float)scale, Y = qy * (float)scale;
        float a, R;
        switch (p->kind) {
        case P_FLASH:
            R = p->size * (0.35f + 0.65f * sqrtf(age)) * (float)scale;
            a = (1.0f - age) * 0.85f * strength;
            add_dot(out, stride, ow, oh, scale, X, Y, R, p->r * a, p->g * a, p->b * a);
            break;
        case P_SPARK:
            R = p->size * (float)scale;
            a = (1.0f - age) * strength;
            add_dot(out, stride, ow, oh, scale, X, Y, R + 0.6f * (float)scale, p->r * a, p->g * (1.0f - 0.4f * age) * a,
                    p->b * (1.0f - age) * a);
            break;
        case P_EMBER:
            R = p->size * (float)scale;
            a = sinf(age * 3.14159f) * (0.8f + 0.5f * frand()) * strength * 1.4f;   /* glow up, fade, flicker */
            add_dot(out, stride, ow, oh, scale, X, Y, R + 0.5f * (float)scale, p->r * a,
                    p->g * (1.0f - 0.5f * age) * a, p->b * a);
            break;
        default:   /* twinkle */
            R = p->size * (float)scale * (1.0f - age);
            a = (1.0f - age) * strength;
            add_dot(out, stride, ow, oh, scale, X, Y, R + 0.5f * (float)scale, p->r * a, p->g * a, p->b * a);
            break;
        }
    }
    /* lens flare when the effects are bright: a horizontal streak through the
     * light and faint ghosts mirrored through the screen centre */
    float flare = (light_amount - 40.0f) / 160.0f;
    if (flare <= 0.0f) return;
    if (flare > 1.0f) flare = 1.0f;
    flare *= strength * 0.8f;
    float lx = (cxs + (light_cx - cxs - F_ENEMY * cam_x) * s) * (float)scale;
    float ly = (cys + (light_cy - cys - F_ENEMY * cam_y) * s) * (float)scale;
    float m = fmaxf(light_rgb[0], fmaxf(light_rgb[1], light_rgb[2]));
    float tr = m > 0 ? light_rgb[0] / m : 1, tg = m > 0 ? light_rgb[1] / m : 1, tb = m > 0 ? light_rgb[2] / m : 1;
    float len = 150.0f * (float)scale, thick = 1.4f * (float)scale;
    int y0 = (int)(ly - thick * 4), y1 = (int)(ly + thick * 4);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < oh; y++) {
        float vy = ((float)y - ly) / thick, fy = expf(-vy * vy);
        for (int x = (int)(lx - len) < 0 ? 0 : (int)(lx - len); x < ow && x <= (int)(lx + len); x++) {
            if (layer[L_UI][y / scale][x / scale]) continue;
            float fx = 1.0f - fabsf((float)x - lx) / len;
            float a = fx * fx * fy * flare * 110.0f;
            uint32_t c = out[(size_t)y * (size_t)stride + (size_t)x];
            int cr = (int)(c >> 16 & 255) + (int)(a * (0.5f + 0.5f * tr)), cg = (int)(c >> 8 & 255) + (int)(a * (0.5f + 0.5f * tg)),
                cb = (int)(c & 255) + (int)(a * (0.6f + 0.4f * tb));
            out[(size_t)y * (size_t)stride + (size_t)x] = 0xFF000000u | (uint32_t)(cr > 255 ? 255 : cr) << 16 |
                                                          (uint32_t)(cg > 255 ? 255 : cg) << 8 |
                                                          (uint32_t)(cb > 255 ? 255 : cb);
        }
    }
    float ccx = (float)ow / 2.0f, ccy = (float)oh / 2.0f;
    static const float pos[3] = {0.55f, 1.25f, 1.7f}, rad[3] = {9.0f, 15.0f, 5.0f};
    for (int i = 0; i < 3; i++) {
        float gx = lx + (ccx - lx) * pos[i] * 2.0f, gy = ly + (ccy - ly) * pos[i] * 2.0f;
        float a = flare * 38.0f;
        add_dot(out, stride, ow, oh, scale, gx, gy, rad[i] * (float)scale, a * tr * 0.8f, a * tg, a * (0.5f + 0.5f * tb));
    }
}

/* the camera of the last composed frame, for overlays drawn over it */
static int cam_valid;
static float cam_last_x, cam_last_y, cam_last_zoom, cam_last_w, cam_last_h, cam_last_en;

void depth_to_screen(int party, float x, float y, float *ox, float *oy) {
    if (!cam_valid) { *ox = x; *oy = y; return; }
    float f = party ? F_PARTY : F_ENEMY, s = 1.0f + f * (cam_last_zoom - 1.0f);
    float cx = cam_last_w / 2.0f, cy = party ? (float)PARTY_FEET_Y : cam_last_h / 2.0f;
    *ox = cx + (x - f * (party ? cam_last_x : cam_last_en) - cx) * s;
    *oy = cy + (y - (party ? 0.0f : f * cam_last_y) - cy) * s;
}

/* source coordinate for every output column and row, per layer */
static int16_t col_bg[VDP_MAX_W * MAX_SCALE], col_en[VDP_MAX_W * MAX_SCALE], col_pa[VDP_MAX_W * MAX_SCALE];
static int16_t row_bg[VDP_MAX_H * MAX_SCALE], row_en[VDP_MAX_H * MAX_SCALE], row_pa[VDP_MAX_H * MAX_SCALE];
/* the fog's noise rows and columns, and how thick it is on each row */
static uint8_t fog_row1[VDP_MAX_H * MAX_SCALE], fog_row2[VDP_MAX_H * MAX_SCALE];
static uint8_t fog_col1[VDP_MAX_W * MAX_SCALE], fog_col2[VDP_MAX_W * MAX_SCALE];
static uint16_t fog_profile[VDP_MAX_H * MAX_SCALE];
static uint8_t mimg[VDP_MAX_H][VDP_MAX_W];                /* the camera pan: the others, whole, at their places */

/* The picture's main pass, a row of the output at a time: for each pixel the
 * nearest thing there, through each layer's own camera. What a row shows
 * depends only on the source rows its layers fall on and on its fog, so a
 * row with the same ones as the row above (most rows, the picture being
 * several times the game's size) is a copy of it. Rows are drawn in pieces
 * on several cores (par.h): a piece writes only its own rows. */
typedef struct {
    uint32_t *out;
    int out_stride, scale, ow;
    const uint32_t *pal, *tpal;
    int panning, t_out, a_out, lit, fog_on, tint_r, tint_g, tint_b;
} Rows;

static void compose_rows(void *ctx, int y0, int y1) {
    const Rows *q = ctx;
    uint32_t *out = q->out;
    const uint32_t *pal = q->pal, *tpal = q->tpal;
    int out_stride = q->out_stride, scale = q->scale, ow = q->ow;
    int panning = q->panning, t_out = q->t_out, a_out = q->a_out, lit = q->lit, fog_on = q->fog_on;
    int tint_r = q->tint_r, tint_g = q->tint_g, tint_b = q->tint_b;
    for (int Y = y0; Y < y1; Y++) {
        int vu = Y / scale, ve = row_en[Y], vp = row_pa[Y], vb = row_bg[Y];
        uint8_t *fr = front[Y];
        uint32_t *dst = out + (size_t)Y * (size_t)out_stride;
        const uint8_t *nrow1 = noise[fog_row1[Y]], *nrow2 = noise[fog_row2[Y]];
        int prof = fog_on ? fog_profile[Y] : 0;
        if (Y > y0 && vu == (Y - 1) / scale && ve == row_en[Y - 1] && vp == row_pa[Y - 1] && vb == row_bg[Y - 1]
            && prof == (fog_on ? fog_profile[Y - 1] : 0)
            && (!prof || (fog_row1[Y] == fog_row1[Y - 1] && fog_row2[Y] == fog_row2[Y - 1]))) {
            memcpy(dst, dst - out_stride, (size_t)ow * sizeof *dst);      /* the row above, again */
            memcpy(fr, front[Y - 1], (size_t)ow);
            continue;
        }
        for (int X = 0; X < ow; X++) {
            int u = -1, c;
            /* in front of the fog: cursor over an enemy, windows, party */
            if (ve >= 0 && (u = col_en[X]) >= 0 && (c = layer[L_POINTER][ve][u]) != 0) {
                dst[X] = pal[c & 0x3F];
                fr[X] = 3;
                continue;
            }
            if ((c = layer[L_UI][vu][X / scale]) != 0) { dst[X] = pal[c & 0x3F]; fr[X] = 3; continue; }
            int pu = 0;                                       /* the party here, and if put back (under action) */
            c = 0;
            if (vp >= 0) {
                if (!panning) {
                    if ((u = col_pa[X]) >= 0 && (c = layer[L_PARTY][vp][u]) != 0) pu = party_under[vp][u];
                } else {
                    int XT = X - t_out, XO = X - a_out;
                    if (XT >= 0 && XT < ow && (u = col_pa[XT]) >= 0 && (c = layer[L_PARTY][vp][u]) != 0) pu = party_under[vp][u];
                    if (!c && XO >= 0 && XO < ow && (u = col_pa[XO]) >= 0 && (c = mimg[vp][u]) != 0) pu = 1;
                }
            }
            if (c && !(pu && ve >= 0 && col_en[X] >= 0 && layer[L_ACTION][ve][col_en[X]])) {
                uint32_t pc = tpal[c & 0x3F];
                if (lit && ve >= 0 && col_en[X] >= 0) {                /* the party catches some of the light */
                    const uint8_t *lp = light_px[ve][col_en[X]];
                    int r = (int)(pc >> 16 & 255) + lp[0] / 3, g = (int)(pc >> 8 & 255) + lp[1] / 3, b = (int)(pc & 255) + lp[2] / 3;
                    pc = 0xFF000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
                }
                dst[X] = pc;
                fr[X] = pu ? 2 : 1;
                continue;
            }
            /* behind it: action, enemies, background */
            uint32_t far;
            uint8_t what = WX_FRONT_ENEMY;
            if (ve >= 0 && (u = col_en[X]) >= 0 && (c = layer[L_ACTION][ve][u]) != 0) far = tpal[c & 0x3F];
            else if (ve >= 0 && u >= 0 && (c = layer[L_ENEMY][ve][u]) != 0) far = tpal[c & 0x3F];
            else { far = tpal[layer[L_BG][vb][col_bg[X]] & 0x3F]; what = WX_FRONT_BG; }
            if (fog_on && prof) {
                int d = (nrow1[fog_col1[X]] * 5 + nrow2[fog_col2[X]] * 3) >> 3;   /* 0..255 */
                d = (d - 80) * 2;                                                 /* contrast: wisps */
                if (d > 0) {
                    if (d > 255) d = 255;
                    int a = (d * prof) >> 8;                                      /* 0..256 */
                    int r = far >> 16 & 255, g = far >> 8 & 255, b = far & 255;
                    r += ((tint_r - r) * a) >> 8;
                    g += ((tint_g - g) * a) >> 8;
                    b += ((tint_b - b) * a) >> 8;
                    far = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
                }
            }
            if (lit && ve >= 0 && (u = col_en[X]) >= 0) {             /* lit by the effects */
                const uint8_t *lp = light_px[ve][u];
                int r = (int)(far >> 16 & 255) + lp[0], g = (int)(far >> 8 & 255) + lp[1], b = (int)(far & 255) + lp[2];
                far = 0xFF000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
            }
            dst[X] = far;
            fr[X] = what;
        }
    }
}

int depth_compose(const VdpLine *lines, int w, int h, uint32_t *out, int out_stride, int scale) {
    const float dt = 1.0f / 59.92f;
    cam_valid = 0;
    uint16_t mode = (uint16_t)(m68k_ram[GAME_MODE_INDEX] << 8 | m68k_ram[GAME_MODE_INDEX + 1]);

    float k = 1.0f - expf(-dt / 0.3f);                    /* level changes ease over ~0.3 s */
    const Params *tp = &LEVEL_PARAMS[level];
    ease(&cur.drift_x, tp->drift_x, k);
    ease(&cur.drift_y, tp->drift_y, k);
    ease(&cur.zoom, tp->zoom, k);
    ease(&cur.pan, tp->pan, k);
    ease(&cur.shake, tp->shake, k);

    ease(&fog_cur, FOG_ALPHA[fog_level], k);
    ease(&fx_cur, FX_STRENGTH[fx_level], k);
    if (mode != MODE_BATTLE) {
        ramp = t = pan_x = pan_y = zoom_extra = shake_amp = pan_a = 0.0f;
        last_popups = 0;
        fog_tint_valid = 0;
        nparts = nhits = 0;
        light_amount = 0.0f;
        light_on = 0;
        memset(light_grid, 0, sizeof light_grid);
        weather_battle_end();
        return 0;
    }
    int wide = vdp_view_x0 < 0;                               /* widescreen: margins to fill */
    int wx = weather_active();                                /* rain, snow, clouds, debris (weather.c) */
    if (!wide && !wx && level == 0 && cur.drift_x < 0.02f && cur.zoom < 0.0005f && fog_cur < 0.005f && fx_cur < 0.005f)
        return 0;                                             /* all eased out */
    if (!noise_ready) make_noise();
    if (scale > MAX_SCALE) scale = MAX_SCALE;
    if (scale < 1) scale = 1;
    ramp = fminf(1.0f, ramp + dt / 0.6f);
    t += dt;

    /* sort the frame into layers */
    static int sx0[128], sy0[128], sx1[128], sy1[128], near_ui[128], hi[128], on_party[128];
    static int ptop_col[VDP_MAX_W];                           /* each column's highest party pixel */
    for (int i = 0; i < 128; i++) { sx0[i] = sy0[i] = 1 << 20; sx1[i] = sy1[i] = -1; hi[i] = on_party[i] = 0; }
    for (int x = 0; x < w; x++) ptop_col[x] = h;
    long act_n = 0, popups = 0;
    double act_x = 0, act_y = 0;
    static int pop_cnt[VDP_MAX_W];
    static long pop_ysum[VDP_MAX_W];
    memset(pop_cnt, 0, sizeof pop_cnt);
    memset(pop_ysum, 0, sizeof pop_ysum);
    /* a vehicle's battle: no party; the art in the party's tiles is the cockpit's dashboard (and the
     * cockpit's own window frames, $196-$19A), which stays put like a window */
    int vehicle = (m68k_ram[VEHICLE_INDEX] << 8 | m68k_ram[VEHICLE_INDEX + 1]) != 0;
    for (int y = 0; y < h; y++) {
        const VdpLine *l = &lines[y];
        for (int x = 0; x < w; x++) {
            uint8_t b = l->plane_b[x], s = l->sprite[x];
            uint8_t a = l->win_in[x] ? l->window[x] : l->plane_a[x];
            uint16_t tile = l->plane_a_tile[x] & 0x7FF;
            for (int n = 1; n < NLAYERS; n++) layer[n][y][x] = 0;
            party_under[y][x] = 0;
            layer[L_BG][y][x] = b ? b : (uint8_t)(l->backdrop | 0x40);   /* never transparent */
            if (a) {
                if (l->win_in[x]) layer[L_UI][y][x] = a;
                else if (!(a & 0x80)) layer[L_ENEMY][y][x] = a;
                else if (vehicle && tile >= TILE_PARTY_LO && tile < TILE_PARTY_HI) layer[L_UI][y][x] = a;
                else if (tile >= TILE_PARTY_LO && tile < TILE_PARTY_HI) {
                    layer[L_PARTY][y][x] = a;
                    party_under[y][x] = l->plane_a_under[x];
                    if (y < ptop_col[x]) ptop_col[x] = y;
                }
                else if (tile >= TILE_PARTY_HI && tile < TILE_UI_HI) {
                    layer[L_ENEMY][y][x] = a;                 /* a damage number */
                    popups++; pop_cnt[x]++; pop_ysum[x] += y;
                }
                else layer[L_UI][y][x] = a;
            }
            if (s) {
                int id = l->sprite_no[x] & 127;
                if (x < sx0[id]) sx0[id] = x;
                if (x > sx1[id]) sx1[id] = x;
                if (y < sy0[id]) sy0[id] = y;
                if (y > sy1[id]) sy1[id] = y;
                if (layer[L_PARTY][y][x]) on_party[id] = 1;
                if (!(s & 0x80)) {
                    layer[L_ACTION][y][x] = s;
                    act_n++; act_x += x; act_y += y;
                } else
                    hi[id] = 1;
            }
        }
    }
    /* widescreen: battle backgrounds are drawn exactly 320 pixels wide, so
     * where a line's margins are empty, mirror the background into them */
    if (wide) {
        int m = -vdp_view_x0, right0 = m + (w - 2 * m);
        for (int y = 0; y < h; y++) {
            int empty = 0;
            for (int x = 0; x < m; x++) empty += lines[y].plane_b[x] == 0;
            for (int x = right0; x < w; x++) empty += lines[y].plane_b[x] == 0;
            if (empty * 10 < 2 * m * 9) continue;              /* the margins have art: keep it */
            for (int x = 0; x < m; x++) layer[L_BG][y][x] = layer[L_BG][y][2 * m - 1 - x];
            for (int x = right0; x < w; x++) layer[L_BG][y][x] = layer[L_BG][y][2 * right0 - 1 - x];
        }
    }
    /* and a vehicle's dashboard is 320 pixels wide too: mirrored into the margins, it spans the view (its
     * art is mirror-symmetric already). Only the dashboard's own tiles: not a window near the edge */
    if (wide && vehicle) {
        int m = -vdp_view_x0, right0 = m + (w - 2 * m);
        for (int y = 0; y < h; y++) {
            const VdpLine *l = &lines[y];
            for (int x = 0; x < m; x++) {
                int sx = 2 * m - 1 - x, tile = l->plane_a_tile[sx] & 0x7FF;
                if (!layer[L_UI][y][x] && layer[L_UI][y][sx] && tile >= TILE_DASH_LO && tile < TILE_PARTY_HI)
                    layer[L_UI][y][x] = layer[L_UI][y][sx];
            }
            for (int x = right0; x < w; x++) {
                int sx = 2 * right0 - 1 - x, tile = l->plane_a_tile[sx] & 0x7FF;
                if (!layer[L_UI][y][x] && layer[L_UI][y][sx] && tile >= TILE_DASH_LO && tile < TILE_PARTY_HI)
                    layer[L_UI][y][x] = layer[L_UI][y][sx];
            }
        }
    }

    /* The camera pan: while an enemy attacks someone (or someone casts),
     * the game stands them under the enemy, often over another member. The
     * view turns to face them instead: the one stood there glides from their
     * place to it, everyone else slides the same way as one row, drawn whole
     * from the pictures kept at the menus (battleui.c), so no one overlaps;
     * and back again after. */
    int pan_m = -1, pan_to = 0;
    battleui_pan(&pan_m, &pan_to);
    ease(&pan_a, (float)pan_to, 1.0f - expf(-dt / 0.14f));
    if (pan_m < 0 && fabsf(pan_a) < 0.3f) pan_a = 0.0f;
    int panning = pan_m >= 0 || pan_a != 0.0f;
    int tx0 = 1 << 20, tx1 = -1, mx0 = 1 << 20, mx1 = -1;   /* columns: the one stood there; the others */
    if (panning) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                if (layer[L_PARTY][y][x]) {
                    if (battleui_tile_member(lines[y].plane_a_tile[x] & 0x7FF) != pan_m) {
                        layer[L_PARTY][y][x] = 0;
                        party_under[y][x] = 0;
                    } else {
                        if (x < tx0) tx0 = x;
                        if (x > tx1) tx1 = x;
                    }
                }
                int px, py, mm = -1;
                vdp_plane_xy(0, x + vdp_view_x0, y, &px, &py);
                mimg[y][x] = battleui_pan_pixel(px, py, &mm);
                if (mimg[y][x]) {
                    if (x < mx0) mx0 = x;
                    if (x > mx1) mx1 = x;
                }
            }
    }
    int a_out = (int)lroundf(pan_a * (float)scale);                       /* the others' shift */
    int t_out = pan_m >= 0 ? (int)lroundf((pan_a - (float)pan_to) * (float)scale) : 0;   /* the one stood there */

    /* objects: sprites that touch, or all but touch (an enemy's lunge, a
     * claw, a technique's burst), so no picture is ever split between layers */
    static int parent[128], obj_ui[128], obj_party[128], ox0[128], oy0[128], ox1[128], oy1[128];
    static float obj_w[128];
    for (int i = 0; i < 128; i++) {
        parent[i] = i;
        obj_ui[i] = obj_party[i] = 0;
        obj_w[i] = 0.0f;
        ox0[i] = oy0[i] = 1 << 20;
        ox1[i] = oy1[i] = -1;
    }
    for (int i = 0; i < 128; i++) {
        if (sx1[i] < 0) continue;
        for (int j = i + 1; j < 128; j++)
            if (sx1[j] >= 0 && sx0[i] <= sx1[j] + 5 && sx0[j] <= sx1[i] + 5 && sy0[i] <= sy1[j] + 5 && sy0[j] <= sy1[i] + 5) {
                int a = sprite_root(parent, i), b = sprite_root(parent, j);
                if (a != b) parent[b] = a;
            }
    }
    /* high-priority sprites: menu cursors stay with their window, the
     * cursor over an enemy moves with the enemies */
    for (int id = 0; id < 128; id++) {
        near_ui[id] = 0;
        if (sx1[id] < 0) continue;
        int r = sprite_root(parent, id);
        if (sx0[id] < ox0[r]) ox0[r] = sx0[id];
        if (sy0[id] < oy0[r]) oy0[r] = sy0[id];
        if (sx1[id] > ox1[r]) ox1[r] = sx1[id];
        if (sy1[id] > oy1[r]) oy1[r] = sy1[id];
        if (on_party[id]) obj_party[r] = 1;
        if (!hi[id]) continue;
        for (int y = sy0[id] - 4; y <= sy1[id] + 4 && !near_ui[id]; y++)
            for (int x = sx0[id] - 4; x <= sx1[id] + 4; x++)
                if (y >= 0 && y < h && x >= 0 && x < w && layer[L_UI][y][x]) { near_ui[id] = 1; break; }
        if (near_ui[id]) obj_ui[r] = 1;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t s = lines[y].sprite[x];
            if (s & 0x80) layer[obj_ui[sprite_root(parent, lines[y].sprite_no[x] & 127)] ? L_UI : L_POINTER][y][x] = s;
        }
    /* on the party: over someone's picture, or coming down on them (all the
     * way by the time it reaches the top of their head) */
    int any_near = 0;
    for (int r = 0; r < 128; r++) {
        if (ox1[r] < 0 || sprite_root(parent, r) != r || obj_ui[r]) continue;
        int top = h;
        for (int x = ox0[r] < 0 ? 0 : ox0[r]; x <= ox1[r] && x < w; x++)
            if (ptop_col[x] < top) top = ptop_col[x];
        if (top >= h) continue;                               /* no one below it */
        float wgt = obj_party[r] ? 1.0f : (float)(oy1[r] + 1 - (top - 16)) / 16.0f;
        obj_w[r] = wgt < 0.0f ? 0.0f : wgt > 1.0f ? 1.0f : wgt;
        any_near |= obj_w[r] > 0.0f;
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
    {   /* PSIV_FX_LOG=1: print battle events (for designing effects) */
        static int log_on = -1, last_act;
        if (log_on < 0) { const char *v = getenv("PSIV_FX_LOG"); log_on = v && *v == '1'; }
        int act_on = act_n > 40;
        if (log_on && ((popups > 0) != (last_popups > 0) || act_on != last_act)) {
            uint16_t actor = (uint16_t)(m68k_ram[0x4142] << 8 | m68k_ram[0x4143]);
            uint16_t target = (uint16_t)(m68k_ram[0x4144] << 8 | m68k_ram[0x4145]);
            uint16_t command = (uint16_t)(m68k_ram[0x4146] << 8 | m68k_ram[0x4147]);
            uint16_t routine = (uint16_t)(m68k_ram[0x4100] << 8 | m68k_ram[0x4101]);
            printf("fx t=%.2f L%llu %s%s actor %04X target %04X command %04X routine %04X act %ld at (%.0f,%.0f) popups %ld\n",
                   t, (unsigned long long)frame_logical, act_on != last_act ? (act_on ? "ACT+ " : "ACT- ") : "",
                   (popups > 0) != (last_popups > 0) ? (popups > 0 ? "HIT+ " : "HIT- ") : "",
                   actor, target, command, routine, act_n,
                   act_n ? act_x / (double)act_n : 0.0, act_n ? act_y / (double)act_n : 0.0, popups);
        }
        last_act = act_on;
    }
    if (popups > 0 && last_popups == 0) { shake_amp = cur.shake; shake_t = 0.0f; }
    last_popups = (int)popups;
    shake_t += dt;
    float sh = shake_amp * expf(-shake_t / 0.12f);
    float cam_x = (dx + pan_x + sh * sinf(shake_t * two_pi * 17.0f)) * e;
    float cam_y = (dy + pan_y + sh * 0.5f * cosf(shake_t * two_pi * 13.0f)) * e;
    float zoom = 1.0f + zoom_extra * e;
    /* the pan turns the view too: it swings round the one stood under the
     * enemy, so beyond them the enemies drift a little the other way and the
     * background further (the party's own camera is cam_x) */
    float cam_en = cam_x + ORBIT_ENEMY * pan_a / F_ENEMY;
    float cam_bg = cam_x + ORBIT_BG * pan_a / F_BG;
    float cam_fog = cam_x + ORBIT_FOG * pan_a / F_FOG;
    cam_valid = 1;
    cam_last_x = cam_x; cam_last_y = cam_y; cam_last_zoom = zoom; cam_last_en = cam_en;
    cam_last_w = (float)w; cam_last_h = (float)h;

    uint32_t pal[128];
    for (int i = 0; i < 64; i++) pal[i] = pal[i + 64] = vdp_color(i);

    /* effects: light from the action sprites, hits, particles */
    int fx_on = fx_cur > 0.005f;
    if (fx_on) {
        fx_build_light(pal, w, h, fx_cur);
        fx_hits(pop_cnt, pop_ysum, w, fx_cur);
        fx_spawn_from_light(w, h, pal, dt, fx_cur);
        fx_update(dt);
    } else {
        light_on = 0;
    }
    int lit = fx_on && light_on;

    /* source coordinate for every output column and row, per layer */
    int ow = w * scale, oh = h * scale;
    build_map(col_bg, ow, scale, w, w / 2.0f, F_BG, zoom, cam_bg, 1);
    build_map(row_bg, oh, scale, h, h * 0.45f, F_BG, zoom, cam_y, 1);
    build_map(col_en, ow, scale, w, w / 2.0f, F_ENEMY, zoom, cam_en, 0);
    build_map(row_en, oh, scale, h, h / 2.0f, F_ENEMY, zoom, cam_y, 0);
    build_map(col_pa, ow, scale, w, w / 2.0f, F_PARTY, zoom, cam_x, 0);
    build_map(row_pa, oh, scale, h, (float)PARTY_FEET_Y, F_PARTY, zoom, 0.0f, 0);   /* no vertical move */

    /* fog: tinted from the background's average colour (eased, so flashes
     * don't make it flicker), densest along the floor, two drifting octaves */
    int fog_on = fog_cur > 0.005f;
    int tint_r = 0, tint_g = 0, tint_b = 0;
    long sr = 0, sg = 0, sb = 0, n = 0;                      /* the background's average colour */
    if (fog_on || wx) {
        for (int y = 0; y < h * 6 / 7; y += 4)
            for (int x = 0; x < w; x += 4) {
                uint32_t c = pal[layer[L_BG][y][x] & 0x3F];
                sr += c >> 16 & 255; sg += c >> 8 & 255; sb += c & 255; n++;
            }
        if (!n) n = 1;
    }
    if (fog_on) {
        float target[3] = {(float)sr / (float)n * 0.5f + 105.0f, (float)sg / (float)n * 0.5f + 105.0f,
                           (float)sb / (float)n * 0.5f + 112.0f};
        for (int i = 0; i < 3; i++) {
            if (!fog_tint_valid) fog_tint[i] = target[i];
            else ease(&fog_tint[i], target[i], 1.0f - expf(-dt / 0.5f));
        }
        fog_tint_valid = 1;
        tint_r = (int)fog_tint[0]; tint_g = (int)fog_tint[1]; tint_b = (int)fog_tint[2];
        float s_fog = 1.0f + F_FOG * (zoom - 1.0f);
        for (int X = 0; X < ow; X++) {
            float q = ((float)X + 0.5f) / (float)scale;
            float fx = w / 2.0f + (q - w / 2.0f) / s_fog + F_FOG * cam_fog;
            fog_col1[X] = (uint8_t)(int)floorf(fx * 0.8f + t * 5.0f);
            fog_col2[X] = (uint8_t)(int)floorf(fx * 1.6f - t * 9.0f + 97.0f);
        }
        for (int Y = 0; Y < oh; Y++) {
            float q = ((float)Y + 0.5f) / (float)scale;
            float fy = h / 2.0f + (q - h / 2.0f) / s_fog + F_FOG * cam_y;
            fog_row1[Y] = (uint8_t)(int)floorf(fy * 0.8f);
            fog_row2[Y] = (uint8_t)(int)floorf(fy * 1.6f + t * 2.0f + 41.0f);
            /* thickest along the floor (y ~120-175), gone where the background ends (~192) */
            float a = (q - 20.0f) / 100.0f, b = (195.0f - q) / 20.0f;
            a = a < 0 ? 0 : a > 1 ? 1 : a * a * (3 - 2 * a);
            b = b < 0 ? 0 : b > 1 ? 1 : b * b * (3 - 2 * b);
            a *= b;
            fog_profile[Y] = (uint16_t)(a * fog_cur * 256.0f);
        }
    }

    /* sprites on the party: out of the enemies' layers, each object to be
     * drawn where the party's camera puts it (eased, see obj_w) */
    static int obj_dx[128], obj_dy[128];
    float s_en = 1.0f + F_ENEMY * (zoom - 1.0f), s_pa = 1.0f + F_PARTY * (zoom - 1.0f);
    if (any_near) {
        for (int r = 0; r < 128; r++) {
            if (obj_w[r] <= 0.0f) continue;
            float ax = (float)(ox0[r] + ox1[r] + 1) / 2.0f, ay = (float)(oy0[r] + oy1[r] + 1) / 2.0f;
            float ex = w / 2.0f + (ax - F_ENEMY * cam_en - w / 2.0f) * s_en;
            float ey = h / 2.0f + (ay - F_ENEMY * cam_y - h / 2.0f) * s_en;
            float px = w / 2.0f + (ax - F_PARTY * cam_x - w / 2.0f) * s_pa;
            float py = (float)PARTY_FEET_Y + (ay - (float)PARTY_FEET_Y) * s_pa;
            obj_dx[r] = (int)lroundf((px - ex) * obj_w[r] * (float)scale);
            obj_dy[r] = (int)lroundf((py - ey) * obj_w[r] * (float)scale);
            if (panning) {                                    /* with the one it is on, as the view turns */
                int on_t = ox0[r] <= tx1 && ox1[r] >= tx0, on_m = ox0[r] <= mx1 && ox1[r] >= mx0;
                if (on_t) obj_dx[r] += (int)lroundf((float)t_out * obj_w[r]);
                else if (on_m) obj_dx[r] += (int)lroundf((float)a_out * obj_w[r]);
            }
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t s = lines[y].sprite[x];
                near_obj[y][x] = 0;
                if (!s) continue;
                int r = sprite_root(parent, lines[y].sprite_no[x] & 127);
                if (obj_w[r] <= 0.0f || layer[(s & 0x80) ? L_POINTER : L_ACTION][y][x] != s) continue;
                near_obj[y][x] = (uint8_t)(r + 1);
                near_px[y][x] = s;
                layer[(s & 0x80) ? L_POINTER : L_ACTION][y][x] = 0;
            }
    }

    /* a storm's darker light and its lightning (weather.c), on all but the windows */
    int tmr = 256, tmg = 256, tmb = 256, tar = 0, tag = 0, tab = 0;
    int tint = wx && weather_tint(&tmr, &tmg, &tmb, &tar, &tag, &tab);
    uint32_t tpal[128];                                       /* the palette under that light */
    for (int i = 0; i < 128; i++) tpal[i] = tint ? tinted(pal[i], tmr, tmg, tmb, tar, tag, tab) : pal[i];
    if (tint && fog_on) {                                     /* the fog under it too */
        uint32_t fc = tinted(0xFF000000u | (uint32_t)tint_r << 16 | (uint32_t)tint_g << 8 | (uint32_t)tint_b,
                             tmr, tmg, tmb, tar, tag, tab);
        tint_r = (int)(fc >> 16 & 255); tint_g = (int)(fc >> 8 & 255); tint_b = (int)(fc & 255);
    }
    Rows rows = {out, out_stride, scale, ow, pal, tpal, panning, t_out, a_out, lit, fog_on, tint_r, tint_g, tint_b};
    par_rows(compose_rows, &rows, oh);
    /* the sprites on the party, each object through the enemies' camera and
     * moved to where the party's puts it: in front of the fog and what is
     * behind; a low-priority one behind the party (but over a picture the
     * port put back), a high-priority one over the party too */
    for (int r = 0; any_near && r < 128; r++) {
        if (obj_w[r] <= 0.0f) continue;
        int X0 = (int)floorf((w / 2.0f + ((float)ox0[r] - F_ENEMY * cam_en - w / 2.0f) * s_en) * (float)scale) + obj_dx[r] - scale;
        int X1 = (int)ceilf((w / 2.0f + ((float)ox1[r] + 1.0f - F_ENEMY * cam_en - w / 2.0f) * s_en) * (float)scale) + obj_dx[r] + scale;
        int Y0 = (int)floorf((h / 2.0f + ((float)oy0[r] - F_ENEMY * cam_y - h / 2.0f) * s_en) * (float)scale) + obj_dy[r] - scale;
        int Y1 = (int)ceilf((h / 2.0f + ((float)oy1[r] + 1.0f - F_ENEMY * cam_y - h / 2.0f) * s_en) * (float)scale) + obj_dy[r] + scale;
        if (X0 < 0) X0 = 0;
        if (Y0 < 0) Y0 = 0;
        if (X1 > ow) X1 = ow;
        if (Y1 > oh) Y1 = oh;
        for (int Y = Y0; Y < Y1; Y++) {
            int sy = Y - obj_dy[r], ve;
            if (sy < 0 || sy >= oh || (ve = row_en[sy]) < 0) continue;
            uint32_t *dst = out + (size_t)Y * (size_t)out_stride;
            for (int X = X0; X < X1; X++) {
                int sx = X - obj_dx[r], u;
                if (sx < 0 || sx >= ow || (u = col_en[sx]) < 0 || near_obj[ve][u] != r + 1) continue;
                uint8_t c = near_px[ve][u], f = front[Y][X];
                if ((c & 0x80) ? f == 3 : (f == 1 || f == 3)) continue;
                dst[X] = tpal[c & 0x3F];
                front[Y][X] = WX_FRONT_PARTY;                 /* with the party, for the weather */
            }
        }
    }
    if (wx) {                                                 /* the weather, at its three depths */
        WeatherFrame wf = {out, out_stride, &front[0][0], VDP_MAX_W * MAX_SCALE, w, h, scale,
                           cam_en, cam_y, zoom, dt, (int)(sr / n), (int)(sg / n), (int)(sb / n)};
        weather_draw(&wf);
    }
    if (fx_on) fx_draw(out, out_stride, w, h, scale, cam_en, cam_y, zoom, fx_cur);
    return 1;
}
