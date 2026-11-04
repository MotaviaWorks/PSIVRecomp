/*
 * M2 smoke test: boot the recompiled game from its reset vector with the stub
 * bus and report where it ends up.
 *
 * Built with PSIV_TRACE: every basic block calls m68k_trace(). After the block
 * budget runs out it prints the hottest blocks, which for a game waiting on a
 * vertical-blank interrupt that never comes is the WaitForVInt spin.
 *
 * With a frame count it also delivers the vertical-blank interrupt whenever
 * the game waits for it, so the game runs frame after frame (no graphics yet).
 *
 *     psiv_smoke [rom/psiv.bin] [block budget] [frames]
 */
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t m68k_ram[0x10000];
const uint8_t *m68k_rom;
uint32_t m68k_rom_size;

uint32_t m68k_trace_pc;
extern uint8_t bus_pad1;
static uint64_t blocks, budget = 50000000;
#define HOT 4096
static uint32_t hot_pc[HOT];
static uint64_t hot_n[HOT];

static void report(void) {
    printf("ran %llu blocks; hottest:\n", (unsigned long long)blocks);
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < HOT; i++)
            if (hot_n[i] && (best < 0 || hot_n[i] > hot_n[best])) best = i;
        if (best < 0) break;
        printf("  $%06X  %llu\n", hot_pc[best], (unsigned long long)hot_n[best]);
        hot_n[best] = 0;
    }
    uint32_t hash = 2166136261u;                 /* FNV-1a over work RAM */
    for (int i = 0; i < 0x10000; i++) hash = (hash ^ m68k_ram[i]) * 16777619u;
    printf("RAM FNV-1a: %08X\n", hash);
}

/* WaitForVInt's spin: `tst.b (VInt_Flag).w / bne.s *-4` at $42076 */
#define WAIT_SPIN 0x42076u
#define VINT_FLAG 0xEF12u
#define GAME_MODE 0xEF00u
static uint64_t frames, frame_limit;
static uint16_t last_mode = 0xFFFF;

void m68k_trace(uint32_t pc) {
    uint32_t slot = (pc >> 1) % HOT;
    m68k_trace_pc = pc;
    if (pc == WAIT_SPIN && frame_limit && m68k_ram[VINT_FLAG]) {
        /* the game is waiting for vertical blank: deliver it, like the hardware would */
        uint16_t mode = (uint16_t)(m68k_ram[GAME_MODE] << 8 | m68k_ram[GAME_MODE + 1]);
        if (mode != last_mode) {
            printf("frame %llu: game mode %u\n", (unsigned long long)frames, mode / 4);
            last_mode = mode;
        }
        if (++frames > frame_limit) {
            report();
            exit(0);
        }
        /* press Start for 4 frames every 3 seconds, C (confirm) in between */
        bus_pad1 = frames % 180 < 4 ? 0x80 : frames % 30 < 2 ? 0x40 : 0;
        m68k_interrupt(6, pc);
    }
    if (hot_pc[slot] != pc) { hot_pc[slot] = pc; hot_n[slot] = 0; }
    hot_n[slot]++;
    if (++blocks >= budget) {
        report();
        exit(0);
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "rom/psiv.bin";
    if (argc > 2) budget = strtoull(argv[2], NULL, 10);
    if (argc > 3) frame_limit = strtoull(argv[3], NULL, 10);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    static uint8_t rom[0x400000];
    m68k_rom_size = (uint32_t)fread(rom, 1, sizeof rom, f);
    fclose(f);
    m68k_rom = rom;

    memset(&m68k, 0, sizeof m68k);
    m68k.s = 1;
    m68k.imask = 7;
    A(7) = rd32(0);
    uint32_t pc = rd32(4);
    printf("reset: SSP $%08X, PC $%06X\n", A(7), pc);
    m68k_call(pc);
    printf("reset handler returned (unexpected)\n");
    report();
    return 0;
}
