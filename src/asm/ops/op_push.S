#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_push_int_overflow:
    .ascii "asnix: integer literal out of asnix's representable range, aborting\n"
    .equ ASNIX_MSG_PUSH_INT_OVERFLOW_LEN, . - .Lmsg_push_int_overflow

    .text

    .globl asnix_op_push_int
    .type asnix_op_push_int, @function
asnix_op_push_int:
    movl ASNIX_IROP_OFF_B(%rsi), %eax
    movq (%r13, %rax, 8), %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lpush_int_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lpush_int_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lpush_int_overflow:
    leaq .Lmsg_push_int_overflow(%rip), %rdi
    movq $ASNIX_MSG_PUSH_INT_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_push_int, . - asnix_op_push_int

    .globl asnix_op_push_true
    .type asnix_op_push_true, @function
asnix_op_push_true:
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    ASNIX_CHECK_VALSTACK %rcx
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
    ASNIX_CHECK_VALSTACK %rcx
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
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_push_null, . - asnix_op_push_null

    .globl asnix_op_push_const
    .type asnix_op_push_const, @function
asnix_op_push_const:
    movl ASNIX_IROP_OFF_B(%rsi), %eax
    movq (%r13, %rax, 8), %rax
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_push_const, . - asnix_op_push_const

