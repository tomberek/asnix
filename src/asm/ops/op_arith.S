// Arithmetic ops assume both operands are already TAG_INT (trusted,
// well-formed IR in this minimal milestone -- type checking for untrusted
// programs is a later-milestone concern, see docs/architecture.md M4+).
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_add
    .type asnix_op_add, @function
asnix_op_add:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs (pushed last, popped first)
    subq $8, %r12
    movq (%r12), %rax                   // lhs
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    addq %rcx, %rax
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_add, . - asnix_op_add

    .globl asnix_op_sub
    .type asnix_op_sub, @function
asnix_op_sub:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs
    subq $8, %r12
    movq (%r12), %rax                   // lhs
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    subq %rcx, %rax                     // lhs - rhs
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_sub, . - asnix_op_sub

    .globl asnix_op_mul
    .type asnix_op_mul, @function
asnix_op_mul:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs
    subq $8, %r12
    movq (%r12), %rax                   // lhs
    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    imulq %rcx, %rax
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mul, . - asnix_op_mul
