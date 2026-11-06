/* The game's own stack (coro.h), AArch64 (Android, Linux).
 *
 * coro_switch(void **save, void *load): x19-x30 and d8-d15 go onto this
 * stack (176 bytes, 16-aligned), its pointer into *save; then the same come
 * off the stack at `load`, and ret goes to the x30 found there. coro_prepare
 * (coro.c) lays out a first frame in this shape. */
    .text
    .globl  coro_switch
    .type   coro_switch, %function
    .p2align 2
coro_switch:
    sub     sp, sp, #176
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    stp     d8, d9, [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]
    mov     x2, sp
    str     x2, [x0]
    mov     sp, x1
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    ldp     d8, d9, [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]
    add     sp, sp, #176
    ret
    .size   coro_switch, .-coro_switch

/* the first switch to a new stack returns here: entry(arg), from x19 and x20 */
    .globl  coro_start
    .type   coro_start, %function
    .p2align 2
coro_start:
    mov     x0, x20
    blr     x19
    brk     #0
    .size   coro_start, .-coro_start

    .section .note.GNU-stack,"",%progbits
