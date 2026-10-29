#include "asnix/env.h"

    .text

    .globl asnix_env_alloc
    .type asnix_env_alloc, @function
asnix_env_alloc:
    .cfi_startproc
    pushq %rbx
    pushq %rbp
    movq %rdi, %rbx
    movq %rsi, %rbp
    leaq ASNIX_ENV_OFF_VALUES(, %rsi, 8), %rdi
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %rbp, asnix_stats_env_elements(%rip)
    testq %rbx, %rbx
    jz .Lenv_alloc_up_null
    subq asnix_env_arena_base(%rip), %rbx
.Lenv_alloc_up_null:
    movl %ebx, ASNIX_ENV_OFF_UP(%rax)
    popq %rbp
    popq %rbx
    ret
    .cfi_endproc
    .size asnix_env_alloc, . - asnix_env_alloc

    .globl asnix_env_lookup
    .type asnix_env_lookup, @function
asnix_env_lookup:
    .cfi_startproc
.Lenv_lookup_walk:
    testq %rsi, %rsi
    jz .Lenv_lookup_done
    movl ASNIX_ENV_OFF_UP(%rdi), %edi
    addq asnix_env_arena_base(%rip), %rdi
    andq $-2, %rdi
    decq %rsi
    jmp .Lenv_lookup_walk
.Lenv_lookup_done:
    movq ASNIX_ENV_OFF_VALUES(%rdi, %rdx, 8), %rax
    ret
    .cfi_endproc
    .size asnix_env_lookup, . - asnix_env_lookup

