/*
 * Frame loop.
 *
 * The game's main loop ends each frame in WaitForVInt: it sets VInt_Flag and
 * spins until the vertical-blank handler clears it. The recompiled spin calls
 * psiv_wait_vblank() (a hook placed by tools/emit.py), which plays out the
 * rest of the frame the way the hardware would:
 *
 *   1. draw lines 0..height-1, firing the horizontal interrupt each time the
 *      VDP's line counter (register 10) runs out, so raster effects land on
 *      the right lines
 *   2. hand the picture to the front end, which also reads input
 *   3. enter vertical blank and run the game's VInt handler (it uploads the
 *      next frame's sprites, palette and scroll by DMA, reads the pads, runs
 *      the sound driver, and clears VInt_Flag so the spin exits)
 */
#include "frame.h"
#include "battleui.h"
#include "fieldmenu.h"
#include "bus.h"
#include "m68k.h"
#include "mixbus.h"
#include "music.h"
#include "algol.h"
#include "rumble.h"
#include "sfx.h"
#include "rewind.h"
#include "sound.h"
#include "weather.h"
#include "battlevox.h"
#include "scenevox.h"
#include "cheats.h"
#include "widemap.h"
#include "titleart.h"
#include "perf.h"

#include <string.h>

#define VINT_FLAG 0xEF12u          /* VInt_Flag = $FFFFEF12 */
#define SOUND_QUEUE 0x500Au        /* Sound_Index and the 3 queue slots after it */
#define SPIN_PC   0x42076u         /* where the interrupted code resumes */

uint32_t frame_pixels[VDP_MAX_W * VDP_MAX_H];
uint64_t frame_count;
uint64_t frame_logical;
int frame_pending_lag;
VdpLine frame_lines[VDP_MAX_H];
int frame_wide_margin;
int frame_wide_field = 1;          /* 0: the field keeps its 320 view (side panels) */

/* Widescreen draws in the field and in battles (and the fades into them):
 * the planes hold more of the map than the 320-pixel view, and battles get
 * their background mirrored into the margins (depth.c). Screens built for
 * exactly 320 pixels (title, intro, menus) stay as they are, pillarboxed. */
#define GAME_MODE_INDEX 0xEF00u
static int wide_allowed(void) {
    int mode = (m68k_ram[GAME_MODE_INDEX] << 8 | m68k_ram[GAME_MODE_INDEX + 1]) / 4;
    if (mode == 2 || mode == 3) return frame_wide_field;   /* LoadFieldMap, Field */
    return mode == 4 || mode == 5;                          /* LoadBattle, Battle */
}

/* Widescreen and objects (see IMM_OVERRIDES in tools/emit.py): the game's
 * sprite builders keep a piece if its x + 128 - base is below the limit:
 * screen x -63 to 319. While frames are drawn wide they keep x -96 to 383
 * (x + 128 from $20 to $1FF, the VDP's 9 bits), as long as the sprite table
 * has room (d6 is the next entry). */
uint32_t psiv_sprite_xbase(void) {
    return vdp_view_x0 >= 0 ? 0x41 : 0x20;
}

uint32_t psiv_sprite_xlimit(uint32_t d6) {
    if (vdp_view_x0 >= 0) return 0x17F;
    return (d6 & 0xFF) < 80 ? 0x1E0 : 0;
}

static void frame_sound(void);

void frame_run(void) {
    cheats_frame();                    /* Settings > Cheats (all off unless chosen) */
    int h = vdp_height(), mw = vdp_width();
    int m = frame_wide_margin > VDP_WIDE_MAX ? VDP_WIDE_MAX : frame_wide_margin;
    int wide = m > 0 && mw == 320 && wide_allowed();
    vdp_view_x0 = wide ? -m : 0;
    vdp_view_w = wide ? mw + 2 * m : mw;
    int w = vdp_view_w;
    int counter = vdp.reg[10];
    widemap_frame(wide);               /* field margins drawn from the map in RAM */
    battleui_frame();                  /* modern battle menus: the game's windows left out of the picture */
    fieldmenu_frame();                 /* the field menu, redrawn: the same for the camp menu's windows */

    vdp.in_vblank = 0;
    perf_begin(PERF_VDP);
    for (int y = 0; y < h; y++) {
        vdp.line = y;
        /* the line counter runs out at the end of the line before, and what
         * the handler writes (vertical scroll: Gra's wave, HInt2's stretch)
         * shows on this line already: Genesis Plus GX (BizHawk) draws Gra's
         * wave so, to the pixel, where the port had it a line late */
        if (counter-- == 0) {
            counter = vdp.reg[10];
            if (vdp_hint_enabled()) m68k_interrupt(4, SPIN_PC);
        }
        vdp_render_line(y, &frame_lines[y]);
        vdp_compose_line(&frame_lines[y], &frame_pixels[y * VDP_MAX_W], w);
    }

    perf_end(PERF_VDP);
    titleart_frame(frame_pixels, w, h);                /* the port's IV and RECOMP beside the title's logo */
    scenevox_frame(frame_lines, w, h);                 /* a voiced page of dialogue (reads only) */
    frame_logical += (uint64_t)frame_vint_will_fire();
    game_show_frame(frame_pixels, w, h);           /* platform_frame, from the program's own stack (rewind.h) */
    frame_count++;

    vdp.in_vblank = 1;
    vdp.line = h;
    rumble_frame();                    /* hits and sound requests, for the rumble (reads only) */
    battlevox_frame();                 /* the party's voices (reads only) */
    /* replaced sound effects: the game's requests wait in the sound queue
     * ($FF500A-$FF500D) for the driver, which runs in VInt; a replaced one
     * starts its sample and is taken out of the queue */
    if (sfx_enabled())
        for (int i = 0; i < 4; i++) {
            uint8_t id = m68k_ram[SOUND_QUEUE + i];
            if (id && sfx_request(id)) m68k_ram[SOUND_QUEUE + i] = 0;
        }
    if (vdp_vint_enabled()) m68k_interrupt(6, SPIN_PC);
    rumble_after_vint();
    vdp.in_vblank = 0;

    /* lag VInts scheduled by a reference replay: they belong at the start of
     * the main code's next frame of work */
    while (frame_pending_lag > 0) {
        frame_pending_lag--;
        frame_lag_vint();
    }

    frame_sound();
}

/* The frame's sound: the Z80, YM2612 and PSG play out the rest of the
 * frame; then any replacement sound effects; the mix bus makes the 16-bit
 * samples, with its limiter on while the port's own sound is in the mix */
static void frame_sound(void) {
    perf_begin(PERF_SOUND);
    const float *chips;
    int n = sound_end_frame(&chips);
    static float mix[2 * 8192], part[2 * 8192];
    static int16_t out[2 * 8192];
    if (n > 8192) n = 8192;
    memcpy(mix, chips, (size_t)n * 2 * sizeof(float));
    /* the port's own sounds, each kind at its level (Settings > Sound,
     * mixbus.h): mixed apart and added in turned down, or at 1 straight into
     * the mix as always. Turned all the way down they still play on, unheard */
    float ge = mixbus_effects(), gv = mixbus_voices();
    int weather_heard = weather_sound_on();
    if (sfx_enabled() || weather_heard) {
        float *to = ge == 1.0f ? mix : part;
        if (to == part) memset(part, 0, (size_t)n * 2 * sizeof(float));
        if (sfx_enabled()) sfx_mix(to, n);
        if (weather_heard) weather_mix(to, n);             /* battle rain and thunder */
        if (to == part) for (int i = 0; i < n * 2; i++) mix[i] += part[i] * ge;
    }
    int voices = battlevox_sounding() || scenevox_sounding();
    if (voices) {                                          /* the party's voices, voiced scenes */
        float *to = gv == 1.0f ? mix : part;
        if (to == part) memset(part, 0, (size_t)n * 2 * sizeof(float));
        battlevox_mix(to, n);
        scenevox_mix(to, n);
        if (to == part) for (int i = 0; i < n * 2; i++) mix[i] += part[i] * gv;
    }
    mixbus_set_limiter(sfx_enabled() || algol_enabled() || music_enabled() || weather_heard || voices);
    mixbus_finish(mix, out, n);
    perf_end(PERF_SOUND);
    platform_audio(out, n);
}

/* The sound driver alone (frame.h). InitSoundDriver ($D0004) stops the Z80,
 * gives it the DAC program and silences the chips; UpdateSound ($D0000) is
 * the tick the game's VInt runs. Neither waits for anything, so they can
 * be called with no game running: RAM clear, the stack where the reset
 * vector puts it, interrupts off -- as at the game's own first call. */
#define INIT_SOUND_DRIVER 0xD0004u
#define UPDATE_SOUND      0xD0000u

static void driver_call(uint32_t routine) { CALL_IND(routine, 0u); }

void frame_song_start(int id) {
    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    sound_busreq_write(0x100);         /* as MainGameProgram: the Z80 stopped and out of reset, */
    sound_reset_write(0x100);          /* or the driver waits for ever for it to stop */
    driver_call(INIT_SOUND_DRIVER);
    m68k_ram[SOUND_QUEUE] = (uint8_t)id;
}

void frame_song_tick(void) {
    driver_call(UPDATE_SOUND);
    frame_sound();
}

int frame_vint_will_fire(void) {
    return vdp_vint_enabled() && m68k.imask < 6;
}

void frame_lag_vint(void) {
    /* a VInt the original takes while the game is still busy (VInt_Flag
     * clear): the handler skips to VInt_Skip and only runs the sound driver */
    uint8_t flag = m68k_ram[VINT_FLAG];
    m68k_ram[VINT_FLAG] = 0;
    vdp.in_vblank = 1;
    if (frame_vint_will_fire()) m68k_interrupt(6, SPIN_PC);
    vdp.in_vblank = 0;
    m68k_ram[VINT_FLAG] = flag;
}

void psiv_wait_vblank(void) {
    if (m68k_ram[VINT_FLAG]) frame_run();
}

void m68k_stop(uint32_t sr) {
    /* `stop` waits for an interrupt: the next one is the end of this frame */
    m68k_set_sr(sr);
    frame_run();
}
