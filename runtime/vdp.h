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
    uint8_t plane_a_txy[VDP_MAX_W];   /* and where in that tile: x | y << 3 (before flips) */
    uint8_t plane_a_under[VDP_MAX_W]; /* 1: drawn in by an override that stays behind low-priority sprites */
    uint8_t window[VDP_MAX_W];   /* window plane, where it is shown */
    uint8_t win_in[VDP_MAX_W];   /* 1 where the window replaces plane A */
    uint8_t sprite[VDP_MAX_W];
    uint8_t sprite_no[VDP_MAX_W];/* which sprite-table entry drew each sprite pixel */
    uint8_t sprite_own[VDP_MAX_W];/* with vdp_sprite_art set: the game's own pixel where a sprite pixel is */
    uint8_t backdrop;            /* palette index */
} VdpLine;

/* Presentation overrides (never game state): the port's modern battle menus
 * draw over the game's own windows, which are hidden by drawing other plane A
 * name-table entries in their cells (64 x 32 cells, row-major, 0xFFFF = the
 * name table's own) and leaving out the menu cursor sprites (one flag per
 * sprite-table entry). Sprites are still evaluated, so the overflow and
 * collision flags the game reads stay as the hardware sets them. NULL = off. */
extern const uint16_t *vdp_plane_a_override;
extern const uint8_t *vdp_sprite_hidden;
/* Other pictures for sprites (a mod's: spritemod.c). NULL, or 80 entries by the sprite table's index;
 * where `px` is set, the piece shows that picture's pixels (w x h bytes, 0 nothing, 1-15 a colour of the
 * piece's own palette line; x0, y0 its place on the screen) in place of its tiles'. Display only: the
 * collision flag the game can read is still worked out from its own pixels. */
typedef struct { const uint8_t *px; int16_t x0, y0, w, h; } VdpSpriteArt;
extern const VdpSpriteArt *vdp_sprite_art;
/* Other pictures for cells of plane A (a mod's: the party standing in battle). NULL, or 32 x 64 entries by the
 * name table's cell; where `px` is set the cell shows those 64 pixels (0 nothing, 1-15 a colour, 255 the
 * game's own pixel) with `attr` (the palette line << 4, $80 priority). Display only. */
typedef struct { const uint8_t *px; uint8_t attr; } VdpCellArt;
extern const VdpCellArt *vdp_plane_a_art;
/* With an override, per cell: bit 0 = its tile stays behind low-priority
 * sprites (a picture the port put back, which the game's own action sprites
 * pass in front of); bit 1 = its pixels come from vdp_tile_shadow, a copy of
 * VRAM the port keeps (the game reuses that VRAM meanwhile). NULL = off. */
extern const uint8_t *vdp_plane_a_flags;
extern const uint8_t *vdp_tile_shadow;
/* With an override, per cell: a second picture (a name-table entry with its
 * pixels from vdp_tile_shadow, 0xFFFF = none) that shows where the cell's own
 * is transparent -- someone standing behind the one the game drew there -- or
 * in front of it if the cell's own is low priority and this one high. Its
 * pixels stay behind low-priority sprites, like the port's own (bit 0 above).
 * NULL = off. */
extern const uint16_t *vdp_plane_a_back;
/* Widescreen margins: for each plane pixel drawn beyond the mode's width,
 * the name-table word to draw there instead of the plane's own (plane 0 = A,
 * 1 = B; x, y on the screen, px, py the pixel in the plane it falls on).
 * Returns -1 for the plane's own word, -2 for nothing (transparent).
 * NULL = off. */
extern int (*vdp_margin_entry)(int plane, int x, int y, int px, int py);

/* Where screen pixel (x, y) falls in plane 0 (A) or 1 (B), scroll included,
 * as the line renderer finds it; and the name-table word at a plane pixel. */
void     vdp_plane_xy(int plane, int x, int y, int *px, int *py);
uint16_t vdp_plane_entry(int plane, int px, int py);
int      vdp_plane_cells(int *wcells, int *hcells);   /* the planes' size; returns the base of plane A */

void vdp_render_line(int y, VdpLine *out);
/* mix a line's layers into 0xAARRGGBB pixels */
void vdp_compose_line(const VdpLine *l, uint32_t *dst, int width);
uint32_t vdp_color(int index);   /* CRAM entry as 0xFFRRGGBB */
/* what a composed line shows at column x, as its layer's pixel (palette line and colour, bit 7 priority),
 * and what it would show there with no sprite */
void vdp_line_codes(const VdpLine *l, int x, uint8_t *shown, uint8_t *under);

#endif
