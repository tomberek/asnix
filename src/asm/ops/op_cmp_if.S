// M162: branch-fused EQ/NEQ/LT/GE for `if <comparison> then ... else ...`
// -- compiler.c's AST_IF case emits one of these instead of
// {OP_EQ/OP_NEQ/OP_LT/OP_GE; OP_IF} whenever the `if`'s own condition is
// LITERALLY a bare comparison, branching directly off the SAME
// true/false result op_cmp.S's plain comparison ops compute internally,
// without ever tagging/pushing/popping/re-deriving a TRUE/FALSE Value
// for OP_IF to immediately re-validate. `a` = byte delta to the else-
// branch (self-relative, same convention as OP_IF -- patched via
// ib_patch_delta), taken when the comparison is FALSE; a TRUE result
// falls through to the very next op (the compiled "then" branch). See
// ir.h's own comment on these ops for the full rationale, including why
// `>`/`<=`-as-condition reuse OP_LT_IF (with swapped operand push order,
// and -- for `<=` only -- swapped then/else branches) rather than having
// their own dedicated ops, mirroring compiler.c's existing swapped-
// operand trick for plain (non-`if`) `>`/`<=`.
//
// Every behavioral nuance of the plain ops is preserved exactly: OP_EQ_IF/
// OP_NEQ_IF defer to the identical asnix_bshared_value_eq_op (real
// structural equality, never fatal); OP_LT_IF/OP_GE_IF keep the identical
// INT fast path and, on anything else, the identical C fallback
// (asnix_cmp_lt_values/asnix_cmp_ge_values -- float/string-aware, fatal on
// a genuine type mismatch) plain OP_LT/OP_GE already use.
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_eq_if
    .type asnix_op_eq_if, @function
asnix_op_eq_if:                         // a = byte delta to else
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
    movq %rax, %rdi                     // arg1 = lhs
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_bshared_value_eq_op
    popq %rdi                           // ENV
    popq %rsi                           // IP

    testl %eax, %eax
    jz .Lcmp_if_false                   // 0 == FALSE (not equal)
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_eq_if, . - asnix_op_eq_if

    .globl asnix_op_neq_if
    .type asnix_op_neq_if, @function
asnix_op_neq_if:                        // a = byte delta to else
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV
    movq %rax, %rdi                     // arg1 = lhs
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_bshared_value_eq_op
    popq %rdi                           // ENV
    popq %rsi                           // IP

    testl %eax, %eax
    jnz .Lcmp_if_false                   // nonzero == equal, so `!=` is FALSE
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_neq_if, . - asnix_op_neq_if

    .globl asnix_op_lt_if
    .type asnix_op_lt_if, @function
asnix_op_lt_if:                         // a = byte delta to else
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_if_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_if_slow
    sarq $ASNIX_TAG_BITS, %rax          // sign-extending shift: real int64 value
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax                     // flags := (lhs cmp rhs)
    jl .Llt_if_true
    jmp .Lcmp_if_false
.Llt_if_slow:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_cmp_lt_values            // returns a tagged TRUE/FALSE Value in %rax
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
asnix_op_ge_if:                         // a = byte delta to else
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw
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
    call asnix_cmp_ge_values            // returns a tagged TRUE/FALSE Value in %rax
    popq %rdi
    popq %rsi
    cmpq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    jne .Lcmp_if_false
.Lge_if_true:
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_ge_if, . - asnix_op_ge_if

    // Shared false-branch tail for all four ops above -- `a` is always a
    // byte delta from THIS op's own address (`%rsi` hasn't moved yet),
    // same self-relative convention as OP_IF's own false path.
.Lcmp_if_false:
    movslq ASNIX_IROP_OFF_A(%rsi), %rax
    addq %rax, %rsi
    movq (%rsi), %rax
    jmp *%rax
