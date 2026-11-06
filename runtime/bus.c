/*
 * Mega Drive bus outside work RAM (and outside the inline ROM fast path).
 *
 *   $000000-$2FFFFF  ROM; $200001-$203FFF odd bytes = battery SRAM while
 *                    $A130F1 bit 0 is set
 *   $A00000-$A01FFF  Z80 RAM          $A04000-$A04003  YM2612
 *   $A10001          version          $A10003/5/9/B    pad data / control
 *   $A11100          Z80 bus request  $A11200          Z80 reset
 *   $C00000-$C00003  VDP data         $C00004-$C00007  VDP control/status
 *   $C00008-$C0000F  HV counter       $C00011          PSG
 *
 * The Z80 area, bus request/reset and the PSG belong to sound.c.
 */
#include "bus.h"
#include "m68k.h"
#include "sound.h"
#include "vdp.h"

#include <stdio.h>
#include <string.h>

uint8_t m68k_ram[0x10000];
const uint8_t *m68k_rom;
uint32_t m68k_rom_size;
uint32_t m68k_rom_fast;

/* ------------------------------------------------------------------ SRAM */
uint8_t bus_sram[BUS_SRAM_SIZE];          /* the odd bytes $200001, $200003, ... */
int bus_sram_dirty;
static int sram_mapped;

static int sram_index(uint32_t a) {
    if (!sram_mapped || a < 0x200000 || a > 0x203FFF || !(a & 1)) return -1;
    return (int)((a - 0x200001) >> 1);
}

/* the game's mark at $200005: "PHANTASY STAR 4" on the odd bytes */
static const char PS4_MARK[] = "PHANTASY STAR 4";

int bus_sram_import(const uint8_t *data, size_t n) {
    memset(bus_sram, 0, sizeof bus_sram);
    int every_byte = n == 2 * BUS_SRAM_SIZE;            /* 16 KB: BizHawk's image */
    if (!every_byte && n >= 2 + 2 * sizeof PS4_MARK && memcmp(data + 2, PS4_MARK, sizeof PS4_MARK - 1)) {
        int marked = 1;                                 /* the mark on every other byte: Gens' 8 KB */
        for (size_t i = 0; i < sizeof PS4_MARK - 1 && marked; i++)
            marked = data[5 + 2 * i] == (uint8_t)PS4_MARK[i] && data[4 + 2 * i] == 0;
        if (marked) {
            for (size_t i = 0; i < n / 2 && i < BUS_SRAM_SIZE; i++) bus_sram[i] = data[2 * i + 1];
            return SRAM_EVERY_BYTE_8K;
        }
    }
    if (every_byte) {
        for (size_t i = 0; i < BUS_SRAM_SIZE; i++) bus_sram[i] = data[2 * i + 1];
        return SRAM_EVERY_BYTE_16K;
    }
    memcpy(bus_sram, data, n < BUS_SRAM_SIZE ? n : BUS_SRAM_SIZE);
    return SRAM_PORT;
}

size_t bus_sram_export(int format, uint8_t *out) {
    if (format == SRAM_PORT) { memcpy(out, bus_sram, BUS_SRAM_SIZE); return BUS_SRAM_SIZE; }
    size_t n = format == SRAM_EVERY_BYTE_16K ? 2 * BUS_SRAM_SIZE : BUS_SRAM_SIZE;
    for (size_t i = 0; i < n / 2; i++) {
        out[2 * i] = format == SRAM_EVERY_BYTE_16K ? 0xFF : 0x00;   /* the unused even bytes, as each keeps them */
        out[2 * i + 1] = bus_sram[i];
    }
    return n;
}

const char *bus_sram_format_name(int format) {
    return format == SRAM_EVERY_BYTE_16K ? "every byte, 16 KB (BizHawk)"
         : format == SRAM_EVERY_BYTE_8K ? "every byte, 8 KB (Gens, Kega Fusion)" : "the port's";
}

/* ------------------------------------------------------------------ pads */
uint8_t bus_pad[2];
static uint8_t pad_th[2] = {0x40, 0x40};

/* rewind: the bus's own state (the battery save itself is left as it is) */
size_t bus_state_save(uint8_t *out) {
    if (out) {
        memcpy(out, &sram_mapped, sizeof sram_mapped);
        memcpy(out + sizeof sram_mapped, pad_th, sizeof pad_th);
        memcpy(out + sizeof sram_mapped + sizeof pad_th, bus_pad, sizeof bus_pad);   /* the next frame's input */
    }
    return sizeof sram_mapped + sizeof pad_th + sizeof bus_pad;
}

size_t bus_state_load(const uint8_t *in) {
    memcpy(&sram_mapped, in, sizeof sram_mapped);
    memcpy(pad_th, in + sizeof sram_mapped, sizeof pad_th);
    memcpy(bus_pad, in + sizeof sram_mapped + sizeof pad_th, sizeof bus_pad);
    m68k_rom_fast = sram_mapped ? 0x200000 : m68k_rom_size;
    return sizeof sram_mapped + sizeof pad_th + sizeof bus_pad;
}

static uint8_t pad_read(int n) {
    uint8_t b = bus_pad[n], v;
    if (pad_th[n]) {                       /* TH=1: C B R L D U */
        v = (uint8_t)((b & 0x0F) | (b >> 6 & 1) << 5 | (b >> 5 & 1) << 4);
        return (uint8_t)(0x40 | (~v & 0x3F));
    }
    v = (uint8_t)((b & 0x03) | (b >> 7 & 1) << 5 | (b >> 4 & 1) << 4);   /* TH=0: Start A 0 0 D U */
    return (uint8_t)(~v & 0x33);
}

void bus_init(const uint8_t *rom, uint32_t size) {
    m68k_rom = rom;
    m68k_rom_size = size;
    m68k_rom_fast = size;
    memset(m68k_ram, 0, sizeof m68k_ram);
    sound_init();
    sram_mapped = 0;
    vdp_reset();
}

/* ------------------------------------------------------------------ reads */
uint8_t bus_rd8(uint32_t a) {
    a &= 0xFFFFFF;
    int si = sram_index(a);
    if (si >= 0) return bus_sram[si];
    if (a < m68k_rom_size) return m68k_rom[a];
    if (a >= 0xA00000 && a < 0xA10000) return sound_68k_read(a);
    switch (a) {
    case 0xA10001: return 0xA0;                         /* overseas, NTSC, no expansion */
    case 0xA10003: return pad_read(0);
    case 0xA10005: return pad_read(1);
    case 0xA11100: return (uint8_t)(sound_busreq_read() >> 8);
    case 0xA11101: return (uint8_t)sound_busreq_read();
    }
    if (a >= 0xC00000 && a < 0xC00020) {
        uint16_t w = bus_rd16(a & ~1u);
        return (uint8_t)(a & 1 ? w : w >> 8);
    }
    return 0;
}

uint16_t bus_rd16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xC00000 && a < 0xC00020) {
        switch (a & 0x1C) {
        case 0x00: return vdp_read_data();
        case 0x04: return vdp_read_status();
        case 0x08: case 0x0C: return vdp_read_hv();
        }
        return 0;
    }
    return (uint16_t)(bus_rd8(a) << 8 | bus_rd8(a + 1));
}

/* ------------------------------------------------------------------ writes */
void bus_wr8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    int si = sram_index(a);
    if (si >= 0) {
        if (bus_sram[si] != v) bus_sram_dirty = 1;
        bus_sram[si] = v;
        return;
    }
    if (a >= 0xA00000 && a < 0xA10000) { sound_68k_write(a, v); return; }
    if (a >= 0xC00010 && a < 0xC00018) { if (a & 1) sound_psg_write(v); return; }
    if (a >= 0xC00000 && a < 0xC00010) {                /* byte writes reach the VDP doubled */
        bus_wr16(a & ~1u, (uint16_t)(v << 8 | v));
        return;
    }
    switch (a) {
    case 0xA130F1:
        sram_mapped = v & 1;
        m68k_rom_fast = sram_mapped ? 0x200000 : m68k_rom_size;
        return;
    case 0xA11100: sound_busreq_write((uint16_t)(v << 8)); return;
    case 0xA11200: sound_reset_write((uint16_t)(v << 8)); return;
    case 0xA10003: pad_th[0] = v & 0x40; return;
    case 0xA10005: pad_th[1] = v & 0x40; return;
    }
}

void bus_wr16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xC00000 && a < 0xC00010) {
        if ((a & 0x0C) == 0x00) vdp_write_data(v);
        else if ((a & 0x0C) == 0x04) vdp_write_control(v);
        return;
    }
    if (a == 0xA11100) { sound_busreq_write(v); return; }
    if (a == 0xA11200) { sound_reset_write(v); return; }
    if (a >= 0xC00010 && a < 0xC00018) { sound_psg_write((uint8_t)v); return; }
    if (a >= 0xA00000 && a < 0xA10000) { bus_wr8(a, (uint8_t)(v >> 8)); return; }   /* Z80 side sees the high byte */
    bus_wr8(a, (uint8_t)(v >> 8));
    bus_wr8(a + 1, (uint8_t)v);
}

void m68k_reset_insn(void) {}
