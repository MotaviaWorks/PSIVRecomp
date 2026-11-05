#ifndef PSIV_BUS_H
#define PSIV_BUS_H

#include <stddef.h>
#include <stdint.h>

#define BUS_SRAM_SIZE 0x2000            /* 8 KB: odd bytes of $200001-$203FFF */

/* pad bits (1 = pressed) */
enum { PAD_UP = 1, PAD_DOWN = 2, PAD_LEFT = 4, PAD_RIGHT = 8,
       PAD_A = 16, PAD_B = 32, PAD_C = 64, PAD_START = 128 };

extern uint8_t bus_pad[2];
extern uint8_t bus_sram[BUS_SRAM_SIZE];
extern int bus_sram_dirty;

void bus_init(const uint8_t *rom, uint32_t size);

/* Battery save files: the port's own (8 KB, the odd bytes), or an emulator's
 * image of every byte of the save RAM -- 16 KB of $200000-$203FFF (BizHawk),
 * or 8 KB of $200000-$201FFF (Gens, Kega Fusion: the first slot and part of
 * the second). Import takes any of them; export writes one back as it was. */
enum { SRAM_PORT, SRAM_EVERY_BYTE_16K, SRAM_EVERY_BYTE_8K };
int bus_sram_import(const uint8_t *data, size_t n);     /* fills bus_sram; returns the format */
size_t bus_sram_export(int format, uint8_t *out);       /* out: room for 16 KB; returns the size */
const char *bus_sram_format_name(int format);

#endif
