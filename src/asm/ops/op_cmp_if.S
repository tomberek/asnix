#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_eq_if
    .type asnix_op_eq_if, @function
asnix_op_eq_if:
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_bshared_value_eq_op
    popq %rdi
    popq %rsi

    testl %eax, %eax
    jz .Lcmp_if_false
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_eq_if, . - asnix_op_eq_if

    .globl asnix_op_neq_if
    .type asnix_op_neq_if, @function
asnix_op_neq_if:
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_bshared_value_eq_op
    popq %rdi
    popq %rsi

    testl %eax, %eax
    jnz .Lcmp_if_false
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_neq_if, . - asnix_op_neq_if

    .globl asnix_op_lt_if
    .type asnix_op_lt_if, @function
asnix_op_lt_if:
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_if_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_if_slow
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax
    jl .Llt_if_true
    jmp .Lcmp_if_false
.Llt_if_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_cmp_lt_values
    popq %rdi
    popq %rsi
    cmpq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    jne .Lcmp_if_false
.Llt_if_true:
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_lt_if, . - asnix_op_lt_if

    .globl asnix_op_ge_if
    .type asnix_op_ge_if, @function
asnix_op_ge_if:
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
    movq (%r12), %rax
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lge_if_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lge_if_slow
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax
    jge .Lge_if_true
    jmp .Lcmp_if_false
.Lge_if_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_cmp_ge_values
    popq %rdi
    popq %rsi
    cmpq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    jne .Lcmp_if_false
.Lge_if_true:
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_ge_if, . - asnix_op_ge_if

.Lcmp_if_false:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax

