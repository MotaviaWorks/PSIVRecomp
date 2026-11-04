/*
 * Mega Drive bus outside work RAM and ROM.
 *
 * M2 stub: enough hardware for the recompiled code to run its boot sequence.
 * Z80 RAM is real memory, the Z80 bus is always granted, controllers report
 * nothing pressed, and the VDP accepts writes and reports vertical blank.
 * The real VDP, Z80 and sound chips replace this in M3-M5.
 */
#include "m68k.h"

#include <stdio.h>

static uint8_t z80_ram[0x2000];
static uint16_t vdp_status = 0x3408;        /* FIFO empty, in vertical blank */
static uint8_t sram_mapped;

/* 3-button pad 1. bus_pad1 bits (1 = pressed): 0 up, 1 down, 2 left, 3 right,
 * 4 A, 5 B, 6 C, 7 Start. The select line (TH, bit 6 of the data port)
 * chooses which half the port reports; bits read 0 when pressed. */
uint8_t bus_pad1;
static uint8_t pad1_th = 0x40;

static uint8_t pad1_read(void) {
    uint8_t b = bus_pad1, v;
    if (pad1_th) {                          /* TH=1: C B R L D U */
        v = (uint8_t)((b & 0x0F) | (b >> 6 & 1) << 5 | (b >> 5 & 1) << 4);
        return (uint8_t)(0x40 | (~v & 0x3F));
    }
    v = (uint8_t)((b & 0x03) | (b >> 7 & 1) << 5 | (b >> 4 & 1) << 4);   /* TH=0: Start A 0 0 D U */
    return (uint8_t)(~v & 0x33);
}

uint8_t bus_rd8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xA04000 && a < 0xA04004) return 0x00;   /* YM2612 status: not busy */
    if (a >= 0xA00000 && a < 0xA02000) return z80_ram[a & 0x1FFF];
    if (a >= 0xA00000 && a < 0xA10000) return 0xFF;
    switch (a) {
    case 0xA10001: return 0xA0;              /* version: overseas, NTSC, no expansion */
    case 0xA10003: return pad1_read();
    case 0xA10005: return 0x7F;              /* pad 2: nothing pressed */
    case 0xA11100: return 0x00;              /* Z80 bus granted */
    }
    if (a >= 0xC00004 && a < 0xC00008) return (uint8_t)(a & 1 ? vdp_status : vdp_status >> 8);
    return 0;
}

uint16_t bus_rd16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xC00004 && a < 0xC00008) return vdp_status;
    if (a >= 0xC00008 && a < 0xC00010) return 0;      /* HV counter */
    return (uint16_t)(bus_rd8(a) << 8 | bus_rd8(a + 1));
}

void bus_wr8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xA00000 && a < 0xA02000) { z80_ram[a & 0x1FFF] = v; return; }
    if (a >= 0xA00000 && a < 0xA10000) return;         /* YM2612 writes: ignored until M5 */
    if (a == 0xA130F1) { sram_mapped = v & 1; return; }
    if (a == 0xA10003) { pad1_th = v & 0x40; return; }
    (void)v;
}

void bus_wr16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xA00000 && a < 0xA10000) { bus_wr8(a, (uint8_t)(v >> 8)); return; }
    bus_wr8(a, (uint8_t)(v >> 8));
    bus_wr8(a + 1, (uint8_t)v);
}

void m68k_stop(uint32_t sr) { m68k_set_sr(sr); }
void m68k_reset_insn(void) {}
