/*
 * Sprites from a mod: see spritemod.h.
 */
#include "spritemod.h"
#include "m68k.h"
#include "vdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MODE    0xEF00
#define MODE_FIELD   0x0C
#define OBJECTS      0xC000            /* the field's objects, $40 each; the first is the party's leader */
#define OBJ_SIZE     0x40
#define O_ID         0x00
#define O_FACING     0x06              /* 0 down, 4 up, 8 right, $C left */
#define O_SCRIPTS    0x08              /* a table of four animations, one a facing */
#define O_FRAME      0x10              /* the animation's frame showing */
#define O_HOLD_LEFT  0x11              /* ticks of it still to come */
#define O_ART_TILE   0x16
#define O_X          0x30
#define O_Y          0x34

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }
static uint32_t r32(unsigned a) { return (uint32_t)r16(a) << 16 | r16(a + 2); }
static unsigned rom8(uint32_t a) { return a < 0x400000 ? m68k_rom[a] : 0; }
static uint32_t rom32(uint32_t a) {
    return (uint32_t)rom8(a) << 24 | (uint32_t)rom8(a + 1) << 16 | (uint32_t)rom8(a + 2) << 8 | rom8(a + 3);
}

/* the file: "PSIVSPR1\n" "W H FRAMES MULT\n", then for each facing (down, up, right, left), each of the
 * game's FRAMES and each of its MULT in-between pictures, W x H bytes: 0 nothing, 1-15 a colour of the
 * sprite's own palette line */
static struct { int w, h, frames, mult; uint8_t *px; } art;
static VdpSpriteArt slots[80];
static int last_x = -1, last_y = -1, still, logging = -1;

int spritemod_set_file(const char *path) {
    free(art.px);
    memset(&art, 0, sizeof art);
    vdp_sprite_art = NULL;
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "spritemod: cannot open %s\n", path); return 0; }
    char magic[16] = {0};
    int ok = fgets(magic, sizeof magic, f) && !strncmp(magic, "PSIVSPR1", 8)
          && fscanf(f, "%d %d %d %d", &art.w, &art.h, &art.frames, &art.mult) == 4 && fgetc(f) == '\n'
          && art.w > 0 && art.w <= 64 && art.h > 0 && art.h <= 64 && art.frames > 0 && art.frames <= 8
          && art.mult > 0 && art.mult <= 8;
    if (ok) {
        size_t n = (size_t)art.w * (size_t)art.h * 4u * (size_t)art.frames * (size_t)art.mult;
        art.px = malloc(n);
        ok = art.px && fread(art.px, 1, n, f) == n;
    }
    fclose(f);
    if (!ok) {
        fprintf(stderr, "spritemod: %s is not a sprite file of this port\n", path);
        free(art.px);
        memset(&art, 0, sizeof art);
        return 0;
    }
    fprintf(stderr, "spritemod: %s: %d x %d, %d frames a facing, each drawn %d times over\n", path, art.w, art.h,
            art.frames, art.mult);
    return 1;
}

void spritemod_frame(void) {
    vdp_sprite_art = NULL;
    if (!art.px || r16(GAME_MODE) != MODE_FIELD) return;
    if (logging < 0) logging = getenv("PSIV_SPRITEMOD_LOG") != NULL;
    unsigned o = OBJECTS;
    if (!r16(o + O_ID)) return;
    int facing = (int)(r16(o + O_FACING) >> 2 & 3);
    int frame = (int)r8(o + O_FRAME), left = (int)(int8_t)r8(o + O_HOLD_LEFT);
    uint32_t script = rom32(r32(o + O_SCRIPTS) + (uint32_t)facing * 4u);
    int hold = (rom8(script) & 0x80) ? 7 : (int)rom8(script + 1);        /* (a list of its own holds: taken as 8 ticks) */
    /* the leader's tiles: from its own art tile up to the next object's */
    int tile0 = (int)(r16(o + O_ART_TILE) & 0x7FF), tile1 = tile0 + 0x40;
    for (int i = 1; i < 64; i++) {
        unsigned p = OBJECTS + (unsigned)i * OBJ_SIZE;
        int t = (int)(r16(p + O_ART_TILE) & 0x7FF);
        if (r16(p + O_ID) && t > tile0 && t < tile1) tile1 = t;
    }
    /* its pieces in the sprite table, and the box they stand in */
    uint32_t sat = (uint32_t)(vdp.reg[5] & 0x7E) << 9;
    int bx0 = 9999, by0 = 9999, bx1 = -9999, by1 = -9999, link = 0, n = 0;
    uint8_t mine[80] = {0};
    for (int count = 0; count < 80; count++) {
        const uint8_t *s = &vdp.vram[(sat + (uint32_t)link * 8u) & 0xFFFF];
        int me = link;
        int y = ((s[0] << 8 | s[1]) & 0x3FF) - 128, x = ((s[6] << 8 | s[7]) & 0x1FF) - 128;
        int vs = (s[2] & 3) + 1, hs = (s[2] >> 2 & 3) + 1, tile = (s[4] << 8 | s[5]) & 0x7FF;
        if (tile >= tile0 && tile < tile1 && y > -64 && y < 240) {
            mine[me] = 1;
            n++;
            if (x < bx0) bx0 = x;
            if (y < by0) by0 = y;
            if (x + hs * 8 > bx1) bx1 = x + hs * 8;
            if (y + vs * 8 > by1) by1 = y + vs * 8;
        }
        link = s[3] & 0x7F;
        if (!link) break;
    }
    if (!n) return;
    /* which picture: the game's frame, and how far through its hold. Standing, the game's count stands
     * too: the frame's first picture */
    int x = (int)r16(o + O_X), y = (int)r16(o + O_Y);
    still = (x == last_x && y == last_y) ? still + 1 : 0;
    last_x = x;
    last_y = y;
    int g = frame % art.frames, k = 0;
    if (still < 2) {
        int gone = hold - left;
        if (gone < 0) gone = 0;
        if (gone > hold) gone = hold;
        k = gone * art.mult / (hold + 1);
    }
    const uint8_t *px = art.px + ((size_t)(facing * art.frames + g) * (size_t)art.mult + (size_t)k) * (size_t)(art.w * art.h);
    /* feet on the box's foot, centred */
    int16_t x0 = (int16_t)((bx0 + bx1) / 2 - art.w / 2), y0 = (int16_t)(by1 - art.h);
    for (int i = 0; i < 80; i++) {
        slots[i].px = mine[i] ? px : NULL;
        slots[i].x0 = x0;
        slots[i].y0 = y0;
        slots[i].w = (int16_t)art.w;
        slots[i].h = (int16_t)art.h;
    }
    vdp_sprite_art = slots;
    if (logging)
        fprintf(stderr, "spritemod: facing %d frame %d hold %d left %d -> picture %d.%d; %d pieces, tiles %03X-%03X, box %d,%d %dx%d\n",
                facing, frame, hold, left, g, k, n, (unsigned)tile0, (unsigned)tile1, bx0, by0, bx1 - bx0, by1 - by0);
}
