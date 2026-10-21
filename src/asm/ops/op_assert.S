// `assert cond; body` ("operators" milestone). Lowered by compiler.c to
// roughly `if cond then body else <this>`: OP_ASSERT_FAIL is only ever
// reached via OP_IF's false branch, and never returns -- there is no
// "resume" path, matching real Nix's assertion-failure semantics (the
// whole evaluation aborts) UNLESS a `builtins.tryEval` is currently
// active (M30): real Nix's `tryEval` catches a failed `assert` exactly
// like `throw` (both are the same `AssertionError` exception class,
// confirmed against `nix-instantiate --eval`), so this routes through
// the same catchable mechanism `prim_throw` uses (asnix_tryeval_raise_
// or_fatal, tryeval.c) instead of jumping to asnix_fatal directly --
// falls back to exactly asnix_fatal's own write+exit(1) behavior when
// no tryEval is active, so a top-level failed assert looks identical to
// before.
#include "asnix/ir.h"

    .text

    .globl asnix_op_assert_fail
    .type asnix_op_assert_fail, @function
asnix_op_assert_fail:                   // b = pool index of {msg ptr, msg len}
    movq ASNIX_IROP_OFF_B(%rsi), %rax
    movq (%r13, %rax, 8), %rdi          // msg ptr (compiler.c-built, names the failed condition)
    movq 8(%r13, %rax, 8), %rsi         // msg len
    // `call`, not `jmp`, even though this never returns: unlike
    // asnix_fatal (hand-written asm, raw syscalls, safe to tail-jump
    // into regardless of alignment), this is a real compiled C function
    // -- `call` correctly sets up the SysV-required 16-byte-aligned-at-
    // entry stack, which a bare `jmp` here (no implicit return-address
    // push) would NOT, since nothing else adjusts %rsp first.
    call asnix_tryeval_raise_or_fatal
    .size asnix_op_assert_fail, . - asnix_op_assert_fail
