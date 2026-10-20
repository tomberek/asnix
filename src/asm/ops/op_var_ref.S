#include "asnix/env.h"
#include "asnix/ir.h"

    .text

    .globl asnix_op_var_ref
    .type asnix_op_var_ref, @function
asnix_op_var_ref:                       // a = depth, b = slot
    movq %rdi, %rcx                     // rcx = env, walked up `depth` hops
    movq ASNIX_IROP_OFF_A(%rsi), %rdx   // rdx = depth
.Lvar_ref_walk:
    testq %rdx, %rdx
    jz .Lvar_ref_done
    movq ASNIX_ENV_OFF_UP(%rcx), %rcx
    decq %rdx
    jmp .Lvar_ref_walk
.Lvar_ref_done:
    movq ASNIX_IROP_OFF_B(%rsi), %rdx   // rdx = slot
    movq ASNIX_ENV_OFF_VALUES(%rcx, %rdx, 8), %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_var_ref, . - asnix_op_var_ref
