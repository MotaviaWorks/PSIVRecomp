/*
 * 68000 CPU state and instruction semantics used by the recompiled code.
 *
 * Generated code (the psiv_NN.c files in build/gen) reads and writes `m68k` and calls the helpers
 * below. Flags are kept as separate 0/1 bytes; the generator only calls a
 * flag-setting helper when something can read the result.
 *
 * Memory: rd8/rd16/rd32/wr8/wr16/wr32 take a 32-bit address and use its low
 * 24 bits like the real bus. Work RAM ($FF0000-$FFFFFF) and ROM have fast
 * paths here; everything else goes to bus.c.
 */
#ifndef PSIV_M68K_H
#define PSIV_M68K_H

#include <stdint.h>

typedef struct {
    uint32_t d[8];
    uint32_t a[8];       /* a[7] is the active stack pointer */
    uint32_t usp;        /* inactive stack pointer while in supervisor mode */
    uint8_t x, n, z, v, c;
    uint8_t s;           /* supervisor bit */
    uint8_t t;           /* trace bit (kept for SR round trips) */
    uint8_t imask;       /* interrupt mask, 0-7 */
} M68KState;

extern M68KState m68k;
extern uint32_t m68k_last_ret;   /* return address popped by the last rts/rte/rtr */

#define D(n) m68k.d[n]
#define A(n) m68k.a[n]

/* ------------------------------------------------------------------ memory */
extern uint8_t  m68k_ram[0x10000];
extern const uint8_t *m68k_rom;
extern uint32_t m68k_rom_size;

uint8_t  bus_rd8(uint32_t a);
uint16_t bus_rd16(uint32_t a);
void     bus_wr8(uint32_t a, uint8_t v);
void     bus_wr16(uint32_t a, uint16_t v);

static inline uint8_t rd8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) return m68k_ram[a & 0xFFFF];
    if (a < m68k_rom_size) return m68k_rom[a];
    return bus_rd8(a);
}
static inline uint16_t rd16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) { a &= 0xFFFF; return (uint16_t)(m68k_ram[a] << 8 | m68k_ram[a + 1]); }
    if (a + 1 < m68k_rom_size) return (uint16_t)(m68k_rom[a] << 8 | m68k_rom[a + 1]);
    return bus_rd16(a);
}
static inline uint32_t rd32(uint32_t a) {
    return (uint32_t)rd16(a) << 16 | rd16(a + 2);
}
static inline void wr8(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) { m68k_ram[a & 0xFFFF] = (uint8_t)v; return; }
    bus_wr8(a, (uint8_t)v);
}
static inline void wr16(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) { a &= 0xFFFF; m68k_ram[a] = (uint8_t)(v >> 8); m68k_ram[a + 1] = (uint8_t)v; return; }
    bus_wr16(a, (uint16_t)v);
}
static inline void wr32(uint32_t a, uint32_t v) {
    wr16(a, v >> 16);
    wr16(a + 2, v);
}

/* ------------------------------------------------------------------ registers */
#define SETB(r, v) ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(v) & 0xFFu))
#define SETW(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(v) & 0xFFFFu))
#define SX8(v)  ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v) ((uint32_t)(int32_t)(int16_t)(v))

static inline void PUSH16(uint32_t v) { A(7) -= 2; wr16(A(7), v); }
static inline void PUSH32(uint32_t v) { A(7) -= 4; wr32(A(7), v); }
static inline uint32_t POP16(void) { uint32_t v = rd16(A(7)); A(7) += 2; return v; }
static inline uint32_t POP32(void) { uint32_t v = rd32(A(7)); A(7) += 4; return v; }

static inline uint32_t GET_CCR(void) {
    return (uint32_t)(m68k.x << 4 | m68k.n << 3 | m68k.z << 2 | m68k.v << 1 | m68k.c);
}
static inline uint32_t GET_SR(void) {
    return (uint32_t)(m68k.t << 15 | m68k.s << 13 | m68k.imask << 8) | GET_CCR();
}
static inline void SET_CCR(uint32_t v) {
    m68k.x = v >> 4 & 1; m68k.n = v >> 3 & 1; m68k.z = v >> 2 & 1; m68k.v = v >> 1 & 1; m68k.c = v & 1;
}
void m68k_set_sr(uint32_t v);   /* swaps stacks on a supervisor/user change */
#define SET_SR(v) m68k_set_sr(v)

/* ------------------------------------------------------------------ flags */
static inline void NZ8(uint32_t r)  { m68k.n = r >> 7 & 1;  m68k.z = (r & 0xFF) == 0; }
static inline void NZ16(uint32_t r) { m68k.n = r >> 15 & 1; m68k.z = (r & 0xFFFF) == 0; }
static inline void NZ32(uint32_t r) { m68k.n = r >> 31 & 1; m68k.z = r == 0; }
/* move, and, or, eor, not, clr, tst, ext, swap, moveq, mul */
static inline void LOGIC8(uint32_t r)  { NZ8(r);  m68k.v = 0; m68k.c = 0; }
static inline void LOGIC16(uint32_t r) { NZ16(r); m68k.v = 0; m68k.c = 0; }
static inline void LOGIC32(uint32_t r) { NZ32(r); m68k.v = 0; m68k.c = 0; }

#define ARITH(BITS, M, B) \
static inline uint32_t ADD##BITS(uint32_t d, uint32_t s) { \
    uint64_t t = (uint64_t)(d & M) + (s & M); uint32_t r = (uint32_t)t & M; \
    m68k.c = m68k.x = t > M; m68k.v = ((s ^ r) & (d ^ r) & B) != 0; \
    m68k.n = (r & B) != 0; m68k.z = r == 0; return r; } \
static inline uint32_t SUB##BITS(uint32_t d, uint32_t s) { \
    uint32_t r = (d - s) & M; \
    m68k.c = m68k.x = (s & M) > (d & M); m68k.v = ((s ^ d) & (r ^ d) & B) != 0; \
    m68k.n = (r & B) != 0; m68k.z = r == 0; return r; } \
static inline void CMP##BITS(uint32_t d, uint32_t s) { \
    uint32_t r = (d - s) & M; \
    m68k.c = (s & M) > (d & M); m68k.v = ((s ^ d) & (r ^ d) & B) != 0; \
    m68k.n = (r & B) != 0; m68k.z = r == 0; } \
static inline uint32_t ADDX##BITS(uint32_t d, uint32_t s) { \
    uint64_t t = (uint64_t)(d & M) + (s & M) + m68k.x; uint32_t r = (uint32_t)t & M; \
    m68k.c = m68k.x = t > M; m68k.v = ((s ^ r) & (d ^ r) & B) != 0; \
    m68k.n = (r & B) != 0; m68k.z = r ? 0 : m68k.z; return r; } \
static inline uint32_t SUBX##BITS(uint32_t d, uint32_t s) { \
    uint32_t r = (d - s - m68k.x) & M; \
    m68k.c = m68k.x = (uint64_t)(s & M) + m68k.x > (d & M); m68k.v = ((s ^ d) & (r ^ d) & B) != 0; \
    m68k.n = (r & B) != 0; m68k.z = r ? 0 : m68k.z; return r; } \
static inline uint32_t NEG##BITS(uint32_t d) { return SUB##BITS(0, d); } \
static inline uint32_t NEGX##BITS(uint32_t d) { return SUBX##BITS(0, d); }
ARITH(8, 0xFFu, 0x80u)
ARITH(16, 0xFFFFu, 0x8000u)
ARITH(32, 0xFFFFFFFFu, 0x80000000u)
#undef ARITH

/* ------------------------------------------------------------------ conditions */
#define CC_T  1
#define CC_F  0
#define CC_HI (!m68k.c && !m68k.z)
#define CC_LS (m68k.c || m68k.z)
#define CC_CC (!m68k.c)
#define CC_CS (m68k.c)
#define CC_NE (!m68k.z)
#define CC_EQ (m68k.z)
#define CC_VC (!m68k.v)
#define CC_VS (m68k.v)
#define CC_PL (!m68k.n)
#define CC_MI (m68k.n)
#define CC_GE (m68k.n == m68k.v)
#define CC_LT (m68k.n != m68k.v)
#define CC_GT (!m68k.z && m68k.n == m68k.v)
#define CC_LE (m68k.z || m68k.n != m68k.v)

/* ------------------------------------------------------------------ shifts and rotates
 * count is the raw count (immediate 1-8 or a register value, used mod 64).
 * bits = 8/16/32. Return the result; set flags like the 68000. */
uint32_t m68k_asl(uint32_t v, uint32_t count, int bits);
uint32_t m68k_asr(uint32_t v, uint32_t count, int bits);
uint32_t m68k_lsl(uint32_t v, uint32_t count, int bits);
uint32_t m68k_lsr(uint32_t v, uint32_t count, int bits);
uint32_t m68k_rol(uint32_t v, uint32_t count, int bits);
uint32_t m68k_ror(uint32_t v, uint32_t count, int bits);
uint32_t m68k_roxl(uint32_t v, uint32_t count, int bits);
uint32_t m68k_roxr(uint32_t v, uint32_t count, int bits);

/* ------------------------------------------------------------------ multiply, divide, BCD */
uint32_t m68k_mulu(uint32_t d, uint32_t s);
uint32_t m68k_muls(uint32_t d, uint32_t s);
/* return 0 on divide by zero (caller raises the exception), else 1; result in *d */
int m68k_divu(uint32_t *d, uint32_t s);
int m68k_divs(uint32_t *d, uint32_t s);
uint32_t m68k_abcd(uint32_t d, uint32_t s);
uint32_t m68k_sbcd(uint32_t d, uint32_t s);
uint32_t m68k_nbcd(uint32_t d);

/* ------------------------------------------------------------------ control flow
 * Every function is `void F_xxxxxx(void)`. A call pushes the 68000 return
 * address and calls the C function; rts pops it and returns. If the stack
 * pointer does not come back balanced, the callee discarded its own return
 * address ("return from my caller too"), so the caller returns as well. */
typedef void (*m68k_fn)(void);

void m68k_call(uint32_t target);                      /* indirect call/jump through the table */
void m68k_exception(int vector, uint32_t pc);         /* push frame, run handler */
void m68k_interrupt(int level, uint32_t pc);          /* autovectored interrupt if not masked */
void m68k_fatal(const char *what, uint32_t addr, uint32_t value);
void m68k_stop(uint32_t sr);                          /* `stop`: wait for an interrupt */
void m68k_reset_insn(void);                           /* `reset` instruction */
m68k_fn m68k_lookup(uint32_t addr);                   /* NULL if addr is not a function */

#ifdef PSIV_CHECK_RETURNS
#define CHECK_RET(ret) do { if (m68k_last_ret != (ret)) m68k_fatal("return address", (ret), m68k_last_ret); } while (0)
#else
#define CHECK_RET(ret) ((void)0)
#endif

#define CALL(fn, ret) do { uint32_t sp0_ = A(7); PUSH32(ret); fn(); \
    if (A(7) != sp0_) { return; } CHECK_RET(ret); } while (0)
#define CALL_IND(target, ret) do { uint32_t sp0_ = A(7); PUSH32(ret); m68k_call(target); \
    if (A(7) != sp0_) { return; } CHECK_RET(ret); } while (0)
#define RTS() do { m68k_last_ret = POP32(); return; } while (0)
#define RTR() do { SET_CCR(POP16()); m68k_last_ret = POP32(); return; } while (0)
#define RTE() do { uint32_t sr_ = POP16(); m68k_last_ret = POP32(); SET_SR(sr_); return; } while (0)
#define TRAP(vec, ret) do { uint32_t sp0_ = A(7); m68k_exception(vec, ret); \
    if (A(7) != sp0_) { return; } } while (0)

#ifdef PSIV_TRACE
extern uint32_t m68k_trace_pc;
void m68k_trace(uint32_t pc);
#define BLOCK(pc) m68k_trace(pc)
#else
#define BLOCK(pc) ((void)0)
#endif

#endif
