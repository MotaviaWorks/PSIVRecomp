/*
 * The world map (an option, off unless chosen): Motavia, Dezolis and Rykros
 * as a map -- a small one in the corner while the party walks them, and the
 * whole of it on a key, with the places named, to zoom, to move about, and to
 * click on: the party walks there.
 *
 * Nothing of the game ships for it. The picture is drawn from the player's
 * own game as it runs: the worlds' layouts (the two planets' from the ROM,
 * where the game keeps them in sixteen slabs and loads four at a time;
 * Rykros' from RAM), the chunk table and the palette in RAM, the tiles in
 * VRAM. What the story changes on a planet shows once the party has been
 * near it, as the game itself only changes the slabs it loads.
 *
 * "Explored": only what the party has seen shows, the rest dark; kept for the
 * player in a file (the front end's), and cleared when a new game begins.
 */
#ifndef PSIV_WORLDMAP_H
#define PSIV_WORLDMAP_H

#include <stddef.h>
#include <stdint.h>

/* -1: nothing at all (Classic, the Bonus Modes); 0: off, but where the party
 * goes is still noted; 1: the map, what was explored; 2: the map, all of it */
void worldmap_set_mode(int mode);
int  worldmap_mode(void);

/* once a frame, after the game's */
void worldmap_frame(void);

/* The party is on a world whose map can be shown (its picture is made);
 * worldmap_shown: ... and the field shows it, with no window up. */
int  worldmap_ready(void);
int  worldmap_shown(void);
const char *worldmap_name(void);
void worldmap_size(int *w, int *h);              /* map pixels */
int  worldmap_wraps(void);                       /* the planets go round, east to west ... */
int  worldmap_wraps_y(void);                     /* ... and Motavia north to south */
int  worldmap_party(int *x, int *y);             /* the leader's feet, a map pixel */
unsigned worldmap_rev(void);                     /* changes whenever the picture would */

/* A view of it into 0xFFRRGGBB pixels. lvl > 0: a map pixel is 1/2^lvl of an
 * output pixel (1-4), (ox, oy) the view's top left in pixels of that level;
 * lvl 0: a map pixel is `mag` output pixels (1 up), (ox, oy) in map pixels.
 * wrap: 1 = a planet goes on round its east and west edges, 2 = round its north and south (3: both);
 * else beyond them is dark. */
void worldmap_render(uint32_t *out, int stride, int w, int h, int ox, int oy, int lvl, int mag, int wrap);

/* the places to name now (those seen, and there as the party last saw it):
 * each a map pixel, the middle of its way in */
typedef struct { int x, y; const char *name; } WorldmapPlace;
int  worldmap_places(WorldmapPlace *out, int max);
int  worldmap_cell_code(int cx, int cy);         /* a 16x16 cell's collision code on the walked layer (testing) */

/* Walk to a map pixel (click to walk across the world): a way over the
 * game's own collision, then walkto.h a few steps at a time; a battle on the
 * way and the party walks on after it. 0: no way to start (not on foot, a
 * window up). The player's own d-pad or buttons end it. */
int  worldmap_go(int mx, int my);
int  worldmap_can_go(void);
int  worldmap_riding(void);                     /* in a vehicle (walkto_riding): the journey drives it */
int  worldmap_going(int *mx, int *my);           /* where to, a map pixel */
int  worldmap_route(int i, int *mx, int *my);    /* the way there, cell by cell: 1 while i is on it */
void worldmap_stop(void);
void worldmap_journey(uint8_t user);             /* each frame, with what the player holds, before walkto_pad */

/* what was explored, for a file: WORLDMAP_FILE_SIZE bytes */
#define WORLDMAP_FILE_SIZE (8 + 3 * 8192)
int  worldmap_explored_dirty(void);              /* changed since the last pack */
void worldmap_explored_pack(uint8_t *out);
int  worldmap_explored_unpack(const uint8_t *in, size_t n);

#endif
