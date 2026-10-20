// `assert cond; body` ("operators" milestone). Lowered by compiler.c to
// roughly `if cond then body else <this>`: OP_ASSERT_FAIL is only ever
// reached via OP_IF's false branch, and never returns -- there is no
// "resume" path, matching real Nix's assertion-failure semantics (the
// whole evaluation aborts).
#include "asnix/ir.h"

    .text

    .globl asnix_op_assert_fail
    .type asnix_op_assert_fail, @function
asnix_op_assert_fail:                   // b = pool index of {msg ptr, msg len}
    movq ASNIX_IROP_OFF_B(%rsi), %rax
    movq (%r13, %rax, 8), %rdi          // msg ptr (compiler.c-built, names the failed condition)
    movq 8(%r13, %rax, 8), %rsi         // msg len
    jmp asnix_fatal
    .size asnix_op_assert_fail, . - asnix_op_assert_fail
