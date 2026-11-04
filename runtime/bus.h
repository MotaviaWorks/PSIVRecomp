#ifndef PSIV_BUS_H
#define PSIV_BUS_H

#include <stdint.h>

#define BUS_SRAM_SIZE 0x2000            /* 8 KB: odd bytes of $200001-$203FFF */

/* pad bits (1 = pressed) */
enum { PAD_UP = 1, PAD_DOWN = 2, PAD_LEFT = 4, PAD_RIGHT = 8,
       PAD_A = 16, PAD_B = 32, PAD_C = 64, PAD_START = 128 };

extern uint8_t bus_pad[2];
extern uint8_t bus_sram[BUS_SRAM_SIZE];
extern int bus_sram_dirty;

void bus_init(const uint8_t *rom, uint32_t size);

#endif
