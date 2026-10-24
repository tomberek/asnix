#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_push_int_overflow:
    .ascii "asnix: integer literal out of asnix's representable range, aborting\n"
    .equ ASNIX_MSG_PUSH_INT_OVERFLOW_LEN, . - .Lmsg_push_int_overflow

    .text

    /* M56: a literal whose true int64 value falls outside the ±2^60
     * ASNIX_INT_MIN/MAX window (include/asnix/value.h) used to get
     * silently corrupted right here -- `salq $ASNIX_TAG_BITS` drops the
     * literal's own top bits with no check at all, e.g. the literal
     * `9223372036854775807` (INT64_MAX) round-tripped to `-1`. Checked
     * inline (not via a `call asnix_make_int`) for the same hot-path
     * reason this op already inlines the tagging bit trick instead of
     * calling asnix_make_int -- see value.S's header comment.
     *
     * M169: the literal no longer lives inline in `a` (IrOp's operand
     * fields are only 32 bits now, too narrow for an arbitrary int64 Nix
     * literal) -- `b` is a pool index instead, exactly like
     * OP_PUSH_CONST, loaded here before the same range check/tag runs
     * against the pool-loaded value. */
    .globl asnix_op_push_int
    .type asnix_op_push_int, @function
asnix_op_push_int:
    movl ASNIX_IROP_OFF_B(%rsi), %eax   // zero-extended pool index
    movq (%r13, %rax, 8), %rax          // rax = raw (untagged) int64 literal
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lpush_int_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lpush_int_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    ASNIX_CHECK_VALSTACK %rcx           // %rcx is unused in this handler
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
asnix_op_push_const:                    // b = pool index of a pre-formed Value.raw
    movl ASNIX_IROP_OFF_B(%rsi), %eax
    movq (%r13, %rax, 8), %rax
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_push_const, . - asnix_op_push_const
