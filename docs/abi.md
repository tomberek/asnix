# Calling-convention contract

## M3/M4 threaded-dispatch evaluator (`asnix_eval`)

Fixed register roles, live for the whole evaluation (see
`src/asm/dispatch.S`):

| register | role |
|---|---|
| `%rsi` | **IP** — current `IrOp*` |
| `%rdi` | **ENV** — current `Env*` |
| `%r12` | **VALSTACK** — next free operand-stack `Value` slot |
| `%r13` | **POOL** — the *active* program/closure's constant pool base |
| `%r14` | **RETSP** — next free call-frame slot (kept off the native machine stack, so the jmp-chain dispatch loop never grows it) |
| `%rbx`/`%rbp`/`%r15` | free scratch for individual op handlers (`op_let.S`, `op_lambda.S`, `op_call.S` use these to hold values across their own calls to `asnix_arena_alloc`) |

### Why six registers are saved at entry, not three

`%rbx`/`%rbp`/`%r15` are ordinary SysV *callee-saved* registers. Even
though op handlers treat them as free scratch, `asnix_eval` as a whole must
still preserve the **caller's** values across the entire call — a handler
clobbering `%rbp` (the C frame pointer under `-O0`!) without it being
restored corrupts the caller the instant `asnix_eval` returns. This was a
real bug found during M3 development (fixture_let crashed, eventually
traced to exactly this) — the fix: `asnix_eval`'s prologue pushes all six
candidate-scratch registers (`r12,r13,r14,rbx,rbp,r15`) plus one alignment
padding slot; `op_halt.S`'s epilogue pops them in mirror order before its
`ret`. See the comments in `dispatch.S` and `op_halt.S` for the exact byte
accounting.

### Calls to `asnix_arena_alloc` (and friends)

`%rsi`/`%rdi` (IP/ENV) are ordinary SysV *caller-saved* registers, so any
handler that calls out must explicitly save them (into scratch registers or
the native stack) before staging call arguments, and **restore them into
the real `%rsi`/`%rdi` registers themselves** afterward — not just copy
the saved value into a heap object and move on. Every call site in
`op_let.S`/`op_lambda.S`/`op_mk_list.S`/`op_mk_attrs.S`/`op_mk_thunk.S`/
`op_call.S` does this by hand currently (push IP/ENV, stage args, call,
pop). This is exactly the pattern that produced known bug #3 in
`docs/architecture.md`: `OP_LAMBDA`/`OP_MK_THUNK` popped the saved ENV into
`%rdx`, stored *that* into the heap object, and left the real `%rdi`
holding leftover size-argument garbage — latent for an entire milestone
because nothing happened to read it before the next handler overwrote it
anyway. A shared `CALL_C_BUILTIN`-style macro (as originally sketched for
this project) would make this class of bug impossible by construction —
worth introducing once M7 adds real C builtins with many more call sites;
not done yet, since the macro-file-inclusion machinery isn't justified for
the current handful of sites, but the bug above is a concrete argument for
not postponing it indefinitely.

### Closures carry their own constant pool

`OP_CALL` installs the callee's `Closure->pool` into `%r13` and saves the
caller's previous `%r13` into the new call-frame (`ASNIX_RETFRAME_OFF_POOL`);
`OP_RET` restores it. Without this, a lambda body containing its own nested
`OP_LAMBDA` would resolve that inner lambda's body pointer against the
*caller's* pool instead of its own — a real bug M4's curried-lambda
compilation first exposed (M3's single-level fixture never had a lambda
body containing another `OP_LAMBDA`, so it never exercised this path).

### Forcing thunks reuses the call machinery (M6)

`OP_VAR_REF`/`OP_SELECT` force an unforced thunk by pushing a call-frame
exactly like `OP_CALL` does, entering the thunk's body/env/pool the same
way — except the `RETFRAME` now carries a fourth field,
`force_target` (0 for an ordinary call), telling `OP_RET` to memoize the
result into that `Thunk` cell before resuming instead of just resuming.
This grew `RETFRAME` from 24 to 32 bytes — `dispatch.S`'s `ASNIX_RETSTACK_BYTES`
comment is kept in sync (128 call frames now, not 256).

`src/asm/force.S`'s `asnix_force_for_print` is a separate, self-contained
entry point for forcing from C code with **no active `asnix_eval`
invocation at all** (used by the printer to force thunks nested inside
lists/attrsets). It brings its own VALSTACK/RETSP and saves all six
candidate-scratch registers, mirroring `asnix_eval`'s own prologue, then
uses a static sentinel `IrOp` (handler = a small epilogue routine) as the
synthetic call-frame's `return_ip` so `OP_RET`'s ordinary jmp-chain lands
back in native-callable code instead of another dispatch step. This is
deliberately narrow (one thunk, one call) — not a general re-entrant
force-from-anywhere mechanism; see `docs/architecture.md`'s M6 notes.

## M4.5 native codegen (`src/c/codegen.c`)

Generated functions are ordinary SysV-ABI `Value (*)(void)` — no special
convention at all. They use the native stack (plain `push`/`pop`) for
intermediate values and `let`-bound locals (addressed by a compile-time-
tracked `%rsp` offset), and end with `pop %rax; ret`. This is possible
specifically because the current scope has no closures/recursion to
protect the native stack from — see `codegen.h` for why lambdas/calls
are out of scope for now, and what would need to change (a heap closure
plus an explicit call/return protocol mirroring `op_call.S`) to add them.
