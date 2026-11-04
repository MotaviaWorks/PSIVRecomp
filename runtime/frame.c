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
#include "bus.h"
#include "m68k.h"
#include "sound.h"

#define VINT_FLAG 0xEF12u          /* VInt_Flag = $FFFFEF12 */
#define SPIN_PC   0x42076u         /* where the interrupted code resumes */

uint32_t frame_pixels[VDP_MAX_W * VDP_MAX_H];
uint64_t frame_count;
int frame_pending_lag;
VdpLine frame_lines[VDP_MAX_H];

void frame_run(void) {
    int w = vdp_width(), h = vdp_height();
    int counter = vdp.reg[10];

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
    if (vdp_vint_enabled()) m68k_interrupt(6, SPIN_PC);
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
