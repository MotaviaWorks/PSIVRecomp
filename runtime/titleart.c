/*
 * The port's own title art (titleart.h).
 *
 * The game builds its title in plane A: the logo's map at cells 11-27 x 3-15,
 * its art and the subtitle's in tiles $10-$C7. It brings both in, and takes
 * them away when Start is pressed, by uploading that art through a mask of
 * pixel columns that widens or narrows a step at a time, turned three pixels
 * further on each row of a tile (TitleRoutine_FadingText's end, loc_42C0E).
 * The port's art keeps to the same mask, read back from what is in VRAM, so
 * it dissolves in and out with the logo; and it is drawn in the colours the
 * game has loaded (palette line 0), so it fades with the screen too.
 */
#include "titleart.h"
#include "m68k.h"
#include "vdp.h"

typedef struct { int x, y, w, h; const char *const *rows; } TitleArt;
#include "titleart_data.h"

#define GAME_MODE   0xEF00u        /* Game_Mode_Index: 4 = the title */
#define MODE_TITLE  4
#define LOGO_TILE   0x10           /* the logo's art, then the subtitle's: uploaded as one */
#define LOGO_TILES  184

static int enabled;

void titleart_set(int on) { enabled = on; }

/* The logo is on the screen: the title's mode, plane A unscrolled, the logo's
 * map in its cells. */
static int logo_up(void) {
    static const struct { uint8_t col, row; uint16_t word; } CELLS[] = {
        {13, 5, 0x0015}, {11, 9, 0x0046}, {26, 14, 0x008D}, {14, 15, 0x008E},
    };
    if ((m68k_ram[GAME_MODE] << 8 | m68k_ram[GAME_MODE + 1]) != MODE_TITLE || vdp_width() != 320) return 0;
    int px, py;
    vdp_plane_xy(0, 0, 0, &px, &py);
    if (px || py) return 0;
    for (unsigned i = 0; i < sizeof CELLS / sizeof CELLS[0]; i++)
        if (vdp_plane_entry(0, CELLS[i].col * 8, CELLS[i].row * 8) != CELLS[i].word) return 0;
    return 1;
}

/* The pixel columns the dissolve has let through, as they stand on a tile's
 * top row (bit n = column n): every pixel of the logo's art that is there
 * tells of one, the mask being three columns further on each row down. */
static unsigned columns_shown(void) {
    unsigned shown = 0;
    for (int t = LOGO_TILE; t < LOGO_TILE + LOGO_TILES && shown != 0xFF; t++)
        for (int row = 0; row < 8; row++) {
            const uint8_t *p = &vdp.vram[t * 32 + row * 4];
            for (int c = 0; c < 8; c++)
                if (c & 1 ? p[c >> 1] & 0x0F : p[c >> 1] >> 4) shown |= 1u << ((c + 3 * row) & 7);
        }
    return shown;
}

static void draw(uint32_t *pixels, const TitleArt *art, unsigned shown) {
    for (int y = 0; y < art->h; y++)
        for (int x = 0; x < art->w; x++) {
            char ch = art->rows[y][x];
            int c = ch <= '9' ? ch - '0' : ch - 'A' + 10, sx = art->x + x, sy = art->y + y;
            if (c && shown >> (((sx & 7) + 3 * (sy & 7)) & 7) & 1)
                pixels[sy * VDP_MAX_W + sx] = vdp_color(c);
        }
}

void titleart_frame(uint32_t *pixels, int w, int h) {
    if (!enabled || w != 320 || h < 224 || !logo_up()) return;
    unsigned shown = columns_shown();
    if (!shown) return;
    draw(pixels, &TITLE_IV, shown);
    draw(pixels, &TITLE_RECOMP, shown);
}
