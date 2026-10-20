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
| M10+ | not started |

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
  `[ (f x) ]`. String literals are plain quoted runs with no escapes or
  interpolation yet.

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
  the pre-frame scope (see Known bugs #6 below — this was a real,
  independently-confirmed latent gap in the original M6 `OP_LET`, not
  just a theoretical risk introduced by this change).
- **`inherit` desugars entirely at parse time** (`src/c/parser.c`):
  `inherit x;` becomes a binding `x = x` (an `AST_VAR`), `inherit (expr)
  x y;` becomes `x = expr.x; y = expr.y` (an `AST_SELECT` per name,
  `expr`'s subtree reused — compiled, and so re-evaluated, once per
  inherited name, same as real Nix). The two forms resolve the
  right-hand side against *different* scopes, confirmed against real
  `nix-instantiate` (see Known bugs #7): `inherit (expr) ...`'s `expr`
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
6. **`let` (and `rec {}`) used as a non-tail subexpression leaked its Env
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
7. **`inherit (expr) x;`'s `expr` does *not* resolve the same way as
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
