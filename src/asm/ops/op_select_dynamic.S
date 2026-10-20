// `expr.${nameExpr}` -- dynamic attribute select. Unlike plain OP_SELECT
// (compile-time-known symbol, hand-rolled binary search in asm) or
// OP_SELECT_OR (ditto, plus a graceful miss path), the attribute name
// here is only known at runtime, so this just pops both operands and
// defers entirely to C (asnix_bshared_select_dynamic, builtins_shared.c:
// interns the name, binary-searches, forces an unforced thunk, fatal on
// a non-attrset target/non-string name/missing key) -- same save-IP/ENV-
// around-a-plain-C-call convention as op_update.S/op_cmp.S/op_arith.S's
// slow paths. Not a hot path (dynamic attribute names are rare relative
// to plain `.foo`), so there's no motivation to hand-roll this one in asm
// the way plain OP_SELECT is.
//
// Pops the name Value (pushed last by the compiler, see compiler.c's
// AST_SELECT case) then the select target.
#include "asnix/ir.h"
#include "asnix/value.h"

    .text

    .globl asnix_op_select_dynamic
    .type asnix_op_select_dynamic, @function
asnix_op_select_dynamic:
    subq $8, %r12
    movq (%r12), %rcx                   // name raw (pushed last, popped first)
    subq $8, %r12
    movq (%r12), %rax                   // target raw

    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV
    movq %rax, %rdi                     // arg1 = target
    movq %rcx, %rsi                     // arg2 = name
    call asnix_bshared_select_dynamic
    popq %rdi                           // ENV
    popq %rsi                           // IP

    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax
    .size asnix_op_select_dynamic, . - asnix_op_select_dynamic
