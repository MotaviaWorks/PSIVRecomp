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
#include "scalex.h"

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
/* An enemy's own pieces (see depth_compose: sprites in its colours, flush against its picture), remembered where
 * they were last known for one: the game draws a technique's flash over the enemy who casts it, and for those
 * frames the join does not show (Juza's Forceflash: the half of him drawn as sprites went dark and slid) */
#define OWN_SEEN 12
#define OWN_TTL  30                      /* frames a place is remembered after the piece was last seen in it */
static struct { int x0, y0, x1, y1, ttl; } own_seen[OWN_SEEN];
static float t;                          /* seconds of battle shown */
static float pan_x, pan_y, zoom_extra;   /* camera, eased */
static float shake_amp, shake_t;
static float pan_a;                      /* the camera pan (battleui_pan): pixels, eased */
static float step_k;                     /* ... or with the Camera off, the step: 0..1, eased */
#define STEP_UP   10.0f                  /* game pixels: the one stood there, up */
#define STEP_DOWN 14.0f                  /* the others, down */
/* the close-up this frame (the opening's, the strike's): every layer enlarged cl_z times more, the layers' point
 * (cl_x, cl_y_*) at their maps' centres; 1 and the centres themselves with none. For what is drawn over the picture
 * (the effects' sparks, the numbers by way of depth_to_screen) */
static float cl_z = 1.0f, cl_x, cl_y_en, cl_y_pa;
/* The strike (depth_set_strike; an experiment, PsiLok 2026-11-19: "when a member uses a technique that makes their
 * sprite approach the enemy, can the camera zoom in instantly on the attack then back"). The game draws one who
 * goes up to an enemy as sprites in that member's own tiles, there. While such sprites are in the picture the view
 * is close on them -- at once, no easing in; the place taken when they first appear and kept, so the view does not
 * follow their leap -- and it draws back over a tenth of a second when they have gone. */
#define STRIKE_ZOOM 1.5f
#define STRIKE_BACK 0.10f                /* s */
static int strike_on = 1, strike_gap;
static float strike_k, strike_gx, strike_gy;
void depth_set_strike(int on) { strike_on = on != 0; }
/* The opening (depth_set_intro): a battle begins on a close-up of the enemies, in a flash, and the view draws
 * back to show the party, who come up from below as it does. The game has the party and its menu there within
 * half a second of the enemies; what the port draws over the battle fades in with the view, and the pad waits. */
#define INTRO_HOLD 0.55f                 /* s on the enemies */
#define INTRO_BACK 0.60f                 /* s drawing back */
#define INTRO_PARTY_TOP 120.0f           /* the party's heads, game y: below the picture's foot in the close-up */
enum { INTRO_WAIT, INTRO_RUN, INTRO_DONE };
static int intro_on = 1, intro_state = INTRO_DONE;
static float intro_t, intro_wait, intro_k, intro_zoom = 1.0f;
static float intro_gx, intro_gy, intro_qy;   /* the anchor: the enemies' feet (their middle, their foot); where it
                                              * is put in the close-up, view y */
static uint64_t intro_stamp;
/* Zio at Nurvus (event battle 6, the Arena's too) has an opening of its own: the fight starts with his Magic
 * Barrier, about two seconds of it before the game shows the party. The close-up stays on him for all of the
 * cast, pushing in slowly, and draws back when the party is in the picture. */
#define EVENT_BATTLE_INDEX 0xECFCu       /* Event_Battle_Index: the story battle's number, negative in the others */
#define EVENT_ZIO_NURVUS   6
#define INTRO_CAST_MOST    6.0f          /* s: no longer than this on a cast, whatever the game does */
static int intro_cast;
static float intro_cast_t, intro_push;
void depth_set_intro(int on) { intro_on = on != 0; }
float depth_intro(void) { return frame_logical - intro_stamp <= 3 ? intro_k : 0.0f; }
int depth_intro_cast(void) { return frame_logical - intro_stamp <= 3 && intro_cast; }
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
/* (zmul, csrc: the opening's close-up -- the layer enlarged that much more, with its point csrc at the view's
 * `center`; 1 and center otherwise) */
static void build_map(int16_t *map, int n, int scale, int size, float center, float factor,
                      float zoom, float cam, int mirror, float zmul, float csrc) {
    float s = (1.0f + factor * (zoom - 1.0f)) * zmul;
    for (int i = 0; i < n; i++) {
        float q = ((float)i + 0.5f) / (float)scale;
        int p = (int)floorf(csrc + (q - center) / s + factor * cam);
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
    float s = (1.0f + F_ENEMY * (zoom - 1.0f)) * cl_z, cxs = w / 2.0f, cys = h / 2.0f;
    for (int i = 0; i < nparts; i++) {
        const Particle *p = &parts[i];
        float qx = cxs + (p->x - cl_x - F_ENEMY * cam_x) * s, qy = cys + (p->y - cl_y_en - F_ENEMY * cam_y) * s;
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
    /* lens flare when the effects are bright: faint ghosts mirrored through the screen centre. (There was a
     * horizontal streak through the light too: across an enemy casting, a bright line straight through its
     * picture, it read as the picture torn in two -- PsiLok saw it on Xe-A-Thoul, 2026-11-19. Gone.) */
    float flare = (light_amount - 40.0f) / 160.0f;
    if (flare <= 0.0f) return;
    if (flare > 1.0f) flare = 1.0f;
    flare *= strength * 0.8f;
    float lx = (cxs + (light_cx - cl_x - F_ENEMY * cam_x) * s) * (float)scale;
    float ly = (cys + (light_cy - cl_y_en - F_ENEMY * cam_y) * s) * (float)scale;
    float m = fmaxf(light_rgb[0], fmaxf(light_rgb[1], light_rgb[2]));
    float tr = m > 0 ? light_rgb[0] / m : 1, tg = m > 0 ? light_rgb[1] / m : 1, tb = m > 0 ? light_rgb[2] / m : 1;
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
    float f = party ? F_PARTY : F_ENEMY, s = (1.0f + f * (cam_last_zoom - 1.0f)) * cl_z;
    float cx = cam_last_w / 2.0f, cy = party ? (float)PARTY_FEET_Y : cam_last_h / 2.0f;
    *ox = cx + (x - f * (party ? cam_last_x : cam_last_en) - cl_x) * s;                 /* (cl_*: a close-up) */
    *oy = cy + (y - (party ? 0.0f : f * cam_last_y) - (party ? cl_y_pa : cl_y_en)) * s;
}

/* source coordinate for every output column and row, per layer */
static int16_t col_bg[VDP_MAX_W * MAX_SCALE], col_en[VDP_MAX_W * MAX_SCALE], col_pa[VDP_MAX_W * MAX_SCALE];
static int16_t row_bg[VDP_MAX_H * MAX_SCALE], row_en[VDP_MAX_H * MAX_SCALE], row_pa[VDP_MAX_H * MAX_SCALE];
/* the fog's noise rows and columns, and how thick it is on each row */
static uint8_t fog_row1[VDP_MAX_H * MAX_SCALE], fog_row2[VDP_MAX_H * MAX_SCALE];
static uint8_t fog_col1[VDP_MAX_W * MAX_SCALE], fog_col2[VDP_MAX_W * MAX_SCALE];
static uint16_t fog_profile[VDP_MAX_H * MAX_SCALE];
static uint8_t mimg[VDP_MAX_H][VDP_MAX_W];                /* the camera pan: the others, whole, at their places */

/* Scale2x (depth_set_filter; scalex.h; GitHub issue 4). The filter rounds off the stair-steps of a
 * picture at the game's size, and there is no such picture of a battle: its layers are put together
 * at the display's scale, each through its own camera. So each layer goes through the filter first,
 * on its own (as palette numbers, 0 = nothing there: a sprite's outline is rounded against whatever
 * shows behind it), and the cameras read the enlarged layers. A layer is enlarged by the composing
 * scale itself, so at rest a pixel of the output is a pixel of the enlarged layer.
 *
 * The planes: the six layers, the sprites on the party (near_px) and the pan's picture of the others
 * (mimg), and one that stays empty, read in place of a layer with nothing on it. */
enum { FINE_NEAR = NLAYERS, FINE_MIMG, FINE_ZERO, NFINE };
#define FINE_W (VDP_MAX_W * MAX_SCALE)
#define FINE_H (VDP_MAX_H * MAX_SCALE)
static int fine_want;
static uint8_t *fine_buf;
void depth_set_filter(int on) { fine_want = on != 0; }
static uint8_t *fine_plane(int n) { return fine_buf + (size_t)n * FINE_W * FINE_H; }
static int fine_ready(void) {
    if (!fine_buf) fine_buf = calloc(NFINE, (size_t)FINE_W * FINE_H);
    return fine_buf != NULL;
}
/* a layer through the filter, f times its size (rows of w * f): where `only` is given, just its
 * pixels. The values kept are the colour (and, keep_hi, the sprite's priority) with bit 6 set, so
 * the same colour at two priorities is one colour to the filter and nothing opaque is 0 */
static const uint8_t *fine_make(int plane, uint8_t (*src)[VDP_MAX_W], uint8_t (*only)[VDP_MAX_W], int keep_hi,
                                int w, int h, int f) {
    static uint8_t norm[VDP_MAX_H][VDP_MAX_W];
    uint8_t keep = keep_hi ? 0xBF : 0x3F, any = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t c = only && !only[y][x] ? 0 : src[y][x];
            norm[y][x] = c ? (uint8_t)((c & keep) | 0x40) : 0;
            any |= norm[y][x];
        }
    if (!any) return fine_plane(FINE_ZERO);
    scalex_upscale8(&norm[0][0], VDP_MAX_W, w, h, fine_plane(plane), w * f, f);
    return fine_plane(plane);
}
/* the maps into the enlarged layers (build_map's, f times as fine), and the game-size maps made
 * from them, so the two never disagree about which game pixel an output pixel shows */
static int16_t colf_bg[VDP_MAX_W * MAX_SCALE], colf_en[VDP_MAX_W * MAX_SCALE], colf_pa[VDP_MAX_W * MAX_SCALE];
static int16_t rowf_bg[VDP_MAX_H * MAX_SCALE], rowf_en[VDP_MAX_H * MAX_SCALE], rowf_pa[VDP_MAX_H * MAX_SCALE];
static void build_map_fine(int16_t *fine, int16_t *map, int n, int scale, int f, int size, float center,
                           float factor, float zoom, float cam, int mirror, float zmul, float csrc) {
    float s = (1.0f + factor * (zoom - 1.0f)) * zmul;
    int fsize = size * f;
    for (int i = 0; i < n; i++) {
        float q = ((float)i + 0.5f) / (float)scale;
        int p = (int)floorf((csrc + (q - center) / s + factor * cam) * (float)f);
        if (mirror) {
            if (p < 0) p = -p - 1;
            if (p >= fsize) p = 2 * fsize - p - 1;
            if (p < 0) p = 0;
            if (p >= fsize) p = fsize - 1;
        } else if (p < 0 || p >= fsize) {
            p = -1;
        }
        fine[i] = (int16_t)p;
        map[i] = (int16_t)(p < 0 ? -1 : p / f);
    }
}

/* The picture's main pass, a row of the output at a time: for each pixel the
 * nearest thing there, through each layer's own camera. What a row shows
 * depends only on the source rows its layers fall on and on its fog, so a
 * row with the same ones as the row above (most rows, the picture being
 * several times the game's size) is a copy of it. Rows are drawn in pieces
 * on several cores (par.h): a piece writes only its own rows.
 *
 * The layers are read through pointers and maps of their own: the game's
 * layers and the game-size maps, or, with Scale2x, the enlarged layers and
 * the fine maps. What stays at the game's size either way (which party
 * pixels were put back, the light) is read through the game-size maps. */
typedef struct {
    uint32_t *out;
    int out_stride, scale, ow;
    const uint32_t *pal, *tpal;
    int panning, t_out, a_out, lit, fog_on, tint_r, tint_g, tint_b;
    int t_dy, a_dy, oh;                              /* the step (Camera off): the one stood there up, the others
                                                      * down (output pixels), the others in front; the rows */
    const uint8_t *L[NLAYERS], *mi;                  /* the layers and the pan's picture, rows of ls */
    int ls, ui_div;                                  /* (ui_div: output pixels to a pixel of the windows' layer) */
    const int16_t *cb, *ce, *cp, *rb, *re, *rp;      /* the maps into them */
} Rows;

/* the storm's light this frame (weather.c), for what the port draws of a stage itself: on, each channel's share
 * of 256, what is added */
static int bd_tint, bd_tm[3] = {256, 256, 256}, bd_ta[3];

/* Zio's stage (depth_set_backdrops). The game's own is a band of blue cloud 320 wide and 112 high: black bars at
 * the sides in widescreen, and a black notch under it that shows whenever the party's windows are away. The
 * port's: a floor of fog to every edge of the view, seen in perspective under a black sky, welling up at Zio's
 * feet and flowing out from them (a noise carried outward in two phases that take turns, so it never jumps), a
 * bank of it standing against the sky at the horizon. Made each frame at twice the game's size in the stage's own
 * space, and shown through the enemies' camera, so Zio's feet stay where they are in it; it takes the game's
 * fades from the game's own picture (how bright and how white its cloud is). */
#define ZF_RES     2
#define ZF_MX      32                    /* the stage beyond the view, game pixels a side (the camera's drift, its turn) */
#define ZF_MY      8
#define ZF_W       ((VDP_MAX_W + 2 * ZF_MX) * ZF_RES)
#define ZF_H       ((VDP_MAX_H + 2 * ZF_MY) * ZF_RES)
#define ZF_HORIZON 60.0f                 /* game y */
#define ZF_FEET    119.0f                /* the foot of his picture */
#define ZF_HEEL    115.0f                /* his heels: where the fog flows out from, and how near he stands */
#define ZF_Z0      (2400.0f / (ZF_HEEL - ZF_HORIZON + 6.0f))
#define ZF_CYCLE   2.6f                  /* s: a phase of the flow */
#define ZF_LEN     26.0f                 /* how far it carries the fog, floor units */
#define ZF_WISP_TOP 124.0f               /* how far above his feet the wisps reach, game pixels */
#define ZF_HIGH    15.0f                 /* a billow's height at most, game pixels at his feet (more nearer, less further) */
static uint32_t zf_buf[ZF_H][ZF_W];
static float zf_hgt[ZF_H][ZF_W], zf_amb[ZF_H][ZF_W];   /* the floor seen at each pixel: its billow's height 0..1, its light */
static uint8_t zf_base[ZF_H][ZF_W];                     /* how near the fog at a pixel stands: its foot's game row + 1 (0: none) */
static uint8_t zf_feet[VDP_MAX_H][VDP_MAX_W];           /* the feet of what the game drew at a pixel, game row: fog that stands
                                                         * nearer goes over it (the enemies: his; a sprite: its object's) */
static int16_t zf_col[VDP_MAX_W * MAX_SCALE], zf_row[VDP_MAX_H * MAX_SCALE];
static int zf_want = 1, zf_on;
void depth_set_backdrops(int on) { zf_want = on != 0; }
typedef struct { float t, cx, dim, white; int bw, bh; float wisp; } ZfGen;   /* (wisp: how far the wisps are up, 0..1) */
static float zf_wisp;

static inline float zf_noise(float x, float y) {
    float fx = floorf(x), fy = floorf(y), ax = x - fx, ay = y - fy;
    int x0 = (int)fx & (NOISE - 1), y0 = (int)fy & (NOISE - 1), x1 = (x0 + 1) & (NOISE - 1), y1 = (y0 + 1) & (NOISE - 1);
    float a = (float)noise[y0][x0] + ((float)noise[y0][x1] - (float)noise[y0][x0]) * ax;
    float b = (float)noise[y1][x0] + ((float)noise[y1][x1] - (float)noise[y1][x0]) * ax;
    return (a + (b - a) * ay) * (1.0f / 255.0f);
}

static inline uint32_t zf_color(float v, const ZfGen *g) {
    static const float RAMP[5][4] = {{0.00f, 0, 0, 0}, {0.14f, 3, 8, 46}, {0.42f, 12, 44, 170}, {0.70f, 64, 132, 238},
                                     {1.00f, 218, 236, 255}};
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    int i = v < RAMP[1][0] ? 0 : v < RAMP[2][0] ? 1 : v < RAMP[3][0] ? 2 : 3;
    float k = (v - RAMP[i][0]) / (RAMP[i + 1][0] - RAMP[i][0]);
    float cr = (RAMP[i][1] + (RAMP[i + 1][1] - RAMP[i][1]) * k) * g->dim;
    float cg = (RAMP[i][2] + (RAMP[i + 1][2] - RAMP[i][2]) * k) * g->dim;
    float cb = (RAMP[i][3] + (RAMP[i + 1][3] - RAMP[i][3]) * k) * g->dim;
    cr += (255.0f - cr) * g->white; cg += (255.0f - cg) * g->white; cb += (255.0f - cb) * g->white;
    return 0xFF000000u | (uint32_t)cr << 16 | (uint32_t)cg << 8 | (uint32_t)cb;
}

/* first, a row at a time: the sky and the bank against it as they are; for the floor, what stands at each point of
 * it -- a billow's height (the flow's noise, carried outward from his heels) and
 * the light there */
static void zf_rows(void *ctx, int y0, int y1) {
    const ZfGen *g = ctx;
    float ph1 = fmodf(g->t / ZF_CYCLE, 1.0f), ph2 = fmodf(ph1 + 0.5f, 1.0f);
    float w1 = 1.0f - fabsf(2.0f * ph1 - 1.0f), w2 = 1.0f - w1, wn = 1.0f / sqrtf(w1 * w1 + w2 * w2);
    float a1 = ph1 * ZF_LEN, a2 = ph2 * ZF_LEN;
    for (int by = y0; by < y1; by++) {
        float gy = ((float)by + 0.5f) / ZF_RES - ZF_MY, above = ZF_HORIZON - gy;
        uint32_t *dst = zf_buf[by];
        memset(zf_base[by], 0, (size_t)g->bw);
        for (int bx = 0; bx < g->bw; bx++) {
            float gx = ((float)bx + 0.5f) / ZF_RES - ZF_MX - g->cx, v;
            if (above > 44.0f) {
                v = 0.0f;                                     /* the sky */
            } else if (above > 0.0f) {                        /* the bank's top against it, and its glow */
                /* (tongues of it, tall and short, leaning and drifting; thin at their tips) */
                float lean = gx + above * 0.5f;
                float crest = 5.0f + 22.0f * zf_noise(lean * 0.5f + g->t * 2.4f + 300.0f, 40.0f + g->t * 1.0f)
                            + 14.0f * zf_noise(lean * 2.3f - g->t * 6.0f, 90.0f + g->t * 2.2f);
                float k = 1.0f - above / crest;
                v = 0.09f / (1.0f + above * 0.25f);
                if (k > 0.0f)
                    v += 0.70f * k * (0.35f + 0.65f * k)
                       + (zf_noise(gx * 2.6f - g->t * 7.0f, above * 3.4f + g->t * 3.0f) - 0.5f) * 0.7f * k;
            } else {                                          /* the floor, from his feet outward */
                float d = -above, zf = 2400.0f / (d + 6.0f);
                float fx = gx * zf * (1.0f / 60.0f), fz = ZF_Z0 - zf;
                float r = sqrtf(fx * fx + fz * fz) + 0.001f, ux = fx / r, uz = fz / r;
                float p1x = (fx - ux * a1) * 3.0f, p1z = (fz - uz * a1) * 9.0f;
                float p2x = (fx - ux * a2) * 3.0f + 97.0f, p2z = (fz - uz * a2) * 9.0f + 31.0f;
                float n1 = zf_noise(p1x, p1z) + 0.45f * zf_noise(p1x * 2.7f + 11.0f, p1z * 2.7f);
                float n2 = zf_noise(p2x, p2z) + 0.45f * zf_noise(p2x * 2.7f + 11.0f, p2z * 2.7f);
                float n = ((n1 - 0.725f) * w1 + (n2 - 0.725f) * w2) * wn;
                float big = zf_noise(fx * 0.9f + g->t * 1.1f, fz * 2.6f + 140.0f) - 0.5f;
                float hh = 0.48f + n * 1.9f + big * 0.7f;
                hh = hh < 0.0f ? 0.0f : hh > 1.0f ? 1.0f : hh;
                hh = hh * hh * (3.0f - 2.0f * hh);
                zf_hgt[by][bx] = hh;
                zf_amb[by][bx] = 0.22f + 0.10f / (1.0f + d * 0.07f) + 0.20f / (1.0f + r * r * 0.02f);
                continue;
            }
            dst[bx] = zf_color(v, g);
        }
    }
}

/* then, a column at a time, from the nearest row back: each point's billow stands up from it (taller the nearer),
 * and is drawn where nothing nearer has been -- dark at its foot, pale to white at its top. Each pixel keeps how
 * near its billow stands (zf_base), so he stands in it: zf_over. */
static void zf_cols(void *ctx, int x0, int x1) {
    const ZfGen *g = ctx;
    int hy = (int)((ZF_HORIZON + ZF_MY) * ZF_RES);
    for (int bx = x0; bx < x1; bx++) {
        int ytop = g->bh;
        for (int by = g->bh - 1; by >= hy; by--) {
            float d = ((float)by + 0.5f) / ZF_RES - ZF_MY - ZF_HORIZON, hh = zf_hgt[by][bx];
            float hpx = hh * ZF_HIGH * (d + 6.0f) * (ZF_RES / 65.0f);
            int top = by - (int)hpx;
            if (top < 0) top = 0;
            if (top >= ytop) continue;                        /* behind something nearer */
            float amb = zf_amb[by][bx], inv = hpx > 0.5f ? 1.0f / hpx : 0.0f, lit = hh > 1.0f ? 1.0f : hh;
            int row = by / ZF_RES - ZF_MY + 1;
            uint8_t base = (uint8_t)(row < 1 ? 1 : row > 255 ? 255 : row);
            for (int y = (by < ytop ? by : ytop - 1); y >= top; y--) {
                float u = (float)(by - y) * inv;
                zf_buf[y][bx] = zf_color(amb + lit * (0.10f + 0.56f * u * u), g);
                zf_base[y][bx] = base;
            }
            ytop = top;
        }
        /* see-through at their tops: where a nearer billow's edge stands against a further one, the two blend over
         * a few pixels (the nearer the more, downward) */
        for (int y = hy + 1; y < g->bh - 2; y++) {
            if (!zf_base[y - 1][bx] || zf_base[y][bx] < zf_base[y - 1][bx] + 3) continue;
            uint32_t back = zf_buf[y - 1][bx];
            for (int j = 0; j < 3 && zf_base[y + j][bx] == zf_base[y][bx]; j++) {
                uint32_t c = zf_buf[y + j][bx], a = j == 0 ? 150 : j == 1 ? 95 : 45;       /* the further one's share */
                zf_buf[y + j][bx] = 0xFF000000u | ((((back >> 16 & 255) * a + (c >> 16 & 255) * (255 - a)) / 255) << 16)
                                  | ((((back >> 8 & 255) * a + (c >> 8 & 255) * (255 - a)) / 255) << 8)
                                  | (((back & 255) * a + (c & 255) * (255 - a)) / 255);
            }
        }
    }
}

/* last, the wisps: dark smoke streaming up off him into the sky, a paler blue close round him, as the story's
 * picture of him has it. Tongues of the noise drawn out upward and rising, thick round his body and ragged at their
 * tips, swaying a little; behind him (he is drawn over them) and behind the fog that stands nearer than he does.
 * They come with Nightmare: rising from him as the great face appears in the sky, there while it is, sinking
 * back when it goes (g->wisp). */
static void zf_wisps(void *ctx, int y0, int y1) {
    const ZfGen *g = ctx;
    for (int by = y0; by < y1; by++) {
        float gy = ((float)by + 0.5f) / ZF_RES - ZF_MY, up = ZF_FEET - gy;        /* above his feet */
        float reach = 46.0f + (ZF_WISP_TOP - 46.0f) * g->wisp;                    /* (their tips climb as they come) */
        if (up < 0.0f || up > reach) continue;
        float rise = up > reach - 46.0f ? (reach - up) * (1.0f / 46.0f) : 1.0f;   /* thinning upward */
        rise *= g->wisp < 0.5f ? g->wisp * 2.0f : 1.0f;
        if (up < 8.0f) rise *= up * (1.0f / 8.0f);
        float sway = 5.0f * sinf(g->t * 0.9f + up * 0.045f) * (up * (1.0f / ZF_WISP_TOP));
        int b0 = (int)((g->cx - 78.0f + ZF_MX) * ZF_RES), b1 = (int)((g->cx + 78.0f + ZF_MX) * ZF_RES);
        if (b0 < 0) b0 = 0;
        if (b1 > g->bw) b1 = g->bw;
        for (int bx = b0; bx < b1; bx++) {
            if (zf_base[by][bx] >= (int)ZF_HEEL + 2) continue;                    /* fog nearer than he is */
            float gx = ((float)bx + 0.5f) / ZF_RES - ZF_MX - g->cx - sway, ax = fabsf(gx);
            /* (a flame's outline: widest at his shoulders' height, drawn in toward its tip; strongest down its
             * middle, where he hides it, so what shows is its ragged sides and the tongues above his head) */
            float half = (up < 30.0f ? 34.0f + up * 0.33f : 44.0f) * (1.0f - 0.62f * up * (1.0f / ZF_WISP_TOP));
            float side = 1.0f - (ax / half) * (ax / half);
            if (side <= 0.0f) continue;
            float body = side * rise;
            float n = zf_noise(gx * 7.0f + 400.0f, (up - g->t * 34.0f) * 2.2f)
                    + 0.5f * zf_noise(gx * 17.0f, (up - g->t * 52.0f) * 5.0f + 70.0f);
            float k = body * 2.1f - (1.5f - n) * 0.92f;
            if (k <= 0.0f) continue;
            if (k > 1.0f) k = 1.0f;
            float near_him = ax < 30.0f && up < 66.0f ? (1.0f - ax * (1.0f / 30.0f)) * (1.0f - up * (1.0f / 66.0f)) : 0.0f;
            uint32_t wc = zf_color(0.20f + 0.20f * k + 0.24f * near_him * k, g), bc = zf_buf[by][bx];
            /* (smoke against the sky; over the pale fog only a shade of it) */
            float most = gy > ZF_HORIZON + 22.0f ? 180.0f : gy > ZF_HORIZON - 14.0f ? 180.0f + (ZF_HORIZON + 22.0f - gy) * (60.0f / 36.0f) : 240.0f;
            uint32_t a = (uint32_t)(k * 1.5f >= 1.0f ? most : k * 1.5f * most);
            zf_buf[by][bx] = 0xFF000000u | ((((wc >> 16 & 255) * a + (bc >> 16 & 255) * (255 - a)) / 255) << 16)
                           | ((((wc >> 8 & 255) * a + (bc >> 8 & 255) * (255 - a)) / 255) << 8)
                           | (((wc & 255) * a + (bc & 255) * (255 - a)) / 255);
        }
    }
}

/* a pixel of the enemies' side (output X, Y; `feet`: zf_feet where it comes from) under the fog that stands nearer
 * than its feet: seen through it, faintly at the fog's top and less and less the deeper it stands in */
static inline uint32_t zf_over(uint32_t col, int X, int Y, int feet) {
    int br = zf_row[Y], bc = zf_col[X], lim = feet + 2;
    if (zf_base[br][bc] < lim) return col;
    int deep = 0;                                         /* rows of that fog over this pixel */
    while (deep < 14 && br - deep - 1 >= 0 && zf_base[br - deep - 1][bc] >= lim) deep++;
    uint32_t fc = zf_buf[br][bc], fa = (uint32_t)(60 + deep * 11);     /* (see-through: 60 at its top to 214 well in) */
    if (bd_tint) fc = tinted(fc, bd_tm[0], bd_tm[1], bd_tm[2], bd_ta[0], bd_ta[1], bd_ta[2]);
    if (fa >= 255) return fc;
    return 0xFF000000u | ((((fc >> 16 & 255) * fa + (col >> 16 & 255) * (255 - fa)) / 255) << 16)
         | ((((fc >> 8 & 255) * fa + (col >> 8 & 255) * (255 - fa)) / 255) << 8)
         | (((fc & 255) * fa + (col & 255) * (255 - fa)) / 255);
}

/* is this Zio's stage (the game's own background: nothing above the cloud, cloud across the middle, nothing in
 * the notch below it), and how far has the game faded it (to black, to white)? */
static int zf_stage(const VdpLine *lines, int w, int h, float *dim, float *white) {
    int ev = m68k_ram[EVENT_BATTLE_INDEX], cx = w / 2;
    if (!zf_want || (ev != 4 && ev != EVENT_ZIO_NURVUS) || h < 200) return 0;
    /* (the sky and the notch are one black of the background's own, the same palette entry: told by the entry, not
     * the colour, so the game's fades make no difference; the cloud's art has that black in it here and there) */
    int black = lines[20].plane_b[cx], sky = 0, cloud = 0, notch = 0, cols = 0;
    for (int x = cx - 120; x <= cx + 120; x += 4, cols++) {
        sky += lines[20].plane_b[x] != black || lines[40].plane_b[x] != black;
        cloud += (lines[100].plane_b[x] != black) + (lines[140].plane_b[x] != black);
        notch += x > cx - 90 && x < cx + 90 && lines[180].plane_b[x] != black;
    }
    if (sky || notch || cloud < cols) return 0;                  /* (cloud: half of the two rows at least) */
    long r = 0, g = 0, b = 0, n = 0;
    for (int y = 72; y <= 160; y += 8)
        for (int x = cx - 120; x <= cx + 120; x += 8) {
            uint32_t c = vdp_color(lines[y].plane_b[x] & 0x3F);
            r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255; n++;
        }
    float ar = (float)r / (float)n, ab = (float)b / (float)n;                 /* (in full light: 66, 112, 209) */
    *white = ar > 70.0f ? fminf(1.0f, (ar - 66.0f) / 189.0f) : 0.0f;
    *dim = *white > 0.0f ? 1.0f : fminf(1.0f, ab / 209.0f);
    return 1;
}

static void compose_rows(void *ctx, int y0, int y1) {
    const Rows *q = ctx;
    uint32_t *out = q->out;
    const uint32_t *pal = q->pal, *tpal = q->tpal;
    int out_stride = q->out_stride, ow = q->ow;
    int panning = q->panning, t_out = q->t_out, a_out = q->a_out, lit = q->lit, fog_on = q->fog_on;
    int t_dy = q->t_dy, a_dy = q->a_dy, step = t_dy || a_dy;
    int tint_r = q->tint_r, tint_g = q->tint_g, tint_b = q->tint_b;
    int ls = q->ls, ui_div = q->ui_div;
    const int16_t *cb = q->cb, *ce = q->ce, *cp = q->cp, *rb = q->rb, *re = q->re, *rp = q->rp;
    for (int Y = y0; Y < y1; Y++) {
        int vu = Y / ui_div, ve = row_en[Y], vp = row_pa[Y];
        int fe = re[Y], fp = rp[Y], fb = rb[Y];
        /* the step: the one stood there read from lower down (drawn higher), the others from higher up */
        int YT = Y - t_dy, YO = Y - a_dy;
        int vpt = vp, fpt = fp, vpo = vp, fpo = fp;
        if (step) {
            vpt = YT >= 0 && YT < q->oh ? row_pa[YT] : -1;
            fpt = YT >= 0 && YT < q->oh ? rp[YT] : 0;
            vpo = YO >= 0 && YO < q->oh ? row_pa[YO] : -1;
            fpo = YO >= 0 && YO < q->oh ? rp[YO] : 0;
        }
        uint8_t *fr = front[Y];
        uint32_t *dst = out + (size_t)Y * (size_t)out_stride;
        const uint8_t *nrow1 = noise[fog_row1[Y]], *nrow2 = noise[fog_row2[Y]];
        int prof = fog_on ? fog_profile[Y] : 0;
        if (Y > y0 && !step && vu == (Y - 1) / ui_div && fe == re[Y - 1] && fp == rp[Y - 1] && fb == rb[Y - 1]
            && (!zf_on || zf_row[Y] == zf_row[Y - 1])
            && prof == (fog_on ? fog_profile[Y - 1] : 0)
            && (!prof || (fog_row1[Y] == fog_row1[Y - 1] && fog_row2[Y] == fog_row2[Y - 1]))) {
            memcpy(dst, dst - out_stride, (size_t)ow * sizeof *dst);      /* the row above, again */
            memcpy(fr, front[Y - 1], (size_t)ow);
            continue;
        }
        /* this row of each layer (the enemies' camera carries four of them) */
        const uint8_t *l_ui = q->L[L_UI] + (size_t)vu * (size_t)ls;
        const uint8_t *l_bg = q->L[L_BG] + (size_t)fb * (size_t)ls;
        const uint8_t *l_po = NULL, *l_ac = NULL, *l_en = NULL, *l_pa = NULL, *l_mi = NULL;
        if (ve >= 0) {
            l_po = q->L[L_POINTER] + (size_t)fe * (size_t)ls;
            l_ac = q->L[L_ACTION] + (size_t)fe * (size_t)ls;
            l_en = q->L[L_ENEMY] + (size_t)fe * (size_t)ls;
        }
        if (vp >= 0) {
            l_pa = q->L[L_PARTY] + (size_t)fp * (size_t)ls;
            l_mi = q->mi + (size_t)fp * (size_t)ls;
        }
        const uint8_t *l_pt = vpt >= 0 ? q->L[L_PARTY] + (size_t)fpt * (size_t)ls : NULL;   /* (the step's rows) */
        const uint8_t *l_mo = vpo >= 0 ? q->mi + (size_t)fpo * (size_t)ls : NULL;
        for (int X = 0; X < ow; X++) {
            int u = -1, c;
            /* in front of the fog: cursor over an enemy, windows, party */
            if (ve >= 0 && (u = col_en[X]) >= 0 && (c = l_po[ce[X]]) != 0) {
                dst[X] = pal[c & 0x3F];
                fr[X] = 3;
                continue;
            }
            if ((c = l_ui[X / ui_div]) != 0) { dst[X] = pal[c & 0x3F]; fr[X] = 3; continue; }
            int pu = 0;                                       /* the party here, and if put back (under action) */
            c = 0;
            if (step) {                                       /* the others in front, a step down; the one up */
                if (l_mo && (u = col_pa[X]) >= 0 && (c = l_mo[cp[X]]) != 0) pu = 1;
                if (!c && l_pt && (u = col_pa[X]) >= 0 && (c = l_pt[cp[X]]) != 0) pu = party_under[vpt][u];
            } else if (vp >= 0) {
                if (!panning) {
                    if ((u = col_pa[X]) >= 0 && (c = l_pa[cp[X]]) != 0) pu = party_under[vp][u];
                } else {
                    int XT = X - t_out, XO = X - a_out;
                    if (XT >= 0 && XT < ow && (u = col_pa[XT]) >= 0 && (c = l_pa[cp[XT]]) != 0) pu = party_under[vp][u];
                    if (!c && XO >= 0 && XO < ow && (u = col_pa[XO]) >= 0 && (c = l_mi[cp[XO]]) != 0) pu = 1;
                }
            }
            if (c && !(pu && ve >= 0 && col_en[X] >= 0 && l_ac[ce[X]])) {
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
            if (ve >= 0 && (u = col_en[X]) >= 0 && (c = l_ac[ce[X]]) != 0) far = tpal[c & 0x3F];
            else if (ve >= 0 && u >= 0 && (c = l_en[ce[X]]) != 0) far = tpal[c & 0x3F];
            else {
                if (zf_on) {
                    far = zf_buf[zf_row[Y]][zf_col[X]];
                    if (bd_tint) far = tinted(far, bd_tm[0], bd_tm[1], bd_tm[2], bd_ta[0], bd_ta[1], bd_ta[2]);
                } else far = tpal[l_bg[cb[X]] & 0x3F];
                what = WX_FRONT_BG;
            }
            if (zf_on && what == WX_FRONT_ENEMY) far = zf_over(far, X, Y, zf_feet[ve][u]);   /* Zio's stage: he stands in it */
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
        strike_k = 0.0f;
        strike_gap = 0;
        cl_z = 1.0f;
        intro_state = INTRO_WAIT;                             /* the next battle opens on its enemies */
        intro_t = intro_wait = intro_cast_t = intro_push = 0.0f;
        intro_k = 1.0f;
        intro_cast = 0;
        ramp = t = pan_x = pan_y = zoom_extra = shake_amp = pan_a = step_k = 0.0f;
        memset(own_seen, 0, sizeof own_seen);
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
    static int sx0[128], sy0[128], sx1[128], sy1[128], near_ui[128], hi[128], on_party[128], sline[128];
    int en_lines = 0;                                         /* the palette lines the enemies' picture is drawn in */
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
                /* (one of the party at low priority: the game draws the one an enemy's technique lands
                 * on that way, for its effect to pass over them. Still the party's, under the action:
                 * with the enemies there were two of them, the other put back in their place) */
                int low_party = !(a & 0x80) && !vehicle && tile >= TILE_PARTY_LO && tile < TILE_PARTY_HI
                                && battleui_tile_member(tile) >= 0;
                if (l->win_in[x]) layer[L_UI][y][x] = a;
                else if (!(a & 0x80) && !low_party) {
                    layer[L_ENEMY][y][x] = a;
                    if (tile < TILE_UI_HI && !(tile >= 0x15E && tile < TILE_PARTY_LO))   /* (not a window's text) */
                        en_lines |= 1 << (a >> 4 & 3);
                }
                else if (vehicle && tile >= TILE_PARTY_LO && tile < TILE_PARTY_HI) layer[L_UI][y][x] = a;
                else if (tile >= TILE_PARTY_LO && tile < TILE_PARTY_HI) {
                    layer[L_PARTY][y][x] = a;
                    party_under[y][x] = low_party ? 1 : l->plane_a_under[x];
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
                sline[id] = s >> 4 & 3;
                if (!(s & 0x80)) {
                    layer[L_ACTION][y][x] = s;
                    act_n++; act_x += x; act_y += y;
                } else
                    hi[id] = 1;
            }
        }
    }
    /* the numbers the modern menus took out of the picture (battleui.h): counted as if they were there,
     * so a hit still sparks and shakes */
    for (int i = 0, px, py, pw, ph; battleui_popup(i, &px, &py, &pw, &ph); i++)
        for (int yy = 0; yy < ph; yy++)
            for (int xx = 0; xx < pw; xx++) {
                int vx = px + xx - vdp_view_x0, vy = py + yy;
                if (vx < 0 || vx >= w || vy < 0 || vy >= h) continue;
                popups++; pop_cnt[vx]++; pop_ysum[vx] += vy;
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

    /* The opening: wait for the enemies to be in the picture (the game brings the battle in from white, the
     * enemies sliding up into their places over a few frames), then hold on them, enlarged about their middle
     * until they fill most of the view, and draw back. No one to be seen within a second and a half: the view
     * draws back as it is. */
    float ik = 0.0f;
    if (!intro_on) intro_state = INTRO_DONE;
    /* (the game's own windows have low-priority cells too -- their text, with the Original menus -- which sort
     * with the enemies: out of the close-up, as the windows are, and not taken for an enemy) */
    if (intro_state == INTRO_WAIT || (intro_state == INTRO_RUN && intro_k > 0.4f))
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int tile = lines[y].plane_a_tile[x] & 0x7FF;
                if (layer[L_ENEMY][y][x] && !lines[y].win_in[x] && (tile >= TILE_UI_HI || (tile >= 0x15E && tile < TILE_PARTY_LO)))
                    layer[L_ENEMY][y][x] = 0;
            }
    if (intro_state == INTRO_WAIT || (intro_state == INTRO_RUN && intro_t < 0.30f)) {
        int x0 = w, x1 = -1, y0 = h, y1 = -1;
        long n = 0, lit_n = 0;
        uint8_t seen[64];                                     /* a colour that shows (some bosses come out of the dark) */
        for (int i = 0; i < 64; i++) {
            uint32_t c = vdp_color(i);
            seen[i] = (c >> 16 & 255) >= 56 || (c >> 8 & 255) >= 56 || (c & 255) >= 56;
        }
        /* (above the game's status window, rows 19 on, and not a row filled from side to side: plane A is
         * filled under that window, and that is no enemy) */
        for (int y = 0; y < 148 && y < h; y++) {
            int rx0 = w, rx1 = -1, rn = 0, rl = 0;
            for (int x = 0; x < w; x++)
                if (layer[L_ENEMY][y][x] || layer[L_ACTION][y][x]) {      /* (an enemy's own sprites too) */
                    uint8_t c = layer[L_ENEMY][y][x] ? layer[L_ENEMY][y][x] : layer[L_ACTION][y][x];
                    rn++;
                    rl += seen[c & 0x3F];
                    if (x < rx0) rx0 = x;
                    if (x > rx1) rx1 = x;
                }
            if (!rn || rn * 100 > w * 85) continue;
            n += rn;
            lit_n += rl;
            if (rx0 < x0) x0 = rx0;
            if (rx1 > x1) x1 = rx1;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        int found = n > 300 && lit_n > 150 && x1 - x0 >= 12 && y1 - y0 >= 12;
        /* the anchor is where they stand: the middle of them, at their feet. The close-up enlarges all of the
         * picture about that point (so the ground stays under them) and puts it low in the view, them above it */
        float zoom_to = 1.5f, gx_to = (float)w / 2.0f, gy_to = 119.0f, qy_to = (float)h * 0.72f;
        if (found) {
            float eh = (float)(y1 - y0 + 1);
            zoom_to = fminf((float)w * 0.72f / (float)(x1 - x0 + 1), (float)h * 0.70f / eh);
            zoom_to = zoom_to < 1.3f ? 1.3f : zoom_to > 1.9f ? 1.9f : zoom_to;
            gx_to = (float)(x0 + x1 + 1) / 2.0f;
            gy_to = (float)(y1 + 1);
            qy_to = (float)h * 0.54f + eh * zoom_to * 0.5f;
            qy_to = qy_to < (float)h * 0.62f ? (float)h * 0.62f : qy_to > (float)h * 0.82f ? (float)h * 0.82f : qy_to;
        }
        if (intro_state == INTRO_WAIT) {
            intro_wait += dt;
            intro_zoom = zoom_to; intro_gx = gx_to; intro_gy = gy_to; intro_qy = qy_to;
            if (found) { intro_state = INTRO_RUN; intro_t = 0.0f; }
            else if (intro_wait > 1.5f) { intro_state = INTRO_RUN; intro_t = INTRO_HOLD; }   /* no one to open on
                                                               * (an entrance the game stages in the dark): back */
        } else if (found) {                                   /* with them as they come to their places */
            ease(&intro_zoom, zoom_to, 0.3f);
            ease(&intro_gx, gx_to, 0.4f);
            ease(&intro_gy, gy_to, 0.4f);
            ease(&intro_qy, qy_to, 0.4f);
        }
    }
    if (intro_state == INTRO_RUN) {
        /* Zio at Nurvus: the hold lasts as long as the party is not in the picture (his cast), the view pushing
         * in a little all the while */
        long party_px = 0;
        for (int y = 0; y < h; y += 2)
            for (int x = 0; x < w; x += 2) party_px += layer[L_PARTY][y][x] != 0;
        intro_cast = !vehicle && m68k_ram[EVENT_BATTLE_INDEX] == EVENT_ZIO_NURVUS && party_px < 60
                     && intro_t < INTRO_HOLD && intro_cast_t < INTRO_CAST_MOST;
        if (intro_cast && intro_t + dt >= INTRO_HOLD) {
            intro_cast_t += dt;
            intro_push = fminf(0.12f, intro_push + 0.035f * dt);
        } else {
            intro_t += dt;
        }
        if (intro_t >= INTRO_HOLD + INTRO_BACK) { intro_state = INTRO_DONE; intro_cast = 0; }
    }
    if (intro_state != INTRO_DONE) {
        float u = (intro_t - INTRO_HOLD) / INTRO_BACK;
        u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        ik = 1.0f - u * u * (3.0f - 2.0f * u);
    }
    intro_k = ik;
    intro_stamp = frame_logical;
    /* the close-up: one camera for the background, the enemies and the party (each layer's own drift apart),
     * the whole picture enlarged about the anchor (a little more at first: it eases off as it holds) and the
     * anchor put at the view's middle, low. Drawing back, the size and the anchor's place go home together, so
     * whatever stands on the ground stays on it. The party is below the picture's foot until then (a little
     * further down than the camera alone has them, where their heads would show); the game's own windows and
     * cursors wait for the view */
    float icreep = intro_state == INTRO_RUN && intro_t < INTRO_HOLD ? 0.05f * (1.0f - intro_t / INTRO_HOLD) : 0.0f;
    float iz = 1.0f + (intro_zoom * (1.0f + icreep + intro_push) - 1.0f) * ik;
    float iqx = intro_gx + ((float)w / 2.0f - intro_gx) * ik, iqy = intro_gy + (intro_qy - intro_gy) * ik;
    float icx = intro_gx - (iqx - (float)w / 2.0f) / iz;                 /* (every column map's centre is w / 2) */
    float icy_bg = intro_gy - (iqy - (float)h * 0.45f) / iz, icy_en = intro_gy - (iqy - (float)h / 2.0f) / iz;
    float idrop = (float)h + 6.0f - (intro_qy + (INTRO_PARTY_TOP - intro_gy) * intro_zoom);
    float icy_pa = intro_gy - (iqy - (float)PARTY_FEET_Y) / iz - (idrop > 0.0f ? idrop : 0.0f) * ik / iz;
    if (ik <= 0.0f) { iz = 1.0f; icx = (float)w / 2.0f; icy_bg = (float)h * 0.45f; icy_en = (float)h / 2.0f; icy_pa = (float)PARTY_FEET_Y; }
    if (ik > 0.4f && !vehicle) {
        memset(layer[L_UI], 0, sizeof layer[L_UI]);
        memset(layer[L_POINTER], 0, sizeof layer[L_POINTER]);
    }
    {   /* the strike: one of the party at an enemy -- the one acting, as low-priority sprites in their own tiles,
         * a figure's size, above where the party stands */
        int actor = m68k_ram[0x4142] << 8 | m68k_ram[0x4143], bx0 = w, bx1 = -1, by0 = h, by1 = -1;
        if (strike_on && ik <= 0.0f && !vehicle && actor >= 1 && actor <= 5) {
            uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
            for (int id = 0; id < 80; id++) {
                if (sx1[id] < 0 || hi[id]) continue;
                const uint8_t *sp = &vdp.vram[(sat + (uint32_t)id * 8) & 0xFFFF];
                if (battleui_tile_member((sp[4] << 8 | sp[5]) & 0x7FF) != actor - 1) continue;
                if (sx0[id] < bx0) bx0 = sx0[id];
                if (sx1[id] > bx1) bx1 = sx1[id];
                if (sy0[id] < by0) by0 = sy0[id];
                if (sy1[id] > by1) by1 = sy1[id];
            }
        }
        int there = bx1 >= 0 && bx1 - bx0 >= 12 && by1 - by0 >= 24 && by1 < PARTY_FEET_Y - 20;
        if (there) {
            if (strike_k < 1.0f) {                            /* cut in: on them as they are now */
                strike_gx = (float)(bx0 + bx1 + 1) / 2.0f;
                strike_gy = (float)(by1 + 1);
                if (strike_gy < 112.0f) strike_gy = 112.0f;   /* (caught mid-leap: the ground under them) */
            }
            strike_k = 1.0f;
            strike_gap = 0;
        } else if (strike_k > 0.0f && ++strike_gap > 3) {     /* gone (not a frame's flicker): back */
            strike_k -= dt / STRIKE_BACK;
            if (strike_k < 0.0f) strike_k = 0.0f;
        }
        if (strike_k > 0.0f && ik <= 0.0f) {
            float k = strike_k * strike_k * (3.0f - 2.0f * strike_k);
            float qx = (float)w / 2.0f, qy = (float)h * 0.64f;
            float lo = (float)w - ((float)w - strike_gx) * STRIKE_ZOOM, hi_x = strike_gx * STRIKE_ZOOM;   /* inside the picture */
            if (qx < lo) qx = lo;
            if (qx > hi_x) qx = hi_x;
            iz = 1.0f + (STRIKE_ZOOM - 1.0f) * k;
            iqx = strike_gx + (qx - strike_gx) * k;
            iqy = strike_gy + (qy - strike_gy) * k;
            icx = strike_gx - (iqx - (float)w / 2.0f) / iz;
            icy_bg = strike_gy - (iqy - (float)h * 0.45f) / iz;
            icy_en = strike_gy - (iqy - (float)h / 2.0f) / iz;
            icy_pa = strike_gy - (iqy - (float)PARTY_FEET_Y) / iz;
        }
    }
    int closeup = ik > 0.0f || strike_k > 0.0f;
    cl_z = iz; cl_x = icx; cl_y_en = icy_en; cl_y_pa = icy_pa;

    /* The camera pan: while an enemy attacks someone (or someone casts),
     * the game stands them under the enemy, often over another member. The
     * view turns to face them instead: the one stood there glides from their
     * place to it, everyone else slides the same way as one row, drawn whole
     * from the pictures kept at the menus (battleui.c), so no one overlaps;
     * and back again after. */
    /* With the Camera off, the step instead (PsiLok, 2026-11-19: "have the attacking member step in front of the
     * sitting member, sitting member or party could scoot back (pan south) during the attack"): the one stood
     * there stays where the game put them but a step up, towards the enemies; everyone else a step down, back
     * towards the windows, and in front of them; so the one in whose place they stand is still seen. */
    int pan_m = -1, pan_to = 0;
    battleui_pan(&pan_m, &pan_to);
    int stepping = !battleui_pan_enabled();
    if (stepping) pan_to = 0;
    ease(&pan_a, (float)pan_to, 1.0f - expf(-dt / 0.14f));
    if (pan_m < 0 && fabsf(pan_a) < 0.3f) pan_a = 0.0f;
    ease(&step_k, stepping && pan_m >= 0 ? 1.0f : 0.0f, 1.0f - expf(-dt / 0.07f));
    if (pan_m < 0 && step_k < 0.02f) step_k = 0.0f;
    int panning = pan_m >= 0 || pan_a != 0.0f || step_k > 0.0f;
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
    int t_dy = (int)lroundf(-STEP_UP * step_k) * scale, a_dy = (int)lroundf(STEP_DOWN * step_k) * scale;   /* the step */

    /* objects: sprites that touch, or all but touch (an enemy's lunge, a
     * claw, a technique's burst), so no picture is ever split between layers.
     * Not across an enemy's own colours, though: a sprite in a palette line the enemies' picture is drawn in
     * joins only others in that line. The game draws a part of an enemy as sprites in the enemy's colours, and
     * a technique's effect in the effects' own line: the effect that lands on someone touches the arms that
     * cast it, and as one object the arms went with it, to the party (Xe-A-Thoul again, PsiLok 2026-11-20) */
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
            if (sx1[j] >= 0 && sx0[i] <= sx1[j] + 5 && sx0[j] <= sx1[i] + 5 && sy0[i] <= sy1[j] + 5 && sy0[j] <= sy1[i] + 5
                && (sline[i] == sline[j] || !((en_lines >> sline[i] | en_lines >> sline[j]) & 1))) {
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
    /* part of an enemy: some enemies cast or strike with a part of their own picture as sprites (a
     * robe's sides lifted with the arms), cut out of plane A and set against what is left of it. Such
     * an object lies along the enemy's picture and not over it (an effect on an enemy is drawn over
     * it): it stays with the enemies whoever stands below, or the enemy comes apart (Xe-A-Thoul's
     * Giwat, with the one it is cast on stood under it and the view turned).
     * And the surest sign of a part of an enemy: its colours. obj_seam and obj_own count the object's pixels
     * that lie against, and on, a pixel of the enemies' picture drawn in the same palette line (ENEMY_ART: low
     * priority, as that picture is; not a damage number) */
#define ENEMY_ART(e, line) ((e) && !((e) & 0x80) && ((e) >> 4 & 3) == (line))
    static int obj_edge[128], obj_over[128], obj_tot[128], obj_seam[128], obj_own[128];
    memset(obj_edge, 0, sizeof obj_edge);
    memset(obj_over, 0, sizeof obj_over);
    memset(obj_tot, 0, sizeof obj_tot);
    memset(obj_seam, 0, sizeof obj_seam);
    memset(obj_own, 0, sizeof obj_own);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t s = layer[L_ACTION][y][x];
            if (!s) continue;
            int r = sprite_root(parent, lines[y].sprite_no[x] & 127), line = s >> 4 & 3;
            obj_tot[r]++;
            if (layer[L_ENEMY][y][x]) {
                obj_over[r]++;
                obj_own[r] += ENEMY_ART(layer[L_ENEMY][y][x], line);
                continue;
            }
            /* beside it: the enemies' picture bare (obj_edge), or in this sprite's colours and not under this
             * object -- bare, or under another's sprite: a technique's effect passing over an enemy hid the
             * seam of the arms he held out, for a frame (obj_seam) */
            static const int nx[4] = {-1, 1, 0, 0}, ny[4] = {0, 0, -1, 1};
            int edge = 0, seam = 0;
            for (int k = 0; k < 4; k++) {
                int xx = x + nx[k], yy = y + ny[k];
                if (xx < 0 || xx >= w || yy < 0 || yy >= h || !layer[L_ENEMY][yy][xx]) continue;
                int bare = !layer[L_ACTION][yy][xx];
                edge |= bare;
                seam |= ENEMY_ART(layer[L_ENEMY][yy][xx], line)
                        && (bare || sprite_root(parent, lines[yy].sprite_no[xx] & 127) != r);
            }
            obj_edge[r] += edge;
            obj_seam[r] += seam;
        }
#undef ENEMY_ART
    /* on the party: over someone's picture, or coming down on them (all the
     * way by the time it reaches the top of their head) */
    int any_near = 0;
    static uint8_t obj_kind[128];                             /* (for the log: how an object was known for an enemy's) */
    memset(obj_kind, 0, sizeof obj_kind);
    for (int i = 0; i < OWN_SEEN; i++)
        if (own_seen[i].ttl > 0) own_seen[i].ttl--;
    for (int r = 0; r < 128; r++) {
        if (closeup) break;                                   /* (a close-up: one camera, no sprite goes over to
                                                               * the party's -- an enemy's own would leave it) */
        if (ox1[r] < 0 || sprite_root(parent, r) != r || obj_ui[r]) continue;
        /* an enemy's own: in its colours, flush against its picture (a part cut out of it, whatever else of the
         * enemy's is drawn with it: the light in Xe-A-Thoul's hands lay over him, and for that the robe's sides
         * he lifts went part of the way to the one stood below, sliding off him as the view turned). Its place
         * is remembered: there, in those colours, it is still the enemy's while the join is hidden */
        if (obj_seam[r] >= 16) {
            int at = -1, old = 0;
            for (int i = 0; i < OWN_SEEN; i++) {
                int ix = (ox1[r] < own_seen[i].x1 ? ox1[r] : own_seen[i].x1) - (ox0[r] > own_seen[i].x0 ? ox0[r] : own_seen[i].x0) + 1;
                int iy = (oy1[r] < own_seen[i].y1 ? oy1[r] : own_seen[i].y1) - (oy0[r] > own_seen[i].y0 ? oy0[r] : own_seen[i].y0) + 1;
                if (own_seen[i].ttl > 0 && ix > 0 && iy > 0
                    && (long)ix * iy * 2 >= (long)(own_seen[i].x1 - own_seen[i].x0 + 1) * (own_seen[i].y1 - own_seen[i].y0 + 1)) { at = i; break; }
                if (own_seen[i].ttl < own_seen[old].ttl) old = i;
            }
            if (at < 0) at = old;                             /* (a new place: in the oldest one's) */
            own_seen[at].x0 = ox0[r]; own_seen[at].y0 = oy0[r]; own_seen[at].x1 = ox1[r]; own_seen[at].y1 = oy1[r];
            own_seen[at].ttl = OWN_TTL;
            obj_kind[r] = 1;
            continue;
        }
        if (en_lines >> sline[r] & 1) {
            long area = (long)(ox1[r] - ox0[r] + 1) * (oy1[r] - oy0[r] + 1);
            for (int i = 0; i < OWN_SEEN && !obj_kind[r]; i++) {
                int ix = (ox1[r] < own_seen[i].x1 ? ox1[r] : own_seen[i].x1) - (ox0[r] > own_seen[i].x0 ? ox0[r] : own_seen[i].x0) + 1;
                int iy = (oy1[r] < own_seen[i].y1 ? oy1[r] : own_seen[i].y1) - (oy0[r] > own_seen[i].y0 ? oy0[r] : own_seen[i].y0) + 1;
                if (own_seen[i].ttl > 0 && ix > 0 && iy > 0 && (long)ix * iy * 2 >= area) {
                    own_seen[i].ttl = OWN_TTL;                /* (its place stays where it was known: one that leaves
                                                               * it, a fist thrown, is let go) */
                    obj_kind[r] = 2;
                }
            }
            if (obj_kind[r]) continue;
        }
        /* ... or, not on the party, in its colours and a good part of it on or against its picture (not a fist
         * on its way to someone, a corner of it still over the arm) */
        if (!obj_party[r] && obj_seam[r] + obj_own[r] >= 16 && (obj_seam[r] + obj_own[r]) * 4 >= obj_tot[r]) {
            obj_kind[r] = 3;
            continue;
        }
        if (obj_edge[r] >= 16 && obj_over[r] * 2 < obj_edge[r]) continue;
        /* ... or lies on it, most of it over the enemy's own picture and none of it on the party (Xe-A-Thoul
         * raising his arms: his shoulders and his robe's upper part drawn as sprites over the rest of him; handed
         * to the party's camera they slid off him, and came out of the fog) */
        if (!obj_party[r] && obj_over[r] >= 16 && obj_over[r] * 2 >= obj_tot[r]) continue;
        int top = h;
        for (int x = ox0[r] < 0 ? 0 : ox0[r]; x <= ox1[r] && x < w; x++)
            if (ptop_col[x] < top) top = ptop_col[x];
        if (top >= h) continue;                               /* no one below it */
        float wgt = obj_party[r] ? 1.0f : (float)(oy1[r] + 1 - (top - 16)) / 16.0f;
        obj_w[r] = wgt < 0.0f ? 0.0f : wgt > 1.0f ? 1.0f : wgt;
        any_near |= obj_w[r] > 0.0f;
    }
    {   /* PSIV_DEPTH_LOG=1: print each frame's objects of low-priority sprites (for sorting them) */
        static int log_on = -1;
        if (log_on < 0) { const char *v = getenv("PSIV_DEPTH_LOG"); log_on = v && *v == '1'; }
        if (log_on) {
            uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
            int elo = 0x7FF, ehi = 0;                         /* the enemies' own tiles in plane A */
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int tile = lines[y].plane_a_tile[x] & 0x7FF;
                    if (!layer[L_ENEMY][y][x] || (layer[L_ENEMY][y][x] & 0x80) || lines[y].win_in[x]) continue;
                    if (tile < elo) elo = tile;
                    if (tile > ehi) ehi = tile;
                }
            for (int r = 0; r < 128; r++) {
                if (ox1[r] < 0 || sprite_root(parent, r) != r || !obj_tot[r]) continue;
                int tlo = 0x7FF, thi = 0, n = 0, pl = 0;
                for (int id = 0; id < 128; id++) {
                    if (sx1[id] < 0 || sprite_root(parent, id) != r) continue;
                    const uint8_t *sp = &vdp.vram[(sat + (uint32_t)id * 8) & 0xFFFF];
                    int tile = (sp[4] << 8 | sp[5]) & 0x7FF;
                    if (tile < tlo) tlo = tile;
                    if (tile > thi) thi = tile;
                    pl |= 1 << ((sp[4] >> 5) & 3);
                    n++;
                }
                printf("depth L%llu pan %d %.1f obj %d box %d,%d-%d,%d sprites %d tiles %03X-%03X pal %X tot %d over %d edge %d party %d w %.2f seam %d own %d kind %d enemy lines %X tiles %03X-%03X\n",
                       (unsigned long long)frame_logical, pan_m, pan_a, r, ox0[r], oy0[r], ox1[r], oy1[r], n, tlo, thi, pl,
                       obj_tot[r], obj_over[r], obj_edge[r], obj_party[r], obj_w[r], obj_seam[r], obj_own[r], obj_kind[r],
                       en_lines, elo, ehi);
            }
        }
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
    int fine = fine_want && scale >= 2 && fine_ready() ? scale : 0;   /* Scale2x: the layers enlarged this much */
    if (fine) {
        build_map_fine(colf_bg, col_bg, ow, scale, fine, w, w / 2.0f, F_BG, zoom, cam_bg, 1, iz, icx);
        build_map_fine(rowf_bg, row_bg, oh, scale, fine, h, h * 0.45f, F_BG, zoom, cam_y, 1, iz, icy_bg);
        build_map_fine(colf_en, col_en, ow, scale, fine, w, w / 2.0f, F_ENEMY, zoom, cam_en, 0, iz, icx);
        build_map_fine(rowf_en, row_en, oh, scale, fine, h, h / 2.0f, F_ENEMY, zoom, cam_y, 0, iz, icy_en);
        build_map_fine(colf_pa, col_pa, ow, scale, fine, w, w / 2.0f, F_PARTY, zoom, cam_x, 0, iz, icx);
        build_map_fine(rowf_pa, row_pa, oh, scale, fine, h, (float)PARTY_FEET_Y, F_PARTY, zoom, 0.0f, 0, iz, icy_pa);
    } else {
        build_map(col_bg, ow, scale, w, w / 2.0f, F_BG, zoom, cam_bg, 1, iz, icx);
        build_map(row_bg, oh, scale, h, h * 0.45f, F_BG, zoom, cam_y, 1, iz, icy_bg);
        build_map(col_en, ow, scale, w, w / 2.0f, F_ENEMY, zoom, cam_en, 0, iz, icx);
        build_map(row_en, oh, scale, h, h / 2.0f, F_ENEMY, zoom, cam_y, 0, iz, icy_en);
        build_map(col_pa, ow, scale, w, w / 2.0f, F_PARTY, zoom, cam_x, 0, iz, icx);
        build_map(row_pa, oh, scale, h, (float)PARTY_FEET_Y, F_PARTY, zoom, 0.0f, 0, iz, icy_pa);   /* (no vertical move but the opening's) */
    }

    {   /* Zio's stage: the fog made for this frame, and the way into it through the enemies' camera */
        float zdim = 1.0f, zwhite = 0.0f;
        zf_on = !vehicle && zf_stage(lines, w, h, &zdim, &zwhite);
        if (!zf_on) zf_wisp = 0.0f;
        if (zf_on) {
            int bh = (h + 2 * ZF_MY) * ZF_RES;
            /* Nightmare's face: the enemies' picture in the sky over him, where nothing else of theirs is */
            long face = 0;
            for (int y = 0; y < 50; y += 2)
                for (int x = w / 2 - 60; x < w / 2 + 60; x += 2) face += layer[L_ENEMY][y][x] != 0;
            ease(&zf_wisp, face > 250 ? 1.0f : 0.0f, 1.0f - expf(-dt / (face > 250 ? 0.7f : 0.45f)));
            ZfGen g = {t, (float)w / 2.0f, zdim, zwhite, (w + 2 * ZF_MX) * ZF_RES, bh, zf_wisp};
            par_rows(zf_rows, &g, bh);
            par_rows(zf_cols, &g, g.bw);
            if (zf_wisp > 0.02f) par_rows(zf_wisps, &g, bh);
            /* whose feet are where: the enemies' picture stands at his; a sprite at its object's foot (his own
             * while he casts, a fighter's who has run up to him), and no further off than he is */
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int f = (int)ZF_HEEL;
                    if (lines[y].sprite[x]) {
                        int o = oy1[sprite_root(parent, lines[y].sprite_no[x] & 127)] + 1;
                        if (o > f) f = o > 250 ? 250 : o;
                    }
                    zf_feet[y][x] = (uint8_t)f;
                }
            float s = (1.0f + F_ENEMY * (zoom - 1.0f)) * iz;
            for (int X = 0; X < ow; X++) {
                float q = ((float)X + 0.5f) / (float)scale, src = icx + (q - (float)w / 2.0f) / s + F_ENEMY * cam_en;
                int bx = (int)floorf((src + (float)ZF_MX) * ZF_RES);
                zf_col[X] = (int16_t)(bx < 0 ? 0 : bx >= g.bw ? g.bw - 1 : bx);
            }
            for (int Y = 0; Y < oh; Y++) {
                float q = ((float)Y + 0.5f) / (float)scale, src = icy_en + (q - (float)h / 2.0f) / s + F_ENEMY * cam_y;
                int by = (int)floorf((src + (float)ZF_MY) * ZF_RES);
                zf_row[Y] = (int16_t)(by < 0 ? 0 : by >= bh ? bh - 1 : by);
            }
        }
    }

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
                if (on_t) {
                    obj_dx[r] += (int)lroundf((float)t_out * obj_w[r]);
                    obj_dy[r] += (int)lroundf((float)t_dy * obj_w[r]);
                } else if (on_m) {
                    obj_dx[r] += (int)lroundf((float)a_out * obj_w[r]);
                    obj_dy[r] += (int)lroundf((float)a_dy * obj_w[r]);
                }
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
    bd_tint = tint;                                           /* (Zio's fog under the storm's light too) */
    bd_tm[0] = tmr; bd_tm[1] = tmg; bd_tm[2] = tmb; bd_ta[0] = tar; bd_ta[1] = tag; bd_ta[2] = tab;
    Rows rows = {out, out_stride, scale, ow, pal, tpal, panning, t_out, a_out, lit, fog_on, tint_r, tint_g, tint_b,
                 t_dy, a_dy, oh};
    const uint8_t *near_fine = NULL;                          /* (Scale2x: the sprites on the party, enlarged) */
    if (fine) {                                               /* every layer through the filter, now they are sorted */
        for (int n = 0; n < NLAYERS; n++) rows.L[n] = fine_make(n, layer[n], NULL, 0, w, h, fine);
        rows.mi = panning ? fine_make(FINE_MIMG, mimg, NULL, 0, w, h, fine) : fine_plane(FINE_ZERO);
        if (any_near) near_fine = fine_make(FINE_NEAR, near_px, near_obj, 1, w, h, fine);
        rows.ls = w * fine;
        rows.ui_div = 1;
        rows.cb = colf_bg; rows.ce = colf_en; rows.cp = colf_pa;
        rows.rb = rowf_bg; rows.re = rowf_en; rows.rp = rowf_pa;
    } else {                                                  /* the game's own layers, as they are */
        for (int n = 0; n < NLAYERS; n++) rows.L[n] = &layer[n][0][0];
        rows.mi = &mimg[0][0];
        rows.ls = VDP_MAX_W;
        rows.ui_div = scale;
        rows.cb = col_bg; rows.ce = col_en; rows.cp = col_pa;
        rows.rb = row_bg; rows.re = row_en; rows.rp = row_pa;
    }
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
                if (sx < 0 || sx >= ow || (u = col_en[sx]) < 0) continue;
                uint8_t c, f = front[Y][X];
                if (!near_fine) {
                    if (near_obj[ve][u] != r + 1) continue;
                    c = near_px[ve][u];
                } else {                                      /* Scale2x: the enlarged sprite's pixel here */
                    c = near_fine[(size_t)rowf_en[sy] * (size_t)ow + (size_t)colf_en[sx]];
                    if (!c) continue;                         /* (a corner the filter rounded away) */
                    int o = near_obj[ve][u];
                    if (o != r + 1) {                         /* ... or one it filled in: this object's if beside it */
                        if (o) continue;
                        if (!((u > 0 && near_obj[ve][u - 1] == r + 1) || (u < w - 1 && near_obj[ve][u + 1] == r + 1)
                              || (ve > 0 && near_obj[ve - 1][u] == r + 1) || (ve < h - 1 && near_obj[ve + 1][u] == r + 1)))
                            continue;
                    }
                }
                if ((c & 0x80) ? f == 3 : (f == 1 || f == 3)) continue;
                dst[X] = zf_on ? zf_over(tpal[c & 0x3F], X, Y, zf_feet[ve][u]) : tpal[c & 0x3F];
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
    if (intro_state == INTRO_RUN && intro_t < 0.24f) {        /* the opening's flash, on the close-up as it appears */
        float f = 1.0f - intro_t / 0.24f;
        int a = (int)(200.0f * f * f);
        for (int Y = 0; Y < oh; Y++) {
            uint32_t *dst = out + (size_t)Y * (size_t)out_stride;
            for (int X = 0; X < ow; X++) {
                uint32_t c = dst[X];
                int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
                r += ((255 - r) * a) >> 8; g += ((255 - g) * a) >> 8; b += ((255 - b) * a) >> 8;
                dst[X] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            }
        }
    }
    return 1;
}
