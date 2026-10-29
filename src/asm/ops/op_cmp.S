#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_not_type:
    .ascii "asnix: '!' operand is not a boolean, aborting\n"
    .equ ASNIX_MSG_NOT_TYPE_LEN, . - .Lmsg_not_type

    .text

    .globl asnix_op_eq
    .type asnix_op_eq, @function
asnix_op_eq:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_bshared_value_eq_op
    popq %rdi
    popq %rsi

    testl %eax, %eax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    cmoveq %rdx, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_eq, . - asnix_op_eq

    .globl asnix_op_neq
    .type asnix_op_neq, @function
asnix_op_neq:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_bshared_value_eq_op
    popq %rdi
    popq %rsi

    testl %eax, %eax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    cmoveq %rdx, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_neq, . - asnix_op_neq

    .globl asnix_op_lt
    .type asnix_op_lt, @function
asnix_op_lt:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_slow
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovlq %rdx, %rax
    jmp .Llt_store
.Llt_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_cmp_lt_values
    popq %rdi
    popq %rsi
.Llt_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_lt, . - asnix_op_lt

    .globl asnix_op_gt
    .type asnix_op_gt, @function
asnix_op_gt:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lgt_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lgt_slow
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovgq %rdx, %rax
    jmp .Lgt_store
.Lgt_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_cmp_gt_values
    popq %rdi
    popq %rsi
.Lgt_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_gt, . - asnix_op_gt

    .globl asnix_op_le
    .type asnix_op_le, @function
asnix_op_le:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lle_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lle_slow
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovleq %rdx, %rax
    jmp .Lle_store
.Lle_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_cmp_le_values
    popq %rdi
    popq %rsi
.Lle_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_le, . - asnix_op_le

    .globl asnix_op_ge
    .type asnix_op_ge, @function
asnix_op_ge:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lge_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lge_slow
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovgeq %rdx, %rax
    jmp .Lge_store
.Lge_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_cmp_ge_values
    popq %rdi
    popq %rsi
.Lge_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_ge, . - asnix_op_ge

    .globl asnix_op_not
    .type asnix_op_not, @function
asnix_op_not:
    movq -8(%r12), %rax
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_IMM, %rcx
    jne .Lnot_type_error
    shrq $ASNIX_TAG_BITS, %rax
    cmpq $ASNIX_IMM_TRUE, %rax
    je .Lnot_to_false
    cmpq $ASNIX_IMM_FALSE, %rax
    jne .Lnot_type_error
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    jmp .Lnot_store
.Lnot_to_false:
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
.Lnot_store:
    movq %rax, -8(%r12)
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lnot_type_error:
    leaq .Lmsg_not_type(%rip), %rdi
    movq $ASNIX_MSG_NOT_TYPE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_not, . - asnix_op_not

