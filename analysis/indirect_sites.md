# PSIV indirect jumps

401 indirect jmp/jsr sites, 4722 resolved target edges; 180 `trap #2` call sites.

## By kind

| kind | sites |
|---|---|
| pc_branch_table | 271 |
| pc_offset_table | 30 |
| lea_table | 22 |
| lea_pointer_table | 20 |
| register_param | 18 |
| lea_table_const_index | 17 |
| offset_table | 11 |
| pc_pointer_table | 5 |
| object_field_pointer | 4 |
| offset_table_param | 1 |
| pointer_table_param | 1 |
| unresolved | 1 |

## By confidence

| confidence | sites |
|---|---|
| high | 374 |
| none | 21 |
| low | 4 |
| medium | 2 |

`trap #2` callers: high 180

## Needs attention (none / low / BAD / unresolved / param)

| addr | func | insn | kind | notes |
|---|---|---|---|---|
| $21C | Trap02Exception | `jsr (a0)` | offset_table_param | a0 from: adda.w (a0,d0.w), a0; table base passed in by caller |
| $9EA | Battle_UpdateFighters | `jsr (a0)` | pointer_table_param | a0 from: movea.l (a0,d0.w), a0; pointer-table base passed in by caller |
| $2FB4E | loc_2FB4A | `jsr (a0)` | object_field_pointer | a0 from: movea.l $34(a4), a0; candidates = every 'move.l #code,$34(aN)' in the game (over-approximation; needs runtime confirmation) |
| $31896 | loc_3187E | `jmp (a3)` | object_field_pointer | a3 from: movea.l $38(a4), a3; candidates = every 'move.l #code,$38(aN)' in the game (over-approximation; needs runtime confirmation) |
| $417CA | Nem_PCD_WritePixel | `jmp (a3)` | register_param | a3 not set in this block: passed in by caller |
| $53632 | loc_53632 | `jsr (a3,d5.w)` | register_param | a3 not set in this block: passed in by caller |
| $5661C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5662C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5663C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5664C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5665C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5666C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5667C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5668C | RunEvent_VahFortConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5677A | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5678A | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $5679A | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $567AA | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $567BA | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $567CA | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $567DA | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $567EA | RunEvent_WpnPlntConveyorBelt | `jsr (a1,d4.w)` | register_param | a1 not set in this block: passed in by caller |
| $57DA8 | MenuOptions_GetOption | `jmp (a0)` | unresolved | a0 from: adda.l d1, a0 |
| $5C0D0 | loc_5C0CC | `jmp (a1)` | object_field_pointer | a1 from: movea.l $2(a0), a1; candidates = every 'move.l #code,$2(aN)' in the game (over-approximation; needs runtime confirmation) |
| $697E2 | Window_Update | `jmp (a0)` | object_field_pointer | a0 from: movea.l $4(a0), a0; candidates = every 'move.l #code,$4(aN)' in the game (over-approximation; needs runtime confirmation) |
