// Comparison/boolean-NOT ops ("operators" milestone).
//
// == / != delegate to asnix_bshared_value_eq (src/c/builtins_shared.c) for
// real structural equality -- ints/IMM-subtag/string-or-path-bytes compare
// directly, lists/attrsets recurse elementwise (forcing lazy elements
// along the way), closures/primops/thunks fall back to pointer identity.
// A pure-asm raw `cmpq` on the two Value words (the original
// implementation here) is only correct for INT/IMM and happens to be
// right whenever the two pointers are literally identical, but is WRONG
// for any two separately-allocated but content-identical strings/lists/
// attrsets -- e.g. `[] != []` evaluated to `true`. This was reached in
// practice evaluating real nixpkgs (`(import ./lib/minfeatures.nix).missing
// != []` always true, since the compared lists are always two distinct
// empty-list allocations) -- see docs/architecture.md's Known Bugs list.
// Not a hot path concern worth hand-rolling in asm: see op_update.S's
// identical save-IP/ENV-around-a-plain-C-call convention, reused as-is.
//
// Ordering (< > <= >=): the all-int fast path below is unchanged since
// before M51; M51 extended the slow path (anything not plain-INT/INT) to
// call a dedicated C fallback per operator (asnix_cmp_{lt,gt,le,ge}_values,
// value_util.c) instead of an immediate fatal, so an int/float or
// float/float pair now compares correctly (real Nix promotes exactly the
// same way, see value_util.h's comment) while a genuine type error still
// fatals, just from C instead of asm now -- same save-IP/ENV-around-a-
// plain-C-call convention as op_update.S/==/!= above.
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
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV (2 pushes -- stays 16-byte aligned)
    movq %rax, %rdi                     // arg1 = lhs
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_bshared_value_eq
    popq %rdi                           // ENV
    popq %rsi                           // IP

    testl %eax, %eax
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    cmoveq %rdx, %rax                   // result == 0 -> FALSE, else TRUE
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_eq, . - asnix_op_eq

    .globl asnix_op_neq
    .type asnix_op_neq, @function
asnix_op_neq:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV
    movq %rax, %rdi                     // arg1 = lhs
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_bshared_value_eq
    popq %rdi                           // ENV
    popq %rsi                           // IP

    testl %eax, %eax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    cmoveq %rdx, %rax                   // result == 0 -> TRUE (not equal), else FALSE
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_neq, . - asnix_op_neq

    .globl asnix_op_lt
    .type asnix_op_lt, @function
asnix_op_lt:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw
    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Llt_slow
    sarq $ASNIX_TAG_BITS, %rax          // sign-extending shift: real int64 value
    sarq $ASNIX_TAG_BITS, %rcx
    cmpq %rcx, %rax                     // flags := (lhs cmp rhs)
    movq $((ASNIX_IMM_FALSE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rax
    movq $((ASNIX_IMM_TRUE << ASNIX_TAG_BITS) | ASNIX_TAG_IMM), %rdx
    cmovlq %rdx, %rax                   // lhs < rhs ?
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
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
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
    cmovgq %rdx, %rax                   // lhs > rhs ?
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
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
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
    cmovleq %rdx, %rax                  // lhs <= rhs ?
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
    subq $8, %r12
    movq (%r12), %rcx
    subq $8, %r12
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
    cmovgeq %rdx, %rax                  // lhs >= rhs ?
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

    // Unary `!`: operates in place on the top of VALSTACK (pop+push the
    // same slot), unlike the binary ops above.
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
