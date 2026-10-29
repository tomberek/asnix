#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_has_attr_dynamic
    .type asnix_op_has_attr_dynamic, @function
asnix_op_has_attr_dynamic:
    subq $16, %r12
    movq 8(%r12), %rcx
    movq (%r12), %rax

    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_bshared_has_attr_dynamic
    popq %rdi
    popq %rsi

    testl %eax, %eax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rcx
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovzq %rdx, %rcx
    movq %rcx, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_has_attr_dynamic, . - asnix_op_has_attr_dynamic

