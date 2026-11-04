/*
 * Mega Drive VDP (Sega 315-5313): ports, registers, DMA and a scanline renderer.
 *
 * The renderer draws one line at a time so the game's horizontal-interrupt
 * handler can change scroll values between lines, as it does on hardware.
 * Each layer (plane B, plane A, window, sprites) is drawn into its own line
 * buffer and only mixed at the end; enhancements (widescreen, battle depth)
 * hook in at that point.
 */
#ifndef PSIV_VDP_H
#define PSIV_VDP_H

#include <stdint.h>

#define VDP_MAX_W 448            /* widest line drawn: 320 plus widescreen margins */
#define VDP_MAX_H 240
#define VDP_WIDE_MAX 64          /* largest widescreen margin on each side */

/* The columns drawn: screen x from vdp_view_x0, vdp_view_w of them. Normally
 * 0 and the mode's width (320); widescreen draws margins beyond both edges,
 * straight from the planes (they hold more of the map than is shown) and the
 * sprite table. Set by the frame loop before each frame. */
extern int vdp_view_x0, vdp_view_w;

typedef struct {
    uint8_t  vram[0x10000];
    uint16_t cram[64];           /* 9-bit BGR: ----bbb-ggg-rrr- */
    uint16_t vsram[40];
    uint8_t  reg[24];

    /* port state */
    int      pending;            /* first half of a two-word command seen */
    uint8_t  code;               /* CD5-CD0 */
    uint16_t addr;
    int      fill_pending;       /* DMA fill waits for the next data write */

    /* timing, set by the frame loop */
    int      in_vblank;
    int      line;               /* line being drawn (for the HV counter) */
    int      hint_counter;
    int      status_toggle;
    uint16_t sprite_status;      /* $20 collision, $40 overflow: set while drawing, cleared on read */
    /* The VDP's internal copy of the sprite table: refreshed only by VRAM
     * writes that land inside the table's current address range, and the
     * source of each sprite's Y, size and link when drawing. Moving the table
     * (register 5) does not refresh it, so the hardware can draw stale
     * positions for a while; the game can observe that through the collision
     * flag. */
    uint8_t  sat_cache[0x400];
} Vdp;

extern Vdp vdp;

void     vdp_reset(void);
void     vdp_write_data(uint16_t v);
void     vdp_write_control(uint16_t v);
uint16_t vdp_read_data(void);
uint16_t vdp_read_status(void);
uint16_t vdp_read_hv(void);

/* Replay: the beam-counter values the reference read during the next frame's
 * work, in order. The game mixes the counter into its battle random numbers
 * (UpdateRNGSeed2), and its exact value depends on cycle timing, so replays
 * take it from the recording like the pad. Returns how many values of the
 * previous feed went unread (negative: reads beyond them). */
int vdp_hv_feed(const uint16_t *values, int n);

int vdp_width(void);             /* 320 (H40) or 256 (H32) */
int vdp_height(void);            /* 224 (V28) or 240 (V30) */
int vdp_hint_enabled(void);
int vdp_vint_enabled(void);

/* Per-line layer output: palette index 0-63 with bit 7 = priority; 0 = transparent */
typedef struct {
    uint8_t plane_b[VDP_MAX_W];
    uint8_t plane_a[VDP_MAX_W];  /* plane A as drawn, also where the window covers it */
    uint16_t plane_a_tile[VDP_MAX_W]; /* plane A's name-table entry under each pixel */
    uint8_t window[VDP_MAX_W];   /* window plane, where it is shown */
    uint8_t win_in[VDP_MAX_W];   /* 1 where the window replaces plane A */
    uint8_t sprite[VDP_MAX_W];
    uint8_t sprite_no[VDP_MAX_W];/* which sprite-table entry drew each sprite pixel */
    uint8_t backdrop;            /* palette index */
} VdpLine;

void vdp_render_line(int y, VdpLine *out);
/* mix a line's layers into 0xAARRGGBB pixels */
void vdp_compose_line(const VdpLine *l, uint32_t *dst, int width);
uint32_t vdp_color(int index);   /* CRAM entry as 0xFFRRGGBB */

#endif
