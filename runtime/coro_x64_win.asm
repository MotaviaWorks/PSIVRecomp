; The game's own stack (coro.h), Windows x64 (MSVC's ml64).
;
; coro_switch(void **save, void *load): the callee-saved registers (rbp rbx
; rsi rdi r12-r15, xmm6-xmm15) and the thread's stack bounds in the TIB
; (StackBase gs:[8], StackLimit gs:[10h], DeallocationStack gs:[1478h]) go
; onto this stack, its pointer into *save; then the same come off the stack
; at `load`, and ret goes on where that one left off. coro_prepare (coro.c)
; lays out a first frame in this shape.

.code

coro_switch PROC
    push    rbp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 168
    movaps  [rsp], xmm6
    movaps  [rsp+16], xmm7
    movaps  [rsp+32], xmm8
    movaps  [rsp+48], xmm9
    movaps  [rsp+64], xmm10
    movaps  [rsp+80], xmm11
    movaps  [rsp+96], xmm12
    movaps  [rsp+112], xmm13
    movaps  [rsp+128], xmm14
    movaps  [rsp+144], xmm15
    mov     rax, qword ptr gs:[8]
    push    rax
    mov     rax, qword ptr gs:[10h]
    push    rax
    mov     rax, qword ptr gs:[1478h]
    push    rax
    mov     [rcx], rsp

    mov     rsp, rdx
    pop     rax
    mov     qword ptr gs:[1478h], rax
    pop     rax
    mov     qword ptr gs:[10h], rax
    pop     rax
    mov     qword ptr gs:[8], rax
    movaps  xmm6, [rsp]
    movaps  xmm7, [rsp+16]
    movaps  xmm8, [rsp+32]
    movaps  xmm9, [rsp+48]
    movaps  xmm10, [rsp+64]
    movaps  xmm11, [rsp+80]
    movaps  xmm12, [rsp+96]
    movaps  xmm13, [rsp+112]
    movaps  xmm14, [rsp+128]
    movaps  xmm15, [rsp+144]
    add     rsp, 168
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbx
    pop     rbp
    ret
coro_switch ENDP

; the first switch to a new stack returns here: entry(arg), from r12 and r13
coro_start PROC
    mov     rcx, r13
    sub     rsp, 32
    call    r12
    int     3
coro_start ENDP

END
