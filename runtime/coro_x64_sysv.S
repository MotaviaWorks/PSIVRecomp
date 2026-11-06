/* The game's own stack (coro.h), x86-64 System V (Linux).
 *
 * coro_switch(void **save, void *load): rbp rbx r12-r15 go onto this stack,
 * its pointer into *save; then the same come off the stack at `load`, and
 * ret goes on where that one left off. coro_prepare (coro.c) lays out a
 * first frame in this shape. No CET marking: this switches stacks. */
    .text
    .globl  coro_switch
    .type   coro_switch, @function
coro_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size   coro_switch, .-coro_switch

/* the first switch to a new stack returns here: entry(arg), from r12 and r13 */
    .globl  coro_start
    .type   coro_start, @function
coro_start:
    movq    %r13, %rdi
    call    *%r12
    ud2
    .size   coro_start, .-coro_start

    .section .note.GNU-stack,"",@progbits
