#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"

    .text

    .globl asnix_op_var_ref_cell
    .type asnix_op_var_ref_cell, @function
asnix_op_var_ref_cell:
    movq %rdi, %rcx
    movl ASNIX_IROP_OFF_A(%rsi), %edx
.Lvar_ref_cell_walk:
    testq %rdx, %rdx
    jz .Lvar_ref_cell_done
    movl ASNIX_ENV_OFF_UP(%rcx), %ecx
    addq asnix_env_arena_base(%rip), %rcx
    andq $-2, %rcx
    decq %rdx
    jmp .Lvar_ref_cell_walk
.Lvar_ref_cell_done:
    movl ASNIX_IROP_OFF_B(%rsi), %edx
    leaq ASNIX_ENV_OFF_VALUES(%rcx, %rdx, 8), %rbx

    pushq %rsi
    pushq %rdi

    movq %rbx, %rdi
    call asnix_make_cell_value

    popq %rdi
    popq %rsi

    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12

    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_var_ref_cell, . - asnix_op_var_ref_cell

