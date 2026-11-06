/*
 * A second stack to run the game on (rewind.c). The recompiled game keeps
 * where it is in the C call chain itself -- every 68000 subroutine is a C
 * function -- so going back to an earlier moment means putting that stack
 * back as it was. The game runs on a stack of its own, at an address that
 * never changes; at each frame it switches to the program's own stack, and
 * while it waits there its stack can be copied, and copied back.
 *
 * coro_switch saves the callee-saved registers on the current stack, stores
 * the stack pointer in *save, loads `load` and returns there: in assembly,
 * one routine for each platform (coro_x64_win.asm, coro_x64_sysv.S,
 * coro_arm64.S); on Windows it also swaps the thread's stack bounds, so stack
 * probes and exceptions work on either stack.
 */
#ifndef PSIV_CORO_H
#define PSIV_CORO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void coro_switch(void **save, void *load);

typedef struct {
    unsigned char *base;                 /* lowest address */
    size_t size;
} CoroStack;

/* a stack of `size` bytes (rounded to pages), with a guard page under it
 * where the platform allows; 0 if there is no memory */
int coro_stack_new(CoroStack *s, size_t size);
/* the stack pointer that, switched to, calls entry(arg) on that stack */
void *coro_prepare(const CoroStack *s, void (*entry)(void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif
