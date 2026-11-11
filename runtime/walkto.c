/*
 * Click to walk: see walkto.h.
 *
 * Collision is GetChunkAndCollision's: the walked layer's layout ($FFEC24
 * set: Map_Layout_BG), each 16x16 cell's code from bit 6 of its four
 * name-table words (top left 1, top right 2, bottom left 4, bottom right 8),
 * with the game's own row stride (the other layer's row size; the same in
 * practice). On foot (TileCollNormalPtrs) 8 solid, 9 water, $A sand and $B an
 * ice block stop the party and $C is a shop counter; 1 changes the map (only
 * ever a destination); the rest is floor. Objects stop it where
 * FieldObj_DoObjCollision looks: Field_Obj_Secondary ($FFC300, 52 objects)
 * with render flag bit 3, where they stand and where their step is taking
 * them -- but not outside on Motavia or Dezolis. The leader reads the pad
 * only between steps (FieldObj_Move), so the walk holds the direction of the
 * step after the one under way, and lets go for the last. Bumping into an
 * object turns the leader to it (FieldObj_Collided), which is how the walk
 * faces what was clicked before pressing C.
 */
#include "walkto.h"
#include "bus.h"
#include "m68k.h"
#include "vdp.h"

#include <stdlib.h>
#include <string.h>

#define GAME_MODE    0xEF00
#define MODE_FIELD   0x0C
#define GAME_ROUTINE 0xEC20            /* $C: an event is running */
#define FIELD_MAP    0xEC28
#define USE_BG       0xEC24
#define VEHICLE      0xF43C
#define WIN_END      0xEC98            /* the end of the plane mappings a window saved: $6040 = none */
#define LEADER       0xC000
#define OBJ_SECOND   0xC300            /* Field_Obj_Secondary */
#define N_SECOND     52

static unsigned r8(unsigned a) { return m68k_ram[a & 0xFFFF]; }
static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a & 0xFFFF] << 8 | m68k_ram[(a + 1) & 0xFFFF]); }
static int wrapi(int v, int n) { v %= n; return v < 0 ? v + n : v; }

static int enabled = 1;
void walkto_set_enabled(int on) { enabled = on; if (!on) walkto_cancel(); }
int  walkto_enabled(void) { return enabled; }

/* the field, ready for the party to walk: no window, event, vehicle or battle */
static int field_free(void) {
    return r16(GAME_MODE) == MODE_FIELD && r16(GAME_ROUTINE) != 0x0C && !r16(VEHICLE) && r16(WIN_END) <= 0x6040;
}

/* ------------------------------------------------------------------ the map */
static int map_w, map_h, world, use_bg;                     /* map_w, map_h: in cells */

static void map_read(void) {
    use_bg = r8(USE_BG) != 0;
    map_w = ((int)r8(use_bg ? 0xEC66 : 0xEC64) + 1) * 2;
    map_h = ((int)r8(use_bg ? 0xEC67 : 0xEC65) + 1) * 2;
    world = (r16(FIELD_MAP) & 0xFFFE) == 0;                  /* outside on Motavia / Dezolis */
}

static int cell_code(int cx, int cy) {
    int x = wrapi(cx, map_w) * 16, y = wrapi(cy, map_h) * 16;
    unsigned stride = r8(use_bg ? 0xEC64 : 0xEC66) + 1, row = (unsigned)y >> 5;
    if (world) row &= 0x1F;
    unsigned c = r8((use_bg ? 0xB000u : 0xA000u) + row * stride + ((unsigned)x >> 5));
    unsigned a = 0x6000 + c * 32 + (x & 16 ? 4u : 0u) + (y & 16 ? 16u : 0u);
    return (r8(a) & 0x40 ? 1 : 0) | (r8(a + 2) & 0x40 ? 2 : 0) | (r8(a + 8) & 0x40 ? 4 : 0) | (r8(a + 10) & 0x40 ? 8 : 0);
}

/* for the field's effects (fieldfx.c): the walked layer's map and camera */
int walkto_field_map(int *w_cells, int *h_cells, int *cam_x, int *cam_y) {
    if (r16(GAME_MODE) != MODE_FIELD) return 0;
    map_read();
    *w_cells = map_w;
    *h_cells = map_h;
    int cx = (int)r16(use_bg ? 0xEF9C : 0xEF94), cy = (int)r16(use_bg ? 0xEF98 : 0xEF90);
    /* as the picture shows it: the camera in RAM is already the next frame's
     * while the party walks (the VDP takes it at the next VInt), a step
     * ahead; the walked layer's plane holds the map at its own coordinates,
     * so the plane's scroll at the view's top left says where it really is */
    int pwc, phc, px, py;
    vdp_plane_cells(&pwc, &phc);
    vdp_plane_xy(use_bg ? 1 : 0, 0, 0, &px, &py);
    int dx = (px - cx) & (pwc * 8 - 1), dy = (py - cy) & (phc * 8 - 1);
    if (dx >= pwc * 4) dx -= pwc * 8;
    if (dy >= phc * 4) dy -= phc * 8;
    *cam_x = cx + dx;
    *cam_y = cy + dy;
    return map_w > 0 && map_h > 0;
}
int walkto_cell_code(int cx, int cy) { return cell_code(cx, cy); }

/* an object's feet cell, and the cell its step is taking it to */
static void obj_cells(unsigned a, int *cx, int *cy, int *nx, int *ny) {
    int x = (int)r16(a + 0x30), y = (int)r16(a + 0x34) + 16;
    int sx = (int)(r16(a + 0x28) >> 8), sy = (int)(r16(a + 0x2A) >> 8);   /* px left in the step */
    if ((int16_t)r16(a + 0x20) < 0) sx = -sx;                              /* x_step_constant's sign */
    if ((int16_t)r16(a + 0x24) < 0) sy = -sy;
    *cx = wrapi(x >> 4, map_w);
    *cy = wrapi(y >> 4, map_h);
    *nx = wrapi((x + sx) >> 4, map_w);
    *ny = wrapi((y + sy) >> 4, map_h);
}

/* ------------------------------------------------------------------ the search */
#define WIN 128                                         /* cells around the leader searched (48 outside) */
static int win_w, win_h, org_x, org_y;
static uint8_t seen[WIN * WIN];                         /* 0, or 1 + the step's direction into the cell */
static uint16_t steps[WIN * WIN];
static uint8_t occupied[WIN * WIN];
static int32_t queue[WIN * WIN];

static const int DX[4] = {0, 0, -1, 1}, DY[4] = {-1, 1, 0, 0};             /* up, down, left, right */
static const uint8_t PADS[4] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};
static const unsigned FACING[4] = {4, 0, 0xC, 8};                         /* facing_dir for each */

static int to_win(int cx, int cy, int *x, int *y) {
    *x = wrapi(cx - org_x, map_w);
    *y = wrapi(cy - org_y, map_h);
    return *x < win_w && *y < win_h;
}

static void mark_objects(void) {
    memset(occupied, 0, (size_t)(win_w * win_h));
    if (world) return;                                  /* outside on the planets nobody blocks */
    for (int i = 0; i < N_SECOND; i++) {
        unsigned a = OBJ_SECOND + (unsigned)i * 0x40;
        if (!r16(a) || !(r8(a + 2) & 8)) continue;
        int cx, cy, nx, ny, x, y;
        obj_cells(a, &cx, &cy, &nx, &ny);
        if (to_win(cx, cy, &x, &y)) occupied[y * win_w + x] = 1;
        if (to_win(nx, ny, &x, &y)) occupied[y * win_w + x] = 1;
    }
}

/* the walk */
enum { WALK, FACE, PRESS };
static int active, phase, tcx, tcy, tslot, ncells, here, frames_still, replans, face_frames, press_frames;
static int last_x, last_y;
static unsigned map_at_start;
static int16_t path_x[WIN * WIN], path_y[WIN * WIN];   /* map cells, from where the leader is */

static unsigned slot_addr(int slot) { return LEADER + (unsigned)slot * 0x40; }

/* a path from cell (pcx, pcy): to the target cell, or the reachable cell
 * nearest it; to the side of the target object. 0: nowhere to go */
static int plan(int pcx, int pcy) {
    map_read();
    int lim = world ? 48 : WIN;
    win_w = map_w < lim ? map_w : lim;
    win_h = map_h < lim ? map_h : lim;
    org_x = pcx - win_w / 2;
    org_y = pcy - win_h / 2;
    mark_objects();
    int ox[2] = {-1, -1}, oy[2] = {-1, -1}, tx = -1, ty = -1, sx, sy;
    if (tslot >= 0) {                                    /* beside the object: where it is or is going */
        int a, b, c, d;
        obj_cells(slot_addr(tslot), &a, &b, &c, &d);
        if (!to_win(a, b, &ox[0], &oy[0])) ox[0] = -1;
        if (!to_win(c, d, &ox[1], &oy[1])) ox[1] = -1;
        for (int k = 0; k < 2; k++) if (ox[k] >= 0) occupied[oy[k] * win_w + ox[k]] = 1;
    } else if (!to_win(tcx, tcy, &tx, &ty)) tx = ty = -1;
    to_win(pcx, pcy, &sx, &sy);
    memset(seen, 0, (size_t)(win_w * win_h));
    int head = 0, tail = 0, start = sy * win_w + sx;
    seen[start] = 5;
    steps[start] = 0;
    queue[tail++] = start;
    int best = -1, best_score = 1 << 30;
    while (head < tail) {
        int i = queue[head++], x = i % win_w, y = i / win_w, score;
        if (tslot >= 0) {                                /* a cell beside the object */
            score = 1 << 30;
            for (int k = 0; k < 2; k++)
                if (ox[k] >= 0 && abs(ox[k] - x) + abs(oy[k] - y) == 1) score = steps[i];
        } else if (tx >= 0) {                            /* the cell, or as near it as the party gets */
            score = (abs(tx - x) + abs(ty - y)) * 4096 + steps[i];
        } else score = 1 << 30;
        if (score < best_score) { best_score = score; best = i; }
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= win_w || ny >= win_h) continue;
            int j = ny * win_w + nx, code = cell_code(org_x + nx, org_y + ny);
            if (seen[j] || occupied[j]) continue;
            if (code >= 8 && code <= 0xC) continue;      /* solid, water, sand, ice, a counter */
            if (code == 1 && !(nx == tx && ny == ty)) continue;   /* another map: only if clicked */
            seen[j] = (uint8_t)(d + 1);
            steps[j] = (uint16_t)(steps[i] + 1);
            queue[tail++] = j;
        }
    }
    if (best < 0 || best_score >= (1 << 30)) return 0;
    /* back from there to here */
    int n = steps[best] + 1;
    if (n > WIN * WIN) return 0;
    for (int i = best, k = n - 1; k >= 0; k--) {
        int x = i % win_w, y = i / win_w;
        path_x[k] = (int16_t)wrapi(org_x + x, map_w);
        path_y[k] = (int16_t)wrapi(org_y + y, map_h);
        if (k) { int d = seen[i] - 1; i = (y - DY[d]) * win_w + (x - DX[d]); }
    }
    ncells = n;
    here = 0;
    return 1;
}

/* ------------------------------------------------------------------ starting */
void walkto_cancel(void) { active = 0; }
int  walkto_active(void) { return active; }

/* a click while the field shows a window or an event's text: a button */
static uint8_t pulse_button;
static int pulse_frames;

int walkto_click_button(int right) {
    if (!enabled || r16(GAME_MODE) != MODE_FIELD) return 0;
    if (r16(WIN_END) <= 0x6040 && r16(GAME_ROUTINE) != 0x0C) return 0;   /* nothing up */
    active = 0;
    pulse_button = right ? PAD_B : PAD_C;
    pulse_frames = 3;
    return 1;
}

/* where the leader is: its feet cell, or where the step under way ends */
static void leader_cell(int *cx, int *cy, int *moving) {
    int a, b;
    obj_cells(LEADER, &a, &b, cx, cy);
    *moving = r16(LEADER + 0x28) || r16(LEADER + 0x2A);
}

int walkto_go(int cx, int cy, int slot) {
    active = 0;
    if (!enabled || !field_free()) return 0;
    map_read();
    int pcx, pcy, moving;
    leader_cell(&pcx, &pcy, &moving);
    tcx = wrapi(cx, map_w);
    tcy = wrapi(cy, map_h);
    tslot = slot;
    if (!plan(pcx, pcy)) return 0;
    if (ncells == 1 && slot < 0) return 0;               /* already there, or nowhere nearer */
    active = 1;
    phase = WALK;
    frames_still = replans = 0;
    last_x = (int)r16(LEADER + 0x30);
    last_y = (int)r16(LEADER + 0x34);
    map_at_start = r16(FIELD_MAP);
    return 1;
}

int walkto_click_screen(int sx, int sy) {
    if (!enabled || !field_free()) return 0;
    map_read();
    int wx = wrapi((int)r16(use_bg ? 0xEF9C : 0xEF94) + sx, map_w * 16);   /* the walked layer's camera */
    int wy = wrapi((int)r16(use_bg ? 0xEF98 : 0xEF90) + sy, map_h * 16);
    /* someone or something there: its sprite's 16 x 32 (feet the lower half) */
    int slot = -1, best = 1 << 30;
    for (int s = 5; s < 64; s++) {
        unsigned a = slot_addr(s);
        if (!r16(a) || (r8(a + 2) & 3)) continue;        /* gone, hidden, or fixed to the screen */
        int dx = wrapi(wx - (int)r16(a + 0x30), map_w * 16), dy = wrapi(wy - (int)r16(a + 0x34), map_h * 16);
        if (dx >= 16 || dy >= 32) continue;
        int d = abs(dx - 8) + abs(dy - 24);
        if (d < best) { best = d; slot = s; }
    }
    return walkto_go(wx >> 4, wy >> 4, slot);
}

int walkto_target(int *wx, int *wy) {
    if (!active) return 0;
    if (tslot >= 0) {
        *wx = (int)r16(slot_addr(tslot) + 0x30) + 8;
        *wy = (int)r16(slot_addr(tslot) + 0x34) + 24;
    } else {
        *wx = tcx * 16 + 8;
        *wy = tcy * 16 + 8;
    }
    return 1;
}

int walkto_target_screen(int *sx, int *sy) {
    int wx, wy;
    if (!walkto_target(&wx, &wy) || r16(GAME_MODE) != MODE_FIELD) return 0;
    map_read();
    int W = map_w * 16, H = map_h * 16;
    int dx = wrapi(wx - (int)r16(use_bg ? 0xEF9C : 0xEF94), W), dy = wrapi(wy - (int)r16(use_bg ? 0xEF98 : 0xEF90), H);
    if (dx > W / 2) dx -= W;                            /* the nearer way round */
    if (dy > H / 2) dy -= H;
    *sx = dx;
    *sy = dy;
    return 1;
}

/* ------------------------------------------------------------------ each frame */
uint8_t walkto_pad(uint8_t user, uint8_t pad) {
    if (pulse_frames > 0) {                              /* a click's button: held a few frames */
        if (user) pulse_frames = 0;
        else { pulse_frames--; return pulse_button; }
    }
    if (!active) return pad;
    if (user || !enabled) { active = 0; return pad; }    /* the player takes over */
    if (!field_free() || r16(FIELD_MAP) != map_at_start) { active = 0; return pad; }
    int x = (int)r16(LEADER + 0x30), y = (int)r16(LEADER + 0x34), pcx, pcy, moving;
    leader_cell(&pcx, &pcy, &moving);
    if (x != last_x || y != last_y) { frames_still = 0; last_x = x; last_y = y; }
    else if (++frames_still > 40) {                      /* stuck: someone in the way */
        frames_still = 0;
        if (++replans > 4 || !plan(pcx, pcy)) { active = 0; return 0; }
        phase = WALK;
    }
    if (phase == WALK) {
        int k = -1;
        for (int i = here; i < ncells && k < 0; i++)
            if (path_x[i] == pcx && path_y[i] == pcy) k = i;
        if (k < 0) {                                     /* off the path: from here, again */
            if (++replans > 8 || !plan(pcx, pcy)) { active = 0; return 0; }
            k = 0;
        }
        here = k;
        if (k + 1 < ncells) {
            for (int d = 0; d < 4; d++)
                if (wrapi(path_x[k] + DX[d], map_w) == path_x[k + 1] && wrapi(path_y[k] + DY[d], map_h) == path_y[k + 1])
                    return PADS[d];
            active = 0;
            return 0;
        }
        if (moving) return 0;                            /* the last step: let it finish */
        if (tslot < 0) { active = 0; return 0; }         /* there */
        phase = FACE;
        face_frames = 0;
    }
    if (phase == FACE) {                                 /* turn to the object, then press C */
        unsigned a = slot_addr(tslot);
        int ocx, ocy, onx, ony;
        if (!r16(a)) { active = 0; return 0; }
        obj_cells(a, &ocx, &ocy, &onx, &ony);
        int dir = -1;
        for (int d = 0; d < 4; d++)
            if (wrapi(pcx + DX[d], map_w) == ocx && wrapi(pcy + DY[d], map_h) == ocy) dir = d;
        if (dir < 0) {                                   /* it moved on: follow it */
            if (++replans > 8 || !plan(pcx, pcy)) { active = 0; return 0; }
            phase = WALK;
            return 0;
        }
        if (!moving && (r16(LEADER + 6) == FACING[dir] || ++face_frames > 12)) {
            phase = PRESS;
            press_frames = 0;
        } else return PADS[dir];
    }
    if (++press_frames <= 3) return PAD_C;               /* talk, open, examine */
    active = 0;
    return 0;
}

/* ------------------------------------------------------------------ diagonal steps */
/* FieldObj_Move begins a step when a party member stands still: the leader's
 * from the pad (up 1, down 2, left 4, right 8; with two held it goes left or
 * right), the others' towards the place the one ahead of them has just left.
 * A step is a distance left to go and a speed, for each axis, and
 * FieldObj_UpdateStepDuration, the camera and FieldObj_UpdatePosition take
 * each axis on its own every frame: a step across AND up or down only needs
 * both set. The hooks:
 *
 *  psiv_field_step_dir, before the leader's collision check (d0 = the pad's
 *  directions). With a diagonal held, the step will be diagonal if the cell
 *  across, the cell up or down and the cell between them are all plain floor
 *  with nobody on them -- so no wall's corner is cut, and doors, exits, shop
 *  counters and people are still met straight on. With the way across walled
 *  off and the other way not, d0 becomes up or down: along the wall. Else d0
 *  is left alone and the game does what it always did.
 *
 *  psiv_field_step_begin, as a step begins (a4 = whose). The leader's, when
 *  the first hook chose a diagonal: the game has begun the step across, and
 *  the same distance and speed are set up or down, and the leader's
 *  destination moved a cell. A follower's, when the place it is going to is
 *  a cell away both ways (the one ahead stepped diagonally): the other axis
 *  is added in the same way, so the line follows the leader's path.
 *
 * Every speed the game has (1, 2 and 4 pixels a frame) divides the cell, so
 * both axes end on the same frame, on the grid. Events move the party by
 * the same routine: left alone ($FFEC20 = $C). Vehicles have their own. */
#define ROUTINE_EVENT    0x0C
#define TILE_FLAG        0xECE1         /* Tile_Coll_Run_Event_Flag: bit 0 = the debug window's "no collision" */
#define NO_OBJ_COLLISION 0xECF6         /* set: FieldObj_Move skips FieldObj_DoObjCollision */
#define TILE_HANDLERS    0x45D72u       /* TileCollNormalPtrs: what walking into a cell does, by its code */
#define TILE_HANDLERS_NC 0x45DB2u       /* TileCollDisabledPtrs */
#define TILE_EMPTY       0x45ED8u       /* TileColl_Empty: floor, nothing happens */
#define TILE_SOLID       0x45F16u       /* TileColl_Solid */
#define PARTY_END        0xC180         /* FieldObj_Move tells the next object where to follow, below this */
#define STEP_CELL        0x1000         /* a step's distance: 16 pixels (8.8) */

static int diagonal;                    /* the option */
static int step_v;                      /* from the first hook to the second: the leader's step also goes up (1) or down (2) */

void walkto_set_diagonal(int on) { diagonal = on; }
int  walkto_diagonal(void) { return diagonal; }

static uint32_t r32(unsigned a) { return (uint32_t)r16(a) << 16 | r16(a + 2); }
static void w16(unsigned a, unsigned v) {
    m68k_ram[a & 0xFFFF] = (uint8_t)(v >> 8);
    m68k_ram[(a + 1) & 0xFFFF] = (uint8_t)v;
}
static void w32(unsigned a, uint32_t v) { w16(a, v >> 16); w16(a + 2, v & 0xFFFF); }

/* what walking into a cell does: the game's own handler for its code */
static uint32_t cell_handler(int cx, int cy) {
    uint32_t t = (r8(TILE_FLAG) & 1 ? TILE_HANDLERS_NC : TILE_HANDLERS) + 4u * (unsigned)cell_code(cx, cy);
    if (t + 3 >= m68k_rom_size) return 0;
    return (uint32_t)m68k_rom[t] << 24 | (uint32_t)m68k_rom[t + 1] << 16 | (uint32_t)m68k_rom[t + 2] << 8 | m68k_rom[t + 3];
}

/* somebody stands on a cell or is stepping onto it (the objects FieldObj_DoObjCollision looks at) */
static int cell_taken(int cx, int cy) {
    if (world || r8(NO_OBJ_COLLISION)) return 0;
    cx = wrapi(cx, map_w);
    cy = wrapi(cy, map_h);
    for (int i = 0; i < N_SECOND; i++) {
        unsigned a = OBJ_SECOND + (unsigned)i * 0x40;
        if (!r16(a) || !(r8(a + 2) & 8)) continue;
        int ax, ay, nx, ny;
        obj_cells(a, &ax, &ay, &nx, &ny);
        if ((ax == cx && ay == cy) || (nx == cx && ny == cy)) return 1;
    }
    return 0;
}

static int cell_floor(int cx, int cy) { return cell_handler(cx, cy) == TILE_EMPTY && !cell_taken(cx, cy); }

void psiv_field_step_dir(void) {
    step_v = 0;
    if (!diagonal) return;
    unsigned d = D(0) & 0x0F, h = d & 0x0C, v = d & 0x03;
    if ((h != 4 && h != 8) || (v != 1 && v != 2)) return;       /* not two directions, one of each */
    map_read();
    int cx, cy, nx, ny;
    obj_cells(LEADER, &cx, &cy, &nx, &ny);
    int dx = h == 4 ? -1 : 1, dy = v == 1 ? -1 : 1;
    if (cell_floor(cx + dx, cy)) {
        if (cell_floor(cx, cy + dy) && cell_floor(cx + dx, cy + dy)) step_v = (int)v;
        return;                                                 /* else across, as the game has it */
    }
    /* the way across is a wall or somebody: up or down instead, if that isn't a wall too */
    if ((cell_handler(cx + dx, cy) == TILE_SOLID || cell_taken(cx + dx, cy)) && cell_handler(cx, cy + dy) != TILE_SOLID)
        D(0) = (D(0) & 0xFFFF0000u) | v;
}

/* the same distance and speed as the axis the game set, on the other one */
static void add_axis(unsigned a, int x_axis, int negative) {
    unsigned from = x_axis ? 0x24u : 0x20u, to = x_axis ? 0x20u : 0x24u;
    uint32_t speed = r32(a + from);
    if (speed & 0x80000000u) speed = 0u - speed;
    w16(a + (x_axis ? 0x28u : 0x2Au), STEP_CELL);
    w32(a + to, negative ? 0u - speed : speed);
}

/* how far a follower's place is, the short way round a map that wraps */
static int cells_away(unsigned to, unsigned from, int size) {
    int d = (int16_t)(uint16_t)(to - from);
    if (d > size / 2) d -= size;
    else if (d < -size / 2) d += size;
    return d;
}

void psiv_field_step_begin(void) {
    int v = step_v;
    step_v = 0;
    if (!diagonal || r16(GAME_ROUTINE) == ROUTINE_EVENT) return;
    unsigned a = A(4) & 0xFFFF, xd = r16(a + 0x28), yd = r16(a + 0x2A);
    if (a == LEADER) {
        if (!v || xd != STEP_CELL || yd || !r32(a + 0x20)) return;      /* (the game's step: one cell across) */
        add_axis(a, 0, v == 1);
        w16(a + 0x3A, r16(a + 0x3A) + (v == 1 ? 0xFFF0u : 0x0010u));    /* its destination: a cell up or down too */
        return;
    }
    if (a < LEADER || a >= PARTY_END) return;
    map_read();
    int dx = cells_away(r16(a + 0x38), r16(a + 0x30), map_w * 16), dy = cells_away(r16(a + 0x3A), r16(a + 0x34), map_h * 16);
    if (abs(dx) != 16 || abs(dy) != 16) return;
    if (xd == STEP_CELL && !yd && r32(a + 0x20)) add_axis(a, 0, dy < 0);
    else if (yd == STEP_CELL && !xd && r32(a + 0x24)) add_axis(a, 1, dx < 0);
}
