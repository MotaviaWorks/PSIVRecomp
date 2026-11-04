#include "diag.h"
#include "m68k.h"

#include <stdio.h>
#include <string.h>

uint32_t diag_stack_top = 0xFFFF4FF0u;

const char *diag_function_name(uint32_t addr, uint32_t *offset) {
    int lo = 0, hi = psiv_function_count - 1, best = -1;
    addr &= 0xFFFFFF;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (psiv_functions[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) { if (offset) *offset = 0; return "?"; }
    if (offset) *offset = addr - psiv_functions[best].addr;
    return psiv_functions[best].name;
}

static uint16_t rom16(uint32_t a) {
    return a + 1 < m68k_rom_size ? (uint16_t)(m68k_rom[a] << 8 | m68k_rom[a + 1]) : 0;
}

/* v looks like a return address: in ROM, even, and right after a call */
static int is_return_address(uint32_t v) {
    if ((v & 1) || v < 0x206 || v >= m68k_rom_size) return 0;
    uint16_t w2 = rom16(v - 2), w4 = rom16(v - 4), w6 = rom16(v - 6);
    if ((w2 & 0xFFF8) == 0x4E90) return 1;                     /* jsr (an) */
    if ((w2 & 0xFF00) == 0x6100 && (w2 & 0xFF)) return 1;      /* bsr.s */
    if ((w2 & 0xFFF0) == 0x4E40) return 1;                     /* trap #n */
    if (w4 == 0x6100) return 1;                                /* bsr.w */
    if (w4 == 0x4EB8 || w4 == 0x4EBA || w4 == 0x4EBB) return 1;   /* jsr abs.w / d16(pc) / d8(pc,xn) */
    if ((w4 & 0xFFF8) == 0x4EA8 || (w4 & 0xFFF8) == 0x4EB0) return 1;   /* jsr d16(an) / d8(an,xn) */
    if (w6 == 0x4EB9) return 1;                                /* jsr abs.l */
    return 0;
}

int diag_callstack(uint32_t *out, int max) {
    int n = 0;
    uint32_t top = diag_stack_top & 0xFFFFFF, a = A(7) & 0xFFFFFF;
    if (a < 0xFF0000 || a > top) return 0;
    for (; a + 4 <= top && n < max; a += 2) {
        uint32_t v = rd32(a);
        if (is_return_address(v)) out[n++] = v;
    }
    return n;
}

void diag_report(char *buf, size_t size, const char *what, uint32_t addr, uint32_t value) {
    size_t o = 0;
#define OUT(...) do { if (o < size) o += (size_t)snprintf(buf + o, size - o, __VA_ARGS__); } while (0)
    uint32_t off;
    const char *fn = diag_function_name(addr, &off);
    OUT("%s\n  at $%06X (%s+$%X), value $%08X\n\n", what, addr & 0xFFFFFF, fn, off, value);
    OUT("d0-d7:");
    for (int i = 0; i < 8; i++) OUT(" %08X", m68k.d[i]);
    OUT("\na0-a7:");
    for (int i = 0; i < 8; i++) OUT(" %08X", m68k.a[i]);
    OUT("\nsr: %04X   game mode: %u\n", GET_SR(), (unsigned)(m68k_ram[0xEF00] << 8 | m68k_ram[0xEF01]) / 4);
    uint32_t calls[24];
    int n = diag_callstack(calls, 24);
    OUT("\ncall chain (innermost first, from return addresses on the 68000 stack):\n");
    for (int i = 0; i < n; i++) {
        fn = diag_function_name(calls[i] - 2, &off);
        OUT("  $%06X  %s+$%X\n", calls[i], fn, off + 2);
    }
    if (!n) OUT("  (none found)\n");
#undef OUT
}
