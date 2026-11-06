/*
 * A second stack: see coro.h. The routine that switches is in assembly; here
 * are the stack itself and the first frame laid out on it, as coro_switch
 * leaves a stack it switched away from, so the first switch "returns" into
 * coro_start (assembly too), which calls entry(arg).
 */
#include "coro.h"

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 5105)          /* winbase.h under the C11 preprocessor */
#endif
#include <windows.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

void coro_start(void);                   /* assembly: calls entry(arg) from the saved registers */

int coro_stack_new(CoroStack *s, size_t size) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t page = si.dwPageSize;
    size = (size + page - 1) / page * page;
    unsigned char *p = VirtualAlloc(NULL, size + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return 0;
    DWORD old;
    VirtualProtect(p, page, PAGE_NOACCESS, &old);             /* the guard under it */
    s->base = p + page;
#else
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size = (size + page - 1) / page * page;
    unsigned char *p = mmap(NULL, size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    mprotect(p, page, PROT_NONE);                             /* the guard under it */
    s->base = p + page;
#endif
    s->size = size;
    memset(s->base, 0, size);                                 /* every page committed and touched */
    return 1;
}

void *coro_prepare(const CoroStack *s, void (*entry)(void *), void *arg) {
    uintptr_t top = ((uintptr_t)s->base + s->size) & ~(uintptr_t)15;
#if defined(_M_X64) || (defined(_WIN32) && defined(__x86_64__))
    /* [0] DeallocationStack, [8] StackLimit, [16] StackBase (the TIB's),
     * [24] xmm6-xmm15 (160 bytes) and 8 spare, [192] r15 r14 r13 r12 rdi rsi
     * rbx rbp, [256] the return address: coro_start, with r12 = entry and
     * r13 = arg. The saved pointer sits 8 past a 16-byte boundary, as a real
     * switch leaves it. */
    uint64_t *sp = (uint64_t *)(top - 280);
    memset(sp, 0, 280);
    sp[0] = (uint64_t)(uintptr_t)s->base;
    sp[1] = (uint64_t)(uintptr_t)s->base;
    sp[2] = (uint64_t)(uintptr_t)(s->base + s->size);
    sp[192 / 8 + 3] = (uint64_t)(uintptr_t)entry;             /* r12 */
    sp[192 / 8 + 2] = (uint64_t)(uintptr_t)arg;               /* r13 */
    sp[256 / 8] = (uint64_t)(uintptr_t)coro_start;
    return sp;
#elif defined(__x86_64__)
    /* [0] r15 r14 r13 r12 rbx rbp, [48] the return address: coro_start, with
     * r12 = entry and r13 = arg */
    uint64_t *sp = (uint64_t *)(top - 72);
    memset(sp, 0, 72);
    sp[3] = (uint64_t)(uintptr_t)entry;                       /* r12 */
    sp[2] = (uint64_t)(uintptr_t)arg;                         /* r13 */
    sp[6] = (uint64_t)(uintptr_t)coro_start;
    return sp;
#elif defined(__aarch64__) || defined(_M_ARM64)
    /* [0] x19 x20 ... x28, [80] x29 x30, [96] d8-d15, 176 bytes: x30 =
     * coro_start, with x19 = entry and x20 = arg; x29 = 0 ends the frame chain */
    uint64_t *sp = (uint64_t *)(top - 192);
    memset(sp, 0, 192);
    sp[0] = (uint64_t)(uintptr_t)entry;                       /* x19 */
    sp[1] = (uint64_t)(uintptr_t)arg;                         /* x20 */
    sp[11] = (uint64_t)(uintptr_t)coro_start;                 /* x30 */
    return sp;
#else
#error "coro.c: no stack switch for this processor"
#endif
}
