/*
 * Battle weather (M7): rain, storms, snow, mist clouds and drifting debris,
 * drawn by battle depth (depth.c) at three depths -- behind the enemies,
 * between the enemies and the party, and in front of everyone -- under its
 * camera, so the near rain moves more than the far.
 *
 * The weather goes by where the battle is: the game picks one of 32
 * backgrounds for it (Battle_SetupBackground), and the port works out the
 * same one. Motavia's grassland, beach and sea get rain or a storm in some
 * battles (lightning, darker skies, thunder) and clear skies in others;
 * Dezolis gets snow; Rykros, its towers and the carnivorous trees get mist
 * clouds; the Air Castle and the Edge get debris floating in space; caves,
 * plants and towers stay dry. Rain and thunder are heard too, synthesized
 * like the port's sound effects (sfxsynth.c). Display and sound only: the
 * game's state is never touched.
 */
#ifndef PSIV_WEATHER_H
#define PSIV_WEATHER_H

#include <stdint.h>

enum { WX_NONE, WX_RAIN, WX_STORM, WX_SNOW, WX_CLOUDS, WX_DEBRIS, WX_KINDS };

#define WEATHER_LEVELS 3                 /* off, light, heavy */
void weather_set_level(int level);
int  weather_get_level(void);
const char *weather_level_name(int level);
/* tests: this weather in every battle (WX_*), or -1 for the battle's own */
void weather_force(int kind);
int  weather_kind_by_name(const char *name);   /* "rain", "storm", ...; -1 if none */
const char *weather_name(int kind);
void weather_set_sound(int on);                /* rain and thunder heard (default on) */

/* what the output shows nearest the eye, per pixel (depth.c fills it) */
enum { WX_FRONT_BG = 0, WX_FRONT_PARTY = 1, WX_FRONT_PARTY_UNDER = 2, WX_FRONT_UI = 3, WX_FRONT_ENEMY = 4 };

typedef struct {
    uint32_t *out;                       /* the composed picture, 0xAARRGGBB */
    int stride;                          /* its pixels a row */
    const uint8_t *front;                /* WX_FRONT_* per output pixel */
    int front_stride;
    int w, h, scale;                     /* game pixels; output pixels per game pixel */
    float cam_x, cam_y, zoom;            /* depth's camera (cam at the party's depth) */
    float dt;                            /* seconds this frame */
    int sky_r, sky_g, sky_b;             /* the background's average colour */
} WeatherFrame;

/* depth.c, while a battle is shown: whether there is weather to draw (the
 * first call of a battle picks it), then the drawing over the composed frame */
int  weather_active(void);
/* the storm's darker light and the lightning's flash, for depth.c to apply
 * as it composes (out = in * m / 256 + add, per channel; the windows stay
 * as they are): 1 if there is any */
int  weather_tint(int *mr, int *mg, int *mb, int *ar, int *ag, int *ab);
void weather_draw(const WeatherFrame *f);
void weather_battle_end(void);           /* out of the battle: the weather clears */

/* the frame loop: rain and thunder into a frame of audio (interleaved stereo
 * float at 16-bit scale, SOUND_RATE), and whether any is playing */
void weather_mix(float *samples, int frames);
int  weather_sound_on(void);

#endif
