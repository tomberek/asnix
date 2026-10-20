# asnix architecture

A from-scratch Nix-language evaluator with a hand-written x86-64 assembly
core and a C boundary for parsing and (eventually) builtins/store protocol.
See `~/.claude/plans/examine-nix-gnix-iets-gnix-fix-silly-shell.md` for the
full milestone roadmap and the prior-art survey (cppnix, `~/gnix/iets`,
`~/gnix/fix`, `~/gnix/nixgoose`) this design is based on. This doc tracks
what's actually implemented and the scope decisions made along the way.

## Status

| Milestone | State |
|---|---|
| M0 — scaffolding | done |
| M1 — value/tagging primitives (`src/asm/value.S`) | done |
| M2 — bump arena allocator (`src/asm/arena.S`) | done |
| M3 — threaded-dispatch evaluator over hand-fed IR | done |
| M4 — real lexer/parser/compiler (`src/c/{lexer,parser,compiler}.c`) | done |
| M4.5 — copy-and-patch native codegen (`src/c/codegen.c`) | done, **scoped** (see below) |
| M5+ | not started |

## Design (see `docs/value-encoding.md`, `docs/ir-format.md`, `docs/abi.md`)

- **Value**: 8-byte pointer-tagged word, never-free bump arena, no GC.
- **Evaluator**: threaded dispatch over a flat `IrOp` stream (M3/M4), *and*
  a parallel native-codegen path (M4.5) for the subset it covers.
- **Env**: flat per-scope frames, De Bruijn `(depth,slot)` resolved at
  compile time.

## M4 scope notes

The parser implements real Nix syntax for the subset it covers: single-
param curried lambdas (`a: b: body`, exactly like real Nix — *not* the
multi-param arrow syntax M3's own hand-fed test fixtures used as a
shortcut), function application by juxtaposition, `let`/`in`, `if`/`then`/
`else`, arithmetic. A multi-binding `let` desugars at parse time into a
chain of nested single-binding `AST_LET` nodes (`src/c/parser.c`), which
keeps name resolution trivial (every frame has exactly one slot) at the
cost of not supporting mutual recursion between sibling bindings in one
`let` — not exercised by the current fixture set. Identifiers exclude `-`
(real Nix allows hyphenated identifiers; that collides with binary minus in
this minimal lexer and isn't needed yet).

## M4.5 scope notes

The plan's original M4.5 design called for extracting relocatable op
templates from GAS-assembled `.o` files (reading their own `.rela.text`
relocation entries) and copy-and-patching them into a generated buffer —
the same technique real copy-and-patch compilers use, letting the
assembler/linker's own relocation machinery do the hard part.

What's actually implemented in `src/c/codegen.c` is a smaller, scoped
version: each instruction's exact byte encoding is hand-derived and
cross-checked against `as`+`objdump` once (not re-extracted at build time),
and the generator covers **literals, arithmetic (`+ - *`), `if`/`then`/
`else`, and `let`/`in`** — using the plain native stack (no heap `Env`
needed, since this subset has no closures to capture). `AST_LAMBDA`/
`AST_APP` are explicitly rejected with a clear error.

This covers 5 of the 6 original M3/M4 parity fixtures (all but
`lambda_call`); the three-way agreement test (`tests/ir/test_m45_codegen.c`)
verifies the native path produces byte-identical results to both the
expected literal and the M3/M4 interpreted path for all five.

**Why lambdas/calls were deferred rather than forced through**: supporting
them in generated native code needs a heap `Closure` plus an explicit
call/return protocol (mirroring `src/asm/ops/op_call.S`'s RETSP mechanism,
including the per-closure constant-pool handling described in
`docs/abi.md`) — a real second project on top of the arithmetic/control-
flow subset, not a small extension. Flagged as a documented follow-up
rather than attempted under time pressure.

**W^X**: `asnix_codegen` always `mmap`s `PROT_READ|PROT_WRITE`, finishes
all patching, then `mprotect`s to `PROT_READ|PROT_EXEC` — never both
simultaneously. **Not implemented**: GDB JIT registration
(`__jit_debug_register_code`) / `perf` `jitdump` — debuggability of the
generated buffers is an accepted gap for now (see the plan's Risk list);
nothing about correctness depends on it.

## Measured: dispatch-elimination thesis (`bench/microbench_m45.c`)

Ad hoc, not a pass/fail gate (see M9 in the roadmap for the real harness).
On this dev machine, comparing repeated calls to an already-compiled
`asnix_eval` program vs. an already-generated native function:

```
arith (2+3*4)        interp≈1480ns/call  native≈2.6ns/call   (~580x)
let x=10;y=20         interp≈1490ns/call  native≈3.0ns/call   (~500x)
```

**Caveat**: this is not a clean "dispatch overhead only" measurement —
`asnix_eval` allocates a fresh VALSTACK+RETSP via the arena on *every*
call (never reused across calls), which dominates the interpreted-path
number; the native path allocates nothing. The qualitative conclusion
(straight-line native code beats threaded dispatch decisively for this
workload shape) is well-supported by the architecture rationale in the
plan regardless; the quantitative ratio above overstates dispatch-only
savings and shouldn't be quoted as such.

## Known bugs found and fixed during implementation (worth knowing about)

1. **Register corruption across `asnix_eval`** (M3): `op_let.S`/
   `op_lambda.S`/`op_call.S` use `%rbx`/`%rbp`/`%r15` as scratch, but the
   original `dispatch.S` prologue only preserved `%r12`/`%r13`/`%r14` —
   corrupting the C caller's frame pointer (`%rbp` at `-O0`) the moment a
   `let` was evaluated. Fixed by widening the save/restore set to all six
   registers (see `docs/abi.md`).
2. **Per-closure constant pool** (M4): nested lambdas (`a: b: body`) each
   compile with their own constant pool, but `OP_LAMBDA`/`OP_CALL` only
   ever installed the *top-level* pool into `%r13` — a lambda body
   containing its own `OP_LAMBDA` resolved the inner body pointer against
   the wrong pool. M3's single-level fixture never exercised this (no
   lambda body contained another lambda); M4's real curried-lambda syntax
   did. Fixed by giving `Closure` and the `RETFRAME` a `pool` field each.
