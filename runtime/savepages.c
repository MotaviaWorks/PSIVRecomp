/*
 * More save slots: pages of three (savepages.h).
 *
 * The cartridge (the odd bytes of $200001-$203FFF, bus_sram):
 *   $000-$0FF  the header, the game's RAM $FFF000-$FFF0FF as it was saved:
 *              "PHANTASY STAR 4" from byte 2, the slot last used at $13, and
 *              each slot's checksum, a word, from $14
 *   $100 + $A00 n   slot n: RAM $FFF100-$FFFAFF (TransferToSRAM, loc_64E62)
 * A slot holds a save when the sum of its bytes is not 0 and is the
 * header's (loc_64D8A); the title sets a bit of $FFED59 for each that
 * does not, and its windows will not take those.
 */
#include "savepages.h"
#include "bus.h"
#include "frame.h"
#include "m68k.h"

#include <stdio.h>
#include <string.h>

#define JOY_PRESSED  0xEF03u          /* Joypad_Pressed: 4 Left, 8 Right, $F0 the buttons */
#define OPTION_2     0xEC9Fu          /* Window_Option_Index_2: the slot the cursor is on */
#define INIT_FLAG    0xEC9Bu          /* Window_Init_Flag: bit 0 = the window in front has listed itself */
#define BAD_SLOTS    0xED59u          /* a bit for each slot with no save in it */
#define SOUND_INDEX  0x500Au
#define SFX_CURSOR   0xF2
#define CURSOR       0xC300u          /* Field_Obj_Secondary: the Continue window's cursor (FieldObj_SaveSlotCursor) */
#define RAM_SUMS     0xF014u          /* SRAM_Bytes_Sum */
#define SUMS         0x14
#define SLOT_0       0x100
#define SLOT_SIZE    0xA00
#define SLOT_WINDOWS 0x43B18u, 0x43D20u, 0x43F28u     /* each draws one slot's window from the cartridge */
#define SLOT_OBJECTS 0xC340u          /* the parties standing in those windows: 15 objects of $40 */
#define RENDER_MODE  0xEC9Cu          /* Window_Render_Mode: bit 0 = at once, no opening */
#define WINDOW_CREATE    0x685B6u     /* d0 = which */
#define WINDOW_DESTROY   0x687CCu     /* the one opened last */
#define UPDATE_OBJECTS   0x447D8u     /* Field_UpdateObjects */
#define UPLOAD_PLANE_A   0x41682u     /* DMAPlane_A_VInt: the windows to the screen, at the next frame */
#define LOAD_TILES       0x69A64u     /* LoadWindowTiles: a0 = the text, d0, d1 = the cell, d2 = $C000, d3 = 0, d4 = 1 */
#define TILE_BUFFER      0xE220u      /* Win_Tile_Buffer: the game's own scratch for such text */
#define WINDOW_INDEX     0xEC90u
#define WINDOW_GROUP     0xEC94u      /* Win_Group_Start_Addr: 8 bytes a window, its cell at +2, +3 */
#define LIST_CELLS       4            /* the Save window's list: "LV" and a level of two digits, from its sixth cell */

enum { NONE, LIST, PICK };

static int pages = 1, page;
static int (*go_fn)(int page);
static uint64_t shown_at;             /* the frame a window of slots last waited */
static int shown;

void savepages_set(int n, int (*go)(int page)) {
    pages = n < 1 ? 1 : n > SAVEPAGES_MAX ? SAVEPAGES_MAX : n;
    go_fn = go;
}
void savepages_set_page(int p) { page = p < 0 || p >= SAVEPAGES_MAX ? 0 : p; }
int  savepages_page(void) { return page; }
int  savepages_count(void) { return go_fn ? pages : 1; }

static int on(void) { return go_fn && pages > 1; }

static int turn_to(int p) {
    if (p == page) return 1;
    if (!go_fn || !go_fn(p)) return 0;
    page = p;
    return 1;
}

int savepages_saved(const unsigned char *c, int k) {
    if (k < 0 || k > 2) return 0;
    const uint8_t *s = &c[SLOT_0 + k * SLOT_SIZE];
    unsigned sum = 0;
    for (int i = 0; i < SLOT_SIZE; i++) sum += s[i];
    sum &= 0xFFFF;
    return sum && sum == (unsigned)(c[SUMS + 2 * k] << 8 | c[SUMS + 2 * k + 1]);
}

static int slot_saved(int k) { return savepages_saved(bus_sram, k); }

/* Continue from the launcher (savepages.h) */
#define GAME_MODE     0xEF00u         /* Game_Mode_Index: 0 the Sega logo, 4 the title, 8 on: the game */
#define CONTINUE_GIVE_UP 6000         /* frames: far more than the logo and the title take */
static int cont_slot = -1;            /* the slot to take: -1 none, or taken */
static int cont_on, cont_frames;
static void (*saved_fn)(int page, int slot);

void savepages_on_save(void (*fn)(int page, int slot)) { saved_fn = fn; }

void savepages_continue(int slot) {
    cont_slot = slot;
    cont_on = slot >= 0 && slot <= 2;
    cont_frames = 0;
}

int savepages_continuing(void) {
    if (!cont_on) return 0;
    unsigned mode = (unsigned)(m68k_ram[GAME_MODE] << 8 | m68k_ram[GAME_MODE + 1]);
    if ((cont_slot < 0 && mode >= 8) || ++cont_frames > CONTINUE_GIVE_UP) {   /* in the game; or something is wrong */
        cont_on = 0;
        cont_slot = -1;
    }
    return cont_on;
}

unsigned char savepages_pad(void) {
    return cont_on && cont_slot >= 0 && (cont_frames & 1) ? 0x80 : 0;      /* Start, pressed anew each time */
}

static int page_has_saves(void) {
    if (memcmp(&bus_sram[2], "PHANTASY STAR 4", 15)) return 0;
    return slot_saved(0) || slot_saved(1) || slot_saved(2);
}

/* The title's menu: "Continue" is offered only if the cartridge holds a save.
 * So if the page in memory has none and another has, that one. (With the
 * pages off -- Classic -- always the first.) */
void psiv_savepage_title(void) {
    if (cont_on && cont_slot >= 0 && !slot_saved(cont_slot)) {   /* Continue, and no save there after all: */
        cont_on = 0;                                             /* the title, as it is */
        cont_slot = -1;
    }
    if (!go_fn) return;
    if (cont_on) return;                              /* Continue: the page it was started with */
    if (pages <= 1) { turn_to(0); return; }
    if (page_has_saves()) return;
    int first = page;
    for (int i = 1; i < pages; i++)
        if (turn_to((first + i) % pages) && page_has_saves()) return;
    turn_to(0);                                       /* no saves anywhere: a new game on the first page */
}

static void call(uint32_t routine) { CALL_IND(routine, 0x435B4u); }

/* the Continue window's cursor on a slot: the slot, the cursor's frame and
 * where it stands */
static void m_cursor_to(int slot) {
    m68k_ram[OPTION_2] = (uint8_t)slot;
    m68k_ram[CURSOR + 0x06] = 0;
    m68k_ram[CURSOR + 0x07] = (uint8_t)(slot * 4);
    m68k_ram[CURSOR + 0x30] = (uint8_t)((0x10 + slot * 0x60) >> 8);
    m68k_ram[CURSOR + 0x31] = (uint8_t)(0x10 + slot * 0x60);
}

/* Continue and Erase: three windows side by side, the cursor on one; Left
 * and Right move it, and stop at the ends. There the page turns: the
 * cursor comes in at the other end, the three windows show the new page's
 * slots, and $FFED59 says which of them can be taken. */
void psiv_savepage_pick(void) {
    if (cont_on && cont_slot >= 0) {                  /* Continue: this slot, and Start pressed on it */
        m_cursor_to(cont_slot);
        m68k_ram[JOY_PRESSED] = 0x80;
        cont_slot = -1;
        return;
    }
    if (!on()) return;
    shown_at = frame_count;
    shown = PICK;
    unsigned pad = m68k_ram[JOY_PRESSED], slot = m68k_ram[OPTION_2];
    int dir = (pad & 0x08) && slot == 2 ? 1 : (pad & 0x04) && slot == 0 ? -1 : 0;
    if (!dir || (pad & 0xF0)) return;
    if (m68k_ram[CURSOR + 0x20] | m68k_ram[CURSOR + 0x21] | m68k_ram[CURSOR + 0x22] | m68k_ram[CURSOR + 0x23])
        return;                                       /* the cursor is still sliding */
    if (!turn_to((page + dir + pages) % pages)) return;
    m68k_ram[JOY_PRESSED] &= (uint8_t)~0x0C;          /* the press is spent: the cursor's own code does not see it */
    m_cursor_to(dir > 0 ? 0 : 2);
    m68k_ram[BAD_SLOTS] = (uint8_t)((!slot_saved(0)) | (!slot_saved(1)) << 1 | (!slot_saved(2)) << 2);
    m68k_ram[SOUND_INDEX] = SFX_CURSOR;
    /* the three windows afresh, as Title_ContinueOption makes them: the
     * parties standing in them gone, the windows closed and opened (at
     * once: cleared), each drawn from the cartridge, and to the screen */
    memset(&m68k_ram[SLOT_OBJECTS], 0, 15 * 0x40);
    for (int i = 0; i < 3; i++) {
        m68k_ram[RENDER_MODE] |= 0x01;
        call(WINDOW_DESTROY);
    }
    for (int i = 0; i < 3; i++) {
        D(0) = (uint32_t)i;
        m68k_ram[RENDER_MODE] |= 0x01;
        call(WINDOW_CREATE);
    }
    static const uint32_t windows[3] = {SLOT_WINDOWS};
    for (int i = 0; i < 3; i++) call(windows[i]);
    call(UPDATE_OBJECTS);
    call(UPLOAD_PLANE_A);
}

/* The Save window's list: each row a number and a box, and for a slot with
 * a save "LV" and the level after them (loc_594F2, loc_595EA) -- written
 * when there is one, never taken away. So before another page is listed:
 * spaces over that part of each row. */
static void list_clear(void) {
    uint32_t group = (uint32_t)m68k_ram[WINDOW_GROUP] << 24 | (uint32_t)m68k_ram[WINDOW_GROUP + 1] << 16
                   | (uint32_t)m68k_ram[WINDOW_GROUP + 2] << 8 | m68k_ram[WINDOW_GROUP + 3];
    uint32_t win = group + ((uint32_t)(m68k_ram[WINDOW_INDEX] << 8 | m68k_ram[WINDOW_INDEX + 1]) << 3);
    int x = (int)rd8(win + 2) + 5, y = (int)rd8(win + 3) + 1, n = LIST_CELLS;
    uint32_t d[5] = {D(0), D(1), D(2), D(3), D(4)}, a0 = A(0);
    memset(&m68k_ram[TILE_BUFFER], 0, (size_t)n);      /* 0: a space; $FE: the end */
    m68k_ram[TILE_BUFFER + n] = 0xFE;
    for (int k = 0; k < 3; k++) {
        D(0) = (uint32_t)x;
        D(1) = (uint32_t)(y + 2 * k);
        D(2) = 0xC000;
        D(3) = 0;
        D(4) = 1;
        A(0) = 0xFFFF0000u | TILE_BUFFER;
        call(LOAD_TILES);
    }
    for (int i = 0; i < 5; i++) D(i) = d[i];
    A(0) = a0;
}

/* Save: a list, Up and Down. Left and Right turn the page; the window then
 * lists its slots afresh (its own first pass, as after "overwrite? No"). */
void psiv_savepage_list(void) {
    if (!on()) return;
    shown_at = frame_count;
    shown = LIST;
    unsigned pad = m68k_ram[JOY_PRESSED];
    int dir = pad & 0x08 ? 1 : pad & 0x04 ? -1 : 0;
    if (!dir || (pad & 0xF0)) return;
    if (!turn_to((page + dir + pages) % pages)) return;
    m68k_ram[JOY_PRESSED] &= (uint8_t)~0x0C;
    list_clear();
    m68k_ram[INIT_FLAG] &= (uint8_t)~0x01;
    m68k_ram[SOUND_INDEX] = SFX_CURSOR;
}

/* A save writes the header from the game's RAM, the other slots' checksums
 * with it -- as they were when the game last read a header, which may have
 * been another page's (or a moment the player has since rewound past). So:
 * the checksums of the page being written, first. */
void psiv_savepage_store(void) {
    if (saved_fn) saved_fn(page, m68k_ram[0xF013] & 3);   /* Save_Slot: the launcher's Continue takes the last save made */
    if (!on()) return;
    memcpy(&m68k_ram[RAM_SUMS], &bus_sram[SUMS], 6);
}

int savepages_note(char *out, size_t n) {
    if (!on() || !shown || frame_count > shown_at + 2) return 0;
    snprintf(out, n, "Saves %d to %d of %d   %s", page * 3 + 1, page * 3 + 3, pages * 3,
             shown == LIST ? "Left / Right: more" : "Left / Right past the ends: more");
    return 1;
}
