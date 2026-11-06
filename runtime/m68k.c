/*
 * Out-of-line 68000 semantics: shifts/rotates, multiply/divide, BCD, status
 * register changes, exceptions and indirect calls.
 */
#include "m68k.h"
#include "diag.h"

#include <stdio.h>
#include <stdlib.h>

M68KState m68k;
uint32_t m68k_last_ret;

static uint32_t mask_of(int bits) { return bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1; }
static uint32_t msb_of(int bits) { return 1u << (bits - 1); }
static void nz(uint32_t r, int bits) {
    m68k.n = (r & msb_of(bits)) != 0;
    m68k.z = (r & mask_of(bits)) == 0;
}

/* ------------------------------------------------------------------ shifts */
uint32_t m68k_asl(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), r;
    count &= 63;
    v &= m;
    if (count == 0) { m68k.c = 0; m68k.v = 0; nz(v, bits); return v; }
    if (count >= (uint32_t)bits) {
        r = 0;
        m68k.c = m68k.x = count == (uint32_t)bits ? v & 1 : 0;
        m68k.v = v != 0;
    } else {
        /* V: did the sign bit change at any point? i.e. top count+1 bits not all equal */
        uint32_t top = v >> (bits - count - 1);
        uint32_t all = (1u << (count + 1)) - 1;
        m68k.v = top != 0 && top != all;
        m68k.c = m68k.x = v >> (bits - count) & 1;
        r = v << count & m;
    }
    nz(r, bits);
    return r;
}

uint32_t m68k_asr(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), sign = v & msb_of(bits) ? 1 : 0, r;
    count &= 63;
    v &= m;
    m68k.v = 0;
    if (count == 0) { m68k.c = 0; nz(v, bits); return v; }
    if (count >= (uint32_t)bits) {
        r = sign ? m : 0;
        m68k.c = m68k.x = (uint8_t)sign;
    } else {
        uint32_t ext = sign ? v | ~m : v;            /* sign-extend to 32 bits */
        m68k.c = m68k.x = ext >> (count - 1) & 1;
        r = (uint32_t)((int32_t)ext >> count) & m;
    }
    nz(r, bits);
    return r;
}

uint32_t m68k_lsl(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), r;
    count &= 63;
    v &= m;
    m68k.v = 0;
    if (count == 0) { m68k.c = 0; nz(v, bits); return v; }
    if (count > (uint32_t)bits) { r = 0; m68k.c = m68k.x = 0; }
    else if (count == (uint32_t)bits) { r = 0; m68k.c = m68k.x = v & 1; }
    else { m68k.c = m68k.x = v >> (bits - count) & 1; r = v << count & m; }
    nz(r, bits);
    return r;
}

uint32_t m68k_lsr(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), r;
    count &= 63;
    v &= m;
    m68k.v = 0;
    if (count == 0) { m68k.c = 0; nz(v, bits); return v; }
    if (count > (uint32_t)bits) { r = 0; m68k.c = m68k.x = 0; }
    else if (count == (uint32_t)bits) { r = 0; m68k.c = m68k.x = v >> (bits - 1) & 1; }
    else { m68k.c = m68k.x = v >> (count - 1) & 1; r = v >> count; }
    nz(r, bits);
    return r;
}

uint32_t m68k_rol(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), r;
    count &= 63;
    v &= m;
    m68k.v = 0;
    if (count == 0) { m68k.c = 0; nz(v, bits); return v; }
    count %= (uint32_t)bits;
    r = count ? (v << count | v >> (bits - count)) & m : v;
    m68k.c = r & 1;
    nz(r, bits);
    return r;
}

uint32_t m68k_ror(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), r;
    count &= 63;
    v &= m;
    m68k.v = 0;
    if (count == 0) { m68k.c = 0; nz(v, bits); return v; }
    count %= (uint32_t)bits;
    r = count ? (v >> count | v << (bits - count)) & m : v;
    m68k.c = (r & msb_of(bits)) != 0;
    nz(r, bits);
    return r;
}

uint32_t m68k_roxl(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), x = m68k.x;
    count &= 63;
    v &= m;
    m68k.v = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t out = (v & msb_of(bits)) != 0;
        v = (v << 1 | x) & m;
        x = out;
    }
    m68k.x = m68k.c = (uint8_t)x;
    nz(v, bits);
    return v;
}

uint32_t m68k_roxr(uint32_t v, uint32_t count, int bits) {
    uint32_t m = mask_of(bits), x = m68k.x;
    count &= 63;
    v &= m;
    m68k.v = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t out = v & 1;
        v = (v >> 1 | x << (bits - 1)) & m;
        x = out;
    }
    m68k.x = m68k.c = (uint8_t)x;
    nz(v, bits);
    return v;
}

/* ------------------------------------------------------------------ multiply / divide */
uint32_t m68k_mulu(uint32_t d, uint32_t s) {
    uint32_t r = (d & 0xFFFF) * (s & 0xFFFF);
    LOGIC32(r);
    return r;
}

uint32_t m68k_muls(uint32_t d, uint32_t s) {
    uint32_t r = (uint32_t)((int32_t)(int16_t)d * (int32_t)(int16_t)s);
    LOGIC32(r);
    return r;
}

int m68k_divu(uint32_t *d, uint32_t s) {
    uint32_t div = s & 0xFFFF, q, r;
    if (div == 0) return 0;
    q = *d / div;
    r = *d % div;
    m68k.c = 0;
    if (q > 0xFFFF) { m68k.v = 1; m68k.n = 1; return 1; }   /* overflow: register unchanged */
    *d = r << 16 | q;
    m68k.v = 0;
    NZ16(q);
    return 1;
}

int m68k_divs(uint32_t *d, uint32_t s) {
    int32_t dd = (int32_t)*d, div = (int16_t)s, q, r;
    if (div == 0) return 0;
    m68k.c = 0;
    if (dd == INT32_MIN && div == -1) { m68k.v = 1; m68k.n = 1; return 1; }
    q = dd / div;
    r = dd % div;                       /* remainder takes the dividend's sign, like the 68000 */
    if (q < -32768 || q > 32767) { m68k.v = 1; m68k.n = 1; return 1; }
    *d = (uint32_t)(r & 0xFFFF) << 16 | (uint32_t)(q & 0xFFFF);
    m68k.v = 0;
    NZ16((uint32_t)q);
    return 1;
}

/* ------------------------------------------------------------------ BCD (N and V are undefined on the 68000) */
uint32_t m68k_abcd(uint32_t d, uint32_t s) {
    uint32_t r = (d & 0x0F) + (s & 0x0F) + m68k.x;
    if (r > 9) r += 6;
    r += (d & 0xF0) + (s & 0xF0);
    m68k.c = m68k.x = r > 0x99;
    if (m68k.c) r -= 0xA0;
    r &= 0xFF;
    if (r) m68k.z = 0;
    m68k.n = r >> 7 & 1;
    m68k.v = 0;
    return r;
}

uint32_t m68k_sbcd(uint32_t d, uint32_t s) {
    uint32_t r = (d & 0x0F) - (s & 0x0F) - m68k.x;
    if (r > 9) r -= 6;
    r += (d & 0xF0) - (s & 0xF0);
    m68k.c = m68k.x = r > 0x99;
    if (m68k.c) r += 0xA0;
    r &= 0xFF;
    if (r) m68k.z = 0;
    m68k.n = r >> 7 & 1;
    m68k.v = 0;
    return r;
}

uint32_t m68k_nbcd(uint32_t d) { return m68k_sbcd(0, d); }

/* ------------------------------------------------------------------ status register */
void m68k_set_sr(uint32_t v) {
    uint8_t s = v >> 13 & 1;
    if (s != m68k.s) {                  /* swap the active stack pointer */
        uint32_t t = m68k.a[7];
        m68k.a[7] = m68k.usp;
        m68k.usp = t;
        m68k.s = s;
    }
    m68k.t = v >> 15 & 1;
    m68k.imask = v >> 8 & 7;
    SET_CCR(v);
}

/* ------------------------------------------------------------------ calls and exceptions */
m68k_fn m68k_lookup(uint32_t addr) {
    int lo = 0, hi = psiv_function_count - 1;
    addr &= 0xFFFFFF;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (psiv_functions[mid].addr == addr) return psiv_functions[mid].fn;
        if (psiv_functions[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void m68k_call(uint32_t target) {
    m68k_fn fn = m68k_lookup(target);
    if (!fn && (target & 0xFFFFFF) >= 0xFF0000) {
        uint32_t to = target;
        if (rd16(target) == 0x4EF9) {
            /* a `jmp (xxx).l` trampoline in RAM (the HInt vector points at one) */
            to = rd32(target + 2);
            fn = m68k_lookup(to);
        }
        if (!fn && (to & 0xFFFFFF) >= 0xFF0000) {
            m68k_run_ram(to);               /* code the game wrote into RAM */
            return;
        }
    }
    if (!fn) m68k_fatal("call to an address that is not a recompiled function", target, 0);
    fn();
}

/* ------------------------------------------------------------------ code in RAM
 * PSIV writes code into RAM and runs it in one place: horizontal-interrupt
 * handlers for raster effects that need a new value on every line. Gra's
 * wave copies a line counter and a chain of blocks to $FF0000 (loc_1CAxx):
 * once the counter runs out, each interrupt runs one block, which writes one
 * vertical scroll value to the VDP and then adds to the counter's branch
 * displacement so the next interrupt runs the next block (it rewrites its
 * own code). Other effects leave a lone `rte` there while the interrupt is
 * on. The recompiler never sees this code, so it is interpreted: the few
 * instructions these handlers use, with the 68000's flags. Anything else
 * stops the game with a report naming the instruction, rather than guess. */
static int ram_cond(unsigned cc) {
    switch (cc) {
    case 0x0: return CC_T;  case 0x1: return CC_F;  case 0x2: return CC_HI; case 0x3: return CC_LS;
    case 0x4: return CC_CC; case 0x5: return CC_CS; case 0x6: return CC_NE; case 0x7: return CC_EQ;
    case 0x8: return CC_VC; case 0x9: return CC_VS; case 0xA: return CC_PL; case 0xB: return CC_MI;
    case 0xC: return CC_GE; case 0xD: return CC_LT; case 0xE: return CC_GT; default: return CC_LE;
    }
}

void m68k_run_ram(uint32_t pc) {
    for (int n = 0; n < 4096; n++) {
        uint32_t op = rd16(pc);
        if (op == 0x4E73) {                                 /* rte */
            uint32_t sr = POP16();
            m68k_last_ret = POP32();
            SET_SR(sr);
            return;
        }
        if (op == 0x4E75) {                                 /* rts */
            m68k_last_ret = POP32();
            return;
        }
        if (op == 0x4E71) {                                 /* nop */
            pc += 2;
        } else if (op == 0x23FC) {                          /* move.l #imm,(xxx).l */
            uint32_t v = rd32(pc + 2);
            wr32(rd32(pc + 6), v);
            LOGIC32(v);
            pc += 10;
        } else if (op == 0x33FC) {                          /* move.w #imm,(xxx).l */
            uint32_t v = rd16(pc + 2);
            wr16(rd32(pc + 4), v);
            LOGIC16(v);
            pc += 8;
        } else if (op == 0x21FC || op == 0x31FC) {          /* move.l / move.w #imm,(xxx).w */
            int lng = op == 0x21FC;
            uint32_t v = lng ? rd32(pc + 2) : rd16(pc + 2), a = (uint32_t)(int16_t)rd16(pc + (lng ? 6 : 4));
            if (lng) { wr32(a, v); LOGIC32(v); } else { wr16(a, v); LOGIC16(v); }
            pc += lng ? 8 : 6;
        } else if ((op & 0xF1FF) == 0x20BC || (op & 0xF1FF) == 0x30BC) {   /* move.l / move.w #imm,(An) */
            int lng = (op & 0xF000) == 0x2000;
            uint32_t a = A(op >> 9 & 7), v = lng ? rd32(pc + 2) : rd16(pc + 2);
            if (lng) { wr32(a, v); LOGIC32(v); } else { wr16(a, v); LOGIC16(v); }
            pc += lng ? 6 : 4;
        } else if (op == 0x0679 || op == 0x0479) {          /* addi.w / subi.w #imm,(xxx).l */
            uint32_t a = rd32(pc + 4), s = rd16(pc + 2), d = rd16(a);
            wr16(a, op == 0x0679 ? ADD16(d, s) : SUB16(d, s));
            pc += 8;
        } else if ((op & 0xF0FF) == 0x5079) {               /* addq.w / subq.w #q,(xxx).l */
            uint32_t a = rd32(pc + 2), q = (op >> 9 & 7) ? (op >> 9 & 7) : 8, d = rd16(a);
            wr16(a, op & 0x100 ? SUB16(d, q) : ADD16(d, q));
            pc += 6;
        } else if ((op & 0xF000) == 0x6000 && (op & 0x0F00) != 0x0100) {   /* bra / bcc (not bsr) */
            int32_t disp = (int8_t)(op & 0xFF);
            uint32_t next = pc + 2;
            if (!disp) {
                disp = (int16_t)rd16(pc + 2);
                next = pc + 4;
            }
            pc = ram_cond(op >> 8 & 15) ? pc + 2 + (uint32_t)disp : next;
        } else if (op == 0x4EF9) {                          /* jmp (xxx).l */
            uint32_t to = rd32(pc + 2);
            m68k_fn fn = m68k_lookup(to);
            if (fn) {
                fn();
                return;
            }
            pc = to;
        } else {
            m68k_fatal("an instruction in code the game wrote to RAM that the port can't run", pc, op);
        }
    }
    m68k_fatal("code the game wrote to RAM ran too long", pc, 0);
}

/* A call came back to a different address than the one pushed: the callee
 * rewrote its return address. The game does this in Battle_CreateWindow and
 * Battle_DestroyWindow (`addq.l #2,(sp)`: skip the `rts` after the call once
 * the window is fully open/closed). The 68000 simply continues there, so
 * continue at the function that starts at that address; it ends by returning
 * from the caller, which then returns too. */
int m68k_resume(uint32_t addr, uint32_t expected) {
    m68k_fn fn = m68k_lookup(addr);
    if (fn) {
        fn();
        return 1;
    }
#ifdef PSIV_CHECK_RETURNS
    m68k_fatal("return address", expected, addr);
#else
    static int warned;
    if (!warned++) fprintf(stderr, "returned to $%06X instead of $%06X, which is not a recompiled "
                                   "function; continuing after the call\n", (unsigned)addr, (unsigned)expected);
#endif
    return 0;
}

void m68k_exception(int vector, uint32_t pc) {
    uint32_t sr = GET_SR();
    m68k_set_sr((sr | 0x2000) & 0x7FFF);    /* supervisor on, trace off */
    PUSH32(pc);
    PUSH16(sr);
    m68k_call(rd32((uint32_t)vector * 4));
}

void m68k_interrupt(int level, uint32_t pc) {
    uint32_t sr = GET_SR();
    if (level <= m68k.imask && level < 7) return;      /* masked */
    m68k_set_sr((sr | 0x2000) & 0x7FFF);
    m68k.imask = (uint8_t)level;
    PUSH32(pc);
    PUSH16(sr);
    m68k_call(rd32((uint32_t)(24 + level) * 4));      /* autovector */
}

void m68k_fatal(const char *what, uint32_t addr, uint32_t value) {
    static char report[8192];
    diag_report(report, sizeof report, what, addr, value);
    platform_fatal(report);
    exit(1);                            /* in case the front end returns */
}
