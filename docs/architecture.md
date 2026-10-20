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
| M5 — strings/lists/attrsets/symbol interning | done (interpreter path only; see below) |
| M6 — lazy `let`/attrset bindings via thunks, blackholing | done, **scoped** (see below) |
| M7 — builtins + primop dispatch (`src/c/builtins.c`) | done, **scoped** (see below) |
| M8 — derivations + store-path hashing + daemon protocol | done, **scoped** (see below): connect/handshake/`IsValidPath` only, no realization |
| M9 — benchmark harness (`bench/`) | done (see below) |
| M10 — recursive `let`/`rec {}`/`inherit` | done (see below) |
| M11 — operators: comparisons, `&&`/`||`/`!`, `assert`, `//` | done, **scoped** (see below) |
| M12 — strings: `"${expr}"` interpolation, `''...''` multi-line strings | done (see below) |
| M13 — `with` expressions | done (see below) |
| M14 — lambda-destructuring patterns | done (see below) |
| M15 — `import` + path literals (`src/c/eval_file.c`) | done, **scoped** (see below) |
| M16 — `asnix_apply`, general re-entrant call-a-Value-from-C (`src/asm/apply.S`) | done (see below) |
| M17 — builtins expansion (list/attrs/string/type-predicate) | done (see below) |
| M18+ | not started |

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
`else`, arithmetic. A multi-binding `let` parses into one flat `AST_LET`
binding list (`src/c/parser.c`) — originally (M4) this desugared into a
chain of nested single-binding `AST_LET` nodes instead, which kept name
resolution trivial but meant sibling bindings in one `let` couldn't see
each other; see the "Recursive `let`/`rec {}`/`inherit`" section below for
why and how that changed. Identifiers exclude `-` (real Nix allows
hyphenated identifiers; that collides with binary minus in this minimal
lexer and isn't needed yet).

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

## M5 scope notes

Adds strings, lists, attrsets, and symbol interning to the M3/M4
threaded-dispatch interpreter path only (M4.5's native codegen explicitly
rejects these node kinds with a clear error — see `codegen.c` — extending
it is future work, not attempted here).

- **Symbol table** (`src/c/symtab.c`): dense `u32` ids, open-addressed hash
  table, FxHash-shape multiply hash finished with a splitmix64-style
  avalanche (not SipHash — see the header comment and `docs/value-encoding.md`
  for why). Grows via rehashing at 75% load factor; ids are stable across
  rehashing (verified in `tests/c/test_symtab.c`).
- **Heap layouts** (`include/asnix/heap.h`): `String{len; bytes[]}` under
  the generic `ASNIX_TAG_PTR` (the only `PTR`-kind so far, so no in-object
  Kind byte is needed yet), `List{count; elems[]}` under `ASNIX_TAG_LIST`,
  `Attrs{count; entries[]}` under `ASNIX_TAG_ATTRS` with entries sorted by
  symbol id for `OP_SELECT`'s binary search.
- **New ops** (`src/asm/ops/op_push.S`'s `asnix_op_push_const`,
  `op_mk_list.S`, `op_mk_attrs.S`, `op_select.S`): `OP_PUSH_CONST` pushes a
  fully pre-formed `Value.raw` stashed in the constant pool (how string
  literals work — built once at compile time, pushed by reference at
  runtime); `OP_MK_LIST`/`OP_MK_ATTRS` pop N stack values into a freshly
  arena-allocated object (same reverse-pop-into-forward-index pattern as
  `OP_LET`'s frame-binding loop); `OP_SELECT` binary-searches sorted
  entries, aborting cleanly (via the shared `asnix_fatal`) on a type
  mismatch or missing key. All validated with hand-fed IR fixtures
  (`tests/ir/test_m5_fixtures.c`) *before* the parser grammar was touched —
  same bug-isolation strategy that caught the M3/M4 register and
  constant-pool bugs.
- **Compiler** (`src/c/compiler.c`): an attrset literal's bindings are
  sorted by interned symbol id at compile time (insertion sort — binding
  counts are always tiny) and their *values* are pushed in that same
  sorted order, so `OP_MK_ATTRS`'s runtime fill loop never needs to sort
  anything itself. No duplicate-key detection yet (not exercised by the
  fixture set).
- **Parser** (`src/c/parser.c`): list elements and function-application
  arguments are parsed at "atom plus postfix `.attr` chain" precedence
  (`parse_select`), not full `expr` — matching real Nix, where `[ f x ]` is
  a 2-element list and a function call inside a list needs parens:
  `[ (f x) ]`. String literals were plain quoted runs with no escapes or
  interpolation at M5 time — superseded by the Strings milestone below
  (escapes, `"${expr}"` interpolation, and `''...''` multi-line strings).

## M6 scope notes

Adds real laziness: `let` and attrset binding *values* are deferred
(wrapped in a `Thunk`, not evaluated) and memoized on first force.

- **Thunk** (`include/asnix/ir.h`): `{state; env; body; pool}`, 32 bytes.
  `state` is `UNFORCED`/`IN_PROGRESS`/`FORCED`; once `FORCED`, the `env`
  field is repurposed to hold the cached `Value.raw` directly (forcing
  overwrites the cell in place — every tagged pointer to the same cell
  sees the memoized value after the first force, computed exactly once;
  no CAS/claim protocol needed since asnix is single-threaded).
- **`OP_MK_THUNK`** (`src/asm/ops/op_mk_thunk.S`): builds a thunk over the
  *current* env/pool, mirroring `OP_LAMBDA`'s shape closely (same
  `{body, pool}` pool-pair convention).
- **Forcing** happens inline in `OP_VAR_REF` and `OP_SELECT` (not a
  separate IR op) — the two places a stored value is read out and handed
  to a consumer. An unforced thunk is entered exactly like a zero-arg
  call: a retframe is pushed with a new `force_target` field (the
  `RETFRAME` grew from 24 to 32 bytes), and `OP_RET` checks it — non-zero
  means "memoize the result into this thunk cell before resuming" instead
  of an ordinary call return. `IN_PROGRESS` re-entry (e.g. a
  self-referential thunk) aborts cleanly via `asnix_fatal` (blackholing).
- **List elements and function-call arguments are still eager** — not
  thunked. Making them lazy too (matching real Nix) is future work.
- **Scope limitation (fixed, see below): at M6, `let`/attrset bindings were
  not recursive.** Real Nix `let` is implicitly `letrec` (bindings can
  reference themselves or siblings — e.g. `let fact = n: if n==0 then 1
  else n*fact(n-1); in ...` relies on `fact` seeing itself). M6's thunks
  captured the *outer* scope (whatever env was active when `OP_MK_THUNK`
  ran, which was *before* `OP_LET` created the new frame) — so a binding
  couldn't see itself or siblings in the same `let`/attrset. Fixed by the
  `OP_FRAME_ALLOC`/`OP_FRAME_SET`/`OP_FRAME_EXIT` rework described in the
  "Recursive `let`/`rec {}`/`inherit`" section below, exactly along the
  lines sketched here (allocate the frame before constructing the
  thunks, so they capture it as their own scope).
- **`asnix_force_for_print`** (`src/asm/force.S`): a narrow, separate
  C-callable force entry point used only by the top-level printer
  (`main.c`) to recursively force thunks nested inside lists/attrsets for
  display — it brings its own self-contained VALSTACK/RETSP/register
  context (mirroring `asnix_eval`'s prologue) since it may run with no
  `asnix_eval` invocation active at all. This is *not* a general
  re-entrant force-from-C-builtins mechanism (that's an M7+ concern, once
  real builtins exist and need one).
- **Validated with hand-fed IR + hand-built `Thunk` cells**
  (`tests/ir/test_m6_fixtures.c`) before the compiler was touched: proves
  "constructing a thunk never runs its body" directly (tag stays `THUNK`,
  state stays `UNFORCED`, if nothing forces it), memoization by directly
  inspecting a hand-built cell's `state`/cached value before and after one
  force, and blackholing via a hand-built self-referential cell (the
  compiler can't produce one, since it only emits non-recursive bindings —
  this tests the asm mechanism itself, independent of that limitation).
  End-to-end (`tests/e2e/m6_*.nix`) demonstrates real observable laziness:
  `let x = {}.missing; in 5` prints `5` without ever erroring, because `x`
  is never forced.

## M7 scope notes

Adds real builtin functions, callable and first-class like any other Nix
value (`let f = builtins.toString; in f 7` works — a primop can be bound,
passed around, etc., same as a closure).

- **`PrimOp`** (`include/asnix/heap.h`): `{fn; arity}`, 16 bytes, tagged
  `ASNIX_TAG_PRIMOP` — the reserved 8th tag value from `docs/value-encoding.md`
  finally has a use. `fn` has signature `Value (*)(const Value *args,
  int64_t argc)`.
- **`OP_CALL` dispatches on tag**: closure and primop calls share the same
  callee-location/arity-check logic, then branch. A primop call skips
  the Env-frame allocation entirely — `fn` is invoked directly with a
  pointer straight into VALSTACK (the same contiguous arg layout the
  closure path already reads from when copying into a new frame), since a
  primop has no lexical body to capture a scope for.
- **Builtins are exposed via a `builtins` attrset**, not ambient bare
  names — matching real Nix's canonical form (real Nix *also* exposes a
  few names bare at the top level via an implicit `with builtins;`-like
  mechanism; this implementation doesn't yet). The compiler's top-level
  scope chain starts with one frame named `"builtins"`
  (`asnix_compile`'s `ScopeFrame`), and `main.c` constructs a matching
  one-slot root `Env` holding `asnix_make_builtins_attrs()`'s result —
  these two must stay in lockstep (if the root env ever grows more slots,
  the compiler's root scope frame(s) must match exactly, in the same
  order).
- **Ships 3 builtins** (`src/c/builtins.c`): `toString`, `typeOf`,
  `length` — implemented natively in C (not asm): none of these are
  hot-path code, and C is simply easier to get right for `snprintf`-style
  formatting and tag-switch dispatch than hand-rolling it. No crypto/hash
  builtins yet (`hashString`, derivation-hashing) — those need the
  dlopen-shim pattern `~/gnix/nixgoose` already proved out, deferred to
  the M8 derivation milestone where they're actually needed.
- **Call arguments to primops are not forced** before the native call —
  consistent with M6's existing scope decision that call arguments are
  eager in general (so by construction they're never raw unforced thunks
  in the current language subset); a primop that someday receives a
  thunk would need to force it explicitly, not assume `args[i]` is already
  reduced.
- **Validated with a hand-built `PrimOp` + hand-fed IR**
  (`tests/ir/test_m7_fixtures.c`) before `builtins.c` existed, proving
  `OP_CALL`'s primop branch (arg layout, arity checking, result handling)
  independent of any real builtin's logic.

## M8 scope notes

Store-path hashing and derivation construction, bit-exact-compatible with
real Nix — a much bigger jump than M0-M7 (external, precisely-specified
binary format and hash algorithm, not just this project's own internal
conventions), so each piece is being verified against real
`nix-instantiate` output *before* being trusted, same discipline as the
nix-conformance check above. Built in parallel by two independent
subagents (`hashString`/`hash_shim`, and research into the exact ATerm
`.drv` format) plus direct work on `base32`/`store_path`/`aterm`.

### `builtins.hashString` (done)

Adds `builtins.hashString algo s` (md5/sha1/sha256/sha512, hex output only
— real Nix's `hashString` itself never produces base32/base64/SRI, so
that's not a scope cut, just matching upstream). This slice was pulled
forward because it's the one piece of M8 the dlopen-shim pattern flagged
as deferred in M7's own scope notes was actually needed for.

- **`src/c/hash_shim.{h,c}`**: `dlopen`/`dlsym` loads libcrypto at first
  use rather than linking it at build time, exactly like
  `~/gnix/nixgoose`'s `openssl_shim.h`/`blake3_shim.h` — keeps this heavy
  C dependency out of the default `make`/`make test` graph (per the "Fast
  iteration loop" section below), only paid for by programs that actually
  call `hashString`. Candidate paths are tried in two tiers: `getenv
  ("ASNIX_LIBCRYPTO_LIB")` first (set by `flake.nix`'s devShell `shellHook`
  to the exact nixpkgs-resolved `openssl.out/lib/libcrypto.so` path, since
  nixpkgs' openssl has no single predictable store path — same convention
  as nixgoose's own `NIXGOOSE_LIBBLAKE3_LIB`/`NIXGOOSE_TOMLC99_LIB`), then
  a handful of bare sonames/absolute paths (`libcrypto.so.3`, `libcrypto.so`,
  plus a few common system locations) in case it's reachable some other
  way. **Which libcrypto API**: confirmed with `nm -D` against both this
  machine's system libcrypto (OpenSSL 3.6) and the nixpkgs-resolved one
  (OpenSSL 3.5.9) that the classic one-shot wrappers `MD5()`/`SHA1()`/
  `SHA256()`/`SHA512()` are still exported in both (deprecated only at the
  *header* level in OpenSSL 3.x, which dlsym-based loading never sees) —
  the same API nixgoose's `openssl_shim.h` already uses and what real
  Nix's own `libutil/hash.cc` calls. No `EVP_MD_CTX` plumbing needed since
  every call site already has the whole input as one contiguous buffer.
- **Curried via two arity-1 stages, not one arity-2 `BuiltinDef`**: real
  Nix's `hashString` is arity 2, but registering it that way here doesn't
  work. `parser.c`'s `parse_app` always desugars multi-argument
  application into nested single-argument `AST_APP` nodes (`f x y` =>
  `App(App(f,x),y)`, matching real Nix's own curried grammar), and
  `compiler.c`'s `AST_APP` case always emits `ib_call(b, 1)` — so
  `builtins.hashString "sha256" "hello"` reaches `OP_CALL` as *two*
  separate argc=1 calls, never one argc=2 call. `OP_CALL`'s primop branch
  requires an exact arity match per call and hands the native function
  nothing but `(args, argc)` (see `docs/abi.md`'s "Primop calls (M7)"
  section) — no self/closure pointer, so there's no channel to carry data
  (like which algorithm was picked) from a first call into a second one.
  The fix mirrors how curried *user* lambdas already get multi-arg calls
  to work: `prim_hashString` (`src/c/builtins.c`) is itself arity 1,
  takes just the algorithm name, and — since only 4 algorithms are
  supported, known statically — returns one of 4 dedicated arity-1
  `PrimOp`s (`prim_hashString_apply_{md5,sha1,sha256,sha512}`), each with
  its algorithm baked into *which function pointer it is* rather than any
  captured runtime data. No mutable global, no extra-data-stashed-after-
  the-`PrimOp`-struct trick, no `src/asm/` changes.
- **Validated** directly against hex digests generated on this machine
  with `md5sum`/`sha1sum`/`sha256sum`/`openssl dgst -sha512` (not
  transcribed from memory) in `tests/c/test_hash_shim.c`, including an
  embedded-NUL input to confirm the shim passes the `String`'s exact
  stored byte length through rather than relying on `strlen` (asnix
  strings are never null-terminated — see `heap.h`). End-to-end fixtures
  (`tests/e2e/m8_hashstring_*.nix`) cover all four algorithms through the
  real lexer/parser/compiler/evaluator path.

### Store-path hashing + ATerm serialization (done; not yet wired to a builtin)

- **`src/c/base32.{h,c}`**: Nix's own base32 variant (alphabet
  `0123456789abcdfghijklmnpqrsvwxyz`, non-standard bit-packing), ported
  directly from `~/nix/src/libutil/base-nix-32.cc`'s `BaseNix32::encode`.
- **`src/c/store_path.{h,c}`**: `makeStorePath`'s algorithm, ported from
  `~/nix/src/libstore/store-dir-config.cc`: assemble
  `"type:hashWithAlgoPrefix:storeDir:name"`, SHA-256 it, XOR-fold
  (`compressHash`) the 32-byte digest down to 20 bytes, base32-encode,
  format as `/nix/store/<32-char-hash>-<name>`. Deliberately has **no
  crypto dependency of its own** — computing the SHA-256 digests is the
  *caller's* job (now that `src/c/hash_shim.c` exists, that's
  `asnix_hash_sha256`), so this module stays pure byte/string
  manipulation, tested against hardcoded known-digest bytes.
- **`src/c/aterm.{h,c}`**: `.drv` ATerm text serialization, scoped to a
  single output named `"out"`, zero input derivations, zero input sources
  (derivation-to-derivation dependencies need Nix's string-context
  tracking, which this project doesn't have yet). Ported from
  `~/nix/src/libstore/derivation/aterm.cc`'s escaping rule (only `\n \r \t
  \" \\` are ever escaped — confirmed directly in that source, not
  guessed) and `~/nix/src/libstore/derivation/masked.cc`'s
  `hashDerivationModulo` shape (serialize once with the output path
  blanked to compute the output's content hash, then once more with the
  real path filled in for the derivation's own hash).
- **Verified against real Nix before writing any code, twice over**: (1)
  hand-derived `makeStorePath` from the C++ source and caught a real
  discrepancy immediately by cross-checking — its hash argument is
  `hash.to_string(HashFormat::Base16, /*includeAlgo=*/true)`, i.e.
  prefixed `"sha256:"`, not the bare hex digest a first reading suggests;
  confirmed by reproducing `builtins.toFile "foo.txt" "hello"`'s real
  store path (via local `nix-instantiate`) in a throwaway Python script
  before porting anything to C — `tests/c/test_store_path.c` keeps 4 such
  verified (name, content) → real-store-path cases as its oracle. (2) The
  full derivation-hashing pipeline (mask → hash → `makeOutputPath` →
  unmask → hash → `makeStorePath` for the `.drv` itself) was independently
  reproduced byte-for-byte against numtide/nix-conformance's real
  `ia-advanced-attributes-defaults.drv` file *and* a second, freshly-run
  `nix-instantiate -E 'derivation { name = "asnix-test"; ... }'` — both in
  Python first, matching exactly, before writing `aterm.c`;
  `tests/c/test_aterm.c` keeps both as its oracle.
### `builtins.derivation {...}` (done)

Wires `base32`/`store_path`/`aterm`/`hash_shim` together (`src/c/derivation.c`)
into a minimal, real `derivation {...}`: single fixed output named `"out"`,
zero input derivations, zero input sources. Required attrs: `name`,
`system`, `builder`; optional `args` (list of strings); every other attr
becomes an environment variable (coerced to a string via the same rules
as `toString`, factored out into `src/c/value_util.c` once a second
caller needed it). Attrset binding values are lazy (M6), so each attr is
explicitly forced (via `asnix_force_for_print`, which turns out to be
safe to call even from *inside* an active primop call — it's just a
properly save/restore'd function, so it nests like any other call despite
its `_for_print`-suggestive name) before being read.

Returns `{ outPath; drvPath; type = "derivation"; out = outPath; }` —
deliberately does **not** merge the original input attrs back into the
result the way real Nix's own Nix-level `derivation` wrapper does (that
needs `//`, which this project doesn't have), and carries no string
context, so a derivation's `outPath` can't yet be used as another
derivation's input. Multiple outputs, fixed-output (content-addressed)
derivations, and derivation-to-derivation dependencies are all explicitly
out of scope for this slice — a prioritized list for what to tackle in
that order lives in the commit history's research notes.

**A third real bug, found by the same "verify against real Nix before
trusting it" discipline**: the first implementation fed `sha256(masked
ATerm)` directly into `asnix_make_store_path_from_digest`, skipping
`makeStorePath`'s *middle* step entirely — hex-encoding that digest with
its `"sha256:"` prefix, assembling the `"type:hash:storeDir:name"`
preimage via `asnix_store_path_preimage`, and hashing *that* to get the
digest `asnix_make_store_path_from_digest` actually expects. `store_path.c`
was deliberately split into two crypto-free pieces precisely so the two
hashing steps couldn't be silently merged into one inside that module —
but nothing stopped the *caller* from doing the equivalent wrong thing by
simply forgetting to call one of the pieces. Caught immediately because
the computed `outPath` didn't match the already-verified real
`nix-instantiate` oracle; fixed by adding `compute_store_path()` as the
single place this two-step composition happens, so there's only one spot
for this specific mistake to ever recur.

- **Verified against the same 2 real references already proven for
  `aterm.c`** (`ia-advanced-attributes-defaults.drv`'s real output path,
  and a fresh `nix-instantiate -E 'derivation { name = "asnix-test"; ...
  }'` run for both `outPath` and `drvPath`), plus extra-attr-as-env-var
  and missing-required-attr error paths, all exercised end-to-end through
  the real lexer/parser/compiler/evaluator in `tests/e2e/m8_derivation_*.nix`.

### Daemon protocol (minimal slice: connect + handshake + `IsValidPath`)

`src/c/daemon_client.{h,c}`: a from-scratch client for the real Nix worker
(daemon) wire protocol, scoped to the smallest slice that proves the
mechanism against a live daemon — connect over the Unix socket, complete
the version handshake, and perform `IsValidPath` (the simplest read-only
op). No build/realization ops, no NAR transfer; the stderr sideband's
`start_activity`/`stop_activity`/`result` messages are parsed just enough
to stay framed correctly and otherwise discarded (there's no build-output
UI to forward them to yet).

- **Cross-checked against two independent real implementations before
  writing any code** (same discipline as the rest of M8): cppnix's own
  `~/nix/src/libstore/include/nix/store/worker-protocol.hh` (magic
  numbers, op codes) and daemon-side `~/nix/src/libstore/daemon.cc`
  (confirms `IsValidPath`'s exact read-a-string/write-a-bool shape), plus
  `~/gnix/fix`'s from-scratch Zig reimplementation
  (`src/store/daemon/{wire,client}.zig`) for the handshake sequence and
  the stderr-sideband error-serialization format. Both agreed exactly.
- **Wire format**: every value is a little-endian `u64`; a string is a
  `u64` byte length followed by the bytes, zero-padded to the next 8-byte
  boundary. Handshake: client sends `WORKER_MAGIC_1`
  (`0x6e697863`), reads `WORKER_MAGIC_2` (`0x6478696f`) and the daemon's
  protocol version, checks the major version matches and the minor is
  `>= 26`, sends asnix's own version (`1.35`), conditionally sends two
  obsolete fields modern daemons still expect (CPU affinity if
  minor`>=`14, reserve-space if minor`>=`11, both always 0), conditionally
  reads a Nix-version string (minor`>=`33) and a trusted-client flag
  (minor`>=`35), then drains the initial stderr sideband.
- **Validated against the real, live `nix-daemon` already running on
  this dev machine** (`tests/c/test_daemon_client.c`): connects for
  real, confirms a made-up (but *well-formed*) store path is reported
  invalid — twice in a row, and interleaved with a third repeat of the
  first path, to specifically exercise that the connection correctly
  resynchronizes between calls, the part of this protocol most likely to
  silently desync if the sideband-draining logic ever mis-parses a
  field — and confirms a real, dynamically-discovered valid path
  (`nix-instantiate`'s own resolved location, not hardcoded) is reported
  valid. Skips gracefully (not a failure) if no daemon is reachable, so
  it doesn't break `make test` in an environment without one.
- **A near-miss worth recording**: the *first* version of this test used
  a malformed placeholder path (a 31-character hash instead of the
  required 32), which the daemon correctly rejected with a structured
  parse error — and then closed the connection, which looked at first
  like a bug in `daemon_client.c`'s framing until re-reading the error
  message (`"...hash part is not followed by a '-'"`) made clear the
  daemon was responding exactly as it should to genuinely malformed
  input; the fix was in the test's fixture data, not the client. Recorded
  here (not in the numbered "Known bugs" list below) precisely because
  it *wasn't* a real bug — a reminder that "the test failed" and "the
  code is wrong" aren't the same claim, and checking which one is true
  is itself a verification step.
- **Daemon realization (actually building derivations) is not
  implemented** — this slice only proves connectivity and the simplest
  op; wiring `derivation {...}`'s computed paths to an actual
  `BuildPaths`/`AddToStore` call (closing the loop from "compute a store
  path" to "the path actually exists on disk") is future work.

## Strings scope notes: interpolation + multi-line strings

Adds `"foo ${expr} bar"` string interpolation and `''...''` multi-line
("indented") strings to the lexer/parser/compiler/interpreter path (same
M4.5-codegen-excluded scope as every other M5+ feature — `codegen.c`
rejects `AST_STRING` unconditionally regardless of whether it has
interpolated parts).

- **Lexer** (`src/c/lexer.c`): `asnix_lex` is no longer a single flat
  per-character scan. It now carries an explicit mode stack (`MODE_NORMAL`/
  `MODE_STRING`/`MODE_ISTRING`), a from-scratch equivalent of real Nix's
  flex `%option stack`/`PUSH_STATE`/`POP_STATE` mechanism (`~/nix`'s
  `src/libexpr/lexer.l`): entering a string pushes `STRING`/`ISTRING`;
  every `"${"` *and* every plain `"{"` pushes `NORMAL` (the plain-`{`
  case matters because `${ { a = 1; }.a }` needs the inner attrset's own
  `"}"` to pop back to `NORMAL`, not prematurely resume string lexing);
  every `"}"` pops one level. Only when a pop reveals `STRING`/`ISTRING`
  underneath does that `"}"` actually mean "resume string-content
  lexing" — verified end-to-end with `tests/e2e/str_interp_nested.nix`
  and a hand-checked `"${ { a = 1; }.a }"` case (nested-brace handling)
  during development. A string literal is no longer one flat `TOK_STRING`;
  it's `TOK_STR_START`/`TOK_ISTR_START`, then any mix of `TOK_STR_PART`/
  `TOK_ISTR_PART` (literal chunks, escapes already decoded) and
  `TOK_DOLLAR_LBRACE ... TOK_RBRACE` (an interpolated expression, parsed
  at full `expr` precedence via an ordinary recursive call into the
  parser — `TOK_RBRACE` is reused as-is, not a separate token kind, since
  the lexer's mode stack already disambiguates "closes an interpolation"
  from "closes a nested attrset"), then `TOK_STR_END`/`TOK_ISTR_END`.
- **Escapes** (`"..."`, verified against real `nix-instantiate --eval`):
  `\n \r \t` map to the control character, any other `\X` (including
  `\" \\ \$`) passes through as the literal `X` — and a literal,
  un-escaped newline inside `"..."` is allowed verbatim (confirmed with
  real Nix; a prior M5 version of this lexer rejected it, see Known Bugs
  #10 below).
- **Indented-string escapes (`''...''`)**, also verified against real
  `nix-instantiate --eval`: `''$` → literal `"$"` (there is no separate
  `"''${"` rule — writing `''${` in source is just this `''$` escape
  immediately followed by an ordinary, non-special `"{"` character,
  which is how it reads as a literal `"${"` without starting an
  interpolation); `'''` → literal `"''"`; `''\<c>` → one escaped
  character with the same decode rules as `"..."`; a bare `''` not
  followed by `'`, `$`, or `\` always closes the string (so `''''` — four
  quotes — decodes as the 3-quote escape `"''"` plus one leftover bare
  quote `"'"`, i.e. `"'''"`, *not* two quotes; verified directly, since
  this was genuinely surprising on first read of the grammar). The
  opening `''` also swallows an immediately-following, optionally
  space-padded newline (`\'\'(\ *\n)?` in real Nix's grammar) — consumed
  entirely or not at all, never partially.
- **Indentation stripping** (`src/c/parser.c`'s `strip_indentation`):
  ported line-for-line from real Nix's `ParserState::stripIndentation`
  (`~/nix`'s `src/libexpr/include/nix/expr/parser-state.hh`) rather than
  reimplemented from a description, specifically because its two
  subtleties are easy to get wrong by guessing: (1) a line that's ended by
  an escape sequence or an interpolation (not just a raw newline) still
  ends the "start of line" run for indentation-counting purposes, exactly
  like a real content character would; (2) the "drop a trailing
  whitespace-only line" rule applies only to the literal's overall *last*
  raw part (upstream's `n == 1` check is "parts remaining including this
  one", true exactly on the last loop iteration — not "the literal has
  exactly one part total", which is a different, wrong condition this
  implementation's first draft briefly had and fixed before landing).
  Deliberate, documented scope cut vs. real Nix's lexer: a lone `'`
  immediately followed by `$` is treated here as two ordinary content
  characters (real Nix special-cases it as an indentation-counting
  boundary, same as an escape). This only affects the indentation
  *counting* boundary in the vanishingly rare case of a leading `'$` run
  of whitespace, never any produced string's actual bytes — not exercised
  by anything in `tests/e2e`, not worth the extra complexity here.
- **Runtime concatenation**: a new `OP_STR_CONCAT` op (`a` = count; same
  pop-`N`-reverse-order convention as `OP_MK_LIST`/`OP_MK_ATTRS`) and
  handler (`src/asm/ops/op_str_concat.S`) call a new C helper,
  `asnix_concat_strings` (`src/c/value_util.c`), with a pointer straight
  into VALSTACK — mirroring `OP_MK_LIST`'s shape exactly, including which
  registers need manual save/restore around the `call` (`%rsi`/`%rdi` via
  `%rbp`/`%r15`; `%r12`/`%r13`/`%r14` are ordinary SysV callee-saved
  registers and need no special handling). The **compiler** (`compiler.c`'s
  `AST_STRING` case) only emits this path when at least one part is an
  interpolated expression; an all-literal string (the common case,
  including every plain `"..."`/`''...''` literal with no `${...}` at
  all) still folds to a single compile-time constant exactly like
  pre-interpolation M5 did — `OP_STR_CONCAT` is pure overhead-avoidance,
  never required for correctness on that path. Validated with hand-fed
  IR (`tests/ir/test_strings_fixtures.c`) before the parser/compiler
  changes landed, same bug-isolation discipline as every other milestone.
- **String interpolation's value coercion is intentionally *not*
  `toString`'s** — this is the one place this slice's own "verify
  against real Nix before trusting it" discipline caught a real design
  mistake in the task's own starting assumption. See Known Bugs #11 below.

## Checked against numtide/nix-conformance

[numtide/nix-conformance](https://github.com/numtide/nix-conformance) is
an external, independently-maintained Nix-language conformance suite
(532 `eval-okay-*` cases as of this check, expected output from real Nix
2.34.8) — a stronger correctness bar than this project's own hand-written
fixtures alone. Full methodology and the 4 verified cases are in
`tests/conformance/` (`make test-conformance`); summary:

- Screened all 532 `lang/eval-okay-*.nix` cases for ones using *only*
  currently-implemented syntax (no `==`/`!=`/comparisons, `&&`/`||`,
  `with`, `rec`, `inherit`, `//`, `?`, string interpolation, `assert`,
  floats, paths, escape sequences, or builtins beyond `toString`/
  `typeOf`/`length`). Only 4 of 532 qualified — expected, given asnix's
  current language subset is still a small fraction of real Nix; this
  isn't a conformance *score*, just the set actually checkable yet.
- All 4 matched Nix's expected output exactly on the first run — **except**
  one real bug the check surfaced: `eval-okay-attrs6`'s expected output
  showed attrset keys printed alphabetically by name (`{ __overrides =
  ...; bar = ...; foo = ...; }`), but asnix's printer displayed them in
  symbol-intern order (first-seen-in-source order) instead, since `Attrs`
  entries are stored sorted by *symbol id* for `OP_SELECT`'s binary
  search — a representation choice with no reason to coincide with
  alphabetical name order. Confirmed directly: `{ b = 1; a = 2; }`
  printed as `{ b = 1; a = 2; }` instead of Nix's `{ a = 2; b = 1; }`.
  Fixed in `main.c`'s printer only (re-sorts entries by name for display;
  the stored symbol-id order, and `OP_SELECT`'s lookup, are untouched).
- As asnix's language coverage grows, re-run this same screening process
  for more candidates rather than assuming today's 4-case result still
  reflects available coverage.

**Re-screened after M10-M17** (recursive `let`/`rec {}`/`inherit`,
comparisons/`&&`/`||`/`!`/`assert`/`//`, string interpolation + multi-line
strings, `with`, lambda-destructuring patterns, `import` + path literals,
and the expanded builtins set): same clone (`/tmp/nix-conformance`,
commit `466c61d`, still 532 `eval-okay-*` cases), same methodology, run
mechanically this time rather than by hand -- every case was actually
executed through `./asnix` and diffed against its `.exp` sidecar (not just
grepped for disallowed tokens), since asnix's lack of type-checking on `+`/
`==`/etc. means a case using an unimplemented feature can still exit 0
with a *wrong, non-matching* answer rather than a clean parse/type error
(see `eval-okay-if`/`eval-okay-new-let`/`eval-okay-patterns`/
`eval-okay-ind-string` below) -- running and comparing catches that for
free, where a pure syntax whitelist would not.

- **15 additional cases qualify** (now 19 total, up from 4): `abs-path-
  default`, `abs-path-warn`, `backslash-newline-1`, `backslash-newline-2`,
  `concatstringssep`, `empty-args`, `equal-function-attrset-distinct-
  similar`, `equal-function-direct-distinct-similar`, `equal-function-
  list-distinct-similar`, `nested-with`, `scope-1`, `scope-2`, `scope-3`,
  `scope-7`, `url-literal-quoted-fatal`. Added to `tests/conformance/` in
  the same `.nix`/`.expected` pair convention; all pass via
  `make test-conformance`.
- **One real bug found and fixed** (see "Known bugs" #15 above): the
  `backslash-newline-{1,2}` pair surfaced that the printer never escaped
  special characters (`\n`/`\r`/`\t`/`"`/`\`/a literal `${` run) when
  displaying a string -- it printed raw bytes verbatim instead of real
  Nix's escaped form. Fixed in `main.c`; four pre-existing
  `tests/e2e/str_indented_*.expected` fixtures had to be updated in the
  same change since they'd baked in the old, wrong unescaped output.
- **Explicitly screened out, not added, and why** (ruling out
  printer-divergence false positives per this project's own caveat about
  the eager-forcing printer before concluding anything else was a real
  mismatch):
  - `eval-okay-eq`, `eval-okay-eq-slot-attrs`: rely on deep structural
    equality on lists/attrsets and/or string *content* equality
    (`"foo" == "f" + "oo"`) -- the already-documented `==`/`!=`
    pointer-identity scope cut (see the Operators section above), not a
    new finding.
  - `eval-okay-equal-function-{direct,attrset,list}-identical`: exercise
    real Nix's documented "value identity optimization" for `==` on
    attrsets/lists (member-pointer-identity short-circuits *before*
    falling back to "functions are never equal, even to themselves,"
    which is why `f == f` is `false` in real Nix but `{a=f;} == {a=f;}`
    is `true`) -- asnix's simpler all-levels pointer-identity rule gives
    the opposite answer on both shapes (`f == f` -> `true`,
    `{a=f;}=={a=f;}` -> `false`). A sharper manifestation of the same
    documented scope cut above, not a separate bug; implementing real
    Nix's exact rule would be new feature work, out of scope here. (The
    three *-distinct-similar siblings of these, which compare two
    genuinely different closures, happen to agree with real Nix under
    either semantics -- both say `false` -- which is why those three
    *did* qualify above.)
  - `eval-okay-if`, `eval-okay-new-let`, `eval-okay-patterns`,
    `eval-okay-ind-string`: all rely on `+` as *string* concatenation
    (`"f" + "oo"`) or multi-string `+`-chains -- asnix's `+` is
    INT-only and untyped (`op_add.S` just treats both operand words as
    ints), so these don't error, they silently compute a garbage integer
    that happens not to match the `.exp` string -- exactly the footgun
    this task's own brief warned about. Not a bug to fix (string `+` is
    a real missing language feature, not in scope here) and not safe to
    add even though asnix "ran" them.
  - `eval-okay-overrides`, `eval-okay-overrides-thunks`: rely on real
    Nix's legacy `__overrides`-in-`rec{}` special-casing (confirmed via
    the comment in `eval-okay-overrides-thunks.nix` itself, a
    `eval.cc`-era mechanism), which asnix's `rec {}` doesn't implement --
    asnix treats `__overrides` as an ordinary attribute name with no
    special effect. A genuine, narrow scope gap, not a bug; not
    implemented (a legacy, rarely-used Nix feature, not attempted here).
  - `eval-okay-autoargs`: needs CLI-level `--arg`/autoargs injection into
    a top-level lambda, a feature of `nix-instantiate`'s own driver, not
    the language -- asnix's `main()` has no equivalent mechanism and
    this isn't a language-evaluator gap at all.
  - `eval-okay-dotslash-path-fatal`, `eval-okay-dotdotslash-path-fatal`,
    `eval-okay-short-path-literal-warn`, `eval-okay-short-path-variation`:
    their `.exp` files use a `/pwd`-prefixed placeholder path that
    nix-conformance's own (unimplemented-here) test harness substitutes
    at run time -- not directly byte-comparable without that harness.
    Spot-checked qualitatively instead: asnix's resolved path has the
    same relative structure (e.g. `.../lang/test/subdir`), just rooted at
    this clone's real directory instead of the placeholder -- not a bug,
    just not mechanically comparable yet (same `SPEC.md`-adapter gap the
    original screening already flagged as future work).
  - Every other case needing a comparison/`&&`/`&&`/`with`/`rec`/
    `inherit`/`//`/`?`/string-interpolation/multi-line-string/pattern-
    lambda/`import`/path-literal feature not yet implemented, or a
    builtin outside the current set (`attrNames`/`attrValues`/`hasAttr`/
    `getAttr`/`mapAttrs`/`removeAttrs`/`map`/`filter`/`foldl'`/`elemAt`/
    `concatLists`/`tail`/`head`/`substring`/`stringLength`/
    `concatStringsSep`/`toString`/`typeOf`/`length`/`hashString`/
    `derivation`/`import`/the type predicates -- see the M7/M17 scope
    notes above for the exhaustive list), errored cleanly (parse error,
    "unrecognized character", "undefined variable", or a fatal type
    error) and was excluded -- the large majority of the 532 cases, as
    expected: real Nix's language surface, builtins (`genericClosure`,
    regex, `fromJSON`/`toJSON`/`fromTOML`, `tryEval`, `builtins.trace`,
    floats, `?`-default function args beyond patterns, `++`, string/
    division operators, deep equality, `__overrides`, and much more) are
    still far larger than this project's current subset.
- As asnix's language coverage grows further, re-run this same screening
  process again rather than assuming this 19-case result is final.

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

## M9 — benchmark harness (`bench/`)

A real harness (`bench/run`, workloads generated by `bench/gen_workloads.sh`)
comparing asnix against `nix-instantiate --eval` — see `bench/README.md`
for full methodology, the workload list, and (important) the caveats
before quoting these numbers anywhere. Short version of the caveat:
`nix-instantiate`'s time is dominated by process-startup/store-connection
cost, not per-node evaluation cost, so this is a *"time to get an answer
from a cold invocation"* comparison, not a clean per-node throughput
comparison. A sample run on this dev machine (`make release`, hyperfine,
`--warmup 3 --min-runs 15`):

| workload | asnix | nix-instantiate |
|---|---|---|
| baseline | 0.0013s | 0.92s |
| arith_deep | 0.0018s | 0.56s |
| let_chain | 0.0012s | 0.09s |
| lambda_chain | 0.0028s | 0.07s |
| select_chain | 0.0014s | 0.06s |
| list_build_large | 0.0018s | 0.61s |
| attrset_build_large | 0.0030s | 0.08s |

(see `bench/results/` for the full table and every run's provenance
header: git commit, `uname -a`, nproc, `nix-instantiate --version`.)

Every workload's asnix output is cross-checked byte-for-byte against
`nix-instantiate --eval` before being trusted as a benchmark — same
"verify against the real oracle" discipline as M7/M8. All 11 workloads
(10 generated + `baseline`) match exactly.

Built by a background subagent in an isolated worktree; its process was
lost when this session's context was compacted (the task-tracking
registry doesn't survive that, though the worktree and its completed
work on disk did) before it had written `bench/README.md` or this
section — picked up and finished directly rather than re-run from
scratch, after independently re-verifying its correctness claims.

## Recursive `let`/`rec {}`/`inherit`

Makes `let ... in` genuinely `letrec` (matching real Nix: every binding in
one block is visible to itself and every sibling, regardless of source
order), adds `rec { ... }` (an attrset literal with the same self/sibling
visibility; plain `{ ... }` stays non-recursive), and adds `inherit`/
`inherit (expr) ...` as a pure parse-time desugaring.

- **AST**: `AST_LET` now carries a flat binding list (`AstBinding[]` +
  `body`), not one name/value pair desugared into nested single-binding
  nodes — this is the structural prerequisite for mutual visibility: one
  `let` block == one scope level == one runtime `Env` frame with a slot
  per binding, so a sibling reference is just an ordinary same-depth
  `OP_VAR_REF`. `AST_ATTRSET` grew an `is_rec` flag. `AstBinding` grew
  `is_inherit`, set by the parser for bare `inherit x;` only (see below).
- **Three new ops** (`src/asm/ops/op_frame.S`): `OP_FRAME_ALLOC` (a =
  nslots) allocates a new `Env` and installs it as the current `ENV`
  *before* any binding's thunk is built, `OP_FRAME_SET` (a = slot) pops
  one value into that `Env`'s slot, `OP_FRAME_EXIT` restores `ENV` to its
  parent. Compiling a `let`/`rec {}` is: `FRAME_ALLOC(n)`, then for each
  binding, build its thunk (still lazy, unchanged `OP_MK_THUNK`) and
  `FRAME_SET` it into its slot, then (for `let`) compile the body or (for
  `rec {}`) read the n values back off the frame in sorted-symbol order
  via plain `OP_VAR_REF(0, slot)` and feed them to the existing
  `OP_MK_ATTRS` unchanged, then `FRAME_EXIT`. Because `OP_MK_THUNK`
  captures "whatever `ENV` is right now", and `FRAME_ALLOC` already
  switched `ENV` to the new frame before any thunk is built, every
  binding's thunk naturally captures the new frame — self/sibling
  references just resolve like any other variable, once forced later.
  `OP_FRAME_EXIT` matters for correctness even though a `let`'s body
  often *is* the rest of the enclosing thunk/lambda body (where it looks
  redundant, immediately followed by `OP_RET`): `let`/`rec {}` are also
  ordinary subexpressions (a function argument, a binop operand, a list
  element, ...), and without restoring `ENV` on the way out, a
  non-tail use would leave the extra frame installed, corrupting the
  depth of every `OP_VAR_REF` the compiler emitted *after* it assuming
  the pre-frame scope (see Known bugs #8 below — this was a real,
  independently-confirmed latent gap in the original M6 `OP_LET`, not
  just a theoretical risk introduced by this change).
- **`inherit` desugars entirely at parse time** (`src/c/parser.c`):
  `inherit x;` becomes a binding `x = x` (an `AST_VAR`), `inherit (expr)
  x y;` becomes `x = expr.x; y = expr.y` (an `AST_SELECT` per name,
  `expr`'s subtree reused — compiled, and so re-evaluated, once per
  inherited name, same as real Nix). The two forms resolve the
  right-hand side against *different* scopes, confirmed against real
  `nix-instantiate` (see Known bugs #9): `inherit (expr) ...`'s `expr`
  resolves against this block's *own* (possibly recursive) scope, just
  like any ordinary binding's value — `let inherit (pt) x y; pt = {
  ... }; in ...` sees the sibling `pt`. A bare `inherit x;`, however,
  resolves `x` against the scope *enclosing* this block, never its own
  — `rec { inherit x; }` can't accidentally self-reference the `x` it's
  pulling in from outside. The compiler threads this through
  `AstBinding.is_inherit` plus a dummy zero-name `ScopeFrame` ("`skip`")
  wrapped around the enclosing scope for inherited bindings, so compile-
  time depths correctly account for the one extra real `Env` level
  `OP_FRAME_ALLOC` always inserts before any binding's thunk — including
  inherited ones — gets built.
- **`rec` keyword**: lexer keyword, parser accepts `rec { ... }` wherever
  a plain `{ ... }` is accepted (atom position).
- **M4.5 native codegen** (`src/c/codegen.c`) is explicitly out of scope
  (per its own documented cut: literals/arithmetic/if/non-recursive
  `let` only) — its `AST_LET` handling was adapted only to the new flat-
  binding-list shape (iterating `n` bindings sequentially onto the
  native stack, exactly the semantics the old nested-desugaring produced),
  not given recursion; `inherit` in this path is a clear `exit(1)` error.
- **Verified against real `nix-instantiate --eval`** for every case named
  in the task: `rec { a = 1; b = a + 1; }`, `rec { a = b; b = 1; }.a` =>
  `1`, nested `let`s referencing outer-scope siblings, self- and mutually-
  recursive named functions (`let f = x: g x; g = x: if x then 1 else f
  true; in f false` => `1` — using boolean branching instead of `==`/`<`
  since comparison operators aren't implemented yet, per the task's own
  guidance), both `inherit` forms (plain and `(expr)`, including the
  sibling-visibility case above), `let`/`rec {}` used in non-tail
  position (`let a = 1; x = (let y = 1; in y) + a; in x` => `2`), and
  that plain non-rec `{ a = a; }` still correctly fails to resolve `a`
  (an "undefined variable" compile-time error, matching real Nix exactly
  — not a blackhole, since no self-reference is even possible without a
  recursive scope). All match real Nix exactly except the pre-existing,
  unrelated difference in how unforced thunks print (`<CODE>` in real
  Nix vs. this project's printer eagerly forcing for display — a known,
  deliberate M6-era choice, not part of this change).

## Operators — comparisons, `&&`/`||`/`!`, `assert`, `//`

Adds comparison operators (`== != < > <= >=`), short-circuiting booleans
(`&& || !`), `assert cond; body`, and the attrset update/merge operator
(`//`), plus real operator precedence for all of the above. Every new
construct was checked against real `nix-instantiate --eval` (not just
this project's own hand-written expectations) before being trusted,
including the precedence-sensitive cases — see below.

- **Precedence** (`src/c/parser.c`): the task brief's own summary table
  placed unary `!` right next to application (tightest), but that's
  *not* what real Nix actually does — confirmed directly:
  `nix-instantiate --eval -E '! 1 + 1'` errors on `2` (the `+` already
  ran, so `!` grabbed the whole `1 + 1`), while
  `nix-instantiate --eval -E '! true // {}'` errors on `false` (the `//`
  did *not* run first, so `!` only grabbed `true`). That pins `!` between
  `+`/`-` (tighter) and `//` (looser), matching the Nix manual's actual
  grammar table, not the approximate one in the brief. Implemented
  precedence, loosest to tightest below `parse_expr`'s keyword forms
  (`let`/`if`/lambda/`assert`): `||` (left-assoc) → `&&` (left-assoc) →
  `==`/`!=` (non-assoc, single occurrence — chaining is a parse error in
  real Nix too: `1 < 2 < 3` errors there) → `<`/`>`/`<=`/`>=` (non-assoc)
  → `//` (right-assoc, chainable) → `!` (prefix) → `+`/`-` (existing) →
  `*` (existing) → application (existing). All of these orderings were
  independently confirmed against `nix-instantiate`, not just the one
  example in the task brief (`1 + 2 == 3 && true`).
- **`&&`/`||` are compiled branch-shaped, not as eager binops**
  (`compiler.c`'s `AST_AND`/`AST_OR` cases): `a && b` lowers to `if a
  then b else false`, `a || b` lowers to `if a then true else b`, reusing
  `OP_IF`/`OP_JMP` wholesale — the untaken side's IR is simply never
  reached by the jmp-chain dispatch loop, so short-circuiting is a direct
  consequence of the control flow, not a separate thing to get right.
  Verified real short-circuiting (not just the right *value*) by
  confirming the untaken side is never *evaluated*:
  `false && (assert false; true)` prints `false` without error (same
  check with real Nix agrees), and the same expression with `true &&`
  instead does error in both this implementation and real Nix.
- **`assert cond; body`** (`AST_ASSERT`): lowers to `if cond then body
  else <OP_ASSERT_FAIL>` (new op, `src/asm/ops/op_assert.S`), reusing the
  same `OP_IF`/`OP_JMP` shape as `AST_IF`/`AST_AND`/`AST_OR` above. The
  failure message names the actual failed condition's source text
  (`asnix: assertion '<cond>' failed, aborting`) — the lexer now records
  every token's starting byte offset (`Token.pos`, previously only
  `TOK_IDENT`/`TOK_STRING` had a position at all), so the parser can
  slice out the exact source span between `assert` and `;` and the
  compiler bakes it into a pool-stashed message string at compile time
  (arena-allocated, same never-free convention as string literals).
- **`==`/`!=` scope cut** (`src/asm/ops/op_cmp.S`): collapses to plain
  64-bit raw equality on the two `Value` words. This is exact for
  `TAG_INT` (numeric equality) and `TAG_IMM` (true/false/null compared by
  subtag), and falls back to **pointer identity** for every other tag
  (strings, lists, attrsets, closures, thunks, primops) — because the
  tag bits are literally part of the raw word, two values with different
  tags are automatically unequal, and two values with the same non-
  INT/IMM tag are equal here iff they're the same heap object. Real Nix
  does deep structural equality for lists/attrsets and content equality
  for strings (confirmed: `"a" == "a"` and `{} == {}` are both `true`
  there); this implementation says `false` for both, since two separate
  string/attrset literals are two separate arena allocations. This is a
  deliberate, documented scope cut (deep structural equality on
  recursive aggregates is a rabbit hole not attempted here), not a bug —
  but it is a real behavioral difference from real Nix worth knowing
  about if this is ever extended.
- **`<`/`>`/`<=`/`>=` scope cut**: `TAG_INT` operands only; anything else
  (including two strings, which real Nix *does* support ordering on) is
  a fatal type error. Matches the task's explicit scope (no string/list/
  attrset comparison support exists yet in this codebase at all).
- **`//` update/merge** (`src/c/attrs_update.c`'s `asnix_attrs_update`,
  called from `src/asm/ops/op_update.S`): a plain two-pointer merge of
  the two (already sorted-by-symbol-id) `Attrs` arrays, right-biased on
  a duplicate key — implemented in C rather than hand-rolled asm, since
  it's not hot-path code and a linear merge is easy to get right in C
  (same rationale `builtins.c` already uses for `toString`/`typeOf`).
  Neither operand is forced or mutated; entries (including any still-
  unforced `Thunk`) are copied through verbatim, so `//` itself stays
  lazy per-key, matching real Nix.
- **Not touched**: `src/c/codegen.c` (the M4.5 native compiler) — per
  this project's standing scope cut, every new op in this slice is
  interpreter-only. The four new `Ast` kinds (`AST_AND`/`AST_OR`/
  `AST_NOT`/`AST_ASSERT`) are simply never passed to `codegen_expr` in
  practice (nothing in `main.c` invokes the native path at all), so this
  is intentionally left as an unhandled-`switch`-case compiler warning
  rather than adding dead rejection cases to a file this project's scope
  rules say not to touch.
- **Validated**: `tests/e2e/ops_*.nix` + `.expected` pairs (wired into
  `make test-e2e`), each independently checked against real
  `nix-instantiate --eval` on the equivalent source during development
  (see the precedence bullet above for the specific disambiguating
  expressions used).

## M13 — `with` expressions

Adds `with expr; body`. Real Nix semantics (confirmed against
`nix-instantiate` for every point below before trusting it, not just
transcribed from memory): a `with` is a *fallback* for names that don't
resolve lexically -- a lexical binding always wins, even one introduced
further out than the `with` -- resolved at runtime against the
with-expression's value, and evaluated lazily (the with-value is only
forced the first time some name inside `body` actually needs to fall back
to it). Nested `with`s chain innermost-first: if the nearest enclosing
`with`'s attrset doesn't have the key, the search falls through to the
next enclosing `with`, and so on; the first one (searching innermost-out)
that *does* have the key wins, even if a nearer one would otherwise have
been consulted first for a different key.

- **Grammar**: only one form, `WITH expr ';' expr_function` (matching
  real Nix's own grammar exactly) -- `cur(p)->kind == TOK_WITH` is handled
  in `parser.c`'s `parse_expr`, the same precedence level as `let`/`if`.
  There's no alternate colon/paren-only form in real Nix either; the
  "parenthesized" form the task description anticipated is just an
  ordinary parenthesized `expr` as the with-value (`with (foo); ...`),
  already handled for free since `expr` parsing already supports parens.
- **Resolution split between compile time and runtime.** `compiler.c`'s
  `ScopeFrame` chain (by the time `with` landed, already multi-slot --
  `names[]`/`name_lens[]`/`count` -- to support recursive `let`/`rec {}`'s
  shared-frame bindings, see that section above) grows a dedicated
  `is_with` flag for its third frame shape: a `with`-marker frame has
  `count == 0` (no statically-known names, so the slot-search loop trivially
  never matches) *and* `is_with` set. `count == 0` alone isn't enough to
  mean "with frame": `AST_LET`'s dummy `skip` level (used to resolve
  `inherit`'s outer-scope reference) is also zero-count but must not join
  the with-fallback chain, which is exactly what the separate flag is for.
  It still corresponds to exactly one new runtime `Env` frame, so it still
  has to count as one hop for every *other* name's De Bruijn depth.
  `AST_VAR`'s resolution walk now does two
  things in one pass: look for a lexical match (as before -- if found,
  return immediately, before ever consulting any collected `with`s, which
  is exactly what makes a lexical binding always win); and collect the
  depth of every with-marker frame passed along the way. If the walk
  exhausts the scope chain with no lexical match: a compile error (as
  before) if no `with`s were collected, otherwise a short runtime fallback
  chain is emitted, one step per collected depth (innermost first,
  bounded by `ASNIX_MAX_WITH_CHAIN = 64` -- real Nix has no such limit,
  but nesting this deep never happens in practice; a documented scope cut
  in the same spirit as M6's non-recursive `let`).
- **Runtime representation reuses `Env{up;values[]}` exactly, per the task's
  own design hint, rather than adding a parallel channel**: a `with`
  introduces an ordinary 1-slot `Env` frame (`OP_WITH`, mirroring
  `OP_LET`'s `nslots=1` case almost byte-for-byte -- see `op_with.S`)
  whose *own* `up` field has one bit (`ASNIX_ENV_WITH_TAG` in `env.h`)
  OR'd in to mark "this frame is a `with`". This costs nothing new: arena
  pointers are always 8-byte-aligned (same precondition `value.h`'s own
  tag bits already rely on), so the low 3 bits of `up` are free. The only
  two places that blindly chase a generic `up` hop (`op_var_ref.S`'s
  depth walk, `env.S`'s `asnix_env_lookup`) now mask the bit back off
  after each hop -- cheap (`andq $-8`) and always safe, since ordinary
  frames' `up` fields never have it set in the first place.
- **The fallback search at each level is just an ordinary `OP_VAR_REF`
  plus one new op, not a bespoke "walk the Env chain looking for
  with-frames" routine**: since each with-marker frame's compile-time
  depth is already known (collected above), reading+forcing its slot 0 is
  *exactly* what `OP_VAR_REF(depth,0)` already does for any lexical
  variable -- no new forcing code needed, and this is what gets `with`'s
  laziness for free (forcing only happens when a level is actually
  consulted). The new op, `OP_WITH_TRY(delta, sym)`, pops that
  already-forced attrs value, type-checks it (fatal if not an attrset --
  but only *when reached*, matching real Nix: `with 5; 1` succeeds since
  the bad value is never consulted, while `with 5; x` errors), and
  binary-searches for `sym` (reusing `OP_SELECT`'s exact search +
  entry-thunk-forcing logic, since attrset binding values are lazy too).
  Found: push the match, fall through to an unconditional `OP_JMP` the
  compiler always emits right after, landing past every remaining level.
  Not found: jump (by the compile-time-patched `delta`, same
  `ib_patch_delta` mechanism `AST_IF`'s then/else branches already use)
  to the next level's `OP_VAR_REF`/`OP_WITH_TRY` pair, or to `OP_WITH_MISS`
  (aborts "undefined variable") if that was the last level.
- **Verified against real `nix-instantiate --eval`** for every case the
  task called out, plus a few more: basic `with`; a lexical name
  shadowing a `with`-provided one (`let a = 100; in with {a=1;}; a` -> both
  print `100`); nested `with`s falling through to an outer one for a key
  the inner one lacks (`with {a=1;}; with {b=2;}; a+b` -> both `3`); an
  inner `with` shadowing an outer one that has the *same* key
  (`with {a=1;}; with {a=2;}; a` -> both `2`); a `with` whose value is a
  variable, a lambda parameter, and a function-call result, not just a
  literal attrset (all matched); laziness (`with 5; 1` succeeds on both,
  `with 5; x` errors on both with a type error); and the
  compile-time-vs-runtime "undefined variable" split (`with {a=1;}; zzz`
  errors on both). See `tests/e2e/m10_with_*.nix`.

## M14 — lambda-destructuring patterns

Adds `{a, b}: body`, `{a, b ? default}: body`, `{a, b, ...}: body`, and
`@`-binding (`args@{a,b}: body` / `{a,b}@args: body`). Still arity-1 at
the `OP_CALL`/`Closure` ABI level -- a pattern lambda still takes exactly
one real attrset argument; destructuring happens entirely inside the
compiled body, not in the call mechanism.

- **Two synthetic scope frames per pattern lambda** (`compiler.c`'s
  `AST_LAMBDA` case): an outer 1-slot `argframe` holding the raw attrset
  argument (and its `@`-alias, if any -- an empty-string name if none,
  which can never match a real identifier), and an inner `fieldframe`
  (`up = &argframe`) with one slot per declared field. `fieldframe` is
  allocated via `OP_FRAME_ALLOC` *before* any field's thunk is built, so
  every field's thunk (built from synthetic `AST_SELECT`/`AST_HAS_ATTR`/
  `AST_IF` nodes, going through the same `compile_expr` path hand-written
  source would) naturally captures it -- giving fields letrec-style
  mutual/forward visibility, confirmed against real Nix: `{a ? b+1, b ?
  5}: a` applied to `{}` sees `b` from `a`'s default even though `b` is
  declared after `a`.
- **Eager, not lazy, pattern validation** (`OP_CHECK_FORMALS`,
  `src/asm/ops/op_check_formals.S`): a missing required field, an
  unexpected extra field (without `...`), or a non-attrset argument all
  abort immediately on application, even if `body` never reads the
  offending field -- matches real Nix's eager argument-pattern checking,
  confirmed directly against `nix-instantiate`.
- **`?` (has-attr)** (`OP_HAS_ATTR`, `src/asm/ops/op_has_attr.S`): `expr ?
  attr` pushes `TRUE`/`FALSE`, `FALSE` (not a fatal error) if `expr` isn't
  even an `Attrs` -- used to compile each optional field's `if hasAttr
  then select else default` shape, and usable standalone in the language
  too.
- **Reused the existing recursive-`let` machinery instead of adding a
  duplicate**: the implementing agent, working from a pre-recursion base,
  independently reinvented `OP_FRAME_ALLOC`/`OP_FRAME_SET` under the names
  `OP_FRAME_NEW`/`OP_FRAME_BIND_SLOT` -- functionally identical register-
  for-register (same alloc-then-fill-before-body shape the "Recursive
  `let`" section above already established). Found while integrating this
  branch; resolved by dropping the duplicate ops entirely and pointing
  pattern-field binding at the existing `OP_FRAME_ALLOC`/`OP_FRAME_SET`
  pair instead of carrying two copies of the same mechanism.
- **Two further integration-time fixes, both from the same root cause**
  (this branch was built from a pre-recursion, pre-operators, pre-strings
  base, so its own copy of shared code had drifted): its `AST_LET` case
  still used the single-binding, non-recursive shape `AST_LET` had before
  the "Recursive `let`" work (would not even have compiled against the
  current `ast.h`) -- replaced with the already-merged recursive version
  wholesale, nothing pattern-specific was lost. Its `ScopeFrame` also
  independently grew multi-slot support (needed for the same reason the
  recursive-`let` work needed it) under different field names (`nslots`
  vs. `count`, and no `with`-fallback support at all) -- reconciled onto
  the single already-merged `ScopeFrame` shape (`names`/`name_lens`/
  `count`/`is_with`) rather than carrying two slightly-different
  definitions.
- **Verified against real `nix-instantiate --eval`**: plain destructuring,
  missing-required-field abort, defaults (both used and overridden),
  `...` with extra fields present, and both `@`-binding positions.

## M15 scope notes: `import` + path literals

**Path literals**: a new lexer token (`src/c/lexer.c`'s `try_scan_path`),
distinct from quoted strings — bare `./foo.nix`, `../foo.nix`, `/abs/foo`,
and bare relative `foo/bar.nix` (all four forms checked against
`nix-instantiate --eval`'s `builtins.typeOf`/`builtins.toString`). There's
no division operator anywhere in this grammar, so any `/` outside a string
is unambiguously a path — that's what lets the lexer try path-scanning
*before* digit/ident scanning with no new lookahead machinery breaking any
existing token. A **relative** path literal resolves (at *compile* time,
not eval time — see `compiler.c`'s `AST_PATH` case and
`g_compile_base_dir`) against the directory of the `.nix` file it's
*written in*, matching real Nix exactly (confirmed with a nested-import
fixture: `a/b.nix`'s own `./c.nix` resolves to `a/c.nix`, not wherever the
file that `import`ed `a/b.nix` lives, and not the process's cwd) — not
against the importing file's directory, and not the process's cwd.
Resolution runs the result through a lexical canonicalizer (collapses
`.`/`..`/doubled slashes, no symlink resolution) matching real Nix's
`canonPath` for the cases checked (`./sub/../foo.nix` → `.../foo.nix`,
verified against `nix-instantiate`). A trailing slash is rejected with the
same message real Nix uses ("path has a trailing slash") rather than
silently stripped.

**Value representation**: no new tag — all 8 `ASNIX_TAG_*` slots are
already spent (see `include/asnix/value.h`), so a Path reuses
`ASNIX_TAG_PTR` exactly as `value.h`'s own comment on that tag
anticipates ("generic heap pointer... via a header Kind byte"):
distinguished from a String by a reserved negative sentinel in the
leading word (a genuine String's leading word is always its own
non-negative length) — see `include/asnix/heap.h`'s `ASNIX_PATH_MARKER`
and `src/c/value_util.c`'s `asnix_value_is_path`/`asnix_ptr_bytes`. Every
place that used to assume "`ASNIX_TAG_PTR` means String" (`typeOf`,
`toString`, the printer, `hashString`) was updated to dispatch on the
marker; `toString`ing a Path produces a plain String with the same bytes
(real Nix changes the value's *type* from `"path"` to `"string"` here —
checked directly), and `hashString` now rejects a Path argument instead
of misreading the marker as a bogus length (checked against
`nix-instantiate`: real Nix also rejects it, "expected a string but found
a path").

**Search-path literals** (`<nixpkgs>`-style): included — cheap enough
(just `NIX_PATH` parsing + one `stat()`, no new value kind). Implements
real Nix's `findFile` rule (checked against `nix-instantiate`, with
`NIX_PATH` set to both `name=dir` and plain-`dir` forms): colon-separated
`NIX_PATH` entries, `name=dir` matched by exact leading-component name
(trusted without a filesystem check — matching real Nix), plain `dir`
entries probed for `dir/<name>` via `stat()`; first match wins. **Not**
added to the committed `tests/e2e` suite, unlike everything else here:
asserting on a `NIX_PATH`-derived absolute path would bake this
particular checkout's filesystem location into a fixture meant to run
from any clone — manually verified against `nix-instantiate --eval
'builtins.toString <foo>'` with `NIX_PATH=foo=<dir>` instead (see this
milestone's session transcript).

**`import`**: `src/c/main.c`'s `main()` used to inline the whole
lex→parse→compile→eval pipeline directly; it's now `asnix_eval_file()`
(`src/c/eval_file.c`), called by both `main()` and the new
`builtins.import` primop (`src/c/builtins.c`'s `prim_import`) so the
pipeline exists in exactly one place. Each call gets a brand-new root
`Env`/`builtins` attrset — an imported file never implicitly sees the
importing file's local scope (checked: this falls out for free from
`asnix_env_alloc(NULL, 1)` building a parentless frame every time, not
from any explicit "don't leak scope" logic). Accepts either a Path or an
**absolute**-path String; a relative-path String is rejected with real
Nix's exact wording ("string '...' doesn't represent an absolute path")
rather than silently resolving it against cwd — there's no "containing
file" to resolve a bare string argument against the way a path *literal*
resolves against its own file at compile time.

**Cut, deliberately**: `import` (and every other builtin) is reachable
only as `builtins.import` — this codebase has no implicit "with builtins"
root scope the way real Nix exposes `import`/`map`/`throw`/etc as bare
globals (consistent with `m7_builtin_as_value.nix`'s existing
`builtins.toString`-only pattern, not a new gap this milestone
introduces). Path **arithmetic** (`somePath + "/file"`, path equality,
etc.) isn't implemented — `AST_BINOP`'s `+` is still integer-only
(`op_add.S`); giving `+` path semantics would mean touching the hot
arithmetic op handler for a feature outside this milestone's ask. String
**interpolation** of a path (`"${./foo}"`) segfaulted (see Known bugs
below) until fixed during integration with the separately-landed string-
interpolation milestone — now coerces the path the same plain way
`toString` does, which is still not what real Nix does: real Nix's
interpolation of a path copies it to the Nix store and substitutes the
resulting store path (checked: `"${./foo.nix}"` evaluates to a
`/nix/store/...` string there, not the plain path text) — a "string
context" / content-addressing feature well beyond either milestone's
scope (no daemon `AddToStore` is wired up — see M8's scope notes above),
not a simple text substitution. Flagged here as a known, documented
behavioral gap (not a crash) for whoever wires up `AddToStore`.

## M16 scope notes: `asnix_apply` — general re-entrant "call this Value from C"

`asnix_force_for_print` (M6, `src/asm/force.S`) only forces a thunk with
zero arguments — its own header comment explicitly scoped out "a general
re-entrant builtin-calls-force mechanism... that's M7+". Builtins like
`map`/`filter`/`foldl'` (next stage) need exactly that: a way to call an
arbitrary Closure or PrimOp Value from plain C code and get its result
back, e.g. `map (x: x + 1) [1 2 3]` needs `builtins.c`'s `map`
implementation to apply the user-supplied closure to each list element.

`Value asnix_apply(Value fn, const Value *args, int64_t argc)`
(`src/asm/apply.S`) fills that gap, modeled closely on
`asnix_force_for_print`'s self-contained-dispatch-context pattern and
`op_call.S`'s two dispatch paths:

- **Type/arity checked exactly like `OP_CALL`**: anything that isn't a
  Closure or PrimOp Value is a fatal type error; an argc that doesn't
  match `fn`'s arity is a fatal arity error — same two checks, same
  wording style (own copies of the messages, since `op_call.S`'s are
  file-local `.L` labels, not exported symbols).
- **PrimOp target**: no Env/VALSTACK/RETSP needed at all — a primop has
  no lexical body to run through the dispatch loop, so this is a plain
  tail call (`jmp`) straight into the native `fn(args, argc)`, letting
  its own `ret` return directly to the C caller with zero overhead.
- **Closure target**: needs a real dispatch context to run the closure's
  IR body, so `asnix_apply` brings its **own fresh VALSTACK/RETSP** (via
  `asnix_arena_alloc`, sized and bounds-checked the same way
  `force.S`'s nested context is — see "Known bugs" item 6 below),
  allocates a new call frame, copies `args` into it,
  and pushes a sentinel `RetFrame` (`force_target = 0`: an ordinary call,
  nothing to memoize) whose `return_ip` points at a small epilogue
  (`asnix_apply_epilogue`) that restores every saved register and hands
  the result back to the C caller — the same "fake RetFrame whose IP is
  native-callable code" trick as `force.S`'s own
  `.Lforce_sentinel_ir`/`asnix_force_epilogue`, just for an ordinary
  application instead of thunk-forcing.
- **Never aliases an already-active context**: this can be called
  reentrantly (a builtin called from within an actively-running
  `asnix_eval` that itself calls back into `asnix_apply`, or nested
  `asnix_apply` calls), so it never reuses whatever VALSTACK/RETSP/
  `asnix_valstack_limit`/`asnix_retsp_limit` an enclosing context set up
  — it saves and restores those globals around its own fresh allocation,
  exactly like `force.S` already does around its own nested use.
- **No user-visible language feature by itself** — pure infrastructure,
  consumed by the next (builtins-expansion) stage. Verified directly via
  `tests/c/test_apply.c`: a hand-built Closure (unary and binary, via
  `IrBuilder`/`ib_lambda`, never `OP_CALL`ed in-language) and a hand-built
  PrimOp target, sequential and nested call sequences (proving no leaked
  state between fresh-buffer setups), plus dedicated abort-cleanly checks
  for a Closure arity mismatch, a PrimOp arity mismatch, and a
  non-function type error (fork + exit-code check, same pattern as
  `tests/asm/test_stacklimits.c`).

## M17 scope notes: builtins expansion

Adds `map`, `filter`, `foldl'`, `elemAt`, `concatLists`, `tail`, `head`
(`src/c/builtins_list.c`); `attrNames`, `attrValues`, `hasAttr`,
`getAttr`, `mapAttrs`, `removeAttrs` (`src/c/builtins_attrs.c`);
`substring`, `stringLength`, `concatStringsSep`, and the type predicates
`isNull`/`isString`/`isList`/`isAttrs`/`isFunction`/`isInt`/`isBool`
(`src/c/builtins_misc.c`). Built by three agents in parallel, each in its
own translation unit specifically to avoid textually colliding on
`builtins.c`'s shared registration table — integrated by merging each
module's own small `{name, fn, arity}` array (or, for `builtins_misc.c`,
a `{name, build()}` array, since `substring`/`concatStringsSep` build a
curried-Closure chain rather than a single PrimOp) into
`asnix_make_builtins_attrs`.

- **`asnix_apply` dependency**: `map`/`filter`/`foldl'`/`mapAttrs` all need
  to call a user-supplied Closure/PrimOp from C — exactly what the M16
  stage above provides. Both agents that needed it correctly anticipated
  it wasn't merged yet (each wrote its own narrowly-scoped local stand-in
  with an explicit "delete this and repoint at the real one" integration
  note), so the integration step here deleted both local stand-ins
  (`src/c/apply_shim.h`+a local `src/asm/apply.S` from the list module,
  `include/asnix/apply.h`+another local `src/asm/apply.S` from the attrs
  module) and repointed both at the real `asnix_apply` (`#include
  "asnix/ir.h"`) — same signature, so no call-site changes were needed
  beyond the `#include` itself.
- **Duplicate typedef, found integrating**: `builtins_list.h` and
  `builtins_attrs.h` independently defined the identical
  `AsnixPrimFn`/`AsnixBuiltinDef` shape (unsurprising — both were mirroring
  `builtins.c`'s own `PrimFn`/`BuiltinDef`), which collides the moment both
  headers land in the same translation unit (`builtins.c`). Fixed by
  keeping one definition (`builtins_list.h`) and having `builtins_attrs.h`
  include it instead of redefining it.
- **`asnix_make_builtins_attrs` rewritten** to merge four sources
  (`builtins.c`'s own `BUILTINS[]` plus the three new modules) into one
  `malloc`'d, sorted-by-symbol-id array before building the final `Attrs`
  object — the function used to work over a single compile-time-sized
  array; `n` is no longer known at compile time once four independent
  source counts are involved.
- **Currying**: every arity>1 builtin here uses the same curried-via-
  dedicated-arity-1-stages pattern established for `hashString` (M8) —
  `hasAttr`/`getAttr`/`mapAttrs`/`removeAttrs` via a shared
  `build_curry2_body` helper, `substring`/`concatStringsSep` via a short
  chain of real Closures (`builtins_misc.c`'s `make_curried_native`,
  needed because their arguments come from an unbounded domain — an
  integer start/length, an arbitrary separator string — unlike
  `hashString`'s finite 4-algorithm enum trick, which can't generalize).
- **`genericClosure` deliberately cut** (`builtins_list.c`) rather than
  rushed under time pressure — documented as a follow-up, not attempted.
- **Verified against real `nix-instantiate --eval`**, beyond what each
  module's own implementer already checked: `map`/`foldl'` composed
  together, `filter`, `attrNames` (sorted-by-name order), `hasAttr`,
  `substring`, `isList`. One real difference confirmed as pre-existing
  and *not* a bug in this stage: `mapAttrs`'s *values* print differently
  (`{ a = 2; b = 3; }` here vs. real Nix's `{ a = <CODE>; b = <CODE>; }`)
  because this project's printer eagerly forces nested values for display
  while real Nix's does not — the same deliberate M6-era divergence
  already documented elsewhere in this file, confirmed by comparing the
  actual (forced) values through `foldl'` instead of the raw printed form.

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
3. **`%rdi` (ENV) never actually restored after `OP_LAMBDA`** (M3, found in
   M6): after allocating the closure, the handler popped the saved ENV
   value into `%rdx`, stored *that* into the closure object, and restored
   IP into `%rsi` — but never copied it back into `%rdi` itself. `%rdi`
   was left holding `$ASNIX_CLOSURE_SIZE` (24), leftover from staging the
   `asnix_arena_alloc` size argument. This is a genuinely different bug
   from #1 above (that one was about *which registers* get saved/restored
   around the whole `asnix_eval` call; this one is about *correctly
   writing back* one of them within a single handler) — and it was
   latent since M3/M4: `fixture_lambda_call`'s very next op is `OP_LET`,
   which immediately overwrites `%rdi` with its own new frame anyway, so
   the corrupted value was never actually read before being clobbered.
   M6's `OP_MK_THUNK` copied the same pattern (and bug) from `OP_LAMBDA`,
   but `let x = 10; y = 20; in x + y`'s *second* binding's `OP_MK_THUNK`
   left `%rdi` corrupted going into `OP_LET` for `y`, which wrote that
   garbage into `y`'s frame's `up` pointer — and this time something
   *did* read it: `x + y`'s `OP_VAR_REF(depth=1)` for `x` walked `up` from
   `y`'s frame straight into garbage memory. Fixed in both `op_lambda.S`
   and `op_mk_thunk.S` by popping directly into `%rdi` (then copying from
   the now-correct `%rdi` into the heap object), not through `%rdx`.
4. **Attrset printing used storage order instead of name order** (M5,
   found by checking against nix-conformance in M7): `Attrs` entries are
   stored sorted by symbol id (intern order) for `OP_SELECT`'s binary
   search, which the printer wrongly assumed was also the right *display*
   order. Real Nix always prints attrset keys sorted alphabetically by
   name, regardless of declaration or internal storage order. Fixed in
   `main.c`'s printer only (re-sorts by name for display); the runtime
   representation and `OP_SELECT` are untouched. See "Checked against
   numtide/nix-conformance" above for how this was found.
5. **Skipped a hashing step in `makeStorePath`** (M8): fed
   `sha256(masked ATerm)` directly into
   `asnix_make_store_path_from_digest`, omitting the middle step —
   hex-encoding that digest with its `sha256:` prefix, assembling the
   `"type:hash:storeDir:name"` preimage, and hashing *that*. Caught
   immediately because the computed `outPath` didn't match the
   already-verified real `nix-instantiate` oracle (`derivation {...}`'s
   M8 scope notes above have the full writeup); fixed by adding a single
   `compute_store_path()` helper so the two-step composition only exists
   in one place.
6. **VALSTACK/RETSP had no bounds check at all** (found by a benchmark
   agent; fixed post-M9): `dispatch.S` sized VALSTACK at 32768 bytes
   (4096 `Value`s) and RETSP at 4096 bytes (128 call frames,
   `ASNIX_RETFRAME_SIZE`=32), both via the shared bump arena, with
   *nothing* checking whether a push still fit before writing — `force.S`
   had the identical gap in its own separate, smaller buffers
   (`ASNIX_FORCE_VALSTACK_BYTES`/`ASNIX_FORCE_RETSTACK_BYTES`, 4096/1024
   bytes). A single list/attrset literal past ~4096 elements, or a chain
   of ~128+ simultaneous dependent thunk-forces (each `let` binding
   forcing the previous one), silently corrupted whatever the arena
   allocated next — sometimes a clean segfault, sometimes a wrong answer,
   never a clear error. Fixed by: (a) growing all four buffers
   substantially (VALSTACK to 1,048,576 `Value`s / 8 MiB, RETSP to 65,536
   frames / 2 MiB; `force.S`'s own to 32,768 `Value`s / 256 KiB and 2,048
   frames / 64 KiB) and (b) adding a real bounds check
   (`include/asnix/stackcheck.h`'s `ASNIX_CHECK_VALSTACK`/
   `ASNIX_CHECK_RETSP` macros, backed by `src/asm/stacklimits.S`'s shared
   `asnix_valstack_limit`/`asnix_retsp_limit` globals) at every push site
   across `op_push.S`, `op_var_ref.S`, `op_select.S`, `op_call.S`,
   `op_lambda.S`, `op_mk_thunk.S`, `op_mk_list.S`, `op_mk_attrs.S`, and
   `force.S` itself, so exceeding the (now much larger) cap is a clean
   `asnix_fatal` abort instead of silent corruption — the same philosophy
   `ASNIX_MAX_MEMORY` already uses in `arena.S`. Because `force.S` can run
   *nested* inside an active `asnix_eval` (e.g. `derivation.c`'s builtin
   forcing a value mid-eval) or inside another `force.S` call, the two
   limit globals are saved and restored around that nested use exactly
   like `%r12`/`%r14` themselves. Verified directly against
   `nix-instantiate` at depth/size well past both old caps (see
   `tests/e2e/m9_deep_let_chain.nix`, `m9_large_list.nix`,
   `m9_large_attrset.nix`) and with dedicated overflow-abort unit tests
   (`tests/asm/test_stacklimits.c`).
7. **Separate, unrelated native-C-stack-recursion limit in the compiler**
   (found while verifying bug #6 above, *not* fixed here — out of scope
   for that fix): a single `let` with enough bindings written as
   literal, flat Nix source (not IR) — e.g. `let x0 = 0; x1 = x0 + 1;
   ...; in x_N` — crashes with a real SIGSEGV around N≈31,000, reproduced
   consistently and confirmed via `gdb` to be a native stack overflow
   inside `compiler.c`'s `compile_expr`'s recursive `AST_LET` handling
   (one native C stack frame per nested binding), not anything to do with
   VALSTACK/RETSP — raising the process's native stack limit
   (`ulimit -s unlimited`) makes it go away, which bug #6's fix (both
   explicit stacks live in heap-arena memory, not the native stack)
   cannot and does not affect either way. Distinguishing this from bug #6
   is why this project's own regression tests for #6
   (`tests/asm/test_stacklimits.c`) build IR directly via `IrBuilder`
   rather than through the lexer/parser/compiler — that sidesteps this
   separate limit entirely so the VALSTACK/RETSP checks stay isolated.
   Left as a known gap for whoever picks up deep `let`-chain support
   through real Nix source next.
8. **`let` (and `rec {}`) used as a non-tail subexpression leaked its Env
   frame into whatever followed it** (latent since M6's `OP_LET`, found
   while implementing recursive `let`/`rec {}`): `OP_LET` switches `ENV`
   to the new frame and never switches it back, which is harmless when a
   `let`'s body is the last thing in its enclosing thunk/lambda body (the
   common case every existing fixture happened to use), but wrong the
   moment a `let`/`rec {}` appears as an ordinary subexpression — e.g.
   `let a = 1; x = (let y = 1; in y) + a; in x`: after the inner
   `let y = 1; in y` finishes, `ENV` is still the inner frame, so the
   following `+ a` (compiled assuming the *outer* `let`'s frame is
   depth 0) reads the wrong slot at the wrong depth. Reproduced directly
   against this exact expression before the fix (returned garbage/wrong
   depth instead of `2`). Fixed by giving every scope-introducing
   construct (`OP_FRAME_ALLOC`'s callers) a matching `OP_FRAME_EXIT`
   once the frame is no longer needed — see "Recursive `let`/`rec {}`/
   `inherit`" above. `OP_LET` itself (now unused by the compiler, kept
   for its existing asm/IR tests) was never patched the same way, since
   nothing compiles it anymore.
9. **`inherit (expr) x;`'s `expr` does *not* resolve the same way as
   bare `inherit x;`'s implicit right-hand side** (found implementing
   `inherit`, confirmed against real `nix-instantiate` before trusting
   either assumption): a first implementation treated both forms as
   "resolve against the scope enclosing this `let`/`rec {}`" by analogy,
   which breaks `let inherit (pt) x y; pt = { x = 1; y = 2; }; in x + y`
   (real Nix: `3` — `pt`, a *sibling* in the same `let`, must be visible
   to `expr`) and also corrupts bare `inherit x;` under the recursive
   frame scheme above: once `OP_FRAME_ALLOC` installs the new frame
   *before* any binding's thunk is built, a thunk compiled as if it
   still captured the old (one-level-shallower) scope actually captures
   the new frame at runtime, a depth-accounting mismatch that made
   `let x = 10; in rec { inherit x; y = x + 1; }` blackhole instead of
   returning `{ x = 10; y = 11; }`. Fixed by (a) treating `inherit
   (expr) ...` as an entirely ordinary binding (its value resolves
   against the block's own scope, siblings included — matching real
   Nix) and (b) resolving bare `inherit x;` against a dummy zero-name
   scope level wrapped around the enclosing scope, so compile-time depth
   counting accounts for the one real `Env` frame `OP_FRAME_ALLOC` always
   inserts first. Both forms verified against real `nix-instantiate`
   after the fix.
10. **`asnix_coerce_to_string` (`builtins.toString`, M7) stringified
   booleans wrong** (found while adding string-interpolation coercion,
   see above): it returned `"true"`/`"false"` for `true`/`false`. Real
   Nix's `toString true`/`toString false` are `"1"`/`""` (confirmed
   directly: `nix-instantiate --eval -E 'builtins.toString true'` →
   `"1"`; `builtins.toString false` and `builtins.toString null` both →
   `""`). This had shipped since M7 with no fixture catching it (no
   existing test exercised `toString` on a bool or `null`). Fixed in
   `value_util.c`'s `asnix_coerce_to_string`; also fixed-forward into the
   new `tests/ir/test_strings_fixtures.c`/`tests/e2e/
   str_interp_tostring_bool_null.nix` so it stays covered.
11. **String interpolation is not `toString`** (found the same way, while
   implementing `OP_STR_CONCAT`): the task's own starting description
   ("coerce each interpolated expression's value to a string — reuse
   `asnix_coerce_to_string`") turns out to not match real Nix. Verified
   directly: `nix-instantiate --eval -E '"${7}"'`,
   `'"${true}"'`, and `'"${null}"'` **all fail** —
   `error: cannot coerce an integer/a Boolean/null to a string` — even
   though `builtins.toString 7`/`true`/`null` all succeed. Real Nix's
   `EvalState::coerceToString` has a `coerceMore` flag; `toString` passes
   `true`, plain `"${...}"`/`''${...}''` interpolation passes `false` —
   only strings (and paths, and attrsets with `__toString`/`outPath`,
   neither of which this project has yet) are accepted without it.
   Implemented as a separate, stricter function
   (`value_util.c`'s file-local `coerce_to_string_for_interp`, used only
   by `asnix_concat_strings`) rather than changing
   `asnix_coerce_to_string` itself, which stays exactly as `toString`
   needs it. An interpolated non-string value needs an explicit
   `builtins.toString` in the source, exactly like real Nix — confirmed
   this asnix now also rejects `"${7}"`/`"${true}"`/`"${null}"`/
   `"${x: x}"` (a bare lambda) with a fatal error, matching real Nix's
   behavior (including, for the lambda case, nix-conformance's
   `eval-fail-bad-string-interpolation-1`/`-3` fixtures, which were not
   added to `tests/conformance/` since asnix has no stderr/exit-code
   comparison harness yet — only the `eval-okay-*` stdout-match shape;
   checked by hand instead). Covered by
   `tests/ir/test_strings_fixtures.c`'s `fixture_concat_rejects_raw_int`
   (via `fork()`, since the rejection path is a process-exit fatal error
   like every other "cannot coerce" site in this project, not a
   catchable exception).
12. **Nested `asnix_eval` corrupted the enclosing invocation's VALSTACK/
   RETSP bounds-check limits** (found integrating `import`/path-literals
   against the already-merged VALSTACK/RETSP fix, bug #6 above):
   `builtins.import` made nested `asnix_eval` calls possible for the
   first time (`eval_file.c`'s `asnix_eval_file` calls `asnix_eval` again,
   from inside the importing file's own still-active `asnix_eval`), but
   `dispatch.S`'s `asnix_eval` unconditionally overwrote the shared global
   `asnix_valstack_limit`/`asnix_retsp_limit` on entry and never restored
   the caller's values on the way out -- `force.S`'s own nested-forcing
   path already saved/restored these two globals around its use (see bug
   #6), but `asnix_eval` itself, written before any nested-`asnix_eval`
   scenario existed, never got the same treatment. Reproduced directly:
   every `import`-based `tests/e2e/m11_*.nix` fixture aborted with
   "value stack overflow" even on trivial imported files, because once
   the inner (imported file's) `asnix_eval` returned, the *resumed outer*
   invocation's pushes were being checked against the inner call's now-
   defunct buffer instead of its own. Fixed by giving `asnix_eval` the
   exact same save-before-overwrite/restore-on-exit treatment `force.S`
   already has, in `dispatch.S`'s prologue and `op_halt.S`'s epilogue.
13. **String-interpolating a path literal segfaulted** (found manually
   probing the interaction between the separately-landed string-
   interpolation and path-literals branches -- neither branch could have
   caught this alone, since each was built without the other): `"${./foo}"`
   crashed. Root cause: `value_util.c`'s `coerce_to_string_for_interp`
   treated *any* `ASNIX_TAG_PTR` value as an already-valid `String` and
   returned it unchanged, but `Path` (added by the path-literals work) is
   a *second*, differently-shaped `ASNIX_TAG_PTR` kind (a 16-byte
   `{marker;len;bytes[]}` header, not `String`'s 8-byte `{len;bytes[]}`) --
   `asnix_concat_strings` then read the Path's marker field as a bogus
   string length, corrupting memory. Fixed by checking
   `asnix_value_is_path` explicitly and routing a Path through
   `asnix_coerce_to_string`'s existing path-handling branch first,
   matching real Nix's rule that a path (unlike an int/bool/null) doesn't
   need an explicit `builtins.toString` in an interpolation.
14. **The path-literals branch's `<` handling unconditionally captured
   every use of `<`, including the already-merged `<` comparison
   operator** (found integrating against bug #6's operators work, which
   the path-literals branch was built without): a bare `if (c == '<')`
   block in `lexer.c`, written when `<` had no other meaning yet (only
   `<nixpkgs>`-style search-path literals), tried to lex a search path
   and `exit(1)`'d with "unrecognized character" on anything that didn't
   match -- never falling through to the two-character-operator lookahead
   below that already handles plain `<` as `TOK_LT` and `<=` as `TOK_LE`.
   Would have silently broken every comparison using `<` (either a lex
   error on `a < b`-shaped source, as above, or outright consuming
   `<...>`-shaped non-path content as a bogus search path) had it been
   merged as-is. Fixed by only consuming the `<` when the lookahead
   actually matches `<name...>`, otherwise falling through unchanged to
   ordinary operator lexing. Verified directly (`1 < 2` -> `true`, `2 < 1`
   -> `false`) after the fix.
15. **The printer never escaped special characters inside a string literal**
   (found re-screening numtide/nix-conformance after the operators/strings/
   `with`/pattern-lambda/`import` milestones landed -- see "Checked against
   numtide/nix-conformance" below): `main.c`'s `print_value_inner` used to
   `printf("\"%.*s\"", ...)` a string's raw bytes verbatim between quotes.
   For a string whose bytes contain an actual newline (e.g. `"a\<literal
   newline>b"` or `''a''\<literal newline>b''`, both of which decode to the
   3-byte sequence `a`, `\n`, `b` per the Strings milestone's own escape
   rules), this printed a raw embedded newline instead of real Nix's
   `"a\nb"` -- confirmed directly against `nix-instantiate --eval` on
   `eval-okay-backslash-newline-{1,2}`. Also affected (same root cause,
   found auditing the fix against every other special character
   `nix-instantiate` escapes): a literal `"` or `\` byte inside the string,
   and a literal `${` run (real Nix backslash-escapes the `$` specifically
   when followed by `{`, so the printed form could be pasted back into Nix
   source without accidentally starting an interpolation -- confirmed a
   bare `$` *not* followed by `{`, e.g. `"e$f"`, is left unescaped). Fixed
   by `print_string_bytes` (`main.c`), a small per-byte loop replacing the
   single `printf`; re-verified all of `tests/e2e/str_indented_*` and
   `str_interp_*` against `nix-instantiate --eval` after the fix (four
   `tests/e2e/str_indented_*.expected` fixtures had themselves been
   silently encoding the *old*, wrong unescaped-newline/dollar-brace
   behavior and needed updating to the correct escaped form once the
   printer stopped matching real Nix by coincidence).
