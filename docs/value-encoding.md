# Value encoding

Canonical source: `include/asnix/value.h` (shared verbatim by C and asm via
cpp). This doc explains the *why*; the header is the source of truth for
exact constants.

## Layout

An `asnix` `Value` is a single 8-byte word:

```
Bit:    63                                  3 2 1 0
       [ ........... payload ........... ][ tag  ]
```

All heap allocations are 8-byte aligned (enforced by the bump arena, see
`src/asm/arena.S`), so the low 3 bits of any pointer are free to use as a
tag, and stripping the tag from a `PTR`-family value is a no-op (the tagged
word *is* the pointer).

| tag | hex | meaning | payload |
|---|---|---|---|
| `PTR` | `0x0` | generic heap pointer (strings etc., via a header byte) | pointer, verbatim |
| `INT` | `0x1` | 61-bit immediate signed integer | `(value << 3) \| 0x1`, decoded via arithmetic right-shift |
| `IMM` | `0x2` | small fixed enumeration | sub-tag in bits [63:3]: `NULL=0, FALSE=1, TRUE=2, BLACKHOLE=3` |
| `ATTRS` | `0x3` | attrset heap object | pointer |
| `LIST` | `0x4` | list heap object | pointer |
| `CLOSURE` | `0x5` | closure heap object | pointer |
| `THUNK` | `0x6` | 2-word thunk cell | pointer |
| `PRIMOP` | `0x7` | native builtin-function heap object (M7) | pointer |

Four tag values are spent on heap *kinds* directly (`ATTRS`/`LIST`/
`CLOSURE`/`THUNK`) rather than following a single generic `PTR` tag plus an
in-object kind byte (as `~/gnix/iets` does) — the thunk-force check and
call/select dispatch are the hottest checks in the evaluator, and a
register-only `and $7` + `cmp` beats a memory load every time. This costs
tag space (`PTR` is the only generic fallback) but every reserved kind is
one profiling already flagged as hot in the prior-art survey this project's
design was based on.

## Heap object layouts

- **Env** (`include/asnix/env.h`): `{Env *up; Value values[nslots];}`.
- **Closure** (`include/asnix/ir.h`): `{Env *up; IrOp *body; int64_t arity;
  int64_t *pool;}` — 32 bytes. The `pool` field exists because every
  compiled program/lambda body carries its *own* constant pool (for its own
  nested lambda literals); a closure must carry a reference to its body's
  pool so `OP_CALL`/`OP_RET` can install/restore the right one in `%r13`
  (see `docs/abi.md` and the "pool-per-closure" note in `ir.h`).
- **IrOp** record: `{void *handler; int64_t a; int64_t b;}` — see
  `docs/ir-format.md`.
- **String/List/Attrs** (`include/asnix/heap.h`, M5): `String{len;
  bytes[]}` (the only `PTR`-tag kind so far), `List{count; elems[]}`,
  `Attrs{count; entries[]}` with entries sorted by symbol id.
- **Thunk** (`include/asnix/ir.h`, M6): `{state; env; body; pool}` — 32
  bytes, the mechanism behind laziness. `state` is `UNFORCED`/
  `IN_PROGRESS`/`FORCED`; once forced, the `env` field is *repurposed* to
  hold the cached `Value.raw` directly rather than a pointer — forcing
  overwrites the cell in place so every tagged pointer to it sees the
  memoized value after the first force. See `docs/architecture.md`'s M6
  scope notes for what this does and doesn't cover (not recursive bindings
  yet).
- **PrimOp** (`include/asnix/heap.h`, M7): `{fn; arity}` — 16 bytes, a
  native builtin function (`fn` signature: `Value (*)(const Value *args,
  int64_t argc)`). `OP_CALL` dispatches to it the same way it dispatches
  to a `Closure`, just skipping the Env-frame-allocation step since a
  primop has no lexical body to capture a scope for.
