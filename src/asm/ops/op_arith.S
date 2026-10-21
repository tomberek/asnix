// `+` is the one arithmetic operator real Nix overloads for string/path
// concatenation too (see asnix_add_values, value_util.c); `-`/`*`/`/` are
// numeric-only (int or float -- M51 added float promotion, see each slow
// path's own comment below) but otherwise all four follow the identical
// shape: the fast all-int case stays inline asm, anything else (a float
// operand, or a genuine type error) defers to a C helper (same
// save-IP/ENV-around-a-plain-C-call convention as op_update.S/op_cmp.S's
// == and !=). This replaces a real bug: this file previously did a raw
// arithmetic shift+add on *any* two operands with no tag check at all
// ("trusted, well-formed IR" assumption from an early milestone), so e.g.
// `"fo" + "o"` silently produced a garbage int instead of concatenating or
// erroring -- found evaluating real nixpkgs, see docs/architecture.md's
// Known Bugs list.
#include "asnix/ir.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_div_zero:
    .ascii "asnix: division by zero, aborting\n"
    .equ ASNIX_MSG_DIV_ZERO_LEN, . - .Lmsg_div_zero

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
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Ladd_store

.Ladd_slow:                             // at least one operand isn't an int --
                                         // asnix_add_values handles int/float
                                         // promotion, int-vs-nonint type
                                         // errors, and string/path
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
    jne .Lsub_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lsub_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    subq %rcx, %rax                     // lhs - rhs
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Lsub_store

.Lsub_slow:                             // at least one operand isn't an int --
                                         // asnix_sub_values (M51) handles
                                         // int/float promotion and the
                                         // genuine-type-error fatal alike.
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_sub_values
    popq %rdi
    popq %rsi

.Lsub_store:
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

    movq %rax, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lmul_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Lmul_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    imulq %rcx, %rax
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Lmul_store

.Lmul_slow:                             // M51: asnix_mul_values handles
                                         // int/float promotion + fatal.
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_mul_values
    popq %rdi
    popq %rsi

.Lmul_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_mul, . - asnix_op_mul

    /* `/` (M23, integer division; M51 added float promotion): real Nix's
     * `/` truncates toward zero for two ints (verified against
     * `nix-instantiate --eval`: `7 / 2` => 3, `(0-7) / 2` => -3, NOT -4 --
     * i.e. C's `idivq` semantics exactly, not floor division). The all-int
     * fast path below is unchanged from before M51; a float operand (or a
     * genuine type error) falls to asnix_div_values (value_util.c), which
     * replicates real Nix's own somewhat surprising rule that EVERY
     * division by a zero divisor is a hard "division by zero" error, even
     * when the result would otherwise be a well-defined IEEE-754 infinity
     * (`1.0 / 0.0` errors on real Nix too, not just `1 / 0`) -- see that
     * function's own comment. Division by zero in the pure-int fast path
     * below is a clean fatal error (checked explicitly before `idivq`),
     * not a SIGFPE crash. */
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
    jne .Ldiv_slow
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_TAG_INT, %r8
    jne .Ldiv_slow

    sarq $ASNIX_TAG_BITS, %rax
    sarq $ASNIX_TAG_BITS, %rcx
    testq %rcx, %rcx
    jz .Ldiv_zero_error
    cqto                                 // sign-extend %rax into %rdx:%rax for idivq
    idivq %rcx                           // quotient (truncated toward zero) in %rax
    salq $ASNIX_TAG_BITS, %rax
    orq $ASNIX_TAG_INT, %rax
    jmp .Ldiv_store

.Ldiv_slow:                             // M51: asnix_div_values handles
                                         // int/float promotion, the
                                         // zero-divisor fatal, and the
                                         // genuine-type-error fatal alike.
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi
    movq %rcx, %rsi
    call asnix_div_values
    popq %rdi
    popq %rsi

.Ldiv_store:
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
.Ldiv_zero_error:
    leaq .Lmsg_div_zero(%rip), %rdi
    movq $ASNIX_MSG_DIV_ZERO_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_div, . - asnix_op_div
