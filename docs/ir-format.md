# IR format

Canonical source: `include/asnix/ir.h`.

## Record shape

Fixed-width, 24-byte, 8-byte-aligned records:

```c
typedef struct {
    void *handler;  // offset 0
    int64_t a;        // offset 8
    int64_t b;         // offset 16
} IrOp;
```

Fixed width (vs. a byte-packed variable-length stream) means "advance to
the next instruction" is a pure pointer increment (`+24`), never a decode —
the thing a threaded-dispatch interpreter most wants to avoid is a
per-instruction length computation before it can even find the next
dispatch target.

## Linking

`handler` starts out holding a small op-id integer (cast through the
pointer-sized field by the builder, `src/c/irbuild.c`), not a real address
— the compiler never needs to know real handler addresses, which don't
exist until `as`+`ld` have placed them. A single pass, `ir_link()` (in
`src/c/dispatch_table.c`), walks a finished `ops` array once and rewrites
every `handler` field to the matching entry in the asm-exported
`asnix_handler_table[OP_COUNT]`. This pays the indirection cost once at
compile time, not per dispatch step.

## Dispatch (M3/M4)

`src/asm/dispatch.S`'s `asnix_eval` loads `prog->ops[0]`'s handler and jumps
to it; every op handler (`src/asm/ops/*.S`) ends with the same tail —
advance `%rsi` (the IP register) by 24, load the next handler address, and
`jmp` straight to it. No central dispatch loop is ever re-entered after the
first jump (direct/indirect threading).

## Op set (M3/M4 minimal subset)

`OP_PUSH_INT/TRUE/FALSE/NULL`, `OP_VAR_REF` (De Bruijn `depth,slot` in
`a,b`), `OP_ADD/SUB/MUL`, `OP_IF`/`OP_JMP` (byte deltas, computed by the
builder — not record counts, so handlers never multiply), `OP_LET` (`a` =
slot count, binds that many just-pushed values into a new frame),
`OP_LAMBDA` (`a` = arity, `b` = an index into a 2-entry-per-lambda constant
pool: `{body IrOp*, body's own pool}` — see the Closure note in
`docs/value-encoding.md`), `OP_CALL`/`OP_RET` (explicit call-frame stack,
kept off the native machine stack — see `docs/abi.md`), `OP_HALT`.

## M4.5: native codegen is a parallel path, not a consumer of this IR

`src/c/codegen.c` compiles the *same AST* directly to x86-64 machine code,
bypassing `IrOp` entirely — there is no "linked IR buffer" in that path,
just a byte buffer. It covers literals/arithmetic/`if`/`let` only (no
closures yet); see `codegen.h` for the exact scope and why lambdas/calls
were deferred.
