#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_push_int
    .type asnix_op_push_int, @function
asnix_op_push_int:
    movq ASNIX_IROP_OFF_A(%rsi), %rax
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_push_int, . - asnix_op_push_int

    .globl asnix_op_push_true
    .type asnix_op_push_true, @function
asnix_op_push_true:
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_push_true, . - asnix_op_push_true

    .globl asnix_op_push_false
    .type asnix_op_push_false, @function
asnix_op_push_false:
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_push_false, . - asnix_op_push_false

    .globl asnix_op_push_null
    .type asnix_op_push_null, @function
asnix_op_push_null:
    movq $((ASNIX_IMM_NULL << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_push_null, . - asnix_op_push_null
