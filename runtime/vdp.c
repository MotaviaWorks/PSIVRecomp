/*
 * VDP implementation. DMA completes instantly: the game never relies on
 * observing a transfer in progress (it waits on nothing but VInt_Flag).
 */
#include "vdp.h"
#include "frame.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Vdp vdp;
static int vdp_debug;                 /* PSIV_DEBUG_VDP=1: log register changes to stderr */

void vdp_reset(void) {
    const char *d = getenv("PSIV_DEBUG_VDP");
    vdp_debug = d && *d == '1';
    memset(&vdp, 0, sizeof vdp);
    vdp.reg[12] = 0x81;          /* H40 until the game says otherwise */
    vdp.reg[15] = 2;
}

int vdp_width(void) { return (vdp.reg[12] & 0x81) ? 320 : 256; }
int vdp_height(void) { return (vdp.reg[1] & 0x08) ? 240 : 224; }
int vdp_hint_enabled(void) { return (vdp.reg[0] & 0x10) != 0; }
int vdp_vint_enabled(void) { return (vdp.reg[1] & 0x20) != 0; }

/* ------------------------------------------------------------------ memory writes */
/* sprite table: 640 bytes on a 1 KB boundary in H40, 512 on 512 in H32 */
static uint16_t sat_base_mask(void) { return (vdp.reg[12] & 0x81) ? 0xFC00 : 0xFE00; }
static uint16_t sat_addr_mask(void) { return (vdp.reg[12] & 0x81) ? 0x03FF : 0x01FF; }
static uint16_t sat_base(void) { return (uint16_t)((vdp.reg[5] << 9) & sat_base_mask()); }

static void vram_byte(uint16_t a, uint8_t v) {
    vdp.vram[a] = v;
    if ((a & sat_base_mask()) == sat_base()) vdp.sat_cache[a & sat_addr_mask()] = v;
}

static void vram_write(uint16_t a, uint16_t v) {
    /* a word write lands on the even address; odd addresses swap the bytes */
    if (a & 1) v = (uint16_t)(v << 8 | v >> 8);
    a &= 0xFFFE;
    vram_byte(a, (uint8_t)(v >> 8));
    vram_byte((uint16_t)(a + 1), (uint8_t)v);
}

static void target_write(uint16_t v) {
    switch (vdp.code & 0x0F) {
    case 0x1: vram_write(vdp.addr, v); break;
    case 0x3: vdp.cram[(vdp.addr >> 1) & 0x3F] = v & 0x0EEE; break;
    /* VSRAM keeps the full word as written (Genesis Plus GX does too); only
     * the low 10 bits matter when drawing */
    case 0x5: if ((vdp.addr >> 1) < 40) vdp.vsram[vdp.addr >> 1] = v; break;
    default: break;              /* writes in a read mode are ignored */
    }
    vdp.addr = (uint16_t)(vdp.addr + vdp.reg[15]);
}

/* ------------------------------------------------------------------ DMA */
static uint32_t dma_length(void) {
    uint32_t n = (uint32_t)vdp.reg[20] << 8 | vdp.reg[19];
    return n ? n : 0x10000;
}

static void dma_68k(void) {
    uint32_t src = ((uint32_t)(vdp.reg[23] & 0x7F) << 17 | (uint32_t)vdp.reg[22] << 9 | (uint32_t)vdp.reg[21] << 1);
    uint32_t n = dma_length();
    for (uint32_t i = 0; i < n; i++) {
        target_write(rd16(src));
        /* the source wraps within a 128 KB window */
        src = (src & 0xFE0000) | ((src + 2) & 0x1FFFF);
    }
    vdp.reg[19] = vdp.reg[20] = 0;
    vdp.reg[21] = (uint8_t)(src >> 1);
    vdp.reg[22] = (uint8_t)(src >> 9);
}

static void dma_fill(uint16_t v) {
    uint32_t n = dma_length();
    vram_write(vdp.addr, v);
    vdp.addr = (uint16_t)(vdp.addr + vdp.reg[15]);
    for (uint32_t i = 0; i < n; i++) {
        vram_byte((uint16_t)(vdp.addr ^ 1), (uint8_t)(v >> 8));
        vdp.addr = (uint16_t)(vdp.addr + vdp.reg[15]);
    }
    vdp.reg[19] = vdp.reg[20] = 0;
}

static void dma_copy(void) {
    uint16_t src = (uint16_t)(vdp.reg[22] << 8 | vdp.reg[21]);
    uint32_t n = dma_length();
    for (uint32_t i = 0; i < n; i++) {
        vram_byte((uint16_t)(vdp.addr ^ 1), vdp.vram[src ^ 1]);
        src++;
        vdp.addr = (uint16_t)(vdp.addr + vdp.reg[15]);
    }
    vdp.reg[19] = vdp.reg[20] = 0;
    vdp.reg[21] = (uint8_t)src;
    vdp.reg[22] = (uint8_t)(src >> 8);
}

/* ------------------------------------------------------------------ ports */
void vdp_write_data(uint16_t v) {
    vdp.pending = 0;
    if (vdp.fill_pending) {
        vdp.fill_pending = 0;
        dma_fill(v);
        return;
    }
    target_write(v);
}

void vdp_write_control(uint16_t v) {
    if (vdp.pending) {
        vdp.pending = 0;
        vdp.addr = (uint16_t)((vdp.addr & 0x3FFF) | (v & 3) << 14);
        vdp.code = (uint8_t)((vdp.code & 0x03) | (v >> 2 & 0x3C));
        if ((vdp.code & 0x20) && (vdp.reg[1] & 0x10)) {          /* DMA requested and enabled */
            switch (vdp.reg[23] >> 6) {
            case 0: case 1: dma_68k(); break;
            case 2: vdp.fill_pending = 1; break;
            case 3: dma_copy(); break;
            }
        }
        return;
    }
    if ((v & 0xC000) == 0x8000) {                                 /* register write */
        int r = v >> 8 & 0x1F;
        if (vdp_debug && r < 24 && vdp.reg[r] != (uint8_t)v)
            fprintf(stderr, "vdp: frame %llu line %d%s: reg %d %02X -> %02X\n", (unsigned long long)frame_count,
                    vdp.line, vdp.in_vblank ? " (vblank)" : "", r, vdp.reg[r], v & 0xFF);
        if (r < 24) vdp.reg[r] = (uint8_t)v;
        vdp.code = 0;
        return;
    }
    vdp.pending = 1;
    vdp.addr = (uint16_t)((vdp.addr & 0xC000) | (v & 0x3FFF));
    vdp.code = (uint8_t)((vdp.code & 0x3C) | v >> 14);
}

uint16_t vdp_read_data(void) {
    uint16_t v = 0;
    vdp.pending = 0;
    switch (vdp.code & 0x0F) {
    case 0x0: v = (uint16_t)(vdp.vram[vdp.addr & 0xFFFE] << 8 | vdp.vram[(vdp.addr & 0xFFFE) + 1]); break;
    case 0x8: v = vdp.cram[(vdp.addr >> 1) & 0x3F]; break;
    case 0x4: v = (vdp.addr >> 1) < 40 ? vdp.vsram[vdp.addr >> 1] : 0; break;
    }
    vdp.addr = (uint16_t)(vdp.addr + vdp.reg[15]);
    return v;
}

uint16_t vdp_read_status(void) {
    /* Bits 15-10 are open bus: whatever the 68000 last prefetched. The one read
     * the game keeps (VInt: move.w (a6),($EF22).w) sees its own address word,
     * $EF22, so $EC00. The VInt-pending bit (7) is already cleared by then:
     * taking the interrupt acknowledges it. Bit 9 = FIFO empty. */
    uint16_t s = 0xEC00 | 0x0200 | vdp.sprite_status;
    vdp.pending = 0;
    vdp.sprite_status = 0;                        /* collision/overflow clear when read */
    if (vdp.in_vblank) s |= 0x0008;
    else if (vdp.status_toggle ^= 1) s |= 0x0008; /* main code polling: let wait loops finish */
    return s;
}

static uint16_t hv_queue[1024];
static int hv_count, hv_pos, hv_extra;

int vdp_hv_feed(const uint16_t *values, int n) {
    int off = hv_extra ? -hv_extra : hv_count - hv_pos;
    if (n > (int)(sizeof hv_queue / sizeof hv_queue[0])) n = (int)(sizeof hv_queue / sizeof hv_queue[0]);
    for (int i = 0; i < n; i++) hv_queue[i] = values[i];
    hv_count = n;
    hv_pos = hv_extra = 0;
    return off;
}

uint16_t vdp_read_hv(void) {
    if (hv_pos < hv_count) return hv_queue[hv_pos++];
    if (hv_count) hv_extra++;
    int v = vdp.in_vblank ? 0xE0 : vdp.line;
    return (uint16_t)((v & 0xFF) << 8);
}

/* ------------------------------------------------------------------ rendering */
uint32_t vdp_color(int index) {
    uint16_t c = vdp.cram[index & 63];
    uint32_t r = c >> 1 & 7, g = c >> 5 & 7, b = c >> 9 & 7;
    r = r * 255 / 7; g = g * 255 / 7; b = b * 255 / 7;
    return 0xFF000000u | r << 16 | g << 8 | b;
}

static int plane_cells(int bits) {
    return bits == 0 ? 32 : bits == 1 ? 64 : bits == 3 ? 128 : 32;
}

/* pixel of a tile: entry = name-table word, tx/ty = pixel inside the 8x8 cell */
static uint8_t tile_pixel(uint16_t entry, int tx, int ty) {
    if (entry & 0x0800) tx = 7 - tx;
    if (entry & 0x1000) ty = 7 - ty;
    uint32_t a = (uint32_t)(entry & 0x07FF) * 32 + (uint32_t)ty * 4 + (uint32_t)(tx >> 1);
    uint8_t b = vdp.vram[a & 0xFFFF];
    uint8_t px = (tx & 1) ? (b & 0x0F) : (b >> 4);
    if (!px) return 0;
    return (uint8_t)((entry >> 13 & 3) << 4 | px | (entry & 0x8000 ? 0x80 : 0));
}

static uint16_t nt_entry(uint32_t base, int cx, int cy, int wcells) {
    uint32_t a = (base + ((uint32_t)cy * (uint32_t)wcells + (uint32_t)cx) * 2) & 0xFFFF;
    return (uint16_t)(vdp.vram[a] << 8 | vdp.vram[a + 1]);
}

static void render_plane(int y, int plane, uint8_t *out, int width) {
    int wcells = plane_cells(vdp.reg[16] & 3), hcells = plane_cells(vdp.reg[16] >> 4 & 3);
    uint32_t base = plane == 0 ? (uint32_t)(vdp.reg[2] & 0x38) << 10 : (uint32_t)(vdp.reg[4] & 7) << 13;
    /* horizontal scroll: full screen, per 8-line cell, or per line */
    uint32_t hs = (uint32_t)(vdp.reg[13] & 0x3F) << 10;
    int hmode = vdp.reg[11] & 3;
    uint32_t hrow = hmode == 3 ? (uint32_t)y : hmode == 2 ? (uint32_t)(y & ~7) : 0;
    uint32_t ha = (hs + hrow * 4 + (uint32_t)plane * 2) & 0xFFFF;
    int hscroll = (vdp.vram[ha] << 8 | vdp.vram[ha + 1]) & 0x3FF;
    int vmode = vdp.reg[11] >> 2 & 1;
    int pw = wcells * 8, ph = hcells * 8;
    for (int x = 0; x < width; x++) {
        int col = vmode ? (x / 16) : 0;           /* per 2-cell column vertical scroll */
        int vscroll = vdp.vsram[(col * 2 + plane) % 40] & 0x3FF;
        int px = (x - hscroll) & (pw - 1);
        int py = (y + vscroll) & (ph - 1);
        uint16_t e = nt_entry(base, px >> 3, py >> 3, wcells);
        out[x] = tile_pixel(e, px & 7, py & 7);
    }
}

static void render_window(int y, uint8_t *out, int width) {
    int h40 = width == 320;
    uint32_t base = (uint32_t)(vdp.reg[3] & (h40 ? 0x3C : 0x3E)) << 10;
    int wcells = h40 ? 64 : 32;
    int hpos = (vdp.reg[17] & 0x1F) * 16, right = vdp.reg[17] & 0x80;
    int vpos = (vdp.reg[18] & 0x1F) * 8, down = vdp.reg[18] & 0x80;
    int line_in = down ? y >= vpos : y < vpos;
    for (int x = 0; x < width; x++) {
        int in = line_in || (right ? x >= hpos : x < hpos);
        if (!in) continue;
        uint16_t e = nt_entry(base, x >> 3, y >> 3, wcells);
        out[x] = tile_pixel(e, x & 7, y & 7);
    }
}

/* Sprite pixels are tracked in a line buffer with a 32-pixel margin on each
 * side, like Genesis Plus GX's: two sprites overlapping just off-screen still
 * set the collision flag, and the game stores that flag (VDP_Status_Reg). */
#define SPRITE_MARGIN 32

static void render_sprites(int y, uint8_t *out, int width) {
    int h40 = width == 320;
    uint32_t base = (uint32_t)(vdp.reg[5] & (h40 ? 0x7E : 0x7F)) << 9;
    int max_sprites = h40 ? 80 : 64, per_line = h40 ? 20 : 16, max_px = h40 ? 320 : 256;
    int link = 0, count = 0, drawn = 0, pixels = 0, masked = 0, prev_on_line = 0;
    uint8_t seen[VDP_MAX_W + 2 * SPRITE_MARGIN];
    memset(seen, 0, sizeof seen);
    memset(out, 0, (size_t)width);
    do {
        uint32_t a = (base + (uint32_t)link * 8) & 0xFFFF;
        const uint8_t *s = &vdp.vram[a];
        const uint8_t *c = &vdp.sat_cache[((uint32_t)link * 8) & sat_addr_mask()];   /* Y, size, link */
        int sy = ((c[0] << 8 | c[1]) & 0x3FF) - 128;
        int vs = (c[2] >> 0 & 3) + 1, hs = (c[2] >> 2 & 3) + 1;
        uint16_t attr = (uint16_t)(s[4] << 8 | s[5]);
        int sx = ((s[6] << 8 | s[7]) & 0x1FF) - 128;
        link = c[3] & 0x7F;
        if (y >= sy && y < sy + vs * 8) {
            if (++drawn > per_line) { vdp.sprite_status |= 0x40; break; }   /* per-line limit: overflow */
            if (sx == -128 && prev_on_line) masked = 1;       /* x=0 sprite masks the rest */
            prev_on_line = 1;
            int ty = y - sy;
            if (attr & 0x1000) ty = vs * 8 - 1 - ty;
            for (int cx = 0; cx < hs * 8; cx++) {
                if (pixels++ >= max_px) { vdp.sprite_status |= 0x40; break; }
                int x = sx + cx;
                if (masked || x < -SPRITE_MARGIN || x >= width + SPRITE_MARGIN) continue;
                int tx = (attr & 0x0800) ? hs * 8 - 1 - cx : cx;
                /* sprite cells are stored column by column */
                uint16_t tile = (uint16_t)((attr & 0x07FF) + (tx >> 3) * vs + (ty >> 3));
                uint16_t e = (uint16_t)((attr & 0xE000) | (tile & 0x07FF));   /* flips already applied */
                uint8_t p = tile_pixel(e, tx & 7, ty & 7);
                if (!p) continue;
                if (seen[x + SPRITE_MARGIN]) {                   /* two opaque sprite pixels: collision */
                    vdp.sprite_status |= 0x20;
                    continue;
                }
                seen[x + SPRITE_MARGIN] = 1;
                if (x >= 0 && x < width) out[x] = p;
            }
        }
    } while (link && ++count < max_sprites);
}

void vdp_render_line(int y, VdpLine *l) {
    int w = vdp_width();
    l->backdrop = vdp.reg[7] & 0x3F;
    if (!(vdp.reg[1] & 0x40)) {                              /* display disabled */
        memset(l->plane_b, 0, (size_t)w);
        memset(l->plane_a, 0, (size_t)w);
        memset(l->sprite, 0, (size_t)w);
        return;
    }
    render_plane(y, 1, l->plane_b, w);
    render_plane(y, 0, l->plane_a, w);
    render_window(y, l->plane_a, w);
    render_sprites(y, l->sprite, w);
}

void vdp_compose_line(const VdpLine *l, uint32_t *dst, int width) {
    for (int x = 0; x < width; x++) {
        uint8_t b = l->plane_b[x], a = l->plane_a[x], s = l->sprite[x];
        uint8_t c = l->backdrop;
        /* back to front: B low, A low, sprite low, B high, A high, sprite high */
        if (b && !(b & 0x80)) c = b;
        if (a && !(a & 0x80)) c = a;
        if (s && !(s & 0x80)) c = s;
        if (b & 0x80) c = b;
        if (a & 0x80) c = a;
        if (s & 0x80) c = s;
        dst[x] = vdp_color(c & 0x3F);
    }
}
