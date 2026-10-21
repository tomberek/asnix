#ifndef ASNIX_TRYEVAL_H
#define ASNIX_TRYEVAL_H

#include <setjmp.h>
#include <stddef.h>

/*
 * M30: a narrow, targeted catchable-throw mechanism for `builtins.tryEval`
 * -- NOT a general exception system. Real Nix's own `tryEval` only ever
 * catches `AssertionError` and its one subclass `ThrownError` (confirmed
 * directly in cppnix's source, `~/nix/src/libexpr/primops.cc`'s
 * `prim_tryEval`: `catch (AssertionError & e)`, and
 * `~/nix/src/libexpr/include/nix/expr/eval-error.hh`'s
 * `MakeError(ThrownError, AssertionError)`) -- i.e. exactly `throw` and a
 * failed `assert`, and nothing else: NOT type errors, NOT infinite
 * recursion, NOT missing attributes, and explicitly NOT `abort` (verified
 * directly against `nix-instantiate --eval`: `tryEval (abort "x")` still
 * aborts the whole process unconditionally). Every other fatal path in
 * this codebase (`asnix_fatal`, used for all of those) is completely
 * unaffected by this file -- only `prim_throw` (builtins.c) and
 * `OP_ASSERT_FAIL`'s handler (op_assert.S) route through here instead.
 *
 * Implemented with plain `setjmp`/`longjmp`, not a sweeping redesign:
 * this project's own dispatch loop already keeps its live evaluator
 * state (VALSTACK/POOL/RETSP: %r12/%r13/%r14) in registers deliberately
 * chosen to be SysV callee-saved specifically so they survive an ordinary
 * C call for free (see docs/abi.md) -- which also means a plain
 * `longjmp` correctly rewinds them back to their `setjmp`-time values,
 * discarding whatever partial evaluation happened since, with no extra
 * bookkeeping needed. `%rsi`/`%rdi` (IP/ENV) need no special handling
 * either: `prim_tryEval` is an ordinary primop called through
 * `op_call.S`'s existing save-IP/ENV-around-the-call convention, so once
 * `prim_tryEval` returns normally (whichever way it got there), the
 * caller's own push/pop restores them regardless of what happened inside.
 *
 * Known, accepted limitation, not engineered around: if the thrown
 * expression's evaluation forced some OTHER thunk partway through (not
 * the thunk `tryEval` was directly asked to evaluate), that thunk is left
 * marked IN_PROGRESS forever (the normal "finish forcing, mark FORCED"
 * step never runs for it) -- referencing that *same* thunk object again
 * later would incorrectly blackhole. A full fix would mean tracking and
 * rolling back every thunk touched since the matching `setjmp`, which is
 * a much larger undertaking; not attempted here, since a `tryEval`'d
 * expression that throws is, in every real pattern seen so far
 * (nixpkgs's own `impure-overlays.nix`), a disposable one-off whose
 * intermediate thunks are never touched again from elsewhere.
 */

#define ASNIX_TRYEVAL_MAX_DEPTH 256

/* Pushes `buf` as the target for the next asnix_tryeval_raise_or_fatal
 * call (supports nested `tryEval`s: a LIFO stack of buffer pointers).
 * `buf` must be `setjmp`'d by the caller itself, immediately after this
 * call returns -- `setjmp` can't be meaningfully wrapped in a helper
 * function, since the saved context would belong to the wrong stack
 * frame. Exits cleanly (not a tryEval-catchable condition itself) if
 * nested too deeply -- matching this project's existing fixed-size-
 * stack-array convention elsewhere (e.g. AST_VAR's with-chain limit). */
void asnix_tryeval_push(jmp_buf *buf);

/* Pops the stack pushed by asnix_tryeval_push -- call once tryEval's own
 * try/catch region is done, on EITHER path (the forced value came back
 * normally, or the catch branch ran after a longjmp here). */
void asnix_tryeval_pop(void);

/* `throw`/a failed `assert` reaches here. If a `tryEval` is currently
 * active (the stack above is non-empty), longjmps to its buffer --
 * never returns. Otherwise falls back to exactly asnix_fatal's own
 * behavior (write `msg` to stderr, exit 1) -- never returns either way. */
_Noreturn void asnix_tryeval_raise_or_fatal(const char *msg, size_t len);

#endif /* ASNIX_TRYEVAL_H */
