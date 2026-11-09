/*
 * Cheats (Settings > Cheats; all off unless chosen). They work on the game's
 * RAM between frames, never on its code (names from ps4.constants.asm):
 *
 *   Infinite HP / TP   each character's curr_hp / curr_tp (Character_Stats
 *                      $FFF500, $80 a character, +$E / +$12) kept at max_hp /
 *                      max_tp (+$10 / +$14) -- the living only. A single blow
 *                      bigger than someone's whole HP still fells them.
 *   No random battles  Random_Battles_Flag ($FFECE7), which each map sets,
 *                      kept clear (RunRandomBattles then does nothing); put
 *                      back when the cheat is turned off on the same map.
 *   Max meseta         Current_Money ($FFF438) kept at 999,999.
 *   Experience x2, x4  in a battle, what a character's exp (+$A) gains is
 *                      added again (once or three times); levels come from
 *                      it as the game next checks them.
 *   Enemies at 1 HP    in a battle, each enemy fighter's (Obj_Fighters
 *                      $FF4400, 5-8) HP kept at 1 at most: one blow each.
 */
#ifndef PSIV_CHEATS_H
#define PSIV_CHEATS_H

enum { CHEAT_HP, CHEAT_TP, CHEAT_NO_BATTLES, CHEAT_MONEY, CHEAT_EXP, CHEAT_WEAK, CHEATS };

void cheats_set(int cheat, int value);   /* 0 off; for CHEAT_EXP the multiplier (1, 2, 4) */
int  cheats_get(int cheat);
void cheats_frame(void);                 /* once a frame, before it runs */

#endif
