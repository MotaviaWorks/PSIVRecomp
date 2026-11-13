/*
 * Drawing for the port's own UI (the widescreen side panels, the modern
 * battle menus): shapes, portraits and TrueType text into an 0xAARRGGBB
 * buffer at the output resolution, plus the game's names and portraits read
 * from the player's ROM and RAM (nothing of the game ships with the port).
 */
#ifndef PSIV_UIKIT_H
#define PSIV_UIKIT_H

#include <stdint.h>

typedef struct { uint32_t *px; int stride, x0, y0, x1, y1; } UiCanvas;   /* buffer + clip box */
typedef struct { int w, h; uint8_t *rgba; } UiImage;

/* colours are 0xRRGGBB; alpha 0-255 */
void  ui_blend(uint32_t *dst, uint32_t rgb, int alpha);
/* on: draw into a transparent buffer (cleared to 0) for an overlay -- shapes
 * and text keep straight alpha; off (the default): over an opaque image */
void  ui_set_straight_alpha(int on);
void  ui_round_rect(const UiCanvas *cv, float x0, float y0, float x1, float y1, float r, uint32_t rgb, int alpha);
/* a vertical gradient from top to bottom colour */
void  ui_round_rect_v(const UiCanvas *cv, float x0, float y0, float x1, float y1, float r,
                      uint32_t top, uint32_t bottom, int alpha);
/* the outline of a rounded rectangle, w pixels wide, inside the box */
void  ui_round_frame(const UiCanvas *cv, float x0, float y0, float x1, float y1, float r, float w,
                     uint32_t rgb, int alpha);
/* A window of the port's own menus, in the manner of the game's: a dark
 * edge, a pale frame and a deep blue inside. `unit` = output pixels per game
 * pixel (the frame is two and a half of them wide). */
void  ui_frame_window(const UiCanvas *cv, float x0, float y0, float x1, float y1, float unit);
/* ... at an alpha (the battle menus follow the game's fades) */
void  ui_frame_window_a(const UiCanvas *cv, float x0, float y0, float x1, float y1, float unit, int alpha);
/* an image (nearest neighbour: pixel art) into a rounded square; dim = grey and dark */
void  ui_image(const UiCanvas *cv, const UiImage *im, float x0, float y0, float size, int dim, int alpha);
/* a coverage mask (mw x mh bytes, 0-255) in one colour, scaled smoothly into the box */
void  ui_mask(const UiCanvas *cv, const uint8_t *mask, int mw, int mh, float x0, float y0, float x1, float y1,
              uint32_t rgb, int alpha);
/* a bar: dark track, `frac` of it filled */
void  ui_bar(const UiCanvas *cv, float x0, float y0, float x1, float h, float frac, uint32_t rgb, int alpha);
/* a small triangle pointing down (dir 0), up (1), right (2) or left (3) */
void  ui_triangle(const UiCanvas *cv, float cx, float cy, float size, int dir, uint32_t rgb, int alpha);

/* UTF-8 text, capitals `cap` pixels tall; return the width */
float ui_text(const UiCanvas *cv, float x, float baseline, float cap, const char *s, uint32_t rgb, int alpha);
float ui_text_right(const UiCanvas *cv, float x_right, float baseline, float cap, const char *s, uint32_t rgb, int alpha);
float ui_text_center(const UiCanvas *cv, float x_mid, float baseline, float cap, const char *s, uint32_t rgb, int alpha);
float ui_text_width(const char *s, float cap);
/* word-wrapped into lines of at most `width`; returns the number of lines drawn */
int   ui_text_wrap(const UiCanvas *cv, float x, float baseline, float width, float line_h, int max_lines,
                   float cap, const char *s, uint32_t rgb, int alpha);
int   ui_text_lines(float width, float cap, const char *s);   /* the lines it takes, wrapped so */
int   ui_ready(void);                    /* the font is loaded */

/* the game's data */
extern const char *const UI_CHAR_NAMES[11];   /* Chaz .. Seth, by character ID */
int   ui_portraits_init(void);           /* the dialogue portraits from the ROM; returns how many */
const UiImage *ui_portrait(int id);      /* NULL when unavailable */
const char *ui_item_name(int id);        /* from the ROM, title case ("Hunt-Knife"); "" if none */
const char *ui_tech_name(int id);        /* techniques, 1-based ("Foi") */
const char *ui_skill_name(int id);       /* skills 1-54 ("Crosscut") */
/* a name in the window charset (ROM or RAM bytes, ended by $FE/$FF), title-cased */
void  ui_decode_name(const uint8_t *src, int max, char *out, int out_size);

unsigned ui_ram8(unsigned addr);         /* $FFxxxx work RAM */
unsigned ui_ram16(unsigned addr);
uint32_t ui_ram32(unsigned addr);
unsigned ui_rom8(uint32_t addr);
unsigned ui_rom16(uint32_t addr);
uint32_t ui_rom32(uint32_t addr);

#endif
