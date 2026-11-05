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
#include "bus.h"
#include "m68k.h"
#include "rumble.h"
#include "sfx.h"
#include "sound.h"
#include "widemap.h"

#include <string.h>

#define VINT_FLAG 0xEF12u          /* VInt_Flag = $FFFFEF12 */
#define SOUND_QUEUE 0x500Au        /* Sound_Index and the 3 queue slots after it */
#define SPIN_PC   0x42076u         /* where the interrupted code resumes */

uint32_t frame_pixels[VDP_MAX_W * VDP_MAX_H];
uint64_t frame_count;
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

void frame_run(void) {
    int h = vdp_height(), mw = vdp_width();
    int m = frame_wide_margin > VDP_WIDE_MAX ? VDP_WIDE_MAX : frame_wide_margin;
    int wide = m > 0 && mw == 320 && wide_allowed();
    vdp_view_x0 = wide ? -m : 0;
    vdp_view_w = wide ? mw + 2 * m : mw;
    int w = vdp_view_w;
    int counter = vdp.reg[10];
    widemap_frame(wide);               /* field margins drawn from the map in RAM */
    battleui_frame();                  /* modern battle menus: the game's windows left out of the picture */

    vdp.in_vblank = 0;
    for (int y = 0; y < h; y++) {
        vdp.line = y;
        vdp_render_line(y, &frame_lines[y]);
        vdp_compose_line(&frame_lines[y], &frame_pixels[y * VDP_MAX_W], w);
        if (counter-- == 0) {
            counter = vdp.reg[10];
            if (vdp_hint_enabled()) m68k_interrupt(4, SPIN_PC);
        }
    }

    platform_frame(frame_pixels, w, h);
    frame_count++;

    vdp.in_vblank = 1;
    vdp.line = h;
    rumble_frame();                    /* hits and sound requests, for the rumble (reads only) */
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

    /* the Z80, YM2612 and PSG play out the rest of the frame */
    const int16_t *samples;
    int n = sound_end_frame(&samples);
    if (sfx_enabled()) {                                 /* plus any replacement samples */
        static int16_t mix[2 * 8192];
        if (n > 8192) n = 8192;
        memcpy(mix, samples, (size_t)n * 2 * sizeof(int16_t));
        sfx_mix(mix, n);
        samples = mix;
    }
    platform_audio(samples, n);
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
