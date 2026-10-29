#include "asnix/env.h"
#include "asnix/ir.h"

    .text

    .globl asnix_op_let
    .type asnix_op_let, @function
asnix_op_let:
    movl ASNIX_IROP_OFF_A(%rsi), %ebx
    movq %rsi, %rbp
    movq %rdi, %r15

    leaq ASNIX_ENV_OFF_VALUES(, %rbx, 8), %rdi
    call asnix_env_arena_alloc
    incq asnix_stats_env(%rip)
    addq %rbx, asnix_stats_env_elements(%rip)
    movq %r15, %rcx
    subq asnix_env_arena_base(%rip), %rcx
    movl %ecx, ASNIX_ENV_OFF_UP(%rax)

    movq %rbx, %rcx
.Lop_let_bind:
    testq %rcx, %rcx
    jz .Lop_let_done
    decq %rcx
    subq $8, %r12
    movq (%r12), %rdx
    movq %rdx, ASNIX_ENV_OFF_VALUES(%rax, %rcx, 8)
    jmp .Lop_let_bind
.Lop_let_done:
    movq %rax, %rdi
    movq %rbp, %rsi
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_let, . - asnix_op_let

