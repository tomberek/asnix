#include "asnix/arena.h"
#include "asnix/env.h"

    .equ ASNIX_SYS_MMAP,       9
    .equ ASNIX_SYS_MADVISE,    28
    .equ ASNIX_MADV_HUGEPAGE,  14
    .equ ASNIX_PROT_READ,      0x1
    .equ ASNIX_PROT_WRITE,     0x2
    .equ ASNIX_MAP_PRIVATE,    0x02
    .equ ASNIX_MAP_ANONYMOUS,  0x20
    .equ ASNIX_MAP_NORESERVE,  0x4000

    .bss
    .align 8
    .globl asnix_env_arena_base
    .type asnix_env_arena_base, @object
asnix_env_arena_base:
    .quad 0
    .size asnix_env_arena_base, 8

    .align 8
asnix_env_arena_cur:
    .quad 0
    .size asnix_env_arena_cur, 8

    .align 8
asnix_env_arena_end:
    .quad 0
    .size asnix_env_arena_end, 8

    .section .rodata
.Lmsg_mmap:
    .ascii "asnix: mmap failed while reserving the Env arena, aborting\n"
    .equ ASNIX_MSG_MMAP_LEN, . - .Lmsg_mmap
.Lmsg_cap:
    .ascii "asnix: Env arena exhausted (ASNIX_ENV_ARENA_SIZE), aborting\n"
    .equ ASNIX_MSG_CAP_LEN, . - .Lmsg_cap

    .text

    .globl asnix_env_arena_alloc
    .type asnix_env_arena_alloc, @function
asnix_env_arena_alloc:
    .cfi_startproc
    leaq asnix_env_arena_cur(%rip), %rax
    movq (%rax), %rcx
    testq %rcx, %rcx
    jnz .Lenv_arena_ready

    pushq %rdi
    call asnix_env_arena_do_init
    popq %rdi

.Lenv_arena_ready:
    addq $3, %rdi
    andq $-4, %rdi
    leaq asnix_env_arena_cur(%rip), %rax
    movq (%rax), %rcx
    leaq (%rcx, %rdi), %rdx
    leaq asnix_env_arena_end(%rip), %rsi
    cmpq (%rsi), %rdx
    ja .Lenv_arena_abort_cap
    movq %rdx, (%rax)
    movq %rcx, %rax
    ret
.Lenv_arena_abort_cap:
    leaq .Lmsg_cap(%rip), %rdi
    movq $ASNIX_MSG_CAP_LEN, %rsi
    jmp asnix_fatal
    .cfi_endproc
    .size asnix_env_arena_alloc, . - asnix_env_arena_alloc

    .type asnix_env_arena_do_init, @function
asnix_env_arena_do_init:
    .cfi_startproc
    xorq %rdi, %rdi
    movq $ASNIX_ENV_ARENA_SIZE, %rsi
    movq $(ASNIX_PROT_READ | ASNIX_PROT_WRITE), %rdx
    movq $(ASNIX_MAP_PRIVATE | ASNIX_MAP_ANONYMOUS | ASNIX_MAP_NORESERVE), %r10
    movq $-1, %r8
    xorq %r9, %r9
    movq $ASNIX_SYS_MMAP, %rax
    syscall
    testq %rax, %rax
    jl .Lenv_arena_abort_mmap

    pushq %rax
    call asnix_huge_pages_available
    popq %rax
    testl %eax, %eax
    jz .Lno_env_hugepage_hint

    pushq %rax
    movq %rax, %rdi
    movq $ASNIX_ENV_ARENA_SIZE, %rsi
    movq $ASNIX_MADV_HUGEPAGE, %rdx
    movq $ASNIX_SYS_MADVISE, %rax
    syscall
    popq %rax

.Lno_env_hugepage_hint:
    leaq asnix_env_arena_base(%rip), %rcx
    movq %rax, (%rcx)
    addq $8, %rax
    leaq asnix_env_arena_cur(%rip), %rcx
    movq %rax, (%rcx)
    subq $8, %rax
    addq $ASNIX_ENV_ARENA_SIZE, %rax
    leaq asnix_env_arena_end(%rip), %rcx
    movq %rax, (%rcx)
    ret
.Lenv_arena_abort_mmap:
    leaq .Lmsg_mmap(%rip), %rdi
    movq $ASNIX_MSG_MMAP_LEN, %rsi
    jmp asnix_fatal
    .cfi_endproc
    .size asnix_env_arena_do_init, . - asnix_env_arena_do_init

