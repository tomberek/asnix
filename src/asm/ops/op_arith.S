// `-`/`*` are int-only in real Nix (no string/path semantics), so both
// operands must be TAG_INT, checked below -- fatal, clear type error
// otherwise. `+` is the one arithmetic operator real Nix overloads for
// string/path concatenation too (see asnix_add_values, value_util.c);
// the fast all-int case stays inline asm, anything else defers to that C
// function (same save-IP/ENV-around-a-plain-C-call convention as
// op_update.S/op_cmp.S's == and !=). This replaces a real bug: this file
// previously did a raw arithmetic shift+add on *any* two operands with no
// tag check at all ("trusted, well-formed IR" assumption from an early
// milestone), so e.g. `"fo" + "o"` silently produced a garbage int instead
// of concatenating or erroring -- found evaluating real nixpkgs, see
// docs/architecture.md's Known Bugs list.
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_subarith_type:
    .ascii "asnix: '-'/'*' operands must both be integers, aborting\n"
    .equ ASNIX_MSG_SUBARITH_TYPE_LEN, . - .Lmsg_subarith_type
.Lmsg_div_type:
    .ascii "asnix: '/' operands must both be integers, aborting\n"
    .equ ASNIX_MSG_DIV_TYPE_LEN, . - .Lmsg_div_type
.Lmsg_div_zero:
    .ascii "asnix: division by zero, aborting\n"
    .equ ASNIX_MSG_DIV_ZERO_LEN, . - .Lmsg_div_zero
.Lmsg_add_overflow:
    .ascii "asnix: integer overflow in '+', aborting\n"
    .equ ASNIX_MSG_ADD_OVERFLOW_LEN, . - .Lmsg_add_overflow
.Lmsg_sub_overflow:
    .ascii "asnix: integer overflow in '-', aborting\n"
    .equ ASNIX_MSG_SUB_OVERFLOW_LEN, . - .Lmsg_sub_overflow
.Lmsg_mul_overflow:
    .ascii "asnix: integer overflow in '*', aborting\n"
    .equ ASNIX_MSG_MUL_OVERFLOW_LEN, . - .Lmsg_mul_overflow
.Lmsg_div_overflow:
    .ascii "asnix: integer overflow in '/', aborting\n"
    .equ ASNIX_MSG_DIV_OVERFLOW_LEN, . - .Lmsg_div_overflow

    /* M56: overflow checks below follow docs/architecture.md's M23 "Known
     * Bugs" writeup verbatim ("check the native overflow flag after each
     * op, then re-check the narrower ASNIX_INT_MIN/MAX window, fatal on
     * either"). Both checks are necessary and distinct: operands are
     * always already within ASNIX_INT_MIN/MAX (±2^60, far inside int64),
     * so `+`/`-` can never trip the CPU's own 64-bit OF flag -- only the
     * narrower-than-64-bit re-check catches e.g. 2^59 + 2^59 = 2^60
     * landing just past ASNIX_INT_MAX. `*` is the opposite: two ±2^60
     * operands can genuinely overflow a 64-bit `imulq` (e.g. the mul
     * test's 4294967297^2), so `jo` right after `imulq` is load-bearing
     * there and is checked *first* -- a tripped 64-bit multiply leaves
     * garbage in the low 64 bits, which the narrower range check alone
     * could coincidentally pass. `/`'s quotient can only ever exceed the
     * ±2^60 window in the ASNIX_INT_MIN / -1 edge case (never a genuine
     * `idivq` CPU trap, since both operands are already bounded), so it
     * only needs the range re-check, no `jo`. */
    .text

    .globl asnix_op_add
    .type asnix_op_add, @function
asnix_op_add:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs raw (pushed last, popped first)
    subq $8, %r12
    movq (%r12), %rax                   // lhs raw

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ladd_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ladd_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    addq %rcx, %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Ladd_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Ladd_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Ladd_store

.Ladd_slow:                             // at least one operand isn't an int --
                                         // asnix_add_values handles int-vs-
                                         // nonint type errors and string/path
                                         // concatenation alike.
    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV
    movq %rax, %rdi                     // arg1 = lhs
    movq %rcx, %rsi                     // arg2 = rhs
    call asnix_add_values
    popq %rdi                           // ENV
    popq %rsi                           // IP

.Ladd_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Ladd_overflow:
    leaq .Lmsg_add_overflow(%rip), %rdi
    movq $ASNIX_MSG_ADD_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_add, . - asnix_op_add

    .globl asnix_op_sub
    .type asnix_op_sub, @function
asnix_op_sub:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs
    subq $8, %r12
    movq (%r12), %rax                   // lhs

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lsub_type_error
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lsub_type_error

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    subq %rcx, %rax                     // lhs - rhs
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lsub_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lsub_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lsub_type_error:
    leaq .Lmsg_subarith_type(%rip), %rdi
    movq $ASNIX_MSG_SUBARITH_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lsub_overflow:
    leaq .Lmsg_sub_overflow(%rip), %rdi
    movq $ASNIX_MSG_SUB_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_sub, . - asnix_op_sub

    .globl asnix_op_mul
    .type asnix_op_mul, @function
asnix_op_mul:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs
    subq $8, %r12
    movq (%r12), %rax                   // lhs

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lmul_type_error
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lmul_type_error

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    imulq %rcx, %rax
    jo .Lmul_overflow                   // true 64-bit overflow: low 64 bits are
                                         // garbage, must check before anything else
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Lmul_overflow
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Lmul_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Lmul_type_error:
    leaq .Lmsg_subarith_type(%rip), %rdi
    movq $ASNIX_MSG_SUBARITH_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lmul_overflow:
    leaq .Lmsg_mul_overflow(%rip), %rdi
    movq $ASNIX_MSG_MUL_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_mul, . - asnix_op_mul

    /* `/` (M23, integer division): real Nix's `/` truncates toward zero
     * for two ints (verified against `nix-instantiate --eval`: `7 / 2` =>
     * 3, `(0-7) / 2` => -3, NOT -4 -- i.e. C's `idivq` semantics exactly,
     * not floor division), and is a hard type error for anything but two
     * ints here (real Nix also allows floats, producing a float result --
     * not implemented, since asnix has no float type at all yet; this is
     * a documented, narrower scope cut, same spirit as every other
     * int-only limitation already in this file). Division by zero is a
     * clean fatal error (checked explicitly before `idivq`), not a SIGFPE
     * crash. */
    .globl asnix_op_div
    .type asnix_op_div, @function
asnix_op_div:
    subq $8, %r12
    movq (%r12), %rcx                   // rhs
    subq $8, %r12
    movq (%r12), %rax                   // lhs

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ldiv_type_error
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ldiv_type_error

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    testq %rcx, %rcx
    jz .Ldiv_zero_error
    cqto                                 // sign-extend %rax into %rdx:%rax for idivq
    idivq %rcx                           // quotient (truncated toward zero) in %rax
    movq $ASNIX_INT_MAX, %r9
    cmpq %r9, %rax
    jg .Ldiv_overflow                    // only possible case: ASNIX_INT_MIN / -1
    movq $ASNIX_INT_MIN, %r9
    cmpq %r9, %rax
    jl .Ldiv_overflow
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Ldiv_type_error:
    leaq .Lmsg_div_type(%rip), %rdi
    movq $ASNIX_MSG_DIV_TYPE_LEN, %rsi
    jmp asnix_fatal
.Ldiv_zero_error:
    leaq .Lmsg_div_zero(%rip), %rdi
    movq $ASNIX_MSG_DIV_ZERO_LEN, %rsi
    jmp asnix_fatal
.Ldiv_overflow:
    leaq .Lmsg_div_overflow(%rip), %rdi
    movq $ASNIX_MSG_DIV_OVERFLOW_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_div, . - asnix_op_div
