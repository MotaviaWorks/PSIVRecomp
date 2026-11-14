/*
 * Auto save (Enhanced): the game saved for the player as they go, in a
 * battery save of its own apart from the cartridge's slots -- a thirteenth
 * save the game's windows never list; the launcher's Continue and Load game
 * take it.
 *
 * The game's own save (SaveGame, TransferToSRAM in the disassembly) notes
 * where the party stands -- the map and the leader's place -- in its saved
 * block, and copies that block, RAM $FFF100-$FFFAFF, to a slot of the
 * cartridge with the header ($FFF000-$FFF0FF: the game's name, the slot,
 * each slot's checksum). So a save can be made without the game's menu:
 * the same block as it is in RAM, with the same four words noted in the
 * copy. Nothing of the game's RAM is written, and the cartridge in memory
 * (bus_sram) is not touched: the game goes on exactly as it would have.
 *
 * It is made only where the game itself would let the player save: on the
 * field with the party free and standing on a cell -- no window, no event,
 * no battle, not in a vehicle -- and not in a dungeon (the game refuses
 * there: Win_System). When: once the party has come to another map (into a
 * town, out of one), and every few minutes of walking the world.
 */
#ifndef PSIV_AUTOSAVE_H
#define PSIV_AUTOSAVE_H

#include <stdint.h>

#define AUTOSAVE_SIZE 0x2000          /* a cartridge in the port's layout: its header and slot 1 */

void autosave_set_enabled(int on);
int  autosave_enabled(void);

/* Once a frame, after the game's. 1 when a save is due and could be made:
 * `out` (AUTOSAVE_SIZE bytes) then holds it, for the front end to write. */
int  autosave_frame(uint8_t *out);

/* The same save made now, whether one is due or not: 1 if the game could be
 * saved at this moment (testing; a save on demand). */
int  autosave_compose(uint8_t *out);

/* the console was reset, or a save was loaded: the next one waits for the
 * party to move on */
void autosave_reset(void);

#endif
