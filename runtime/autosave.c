/*
 * Auto save: see autosave.h.
 */
#include "autosave.h"
#include "m68k.h"

#include <string.h>

/* RAM ($FFxxxx); see the disassembly's ps4.constants.asm */
#define GAME_MODE     0xEF00u         /* Game_Mode_Index: $C = the field */
#define FIELD_ROUTINE 0xEC20u         /* 0 = the party walks free (4 the camp menu, $C an event, $10 the Start menu) */
#define WIN_END       0xEC98u         /* past $6040: a window is open */
#define WINDOWS_OPEN  0xEC9Au
#define VEHICLE       0xF43Cu         /* Vehicle_Index */
#define NO_SAVE_HERE  0xED51u         /* Dungeon_Teleport_Index: not 0 in a dungeon, where the game will not save */
#define FIELD_MAP     0xEC28u         /* Field_Map_Index, Field_Map_Index_2 */
#define FIELD_MAP_2   0xEC2Au
#define LEADER_X      0xC030u         /* Character_1's place, pixels (the high words of curr_x_pos, curr_y_pos) */
#define LEADER_Y      0xC034u
#define HEADER        0xF000u         /* SRAM_Buffer: $100 bytes */
#define SAVE_BLOCK         0xF100u         /* Event_Flags .. the vehicles' stats: $A00 bytes */
#define SAVE_BLOCK_SIZE    0xA00
/* in the block: where the party stands, as SaveGame notes it */
#define SAVED_MAP_2   0x302           /* Saved_Field_Map_Index_2 ($FFF402) */
#define SAVED_MAP     0x304
#define SAVED_X       0x306
#define SAVED_Y       0x308

#define SETTLED       30              /* frames the party has been free before a save is made */
#define EVERY         (5 * 60 * 60)   /* frames: a save every five minutes, if the party has moved */

static int enabled;
static int free_frames;               /* how long the party has been free to walk */
static int seen_map = -1;             /* the map number as last seen */
static int moving_on;                 /* it has changed and the game has yet to take the party there: frames so far */
static int last_map = -1;             /* the map the last save was made on (-1: none yet since the start) */
static unsigned since;                /* frames since it */
static uint8_t last_block[SAVE_BLOCK_SIZE];

void autosave_set_enabled(int on) { enabled = on; }
int  autosave_enabled(void) { return enabled; }

void autosave_reset(void) {
    free_frames = 0;
    seen_map = -1;
    moving_on = 0;
    last_map = -1;
    since = 0;
}

static unsigned r16(unsigned a) { return (unsigned)(m68k_ram[a] << 8 | m68k_ram[a + 1]); }

/* the places the game's own Start menu will not save in (Win_System): a
 * dungeon, and five maps besides */
static int no_save_map(unsigned map) {
    return map == 0x15 || map == 0x16 || map == 0x17 || map == 0xFE || map == 0x16F;
}

/* the party is free on the field, where the game would save */
static int field_free(void) {
    return r16(GAME_MODE) == 0x0C && r16(FIELD_ROUTINE) == 0 && r16(WIN_END) <= 0x6040 && !m68k_ram[WINDOWS_OPEN]
        && !r16(VEHICLE) && !m68k_ram[NO_SAVE_HERE] && !no_save_map(r16(FIELD_MAP));
}

/* ... and standing on a cell (between two steps) */
static int on_cell(void) { return !(r16(LEADER_X) & 15) && !(r16(LEADER_Y) & 15); }

static void compose(uint8_t *out) {
    memset(out, 0, AUTOSAVE_SIZE);
    uint8_t *h = out, *b = out + 0x100;
    memcpy(h, &m68k_ram[HEADER], 0x100);
    memcpy(h + 2, "PHANTASY STAR 4", 15);                  /* PS4String, as TransferToSRAM writes it */
    h[0x12] = h[0x13] = 0;                                 /* Save_Slot: the first */
    memset(h + 0x14, 0, 6);                                /* the other two slots: no save */
    memcpy(b, &m68k_ram[SAVE_BLOCK], SAVE_BLOCK_SIZE);
    memcpy(b + SAVED_MAP_2, &m68k_ram[FIELD_MAP_2], 2);    /* where the party stands (SaveGame) */
    memcpy(b + SAVED_MAP, &m68k_ram[FIELD_MAP], 2);
    memcpy(b + SAVED_X, &m68k_ram[LEADER_X], 2);
    memcpy(b + SAVED_Y, &m68k_ram[LEADER_Y], 2);
    unsigned sum = 0;                                      /* SumSRAMBytes: the block's bytes, 16 bits */
    for (int i = 0; i < SAVE_BLOCK_SIZE; i++) sum += b[i];
    h[0x14] = (uint8_t)(sum >> 8);
    h[0x15] = (uint8_t)sum;
}

int autosave_compose(uint8_t *out) {
    if (!field_free() || !on_cell()) return 0;
    compose(out);
    return 1;
}

int autosave_frame(uint8_t *out) {
    if (!enabled) { free_frames = 0; return 0; }
    if (since < 0x7FFFFFFFu) since++;
    if (r16(GAME_MODE) < 8) { autosave_reset(); return 0; }   /* the logo, the title: another save may be loaded,
                                                               * which is not to be saved over this one at once */
    /* Another map: the game sets its number, fades the picture out with the party still standing
     * on the old one (free, as far as RAM says, for a quarter of a second), then loads it and
     * places them. A save between would hold the new map and the old place: so none until the
     * game has been away from the field and come back (or, if it never leaves it, two seconds). */
    int map = (int)r16(FIELD_MAP);
    if (map != seen_map) {
        if (seen_map >= 0) moving_on = 1;
        seen_map = map;
    }
    if (!field_free()) { free_frames = 0; moving_on = 0; return 0; }
    if (moving_on) {
        if (++moving_on <= 120) { free_frames = 0; return 0; }
        moving_on = 0;
    }
    if (free_frames < SETTLED) { free_frames++; return 0; }
    if (last_map < 0) {                                    /* the start, or a save just loaded: from here on */
        last_map = map;
        since = 0;
        memcpy(last_block, &m68k_ram[SAVE_BLOCK], SAVE_BLOCK_SIZE);
        return 0;
    }
    if (map == last_map && since < EVERY) return 0;        /* not due */
    if (!on_cell()) return 0;                              /* mid-step: at its end */
    compose(out);
    last_map = map;
    since = 0;
    if (!memcmp(last_block, out + 0x100, SAVE_BLOCK_SIZE)) return 0;   /* nothing has changed since the last */
    memcpy(last_block, out + 0x100, SAVE_BLOCK_SIZE);
    return 1;
}
