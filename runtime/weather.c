/*
 * Battle weather: see weather.h.
 */
#include "weather.h"
#include "m68k.h"
#include "sfxsynth.h"
#include "sound.h"
#include "uikit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAM ($FFxxxx) and ROM the game picks a battle's background from
 * (Battle_SetupBackground; ps4.constants.asm) */
#define FIELD_MAP_INDEX      0xEC28u
#define MOTA_BATTLE_BG_INDEX 0xECEFu   /* outside on Motavia: by the tile you stand on */
#define EVENT_BATTLE_INDEX   0xECFCu   /* a set battle (bosses); negative = none */
#define EVENT_FLAGS          0xF100u
#define FLAG_DARKFORCE2      0x9E      /* Dezolis looks different after it */
#define VEHICLE_INDEX        0xF43Cu
#define ROM_EVENT_BG         0x6C8Cu   /* EventBattleBGIndexes */
#define ROM_MAP_BG           0x6CA8u   /* Battle_BackgroundIndexes */

static const char *const BG_NAMES[32] = {
    "Motavia desert", "Motavia beach", "Motavia grassland", "Motavia sea", "Dezolis", "Dezolis (later)",
    "Bio-plant", "Wreckage", "Plate system", "Climate center", "Weapon plant", "Vahal fort",
    "Plate system F1", "Academy basement", "Ladea tower", "Zio's fort", "Garuberk tower",
    "Tonoe basement", "Air castle", "Monsen cave", "Island cave", "Rykros", "Zema", "Reshel",
    "Tower of Courage", "Tower of Strength", "Tower of Anger", "The Edge", "Elsydeon cave",
    "Passageway", "Carnivorous trees", "Dark Force",
};
static const char *const KIND_NAMES[WX_KINDS] = {"clear", "rain", "storm", "snow", "clouds", "debris"};
static const char *const LEVEL_NAMES[WEATHER_LEVELS] = {"off", "light", "heavy"};

static int level = 1, forced = -1, sound_on = 1;
static int kind = WX_NONE, picked, bg_index = -1;
static float t_wx, fade;                 /* seconds of this battle's weather; 0..1 easing in */
static uint32_t rs = 0x2545F491u;

static void sound_init(void);
void weather_set_level(int l) {
    level = l < 0 ? 0 : l >= WEATHER_LEVELS ? WEATHER_LEVELS - 1 : l;
    if (level && sound_on) sound_init();         /* rain and thunder made now, not at a battle's start */
}
int  weather_get_level(void) { return level; }
const char *weather_level_name(int l) { return l >= 0 && l < WEATHER_LEVELS ? LEVEL_NAMES[l] : "?"; }
void weather_force(int k) { forced = k >= 0 && k < WX_KINDS ? k : -1; }
const char *weather_name(int k) { return k >= 0 && k < WX_KINDS ? KIND_NAMES[k] : "?"; }
void weather_set_sound(int on) { sound_on = on; }
int  weather_kind_by_name(const char *name) {
    for (int k = 0; k < WX_KINDS; k++)
        if (!strcmp(name, KIND_NAMES[k])) return k;
    return -1;
}

static float frand(void) {                                /* xorshift: 0..1 */
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return (float)(rs >> 8) * (1.0f / 16777216.0f);
}
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float smoothf(float e0, float e1, float x) { x = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

/* the background the game picked for this battle (Battle_SetupBackground) */
static int battle_bg(void) {
    int ev = (int8_t)m68k_ram[EVENT_BATTLE_INDEX], bg;
    if (ev >= 0) bg = (int)ui_rom8(ROM_EVENT_BG + (unsigned)ev);
    else {
        unsigned map = (unsigned)(m68k_ram[FIELD_MAP_INDEX] << 8 | m68k_ram[FIELD_MAP_INDEX + 1]);
        bg = map ? (int)ui_rom8(ROM_MAP_BG + map) : m68k_ram[MOTA_BATTLE_BG_INDEX];
    }
    if (bg == 4 && (m68k_ram[EVENT_FLAGS + (FLAG_DARKFORCE2 >> 3)] >> (7 - (FLAG_DARKFORCE2 & 7)) & 1)) bg = 5;
    return bg;
}

/* the weather for a background: some battles in the open are clear, the rest
 * rain or a storm (the sea's more often); heavy means fewer clear ones */
static int pick(int bg) {
    float r = frand();
    int heavy = level >= 2;
    switch (bg) {
    case 18: case 27: return WX_DEBRIS;                       /* the Air Castle, the Edge */
    case 4: case 5: return WX_SNOW;                           /* Dezolis */
    case 21: case 24: case 25: case 26: case 30: return WX_CLOUDS;   /* Rykros, its towers, the trees */
    case 1: case 2:                                           /* Motavia's beach and grassland */
        return r < (heavy ? 0.25f : 0.45f) ? WX_NONE : r < (heavy ? 0.6f : 0.78f) ? WX_RAIN : WX_STORM;
    case 3:                                                   /* the sea */
        return r < (heavy ? 0.2f : 0.35f) ? WX_NONE : r < (heavy ? 0.5f : 0.62f) ? WX_RAIN : WX_STORM;
    default: return WX_NONE;
    }
}

/* ------------------------------------------------------------------ particles */
enum { FAR, MID, NEAR, NL };
static const float LF[NL] = {0.25f, 0.6f, 1.15f};         /* how far each follows the camera */
static const unsigned MASK[NL] = {                          /* what each may be drawn over */
    1u << WX_FRONT_BG,
    1u << WX_FRONT_BG | 1u << WX_FRONT_ENEMY,
    1u << WX_FRONT_BG | 1u << WX_FRONT_ENEMY | 1u << WX_FRONT_PARTY | 1u << WX_FRONT_PARTY_UNDER,
};
#define MARGIN 48.0f                                        /* game pixels beyond each side */

typedef struct { float x, y, v, len, ground; uint8_t layer; } Drop;
#define MAX_DROPS 720
static Drop drops[MAX_DROPS];
static int ndrops;

typedef struct { float x, y, age; uint8_t layer; } Splash;
#define MAX_SPLASHES 200
static Splash splashes[MAX_SPLASHES];
static int nsplashes;

typedef struct { float x, y, v, phase, freq, sway, size, ground; uint8_t layer; } Flake;
#define MAX_FLAKES 420
static Flake flakes[MAX_FLAKES];
static int nflakes;

typedef struct { float x, y, vx, vy, ang, spin, r, rad[9]; int nv; uint8_t layer, tone; } Rock;
#define MAX_ROCKS 48
static Rock rocks[MAX_ROCKS];
static int nrocks;

typedef struct { float x, y, ph, rate; uint8_t big; } Star;
#define MAX_STARS 140
static Star stars[MAX_STARS];
static int nstars;

/* a storm's lightning */
static float next_strike, since_strike = 99.0f, flash;
static float bolt_x[64], bolt_y[64];
static int bolt_n, bolt_branch_at, bolt_branch_n;
static float branch_x[16], branch_y[16];
static float dark_r = 1, dark_g = 1, dark_b = 1;           /* eased */
static float wind;

/* sound requests from the picture to the mixer */
static float rain_target;                                   /* the rain's loudness, 0 = none */
static int thunder_due_variant = -1;
static float thunder_due_in, thunder_gain;

static int count_for(int base) {
    return (int)((float)base * (level >= 2 ? 1.6f : 1.0f));
}

static void drop_spawn(Drop *d, int w, int anywhere) {
    static const float V0[NL] = {330, 480, 650}, V1[NL] = {420, 570, 780};
    static const float L0[NL] = {6, 10, 18}, L1[NL] = {9, 15, 27};
    int l = d->layer;
    d->v = V0[l] + (V1[l] - V0[l]) * frand();
    d->len = L0[l] + (L1[l] - L0[l]) * frand();
    d->x = -MARGIN + frand() * ((float)w + 2.0f * MARGIN) - wind * 60.0f;
    d->ground = l == FAR ? 82.0f + 40.0f * frand() : l == MID ? 122.0f + 52.0f * frand() : 260.0f;
    d->y = anywhere ? -d->len + frand() * (d->ground + d->len) : -d->len - 40.0f * frand();
}

static void flake_spawn(Flake *k, int w, int anywhere) {
    static const float V0[NL] = {13, 23, 38}, V1[NL] = {21, 34, 58};
    static const float S0[NL] = {0.55f, 1.0f, 2.0f}, S1[NL] = {0.85f, 1.5f, 3.2f};
    static const float W[NL] = {3, 6, 11};
    int l = k->layer;
    k->v = V0[l] + (V1[l] - V0[l]) * frand();
    k->size = S0[l] + (S1[l] - S0[l]) * frand();
    k->phase = frand() * 6.2831853f;
    k->freq = 0.5f + 0.9f * frand();
    k->sway = W[l] * (0.6f + 0.4f * frand());
    k->x = -MARGIN + frand() * ((float)w + 2.0f * MARGIN);
    k->ground = l == FAR ? 96.0f + 30.0f * frand() : l == MID ? 132.0f + 44.0f * frand() : 250.0f;
    k->y = anywhere ? -4.0f + frand() * (k->ground + 4.0f) : -4.0f - 20.0f * frand();
}

static void rock_spawn(Rock *r, int w, int h, int anywhere) {
    static const float R0[NL] = {1.4f, 4.0f, 11.0f}, R1[NL] = {3.4f, 8.0f, 19.0f};
    static const float V0[NL] = {2.5f, 5.0f, 9.0f}, V1[NL] = {7.0f, 13.0f, 17.0f};
    int l = r->layer;
    r->r = R0[l] + (R1[l] - R0[l]) * frand();
    float v = V0[l] + (V1[l] - V0[l]) * frand(), a = (frand() - 0.5f) * 0.9f;
    float dir = frand() < 0.5f ? 1.0f : -1.0f;               /* drifting one way or the other */
    r->vx = dir * v * cosf(a);
    r->vy = v * sinf(a) * 0.5f;
    r->ang = frand() * 6.2831853f;
    r->spin = (frand() - 0.5f) * (l == NEAR ? 0.6f : 1.2f);
    r->nv = 6 + (int)(frand() * 3.0f);
    for (int i = 0; i < r->nv; i++) r->rad[i] = 0.62f + 0.38f * frand();
    r->tone = (uint8_t)(frand() * 4.0f);
    float top = l == NEAR ? 8.0f : 4.0f, bottom = l == NEAR ? 120.0f : l == MID ? 150.0f : 118.0f;
    if (bottom > (float)h - 20.0f) bottom = (float)h - 20.0f;
    r->y = top + frand() * (bottom - top);
    if (anywhere) r->x = -MARGIN + frand() * ((float)w + 2.0f * MARGIN);
    else r->x = dir > 0 ? -MARGIN - r->r : (float)w + MARGIN + r->r;
}

static void start_weather(int w, int h) {
    ndrops = nsplashes = nflakes = nrocks = nstars = 0;
    wind = kind == WX_STORM ? 0.3f : 0.12f;
    if (kind == WX_RAIN || kind == WX_STORM) {
        static const int N[NL] = {110, 70, 22};
        float k = kind == WX_STORM ? 1.7f : 1.0f;
        for (int l = 0; l < NL; l++)
            for (int i = 0; i < (int)((float)count_for(N[l]) * k) && ndrops < MAX_DROPS; i++) {
                drops[ndrops].layer = (uint8_t)l;
                drop_spawn(&drops[ndrops++], w, 1);
            }
    }
    if (kind == WX_SNOW) {
        static const int N[NL] = {160, 105, 28};
        for (int l = 0; l < NL; l++)
            for (int i = 0; i < count_for(N[l]) && nflakes < MAX_FLAKES; i++) {
                flakes[nflakes].layer = (uint8_t)l;
                flake_spawn(&flakes[nflakes++], w, 1);
            }
    }
    if (kind == WX_DEBRIS) {
        static const int N[NL] = {18, 8, 2};
        for (int l = 0; l < NL; l++)
            for (int i = 0; i < count_for(N[l]) && nrocks < MAX_ROCKS; i++) {
                rocks[nrocks].layer = (uint8_t)l;
                rock_spawn(&rocks[nrocks++], w, h, 1);
            }
        int n = bg_index == 27 ? 110 : 40;                    /* the Edge is open space: stars */
        for (int i = 0; i < n && nstars < MAX_STARS; i++) {
            Star *s = &stars[nstars++];
            s->x = -MARGIN + frand() * ((float)w + 2.0f * MARGIN);
            s->y = frand() * (bg_index == 27 ? 150.0f : 120.0f);
            s->ph = frand() * 6.2831853f;
            s->rate = 0.4f + 1.6f * frand();
            s->big = frand() < 0.12f;
        }
    }
    next_strike = 1.5f + 2.5f * frand();
    since_strike = 99.0f;
    flash = 0.0f;
    dark_r = dark_g = dark_b = 1.0f;
}

int weather_active(void) {
    if (!level) { kind = WX_NONE; return 0; }
    if (!picked) {
        picked = 1;
        bg_index = battle_bg();
        kind = forced >= 0 ? forced : (m68k_ram[VEHICLE_INDEX] << 8 | m68k_ram[VEHICLE_INDEX + 1]) ? WX_NONE
                                                                                                  : pick(bg_index);
        t_wx = fade = 0.0f;
        start_weather(424, 224);
        static int log_on = -1;
        if (log_on < 0) { const char *v = getenv("PSIV_FX_LOG"); log_on = v && *v == '1'; }
        if (log_on)
            printf("weather: background %d (%s): %s\n", bg_index,
                   bg_index >= 0 && bg_index < 32 ? BG_NAMES[bg_index] : "?", KIND_NAMES[kind]);
    }
    return kind != WX_NONE;
}

void weather_battle_end(void) {
    picked = 0;
    kind = WX_NONE;
    rain_target = 0.0f;
    ndrops = nsplashes = nflakes = nrocks = nstars = 0;
}

/* ------------------------------------------------------------------ drawing */
static const WeatherFrame *F;

static void to_out(int layer, float x, float y, float *ox, float *oy) {
    float k = LF[layer], s = 1.0f + k * (F->zoom - 1.0f), cx = (float)F->w * 0.5f, cy = (float)F->h * 0.5f;
    *ox = (cx + (x - k * F->cam_x - cx) * s) * (float)F->scale;
    *oy = (cy + (y - k * F->cam_y - cy) * s) * (float)F->scale;
}

static void plot(int X, int Y, uint32_t c, int a, unsigned mask) {
    if (a <= 0 || (unsigned)X >= (unsigned)(F->w * F->scale) || (unsigned)Y >= (unsigned)(F->h * F->scale)) return;
    if (!(mask >> F->front[(size_t)Y * (size_t)F->front_stride + (size_t)X] & 1)) return;
    uint32_t *d = &F->out[(size_t)Y * (size_t)F->stride + (size_t)X], s = *d;
    if (a > 256) a = 256;
    int r = (int)(s >> 16 & 255), g = (int)(s >> 8 & 255), b = (int)(s & 255);
    r += (((int)(c >> 16 & 255) - r) * a) >> 8;
    g += (((int)(c >> 8 & 255) - g) * a) >> 8;
    b += (((int)(c & 255) - b) * a) >> 8;
    *d = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static void add_px(int X, int Y, int ar, int ag, int ab, unsigned mask) {
    if ((unsigned)X >= (unsigned)(F->w * F->scale) || (unsigned)Y >= (unsigned)(F->h * F->scale)) return;
    if (!(mask >> F->front[(size_t)Y * (size_t)F->front_stride + (size_t)X] & 1)) return;
    uint32_t *d = &F->out[(size_t)Y * (size_t)F->stride + (size_t)X], s = *d;
    int r = (int)(s >> 16 & 255) + ar, g = (int)(s >> 8 & 255) + ag, b = (int)(s & 255) + ab;
    *d = 0xFF000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8
         | (uint32_t)(b > 255 ? 255 : b);
}

/* a line fading in from (x0,y0) to (x1,y1), `width` output pixels wide,
 * anti-aliased across */
static void streak(float x0, float y0, float x1, float y1, uint32_t c, float a, int width, unsigned mask) {
    float dx = x1 - x0, dy = y1 - y0, L = fmaxf(fabsf(dx), fabsf(dy));
    int n = (int)L + 1;
    for (int i = 0; i <= n; i++) {
        float u = (float)i / (float)n, x = x0 + dx * u, y = y0 + dy * u;
        int aa = (int)(a * (0.15f + 0.85f * u) * 256.0f), X = (int)floorf(x), Y = (int)floorf(y);
        float fx = x - (float)X;
        int a0 = (int)((float)aa * (1.0f - fx));
        plot(X, Y, c, a0, mask);
        for (int k = 1; k < width; k++) plot(X + k, Y, c, aa, mask);
        plot(X + width, Y, c, aa - a0, mask);
    }
}

/* a soft round dot */
static void dot(float cx, float cy, float r, uint32_t c, float a, unsigned mask) {
    if (r < 0.6f) r = 0.6f;
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int Y = y0; Y <= y1; Y++)
        for (int X = x0; X <= x1; X++) {
            float ddx = (float)X + 0.5f - cx, ddy = (float)Y + 0.5f - cy, d = sqrtf(ddx * ddx + ddy * ddy) / (r + 0.5f);
            if (d >= 1.0f) continue;
            plot(X, Y, c, (int)(a * (1.0f - smoothf(0.5f, 1.0f, d)) * 256.0f), mask);
        }
}

static void draw_rain(int layer, float a, int storm) {
    uint32_t c = storm ? 0xD8E2F2u : 0xC8D4E6u;
    float sc = (float)F->scale, la = layer == FAR ? 0.26f : layer == MID ? 0.38f : 0.5f;
    if (storm) la += 0.06f;
    int width = layer == NEAR ? (F->scale >= 2 ? 2 : 1) : 1;
    for (int i = 0; i < ndrops; i++) {
        const Drop *d = &drops[i];
        if (d->layer != layer) continue;
        float hx, hy, tx, ty;
        to_out(layer, d->x, d->y, &hx, &hy);
        to_out(layer, d->x - wind * d->len, d->y - d->len, &tx, &ty);
        streak(tx, ty, hx, hy, c, la * a, width, MASK[layer]);
    }
    for (int i = 0; i < nsplashes; i++) {                   /* rings on the ground, two drops thrown up */
        const Splash *s = &splashes[i];
        if (s->layer != layer) continue;
        float u = s->age / 0.18f, cx, cy, k = layer == FAR ? 0.6f : 1.0f;
        to_out(layer, s->x, s->y, &cx, &cy);
        float rx = (1.0f + 5.0f * u) * k * sc, ry = rx * 0.28f, al = (1.0f - u) * 0.45f * a;
        for (int j = 0; j < 10; j++) {
            float t = (float)j * 0.6283185f;
            plot((int)(cx + cosf(t) * rx), (int)(cy + sinf(t) * ry), c, (int)(al * 256.0f), MASK[layer]);
        }
        float up = (u * 9.0f - u * u * 9.0f) * k * sc;
        plot((int)(cx - 2.0f * u * k * sc), (int)(cy - up), c, (int)(al * 300.0f), MASK[layer]);
        plot((int)(cx + 2.0f * u * k * sc), (int)(cy - up), c, (int)(al * 300.0f), MASK[layer]);
    }
}

static void draw_snow(int layer, float a) {
    static const float A[NL] = {0.62f, 0.82f, 0.92f};
    for (int i = 0; i < nflakes; i++) {
        const Flake *k = &flakes[i];
        if (k->layer != layer) continue;
        float x = k->x + sinf(k->phase) * k->sway, X, Y;
        to_out(layer, x, k->y, &X, &Y);
        float fadeout = 1.0f - smoothf(k->ground - 6.0f, k->ground, k->y);
        dot(X, Y, k->size * (float)F->scale * (1.0f + LF[layer] * (F->zoom - 1.0f)), 0xF4F8FFu, A[layer] * a * fadeout, MASK[layer]);
    }
}

/* clouds: a tileable noise, sampled bilinearly */
#define CN 256
static uint8_t cloud_tex[CN][CN];
static int cloud_ready;

static float lattice(int x, int y, int period, int seed) {
    x &= period - 1; y &= period - 1;
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float vnoise(float x, float y, int period, int seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - (float)xi, fy = y - (float)yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = lattice(xi, yi, period, seed), b = lattice(xi + 1, yi, period, seed);
    float c = lattice(xi, yi + 1, period, seed), d = lattice(xi + 1, yi + 1, period, seed);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static void make_clouds(void) {
    float lo = 1e9f, hi = -1e9f;
    static float v[CN][CN];
    for (int y = 0; y < CN; y++)
        for (int x = 0; x < CN; x++) {
            float n = 0.5f * vnoise((float)x / 64.0f, (float)y / 64.0f, 4, 1) + 0.27f * vnoise((float)x / 32.0f, (float)y / 32.0f, 8, 2)
                      + 0.15f * vnoise((float)x / 16.0f, (float)y / 16.0f, 16, 3) + 0.08f * vnoise((float)x / 8.0f, (float)y / 8.0f, 32, 4);
            v[y][x] = n;
            if (n < lo) lo = n;
            if (n > hi) hi = n;
        }
    for (int y = 0; y < CN; y++)
        for (int x = 0; x < CN; x++) cloud_tex[y][x] = (uint8_t)((v[y][x] - lo) / (hi - lo) * 255.0f);
    cloud_ready = 1;
}

/* a band of cloud at one depth: density from the noise (threshold th),
 * strongest between y0 and y1 (game pixels), drifting at `speed` */
#define CW 640                                              /* world columns: the view and its margins */
#define CX0 112                                             /* world x of column 0 is -CX0 */
static void draw_cloud_band(int layer, float y0, float y1, float th, float alpha, float speed, float su, float sv,
                            uint32_t col, int seed) {
    int ow = F->w * F->scale, oh = F->h * F->scale;
    float k = LF[layer], s = 1.0f + k * (F->zoom - 1.0f), cx = (float)F->w * 0.5f, cy = (float)F->h * 0.5f;
    static int col_w[448 * 4], row_w[240 * 4];
    static uint8_t dens[240][CW];                           /* the band's opacity, a world pixel each */
    int wx0 = CW, wx1 = -1;
    for (int X = 0; X < ow && X < 448 * 4; X++) {           /* the world column of each output column */
        float q = ((float)X + 0.5f) / (float)F->scale;
        int wx = (int)floorf(cx + (q - cx) / s + k * F->cam_x) + CX0;
        col_w[X] = wx < 0 ? 0 : wx >= CW ? CW - 1 : wx;
        if (col_w[X] < wx0) wx0 = col_w[X];
        if (col_w[X] > wx1) wx1 = col_w[X];
    }
    int th0 = (int)(th * 255.0f), th1 = th0 + 70;
    static uint8_t row_done[240];
    memset(row_done, 0, sizeof row_done);
    for (int Y = 0; Y < oh && Y < 240 * 4; Y++) {
        float q = ((float)Y + 0.5f) / (float)F->scale;
        int wy = (int)floorf(cy + (q - cy) / s + k * F->cam_y);
        row_w[Y] = wy;
        if (wy < 0 || wy >= 240 || row_done[wy]) continue;
        row_done[wy] = 1;                                     /* the noise for this world row, once */
        float prof = smoothf(y0 - 18.0f, y0 + 10.0f, (float)wy) * (1.0f - smoothf(y1 - 10.0f, y1 + 18.0f, (float)wy));
        uint8_t *dr = dens[wy];
        if (prof <= 0.002f) { memset(dr + wx0, 0, (size_t)(wx1 - wx0 + 1)); continue; }
        float v = (float)wy * sv + (float)seed * 37.0f;
        int vi = (int)floorf(v), vf = (int)((v - (float)vi) * 255.0f);
        const uint8_t *r0 = cloud_tex[vi & (CN - 1)], *r1 = cloud_tex[(vi + 1) & (CN - 1)];
        int pa = (int)(prof * alpha * fade * 256.0f);
        for (int wx = wx0; wx <= wx1; wx++) {
            float u = (float)(wx - CX0) * su + t_wx * speed + (float)seed * 61.0f;
            int ui = (int)floorf(u), fx = (int)((u - (float)ui) * 255.0f);
            int x0 = ui & (CN - 1), x1 = (ui + 1) & (CN - 1);
            int a = r0[x0] + (((int)r0[x1] - (int)r0[x0]) * fx >> 8);
            int b = r1[x0] + (((int)r1[x1] - (int)r1[x0]) * fx >> 8);
            int n = a + ((b - a) * vf >> 8);
            int d = n <= th0 ? 0 : n >= th1 ? 256 : ((n - th0) << 8) / (th1 - th0);
            d = d * d >> 8;                                    /* soft edges */
            int al = d * pa >> 8;
            dr[wx] = (uint8_t)(al > 255 ? 255 : al);
        }
    }
    int cr = (int)(col >> 16 & 255), cg = (int)(col >> 8 & 255), cb = (int)(col & 255);
    for (int Y = 0; Y < oh && Y < 240 * 4; Y++) {
        int wy = row_w[Y];
        if (wy < 0 || wy >= 240) continue;
        const uint8_t *dr = dens[wy];
        const uint8_t *fr = &F->front[(size_t)Y * (size_t)F->front_stride];
        uint32_t *dst = &F->out[(size_t)Y * (size_t)F->stride];
        for (int X = 0; X < ow; X++) {
            int al = dr[col_w[X]];
            if (!al || !(MASK[layer] >> fr[X] & 1)) continue;
            uint32_t sp = dst[X];
            int r = (int)(sp >> 16 & 255), g = (int)(sp >> 8 & 255), bb = (int)(sp & 255);
            r += ((cr - r) * al) >> 8;
            g += ((cg - g) * al) >> 8;
            bb += ((cb - bb) * al) >> 8;
            dst[X] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bb;
        }
    }
}

/* debris: a rough rock, turning, lit from the upper left, rimmed in the
 * colour of what lies beyond it */
static void draw_rock(const Rock *rk, float a) {
    static const int TONE[4][3] = {{104, 96, 90}, {92, 90, 96}, {116, 104, 92}, {82, 80, 84}};
    float X, Y, s = (1.0f + LF[rk->layer] * (F->zoom - 1.0f)) * (float)F->scale;
    to_out(rk->layer, rk->x, rk->y, &X, &Y);
    float R = rk->r * s;
    int x0 = (int)floorf(X - R - 1), x1 = (int)ceilf(X + R + 1), y0 = (int)floorf(Y - R - 1), y1 = (int)ceilf(Y + R + 1);
    float ca = cosf(-rk->ang), sa = sinf(-rk->ang);
    float haze = rk->layer == FAR ? 0.5f : rk->layer == MID ? 0.2f : 0.0f;
    const int *tone = TONE[rk->tone & 3];
    int sky[3] = {F->sky_r, F->sky_g, F->sky_b};
    for (int py = y0; py <= y1; py++)
        for (int px = x0; px <= x1; px++) {
            float dx = (float)px + 0.5f - X, dy = (float)py + 0.5f - Y, rho = sqrtf(dx * dx + dy * dy);
            if (rho > R + 1.0f) continue;
            float lx = dx * ca - dy * sa, ly = dx * sa + dy * ca;   /* in the rock's own frame */
            float th = atan2f(ly, lx) / 6.2831853f;
            if (th < 0) th += 1.0f;
            float f = th * (float)rk->nv;
            int i0 = (int)f % rk->nv, i1 = (i0 + 1) % rk->nv;
            float fr = f - floorf(f), edge = (rk->rad[i0] + (rk->rad[i1] - rk->rad[i0]) * fr) * R;
            if (rho > edge + 0.5f) continue;
            float cover = clampf(edge + 0.5f - rho, 0.0f, 1.0f);   /* anti-aliased rim */
            float nx = dx / (edge + 0.001f), ny = dy / (edge + 0.001f);
            float lit = clampf(-0.62f * nx - 0.78f * ny, -1.0f, 1.0f);
            float tex = (float)cloud_tex[((int)(lx / s * 4.0f) + rk->tone * 50) & (CN - 1)][((int)(ly / s * 4.0f)) & (CN - 1)] / 255.0f;
            float sh = 0.42f + 0.38f * lit + 0.28f * (tex - 0.5f);
            float rr = rho / (edge + 0.001f);
            if (rr > 0.86f) sh *= 0.72f;                        /* a dark rim */
            int c[3];
            static const float RIM[3] = {235.0f, 222.0f, 200.0f};       /* a far sun, warm */
            for (int j = 0; j < 3; j++) {
                float v = (float)tone[j] * sh * 1.4f;
                if (rr > 0.66f && lit > 0.3f) {                         /* rim light: the sun and the sky */
                    float k = (lit - 0.3f) * (rr - 0.66f) * 4.2f;
                    v += (RIM[j] * 0.7f + (float)sky[j] * 0.3f - v) * clampf(k, 0.0f, 0.85f);
                }
                v = v + ((float)sky[j] * 0.8f - v) * haze;
                c[j] = (int)clampf(v, 0.0f, 255.0f);
            }
            plot(px, py, (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2], (int)(cover * a * 256.0f), MASK[rk->layer]);
        }
}

static void make_bolt(int w) {
    float x = (float)w * (0.15f + 0.7f * frand()), y = -8.0f, end = 64.0f + 30.0f * frand();
    bolt_n = 0;
    while (y < end && bolt_n < 64) {
        bolt_x[bolt_n] = x;
        bolt_y[bolt_n++] = y;
        y += 5.0f + 6.0f * frand();
        x += (frand() - 0.5f) * 12.0f;
    }
    bolt_branch_at = bolt_n > 6 ? 2 + (int)(frand() * (float)(bolt_n - 5)) : -1;
    bolt_branch_n = 0;
    if (bolt_branch_at >= 0) {
        float bx = bolt_x[bolt_branch_at], by = bolt_y[bolt_branch_at], dir = frand() < 0.5f ? -1.0f : 1.0f;
        for (int i = 0; i < 16 && by < end + 8.0f; i++) {
            branch_x[bolt_branch_n] = bx;
            branch_y[bolt_branch_n++] = by;
            by += 4.0f + 5.0f * frand();
            bx += dir * (3.0f + 6.0f * frand());
        }
    }
}

static void draw_bolt_path(const float *xs, const float *ys, int n, float a, float glow) {
    for (int i = 0; i + 1 < n; i++) {
        float x0, y0, x1, y1;
        to_out(FAR, xs[i], ys[i], &x0, &y0);
        to_out(FAR, xs[i + 1], ys[i + 1], &x1, &y1);
        float dx = x1 - x0, dy = y1 - y0, L = fmaxf(fabsf(dx), fabsf(dy));
        int steps = (int)L + 1, gw = (int)(glow * (float)F->scale);
        for (int k = 0; k <= steps; k++) {
            float u = (float)k / (float)steps;
            int X = (int)(x0 + dx * u), Y = (int)(y0 + dy * u);
            for (int g = -gw; g <= gw; g++) {
                float fall = 1.0f - (float)abs(g) / (float)(gw + 1);
                int v = (int)(a * 150.0f * fall * fall);
                add_px(X + g, Y, v, v, v + 25 * (v > 0), MASK[FAR]);
            }
            for (int c = 0; c < (F->scale >= 2 ? 2 : 1); c++) plot(X + c, Y, 0xFFFFFFu, (int)(a * 256.0f), MASK[FAR]);
        }
    }
}

/* the storm's darkness and the lightning's flash over everything but the
 * windows: applied by depth.c while it composes (this frame's, from the last
 * update: a frame late, which no one sees) */
int weather_tint(int *mr, int *mg, int *mb, int *ar, int *ag, int *ab) {
    if (kind == WX_NONE) return 0;
    float a = fade, fl = flash * (level >= 2 ? 0.7f : 0.5f) * a;
    if (dark_r > 0.995f && fl <= 0.01f) return 0;
    *mr = (int)((1.0f + (dark_r - 1.0f) * a) * 256.0f);
    *mg = (int)((1.0f + (dark_g - 1.0f) * a) * 256.0f);
    *mb = (int)((1.0f + (dark_b - 1.0f) * a) * 256.0f);
    *ar = (int)(105.0f * fl);
    *ag = (int)(112.0f * fl);
    *ab = (int)(140.0f * fl);
    return 1;
}

static void update(float dt, int w, int h) {
    t_wx += dt;
    fade = smoothf(0.0f, 1.2f, t_wx);
    if (kind == WX_STORM)
        wind = 0.3f + 0.1f * sinf(t_wx * 0.6f) + 0.07f * sinf(t_wx * 1.7f + 1.0f);
    else
        wind = 0.12f + 0.04f * sinf(t_wx * 0.4f);
    for (int i = 0; i < ndrops; i++) {
        Drop *d = &drops[i];
        d->y += d->v * dt;
        d->x += wind * d->v * dt;
        if (d->y >= d->ground) {
            if (d->layer != NEAR && nsplashes < MAX_SPLASHES && frand() < 0.7f) {
                Splash *s = &splashes[nsplashes++];
                s->x = d->x; s->y = d->ground; s->age = 0.0f; s->layer = d->layer;
            }
            drop_spawn(d, w, 0);
        }
    }
    for (int i = 0; i < nsplashes; i++) {
        splashes[i].age += dt;
        if (splashes[i].age >= 0.18f) splashes[i--] = splashes[--nsplashes];
    }
    for (int i = 0; i < nflakes; i++) {
        Flake *k = &flakes[i];
        k->y += k->v * dt;
        k->x += 6.0f * dt;
        k->phase += k->freq * 6.2831853f * dt;
        if (k->y >= k->ground) flake_spawn(k, w, 0);
    }
    for (int i = 0; i < nrocks; i++) {
        Rock *r = &rocks[i];
        r->x += r->vx * dt;
        r->y += r->vy * dt;
        r->ang += r->spin * dt;
        if (r->x < -MARGIN - 2 * r->r - 1 || r->x > (float)w + MARGIN + 2 * r->r + 1 || r->y < -30 || r->y > (float)h + 30)
            rock_spawn(r, w, h, 0);
    }
    /* lightning: a strike now and then, two quick flashes and a fade */
    since_strike += dt;
    if (kind == WX_STORM) {
        next_strike -= dt;
        if (next_strike <= 0.0f) {
            next_strike = (level >= 2 ? 2.2f : 3.5f) + (level >= 2 ? 3.3f : 4.5f) * frand();
            since_strike = 0.0f;
            make_bolt(w);
            float delay = 0.3f + 1.5f * frand();
            thunder_due_in = delay;
            thunder_due_variant = delay < 0.8f ? (frand() < 0.5f ? 0 : 1) : (frand() < 0.5f ? 2 : 3);
            thunder_gain = delay < 0.8f ? 1.15f : 0.75f;
        }
    }
    float t = since_strike;
    flash = t < 0.05f ? 1.0f : t < 0.1f ? 0.25f : t < 0.16f ? 0.85f : 0.85f * expf(-(t - 0.16f) / 0.18f);
    if (t > 2.0f) flash = 0.0f;
    float tr = 1, tg = 1, tb = 1;
    if (kind == WX_STORM) { tr = 0.7f; tg = 0.73f; tb = 0.85f; }
    else if (kind == WX_RAIN) { tr = 0.84f; tg = 0.86f; tb = 0.94f; }
    float k = 1.0f - expf(-dt / 0.6f);
    dark_r += (tr - dark_r) * k; dark_g += (tg - dark_g) * k; dark_b += (tb - dark_b) * k;
    /* the rain heard */
    rain_target = !sound_on ? 0.0f : kind == WX_STORM ? 0.3f : kind == WX_RAIN ? 0.17f : 0.0f;
    rain_target *= fade * (level >= 2 ? 1.25f : 1.0f);
}

void weather_draw(const WeatherFrame *f) {
    if (kind == WX_NONE) return;
    F = f;
    if (!cloud_ready) make_clouds();
    update(f->dt, f->w, f->h);
    float a = fade;
    int storm = kind == WX_STORM;
    /* (darker skies and the flash: weather_tint, applied as depth.c composes) */
    /* far: behind the enemies */
    if (storm && flash > 0.2f) {
        draw_bolt_path(bolt_x, bolt_y, bolt_n, flash * a, 2.5f);
        if (bolt_branch_n > 1) draw_bolt_path(branch_x, branch_y, bolt_branch_n, flash * a * 0.6f, 1.5f);
    }
    uint32_t mist = (uint32_t)clampf((float)f->sky_r * 0.3f + 172.0f, 0, 255) << 16
                  | (uint32_t)clampf((float)f->sky_g * 0.3f + 174.0f, 0, 255) << 8
                  | (uint32_t)clampf((float)f->sky_b * 0.3f + 184.0f, 0, 255);
    if (kind == WX_CLOUDS)
        draw_cloud_band(FAR, 8.0f, 108.0f, 0.34f, level >= 2 ? 0.72f : 0.6f, 10.0f, 0.5f, 1.0f, mist, 1);
    if (storm) {                                              /* low storm clouds racing over the sky */
        uint32_t dark = (uint32_t)clampf((float)f->sky_r * 0.25f + 52.0f, 0, 255) << 16
                      | (uint32_t)clampf((float)f->sky_g * 0.25f + 56.0f, 0, 255) << 8
                      | (uint32_t)clampf((float)f->sky_b * 0.25f + 68.0f, 0, 255);
        draw_cloud_band(FAR, -20.0f, 46.0f, 0.38f, 0.6f, 22.0f, 0.5f, 1.3f, dark, 5);
    }
    if (kind == WX_DEBRIS) {
        for (int i = 0; i < nstars; i++) {
            const Star *s = &stars[i];
            float X, Y, tw = 0.55f + 0.45f * sinf(s->ph + t_wx * s->rate * 6.2831853f);
            to_out(FAR, s->x, s->y, &X, &Y);
            dot(X, Y, (s->big ? 0.9f : 0.5f) * (float)f->scale, 0xFFFFF4u, tw * a * (s->big ? 0.95f : 0.7f), MASK[FAR]);
        }
        for (int i = 0; i < nrocks; i++)
            if (rocks[i].layer == FAR) draw_rock(&rocks[i], a);
    }
    if (kind == WX_RAIN || storm) draw_rain(FAR, a, storm);
    if (kind == WX_SNOW) draw_snow(FAR, a);
    /* mid: between the enemies and the party */
    if (kind == WX_CLOUDS)
        draw_cloud_band(MID, 92.0f, 182.0f, 0.4f, level >= 2 ? 0.75f : 0.62f, 24.0f, 0.4f, 0.85f, mist, 3);
    if (kind == WX_DEBRIS)
        for (int i = 0; i < nrocks; i++)
            if (rocks[i].layer == MID) draw_rock(&rocks[i], a);
    if (kind == WX_RAIN || storm) draw_rain(MID, a, storm);
    if (kind == WX_SNOW) draw_snow(MID, a);
    /* near: in front of everyone (the windows stay clear) */
    if (kind == WX_DEBRIS)
        for (int i = 0; i < nrocks; i++)
            if (rocks[i].layer == NEAR) draw_rock(&rocks[i], a);
    if (kind == WX_RAIN || storm) draw_rain(NEAR, a, storm);
    if (kind == WX_SNOW) draw_snow(NEAR, a);
}

/* ------------------------------------------------------------------ sound */
static int16_t *rain_pcm, *thunder_pcm[4];
static int rain_frames, thunder_frames[4], sound_ready;
static float rain_gain;
static double rain_pos;
typedef struct { int v, pos; float gain; } Voice;
static Voice voices[2] = {{-1, 0, 0.0f}, {-1, 0, 0.0f}};

static void sound_init(void) {
    if (sound_ready) return;
    sound_ready = 1;
    rain_pcm = sfxsynth_render("rain", 0, &rain_frames);
    for (int i = 0; i < 4; i++) thunder_pcm[i] = sfxsynth_render("thunder", i, &thunder_frames[i]);
}

int weather_sound_on(void) {
    return sound_on && sound_ready && (rain_gain > 0.0005f || rain_target > 0.0f || voices[0].v >= 0
                                       || voices[1].v >= 0 || thunder_due_variant >= 0);
}

void weather_mix(float *out, int frames) {
    if (!sound_on || !sound_ready) return;
    float dt = (float)frames / (float)SOUND_RATE;
    if (thunder_due_variant >= 0) {
        thunder_due_in -= dt;
        if (thunder_due_in <= 0.0f) {
            Voice *v = voices[0].v < 0 ? &voices[0] : voices[1].v < 0 ? &voices[1]
                      : voices[0].pos > voices[1].pos ? &voices[0] : &voices[1];
            v->v = thunder_due_variant;
            v->pos = 0;
            v->gain = thunder_gain;
            thunder_due_variant = -1;
        }
    }
    /* the rain: a loop, its seam cross-faded over a quarter second */
    if (rain_pcm && rain_frames > SOUND_RATE) {
        int xf = SOUND_RATE / 4, n = rain_frames;
        float step = (rain_target - rain_gain) / (float)frames;
        if (fabsf(step) > 0.6f / (float)SOUND_RATE) step = step > 0 ? 0.6f / (float)SOUND_RATE : -0.6f / (float)SOUND_RATE;
        for (int i = 0; i < frames; i++) {
            rain_gain += step;
            if (rain_gain < 0.0f) rain_gain = 0.0f;
            if (rain_gain <= 0.0f && rain_target <= 0.0f) continue;
            int p = (int)rain_pos;
            float l = rain_pcm[2 * p], r = rain_pcm[2 * p + 1];
            if (p >= n - xf) {                                 /* the end fading into the start */
                int q = p - (n - xf);
                float u = (float)q / (float)xf;
                l = l * (1.0f - u) + rain_pcm[2 * q] * u;
                r = r * (1.0f - u) + rain_pcm[2 * q + 1] * u;
            }
            out[2 * i] += l * rain_gain;
            out[2 * i + 1] += r * rain_gain;
            rain_pos += 1.0;
            if (rain_pos >= (double)n) rain_pos = (double)xf;
        }
    }
    for (int k = 0; k < 2; k++) {
        Voice *v = &voices[k];
        if (v->v < 0 || !thunder_pcm[v->v]) { v->v = -1; continue; }
        const int16_t *pcm = thunder_pcm[v->v];
        int n = thunder_frames[v->v];
        for (int i = 0; i < frames && v->pos < n; i++, v->pos++) {
            out[2 * i] += pcm[2 * v->pos] * v->gain;
            out[2 * i + 1] += pcm[2 * v->pos + 1] * v->gain;
        }
        if (v->pos >= n) v->v = -1;
    }
}
