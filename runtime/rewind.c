/*
 * Rewind: see rewind.h.
 */
#include "rewind.h"
#include "algol.h"
#include "battleui.h"
#include "bus.h"
#include "coro.h"
#include "frame.h"
#include "m68k.h"
#include "rumble.h"
#include "sfx.h"
#include "battlevox.h"
#include "scenevox.h"
#include "sound.h"
#include "vdp.h"
#include "walkto.h"
#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_STACK  (4u << 20)          /* the game's own stack; it uses a few kilobytes */
#define EVERY       600                 /* logical frames between states: ten seconds */
#define KEEP        12                  /* the last two minutes */
#define MIN_BACK    180                 /* a rewind goes back three seconds at least */
#define FADE_OUT    0.35f               /* seconds to white */
#define HOLD        4                   /* frames held white, the state put back */
#define FADE_IN     0.65f               /* seconds back from white */
#define NOTE_FRAMES 150                 /* how long "N seconds back" stays */

static CoroStack stack;
static void *host_sp, *game_sp;
static int running, enabled = 1;
static void (*game_entry)(void);
static const uint32_t *show_pixels;
static int show_w, show_h;
static uint64_t game_frame;             /* the game's logical frames: they go back with it */
static uint64_t next_keep = EVERY;
void (*rewind_on_restore)(uint64_t game_frame);

typedef struct {
    uint64_t frame;
    uint8_t *sp;                        /* the game's stack pointer then */
    size_t stack_len, machine_len;
    uint8_t *data;                      /* the stack from sp up, then the machine */
} State;
static State states[KEEP];
static int nstates;                     /* oldest first */

enum { IDLE, OUT, HELD, IN };
static int phase, held, target = -1, note_frames, note_secs;
static float white;

void rewind_set_enabled(int on) { enabled = on; }
int  rewind_enabled(void) { return enabled && running; }
uint64_t rewind_game_frame(void) { return game_frame; }
float rewind_white(void) { return white; }
float rewind_volume(void) { return 1.0f - white; }
int  rewind_note_seconds(void) { return note_frames > 0 ? note_secs : 0; }

/* ------------------------------------------------------------------ the machine */
#define MACHINE_FIELDS(F) F(m68k) F(m68k_last_ret) F(m68k_ram) F(vdp) F(frame_pending_lag)

static size_t machine_save(uint8_t *out) {
    size_t n = 0;
#define SAVE(x) if (out) memcpy(out + n, &(x), sizeof(x)); n += sizeof(x);
    MACHINE_FIELDS(SAVE)
#undef SAVE
    n += bus_state_save(out ? out + n : NULL);
    n += sound_state_save(out ? out + n : NULL);
    n += battleui_state_save(out ? out + n : NULL);
    return n;
}

static void machine_load(const uint8_t *in) {
    size_t n = 0;
#define LOAD(x) memcpy(&(x), in + n, sizeof(x)); n += sizeof(x);
    MACHINE_FIELDS(LOAD)
#undef LOAD
    n += bus_state_load(in + n);
    n += sound_state_load(in + n);
    n += battleui_state_load(in + n);
    /* the port's own sound and gestures start over from here */
    algol_reset();
    sfx_stop_all();
    battlevox_stop();
    scenevox_stop();
    rumble_stop();
    walkto_cancel();
    weather_battle_end();
}

/* a state, while the game waits at its frame (its stack pointer in game_sp) */
static void keep_state(void) {
    uint8_t *sp = (uint8_t *)game_sp;
    size_t slen = (size_t)(stack.base + stack.size - sp), mlen = machine_save(NULL);
    uint8_t *data = malloc(slen + mlen);
    if (!data) return;
    if (nstates == KEEP) {
        free(states[0].data);
        memmove(states, states + 1, (KEEP - 1) * sizeof states[0]);
        nstates--;
    }
    memcpy(data, sp, slen);
    machine_save(data + slen);
    State *s = &states[nstates++];
    s->frame = game_frame;
    s->sp = sp;
    s->stack_len = slen;
    s->machine_len = mlen;
    s->data = data;
}

static void put_back(int i) {
    State *s = &states[i];
    memcpy(s->sp, s->data, s->stack_len);           /* the game's call chain, as it was */
    game_sp = s->sp;
    machine_load(s->data + s->stack_len);
    note_secs = (int)((game_frame - s->frame + 30) / 60);
    note_frames = NOTE_FRAMES;
    game_frame = s->frame;
    for (int j = i + 1; j < nstates; j++) free(states[j].data);   /* a future that won't be */
    nstates = i + 1;
    next_keep = game_frame + EVERY;
    if (rewind_on_restore) rewind_on_restore(game_frame);
}

static int pick(void) {                             /* the newest state three seconds old */
    int i = nstates - 1;
    while (i >= 0 && game_frame < states[i].frame + MIN_BACK) i--;
    return i;
}

void rewind_keep_now(void) {
    if (running) keep_state();
}

int rewind_restore_frame(uint64_t frame) {
    for (int i = 0; i < nstates; i++)
        if (states[i].frame == frame) {
            put_back(i);
            return 1;
        }
    return 0;
}

int rewind_request(void) {
    if (!enabled || !running) return 0;
    if (phase == OUT || phase == HELD) return 1;    /* on its way already */
    int i = pick();
    if (i < 0) return 0;
    target = i;
    phase = OUT;
    return 1;
}

/* ------------------------------------------------------------------ each frame */
static void after_frame(void) {
    if (note_frames > 0) note_frames--;
    switch (phase) {
    case OUT:
        white += 1.0f / (FADE_OUT * 60.0f);
        if (white >= 1.0f) {
            white = 1.0f;
            if (target >= 0 && target < nstates) put_back(target);
            target = -1;
            phase = HELD;
            held = HOLD;
        }
        break;
    case HELD:
        if (--held <= 0) phase = IN;
        break;
    case IN:
        white -= 1.0f / (FADE_IN * 60.0f);
        if (white <= 0.0f) { white = 0.0f; phase = IDLE; }
        break;
    default:
        break;
    }
    if (enabled && phase == IDLE && game_frame >= next_keep) {
        keep_state();
        next_keep = game_frame + EVERY;
    }
}

/* ------------------------------------------------------------------ the two stacks */
static void game_main(void *arg) {
    (void)arg;
    game_entry();                                   /* the console reset: the game never returns */
    for (;;) game_show_frame(frame_pixels, 0, 0);
}

void game_show_frame(const uint32_t *pixels, int width, int height) {
    if (frame_vint_will_fire()) game_frame++;
    if (!running) {
        platform_frame(pixels, width, height);
        return;
    }
    show_pixels = pixels;
    show_w = width;
    show_h = height;
    coro_switch(&game_sp, host_sp);                 /* the frame is shown from the program's stack */
}

void game_run(void (*entry)(void)) {
    game_entry = entry;
    if (!coro_stack_new(&stack, GAME_STACK)) {      /* no memory for it: no rewind */
        fprintf(stderr, "rewind: no stack for the game; rewind is off\n");
        entry();
        return;
    }
    game_sp = coro_prepare(&stack, game_main, NULL);
    running = 1;
    for (;;) {
        coro_switch(&host_sp, game_sp);             /* the game, until it has a frame to show */
        platform_frame(show_pixels, show_w, show_h);
        after_frame();
    }
}
