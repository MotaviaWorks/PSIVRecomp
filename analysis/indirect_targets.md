# PSIV indirect jumps (resolve.py)

401 indirect sites, 180 `trap #2` callers; 101423/102494 listed instructions reachable from the vectors; 128 unreferenced code runs (1071 instructions) compiled anyway; 3925 functions; 3 discovery rounds.

Function entry points by reason: indirect 2449, data 2103, call 2010, unreferenced 128, imm 35, vector 6, ram_vector 2

| confidence | sites |
|---|---|
| high | 396 |
| low | 4 |
| medium | 1 |

| kind | sites |
|---|---|
| pc_branch_table | 271 |
| table_const_index | 33 |
| pc_offset_table | 30 |
| pointer_table | 26 |
| register_table | 23 |
| offset_table | 12 |
| object_field | 4 |
| trap2_dispatch | 1 |
| constant+ram_pointer | 1 |

`trap #2` callers: high 180

## Not high confidence

| addr | function | insn | kind | conf | targets | notes |
|---|---|---|---|---|---|---|
| $2FB4E | loc_2FB0E | `jsr	(a0)` | object_field | low | 2 | pointer loaded from field $34: candidates are every code address the game stores at that offset (needs runtime check) |
| $31896 | loc_31856 | `jmp	(a3)` | object_field | low | 0 | pointer loaded from field $38: candidates are every code address the game stores at that offset (needs runtime check) |
| $417CA | Nem_ProcessCompressedData | `jmp	(a3)` | constant+ram_pointer | medium | 4 | pointer loaded from RAM $FFFFE440 |
| $5C0D0 | loc_5C0B0 | `jmp	(a1)` | object_field | low | 0 | pointer loaded from field $2: candidates are every code address the game stores at that offset (needs runtime check) |
| $697E2 | Window_Update | `jmp	(a0)` | object_field | low | 0 | pointer loaded from field $4: candidates are every code address the game stores at that offset (needs runtime check) |
