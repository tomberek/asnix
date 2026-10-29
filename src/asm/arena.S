#include "asnix/arena.h"

    .equ ASNIX_SYS_MMAP,       9
    .equ ASNIX_SYS_MADVISE,    28
    .equ ASNIX_MADV_HUGEPAGE,  14
    .equ ASNIX_MADV_POPULATE_WRITE, 23
    .equ ASNIX_PROT_READ,      0x1
    .equ ASNIX_PROT_WRITE,     0x2
    .equ ASNIX_MAP_PRIVATE,    0x02
    .equ ASNIX_MAP_ANONYMOUS,  0x20
    .equ ASNIX_PAGE_SIZE,      0x1000

    .bss
    .align 8
    .globl asnix_arena
    .type asnix_arena, @object
asnix_arena:
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .size asnix_arena, 32

    .section .rodata
.Lmsg_cap:
    .ascii "asnix: arena memory cap (ASNIX_MAX_MEMORY) exceeded, aborting\n"
    .equ ASNIX_MSG_CAP_LEN, . - .Lmsg_cap
.Lmsg_mmap:
    .ascii "asnix: mmap failed while growing the bump arena, aborting\n"
    .equ ASNIX_MSG_MMAP_LEN, . - .Lmsg_mmap

    .text

    .globl asnix_arena_init
    .type asnix_arena_init, @function
asnix_arena_init:
    .cfi_startproc
    leaq asnix_arena(%rip), %rax
    movq $0, ASNIX_ARENA_OFF_CUR(%rax)
    movq $0, ASNIX_ARENA_OFF_END(%rax)
    movq $0, ASNIX_ARENA_OFF_TOTAL(%rax)
    movq %rdi, ASNIX_ARENA_OFF_MAX(%rax)
    ret
    .cfi_endproc
    .size asnix_arena_init, . - asnix_arena_init

    .globl asnix_arena_alloc
    .type asnix_arena_alloc, @function
asnix_arena_alloc:
    .cfi_startproc
    pushq %rbx
    addq $7, %rdi
    andq $-8, %rdi
    movq %rdi, %rbx

    leaq asnix_arena(%rip), %rax
    movq ASNIX_ARENA_OFF_MAX(%rax), %rcx
    testq %rcx, %rcx
    jz .Lno_cap_check
    movq ASNIX_ARENA_OFF_TOTAL(%rax), %rdx
    addq %rbx, %rdx
    cmpq %rcx, %rdx
    jbe .Lno_cap_check
    jmp .Labort_cap

.Lno_cap_check:
    movq ASNIX_ARENA_OFF_CUR(%rax), %rcx
    movq ASNIX_ARENA_OFF_END(%rax), %rdx
    leaq (%rcx, %rbx), %rsi
    cmpq %rdx, %rsi
    ja .Lgrow

    movq %rsi, ASNIX_ARENA_OFF_CUR(%rax)
    movq ASNIX_ARENA_OFF_TOTAL(%rax), %rdx
    addq %rbx, %rdx
    movq %rdx, ASNIX_ARENA_OFF_TOTAL(%rax)
    movq %rcx, %rax
    popq %rbx
    ret

.Lgrow:
    movq %rbx, %r8
    addq $(ASNIX_PAGE_SIZE - 1), %r8
    andq $-ASNIX_PAGE_SIZE, %r8
    movq $ASNIX_ARENA_CHUNK_SIZE, %r9
    cmpq %r9, %r8
    cmovbe %r9, %r8

    pushq %r8

    xorq %rdi, %rdi
    movq %r8, %rsi
    movq $(ASNIX_PROT_READ | ASNIX_PROT_WRITE), %rdx
    movq $(ASNIX_MAP_PRIVATE | ASNIX_MAP_ANONYMOUS), %r10
    movq $-1, %r8
    xorq %r9, %r9
    movq $ASNIX_SYS_MMAP, %rax
    syscall
    testq %rax, %rax
    jl .Labort_mmap

    popq %r8
    pushq %rax
    pushq %r8
    call asnix_huge_pages_available
    popq %r8
    popq %rax
    testl %eax, %eax
    jz .Lno_hugepage_hint

    pushq %rax
    pushq %r8
    movq %rax, %rdi
    movq %r8, %rsi
    movq $ASNIX_MADV_HUGEPAGE, %rdx
    movq $ASNIX_SYS_MADVISE, %rax
    syscall

    movq $ASNIX_MADV_POPULATE_WRITE, %rdx
    movq $ASNIX_SYS_MADVISE, %rax
    syscall
    popq %r8
    popq %rax

.Lno_hugepage_hint:
    leaq asnix_arena(%rip), %rcx
    leaq (%rax, %r8), %rdx
    movq %rdx, ASNIX_ARENA_OFF_END(%rcx)

    movq %rax, %rsi
    addq %rbx, %rax
    movq %rax, ASNIX_ARENA_OFF_CUR(%rcx)
    movq ASNIX_ARENA_OFF_TOTAL(%rcx), %rdx
    addq %rbx, %rdx
    movq %rdx, ASNIX_ARENA_OFF_TOTAL(%rcx)

    movq %rsi, %rax
    popq %rbx
    ret

.Labort_mmap:
    leaq .Lmsg_mmap(%rip), %rdi
    movq $ASNIX_MSG_MMAP_LEN, %rsi
    jmp asnix_fatal

.Labort_cap:
    leaq .Lmsg_cap(%rip), %rdi
    movq $ASNIX_MSG_CAP_LEN, %rsi
    jmp asnix_fatal
    .cfi_endproc
    .size asnix_arena_alloc, . - asnix_arena_alloc

