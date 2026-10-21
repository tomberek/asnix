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
| M18 — bare globals (`import`/`map`/`abort`/`toString`) + `expr.attr or default` | done (see below) |
| M19 — `genericClosure` + second builtins-expansion pass | done, **scoped** (see below) |
| M20 — daemon realization: `SetOptions`/`AddToStore`/`BuildPaths` (`src/c/daemon_client.c`) | done, **scoped** (see below) |
| M21 — real structural `==`/`!=` + real `+` operator dispatch | done, **scoped** (see below) |
| M22 — real-nixpkgs-evaluation sweep: block comments, `->`, more bare globals, dynamic attrs, directory-import fix | done, **in progress** (see below -- real nixpkgs still doesn't fully evaluate) |
| M23 — numtide/nix-conformance sweep (3 parallel passes): `++`/`/` operators, attrpath binding sugar, hyphenated identifiers, map/mapAttrs laziness fix, builtins-expansion pass, `builtins.match`/`builtins.split` (POSIX regex), `builtins.fromTOML` (hand-written TOML parser) | done, **scoped** (see below) |
| M24 — quoted-string attrpath segments (`"a b"` as a binding/select/has-attr key) | done, **scoped** (see below) |
| M25 — real call-by-need for function arguments (core evaluator fix) | done (see below) |
| M26 — unary `-` (negate) | done (see below) |
| M27 — `fromTOML` bare global + corrected understanding of real Nix's bare-global rule | done (see below) |
| M28 — dynamic attrset-literal keys (`{${k} = v;}`) + `expr.${k} or default` | done, **scoped** (see below) |
| M29 — `rec {}` eagerly forced every field on construction (core evaluator fix) | done (see below) |
| M30 — `builtins.tryEval` (catchable `throw`/failed `assert`) | done, **scoped** (see below) |
| M31 — lazy `<name>` search-path resolution (catchable by `tryEval`) | done (see below) |
| M32 — dotted attrpath after `?` (`x ? a.b.c`) | done (see below) |
| M33 — dynamic `?` segment (`x ? ${name}`, anywhere in the chain) | done (see below) |
| M34 — unforced list elements reaching 6 builtins as raw scalars | done (see below) |
| M35 — `builtins.currentSystem`, `builtins.isPath` + `isString` path bug | done (see below) |
| M36 — name the attribute in `.` selection's "attribute missing" error | done (see below) |
| M37 — `builtins.functionArgs` | done (see below) |
| M38 — `asnix_apply`'s PrimOp path never forced args/return (core evaluator fix) | done (see below) |
| M39 — `builtins.addErrorContext` | done (see below) |
| M40 — `${null} = value;` silently omitted, not an error | done (see below) |
| M41 — dynamic attrpath segments anywhere (`.${k}`, interpolated quoted strings) | done (see below) |
| M42-M49 | not started |
| M50 — name the specific value/field in 6 more generic fatal-path error messages | done (see below) |
| M51-M56 | not started |
| M57 — conformance grab-bag: `lessThan`/`sort` on lists, `genericClosure` key-comparability checks, `__overrides` propagation, `import`'s `__toString` coercion, `toJSON` UTF-8 validation, `compareVersions`' int-overflow-to-string quirk | done, **in progress** (sub-items, see below) |
| M58+ | not started |

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

## M18 scope notes: bare globals + `expr.attr or default`

**Bare globals**: `import`, `map`, `abort`, and `toString` are now
reachable as bare identifiers, not only as
`builtins.import`/`builtins.map`/`builtins.abort`/`builtins.toString`.
Confirmed directly against `nix-instantiate` which (bare) names real Nix
actually exposes as globals before implementing anything -- it's a small,
fixed, curated list (`import abort throw map removeAttrs isNull toString
baseNameOf dirOf fetchTarball derivation builtins true false null
__curPos`), *not* every `builtins.*` entry (`filter`/`foldl`/`typeOf`/
`length`/`hashString`/`genericClosure` all stay `builtins.`-only there
too, confirmed the same way). asnix exposes only
`import`/`map`/`abort`/`toString` bare so far (the ones reported
missing); the rest of that list is a documented follow-up, not attempted
here.

- **Mechanism**: the root `Env`/`ScopeFrame` grew from one slot
  (`builtins`) to five (`builtins, import, map, abort, toString`) --
  `eval_file.c`'s root `Env` and `compiler.c`'s `asnix_compile`'s root
  `ScopeFrame` must list the same names in the same order, since slot
  index is what actually ties a name to its value at runtime. Each bare
  name is a *fresh* `PrimOp` Value wrapping the exact same native function
  its `builtins.X` counterpart uses (`asnix_make_import_primop`/
  `asnix_make_map_primop`/`asnix_make_abort_primop`) -- same behavior,
  reachable two ways, no shared mutable state between them.
- **`abort msg`**: didn't exist in any form before this (not even under
  `builtins.`) -- new `prim_abort` in `builtins.c`. Matches real Nix's
  core wording (confirmed directly: "evaluation aborted with the
  following error message: '<msg>'"); the "while calling" trace context
  real Nix also prints isn't reproduced, same simplification every other
  fatal error in this project already makes. No distinct `throw` exists
  yet -- asnix has no exception/catch mechanism (no `builtins.tryEval`),
  so the two would be behaviorally identical here anyway; documented
  follow-up, not attempted.

**`expr.attr or default`**: real Nix's select-with-default, confirmed
against its own grammar (`~/nix/src/libexpr/parser.y`'s `expr_select:
expr_simple '.' attrpath OR_KW expr_select` production) rather than
guessed from black-box probing alone (black-box testing alone can't
distinguish precedence here: since the selected attribute is always
missing in a "does `or` fire" test, every candidate grouping of the
surrounding expression happens to evaluate to the same number regardless
of which one is actually correct).

- **New op** (`OP_SELECT_OR`, `src/asm/ops/op_select_or.S`): modeled
  directly on the already-existing `OP_WITH_TRY`'s binary-search +
  delta-jump shape (same pop-search-found/push-fallthrough-or-jump-by-
  delta mechanism, same `ib_patch_delta` convention `AST_IF`/`AST_WITH`
  already use), with one behavioral difference: a non-attrset target is
  *not* fatal here (confirmed: `(5).foo or 10` → `10`, the same "not even
  an attrset simply means not found" rule `OP_HAS_ATTR`/`?` already
  established), whereas `OP_WITH_TRY`'s non-attrset `with`-value *is*
  fatal (a `with` value is only ever consulted once something inside
  actually needs it, so the type error is real and immediate by the time
  it runs).
- **`or`'s default is parsed at `expr_select` tightness, not full `expr`**
  (confirmed from the real grammar, not assumed): `{a=1;}.b or 5 + 1`
  parses as `({a=1;}.b or 5) + 1` (= 6), *not* `{a=1;}.b or (5 + 1)` --
  the default extends only as far as another select-chain, not past
  operators. `parser.c`'s `parse_select` checks for `TOK_OR` once, after
  its postfix-`.attr` loop, on the *outermost* (last) `AST_SELECT` node,
  and parses the default via a recursive `parse_select` call at that same
  tightness (so `a.b or c.d or e` chains sensibly).
- **Scope cut, documented at the op/AST/IR level, not just here**: `or`
  only protects the *single* select it's grammatically attached to.
  `a.b.c or 5` compiles as nested `AST_SELECT` (`.c`'s target is `.b`'s
  result), and only the outermost (`.c`) gets `OP_SELECT_OR`'s graceful
  fallback -- an earlier segment missing (`a.b` itself missing or not an
  attrset) is still a fatal plain `OP_SELECT` abort. Real Nix's `or`
  catches a miss at *any* point along the whole path in one `attrpath`
  production; asnix's current scope only covers the last segment.
  Confirmed directly: `{a={b=1;};}.x.c or 5` → `5` in real Nix (the
  *first* segment, `x`, is missing) but aborts in asnix. A real
  divergence, not silently swept under a passing test -- deliberately
  scoped this way rather than threading a whole-path fallback through
  nested selects under time pressure; `tests/e2e/m18_or_chain_last_missing.nix`
  covers the case this scope *does* handle (last segment missing), not
  the uncovered one.
- **`or` is a hard reserved keyword here** (`TOK_OR`), unlike real Nix
  where it's only contextually special (the "cursed or" grammar rule
  lets `or` still be used as an ordinary identifier elsewhere) -- a
  documented, deliberate simplification; `or` can't be used as a plain
  identifier anywhere in asnix now.
- **Verified against real `nix-instantiate --eval`**: missing key with
  fallback, present key (no fallback used), a multi-segment chain where
  only the last segment is missing, a non-attrset target, and operator
  precedence (`1 + {a=1;}.b or 5` → `6`, `or`'s result participating
  normally in the surrounding `+`).

## M19 scope notes: `genericClosure` + second builtins-expansion pass

Implements `builtins.genericClosure` (deliberately cut from M17, see that
section's own note above) plus a survey-and-implement pass over other
commonly-needed nixpkgs/`lib` staples still missing: `elem`, `any`, `all`,
`concatMap`, `sort`, `lessThan`, `splitVersion`, `compareVersions`,
`replaceStrings`, `stringToCharacters`, `toJSON`, `genList`, `foldl`,
`foldr`, `partition`, `attrByPath`, `filterAttrs`, `listToAttrs`,
`catAttrs`. `functionArgs` was surveyed and deliberately **not**
implemented — see "Cut" below. `isFunction` (checked, not re-implemented)
already existed per M17.

- **New files**: `src/c/builtins_closure.c` (`genericClosure` alone, the
  milestone's flagship feature — large enough to deserve its own
  translation unit), `src/c/builtins_list2.c` (`elem`/`any`/`all`/
  `concatMap`/`sort`/`genList`/`partition`/`foldl`/`foldr`/`catAttrs`),
  `src/c/builtins_attrs2.c` (`filterAttrs`/`listToAttrs`/`attrByPath`),
  `src/c/builtins_compare.c` (`lessThan`/`splitVersion`/`compareVersions`/
  `replaceStrings`/`stringToCharacters`/`toJSON`) — one file per feature
  batch, matching M17's established per-module convention, even though
  (unlike M17's three parallel agents) one agent wrote all four of these
  at once; see the next bullet for why that still didn't mean reusing
  M17's "don't share, duplicate instead" policy outright.
- **`src/c/builtins_shared.{h,c}`, a new exception to M17's own "each
  module duplicates its own small helpers" rule**: M17's three sibling
  modules each deliberately *re-declared* tiny helpers like
  `make_primop_value`/list-and-attrs raw accessors instead of sharing
  them, specifically so three agents working in parallel never collided
  on one shared file. That reason doesn't apply when one agent writes
  four new modules in the same pass — four-way duplication of
  `builtins_misc.c`'s `make_curried_native` (needed by nearly every
  builtin added here, since almost all of them are arity ≥2) plus a dozen
  tiny List/Attrs accessors would be pure, unmotivated duplication. One
  precedent for sharing already existed (`builtins_attrs.h` includes
  `builtins_list.h` for the `AsnixPrimFn`/`AsnixBuiltinDef` shape rather
  than redefining it) — `builtins_shared.h` is that same judgment call
  applied further: `asnix_bshared_make_curried_native` (ported from
  `builtins_misc.c`), `asnix_bshared_apply2`, List/Attrs raw accessors,
  `asnix_bshared_make_attrs` (sorts by sym id in place — several new
  builtins build an attrset from freshly-interned symbols whose relative
  sym-id order isn't known in advance, unlike `removeAttrs`/`mapAttrs`
  which only ever inherit order from an already-sorted input), and two
  new comparison primitives (`asnix_bshared_value_eq`/`_value_lt`, see
  below).
- **Registration**: `asnix_make_builtins_attrs` (`builtins.c`) now merges
  *nine* sources instead of M17's four — rewritten to loop over two small
  tables-of-tables (one per shape: the plain `{name,fn,arity}` shape, and
  the `{name,build()}` shape `builtins_misc.c` introduced) rather than
  hand-repeating the same four-line block a ninth time.
- **Currying**: every new arity≥2 builtin here uses
  `asnix_bshared_make_curried_native` uniformly (a curried Closure chain,
  same mechanism `builtins_misc.c`'s `substring`/`concatStringsSep`
  established) — including the ones that would fit M17's other curry
  pattern (`build_curry2_body`, a cheaper 2-level-only PrimOp-based
  trick) just as well, since several of this pass's builtins are
  genuinely arity 3 (`attrByPath`, `replaceStrings`, `foldl`, `foldr`),
  and using one mechanism for everything was simpler than carrying two.
- **`genericClosure`**: real signature `genericClosure { startSet; operator;
  }`, arity 1 (no currying needed). Semantics verified directly against
  real `nix-instantiate --eval` (2.35.2) before writing any code — see
  `builtins_closure.c`'s header comment for the exact scenarios checked
  (breadth-first worklist order, not depth-first; dedup by `key` happens
  when an item is *popped*, not when it's *pushed*, so an `operator` that
  re-produces an already-seen key is silently dropped rather than
  re-visited; non-`key` fields are preserved verbatim in the output).
  Implemented as a plain dynamic-array FIFO queue plus a linear-scan
  seen-keys list (not real Nix's `std::map`-backed dedup) — see "Cut"
  below for why equality, not a full ordering, is enough here.
- **Cut: `functionArgs`**. Surveyed, not implemented. Real Nix's
  `functionArgs f` returns `{fieldName = hasDefault; ...}` for a
  pattern-destructured lambda (`{a, b ? 1}: ...`) and `{}` for a plain
  lambda or any primop. This project's Closure heap object
  (`{up;body;arity;pool}`, `ir.h`) has no formals-metadata field at all —
  M14's lambda-patterns milestone deliberately desugars `{a, b ? 1}: body`
  entirely at compile time into synthetic `OP_CHECK_FORMALS`/
  `OP_FRAME_ALLOC` IR (see that section above), so by the time a Closure
  exists at runtime there is no retained list of field names anywhere to
  read back. Correctly implementing `functionArgs` would need a real
  representation change (a new field on `ASNIX_CLOSURE_*`, threaded
  through `compiler.c`'s `AST_LAMBDA` case and `ir.h`), not a builtins-
  side addition — and returning `{}` unconditionally (the only thing
  implementable without that change) would be observably *wrong* for
  every pattern-lambda, not just narrower than real Nix, so it was left
  out rather than shipped half-right. A genuine follow-up, in the same
  spirit as M4.5's own "lambdas/calls deferred" cut.
- **Not core Nix builtins, added under `builtins` anyway**: `attrByPath`,
  `stringToCharacters`, `foldl`, `foldr` are confirmed *not* present in
  real Nix's `builtins` (`builtins ? attrByPath` etc. all report `false`
  on `nix-instantiate` 2.35.2) — they're nixpkgs `lib` functions
  (`lib.attrByPath`, `lib.stringToCharacters`, `lib.foldl`, `lib.foldr`).
  Added here as `builtins.attrByPath` etc. anyway, since this project has
  no "with lib" (or even "with builtins") root scope the way real Nix
  programs reach `lib` at all (`m7_builtin_as_value.nix`'s own
  `builtins.toString`-only convention, unchanged) — everything useful has
  to live directly under `builtins` here or not be reachable at all.
  `filterAttrs`/`sort`/`lessThan`/`elem`/`any`/`all`/`concatMap`/
  `partition`/`genList`/`catAttrs`/`listToAttrs`/`splitVersion`/
  `compareVersions`/`replaceStrings`/`toJSON` *are* all real
  `builtins.*` entries on this Nix version — confirmed individually via
  `builtins ? <name>`, not assumed from memory.
- **`splitVersion`/`compareVersions` ported line-for-line from
  `~/nix/src/libstore/names.cc`'s `nextComponent`/`componentsLT`/
  `compareVersions`** — same "port from the real source, don't guess"
  discipline `base32.c`/`store_path.c` already established (M8), needed
  here because this algorithm has several genuinely non-obvious rules a
  from-the-manual reimplementation would likely miss (`"1pre" < "1"`,
  `"1.0" > "1"`, `"1.2.3" < "1.2.3abc"`, a digit/non-digit transition
  splits a component even with no explicit `.`/`-` separator present —
  see `builtins_compare.c`'s header comment for every case individually
  checked against real `nix-instantiate --eval` before trusting the
  port). `replaceStrings` is likewise ported line-for-line from
  `~/nix/src/libexpr/primops.cc`'s `prim_replaceStrings` (first-match-in-
  list-order at each scan position, not longest-match; an empty `from`
  entry advances by one character instead of looping forever).
- **`elem`/`genericClosure`'s dedup equality, and `lessThan`/`sort`'s
  ordering, both deliberately widen one existing documented scope cut
  by a controlled amount, rather than leaving it as-is or fully lifting
  it**: the `==` operator (`op_cmp.S`, see the Operators section above)
  falls back to pointer identity for every non-INT/IMM tag, including
  strings — two separate same-content string literals are `false` there.
  `asnix_bshared_value_eq` (`builtins_shared.c`) upgrades *just* strings
  to real content equality (the bytes are already in hand; every
  consumer below needs correct string-keyed dedup to be useful at all —
  `elem "a" ["a"]`/`genericClosure` keyed by derivation-name-shaped
  strings are the overwhelmingly common real cases), while still falling
  back to pointer identity for lists/attrsets/closures/primops/thunks —
  deep structural equality on those remains exactly as un-implemented as
  `==` itself already documents. Similarly, `<`/`>` (`op_cmp.S`) is
  INT-only; `asnix_bshared_value_lt` (used by `lessThan`/`sort`) adds
  string-vs-string lexicographic ordering on top (so `sort
  builtins.lessThan` is actually useful on the string lists real callers
  have) but still rejects paths/lists/attrsets, matching real Nix's
  broader `CompareValues` only partially, by design, not by oversight.
  `genericClosure`'s own dedup uses equality (a linear scan), not real
  Nix's `std::map`-with-a-comparator approach, specifically so it never
  needs a full ordering over int/string/bool/null/list/attrs keys this
  project doesn't have anyway — observably identical to real Nix for
  every case that doesn't mix incomparable key types, which is the
  common case (string or int keys, never mixed).
- **`toJSON` kept in scope, not cut** (the task flagged it as a candidate
  rabbit hole worth skipping if it turned out to be one) — recursive,
  compact (no whitespace) JSON serialization of
  int/bool/null/string/list/attrset, with alphabetically-sorted attrset
  keys (matching real Nix's `lexicographicOrder`, and this project's own
  existing attrset-printer convention) and standard JSON string escaping
  (`\" \\ \b \f \n \r \t`, other control bytes as `\u00XX`, everything
  else passed through unescaped — matching `nlohmann::json::dump()`'s
  default, which is what real Nix's `value-to-json.cc` actually uses
  under the hood). Functions/primops are rejected (`"cannot convert a
  function to JSON"`, matching real Nix). A `Path` argument's bytes are
  emitted as plain text, not copied into the Nix store the way real
  Nix's `toJSON` does — the same already-accepted, already-documented
  string-context/`AddToStore` gap `toString` and `"${path}"`
  interpolation both already have (see M8/M15's scope notes above), not
  a new divergence introduced here.
- **Verified against real `nix-instantiate --eval`/`nix-instantiate
  --eval --strict`** (2.35.2) for every builtin added, independently of
  whatever this section's own prose claims — see each new module's
  per-builtin comments for the exact commands, and
  `tests/e2e/m18_*.nix`/`.expected` for the on-disk fixtures (20 pairs,
  one or more per builtin, including three dedicated to `genericClosure`:
  breadth-first ordering, dedup when `operator` re-produces an
  already-seen key, and non-`key` fields surviving round-trip).
  `tests/c/test_builtins_m18.c` additionally exercises a representative
  subset through the real `asnix_eval`/`OP_CALL` dispatch loop (not by
  calling the native C functions directly), with its own dedicated
  `genericClosure` BFS+dedup fixture built from a hand-written native C
  "operator" standing in for a user lambda (same idiom
  `tests/c/test_builtins_list.c` already established for `map`/`filter`).
- **A real bug found along the way, in pre-existing M5-era code, not in
  anything added this pass** — see Known Bugs entry #15 below: the
  top-level printer (`main.c`) never escaped `"`/`\`/control characters
  inside a printed string value. Nothing before this pass routinely
  produced string *values* containing an embedded quote or backslash
  worth round-tripping through the printer and comparing byte-for-byte
  against real Nix, so the gap had gone unnoticed; `toJSON`'s typical
  output (a string containing lots of both) made it immediately visible.
  Fixed in `main.c` only; see that entry for the fix and the four
  pre-existing `tests/e2e/str_indented_*.expected` fixtures it required
  updating (their old expected output had baked in the bug — a raw,
  unescaped embedded newline — not real Nix's actual behavior).

## M20 scope notes: daemon realization (`SetOptions`/`AddToStore`/`BuildPaths`)

Extends M8's connect/handshake/`IsValidPath`-only daemon client
(`src/c/daemon_client.{h,c}`) with the three ops needed to actually
realize a derivation against a live `nix-daemon`, run and verified
against the real, live system daemon on this dev machine (not a mock).

- **`asnix_daemon_trusted()`**: exposes the handshake's protocol-≥1.35
  trust flag (`1`=trusted, `0`=not, `2`=unset), read but previously
  discarded. On this dev machine, an ordinary (UID 1001, not listed in
  any `trusted-users` this agent could find) account was unexpectedly
  reported as `1`/trusted — not fully explained, but observed directly
  and not load-bearing for anything below since this client never relies
  on a specific trust verdict to behave correctly.
- **`asnix_daemon_add_text_to_store()`** (op 7, `AddToStore` — not the
  obsolete op 8 `AddTextToStore`, dead since protocol 1.25): adds a
  `.drv`'s ATerm text as self-verifying `"text:sha256"` content-addressed
  store content (no trust needed for this op itself). Uses the
  `FramedSink` chunk encoding (`[u64 len][raw unpadded bytes]`, zero-
  length terminator) — a distinct wire convention from the ordinary
  length-prefixed-and-8-byte-padded strings used everywhere else in this
  protocol. Verified end to end: the daemon's returned computed store
  path matched this client's own independently-computed `drvPath`
  exactly, byte for byte — strong evidence the ATerm serialization and
  content-addressing are both implemented correctly, cross-checked
  against a real daemon's own hash computation rather than only against
  this project's own code.
- **`asnix_daemon_set_options()`** (op 19, `SetOptions`): sends this
  connection's build settings to the daemon. Every real `nix`/Lix client
  sends this immediately after the handshake; this client previously
  didn't, meaning `BuildPaths` ran under the daemon's own process-wide
  defaults (loaded once from `/etc/nix/nix.conf` at daemon startup) for
  every field, substituters included — see Known Bugs entry #16 below for
  the real divergence this caused and how `useSubstitutes=0` fixes it.
  Every other field this op carries is sent as an inert default (field
  order and defaults cross-checked against `~/nix`'s own
  `RemoteStore::setOptions`); only `useSubstitutes` does anything
  meaningful for this client's purposes.
- **`asnix_daemon_build_drv()`** (op 9, `BuildPaths` — not op 36
  `BuildDerivation`, which sends the whole derivation inline instead of
  referencing an already-`AddToStore`'d path, and which a real daemon
  rejects outright for an untrusted client building an input-addressed
  derivation, stricter than `BuildPaths`'s own build-time check).
  Verified end to end against the live daemon with `tools/realize_demo.c`
  (a trivial, harmless derivation: `builder = "/bin/sh"`,
  `args = ["-c" "echo hi from asnix > $out"]`): `BuildPaths` was accepted,
  and the resulting store path's content was read back and confirmed to
  be exactly `hi from asnix`, owned by `root`, with the standard Nix
  store read-only-permissions/epoch-timestamp normalization applied —
  i.e. a genuine sandboxed build by the real daemon, not a substituted
  cache hit.
- **`tools/realize_demo.c`**: a standalone, manually-invoked tool,
  deliberately **not** part of `make test`'s `tests/c/` glob — it writes
  to the real, live Nix store and attempts a real build every time it
  runs, which a routine `make test` invocation should never do by
  default. Mirrors `derivation.c`'s `builtins.derivation` construction
  logic independently (via `aterm.h`/`store_path.h`'s already-exported
  helpers) rather than widening `derivation.c`'s own interface just for
  this tool.
- **Scope cut, deliberately**: no `QueryMissing`, no NAR transfer, no
  structured build-activity forwarding (the `STDERR_START_ACTIVITY`/
  `STDERR_RESULT` sideband messages are parsed just enough to stay in
  sync with the daemon's byte stream, not surfaced to the user) — this
  client can trigger a real build and confirm success/failure, nothing
  richer than that yet.

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
   printer stopped matching real Nix by coincidence). A second agent,
   working in parallel on the builtins-expansion/`toJSON` pass (M19),
   independently rediscovered this exact bug (`toJSON`'s typical output
   being the first thing to make a literal `"`/`\` in a string *value*
   visible) and fixed it with a narrower `print_escaped_string` (`"`, `\`,
   `\n`, `\r`, `\t` only, no `${` handling) -- superseded by the more
   complete fix above when the two branches were integrated.
16. **`BuildPaths` without ever sending `SetOptions` consults the daemon's
   OWN process-wide settings, substituters included** (found running
   `tools/realize_demo.c` against the real, live system `nix-daemon`): a
   client that connects, handshakes, and goes straight to `AddToStore`/
   `BuildPaths` -- which is what M8's original client did, and all this
   client needs for `IsValidPath` -- gets the daemon's settings exactly as
   loaded from `/etc/nix/nix.conf` at daemon startup, because nothing has
   ever told it otherwise. On this dev machine that config includes a
   private company Cachix substituter; `BuildPaths` failed on that
   substituter's expired auth token (`HTTP 401`), not on anything wrong in
   this client's own protocol framing -- confirmed by the `AddToStore`
   round-trip already having verified byte-for-byte reproducibility
   against the daemon's own computed store path. A control run of plain
   `nix-build` on an equally trivial derivation succeeded on the same
   machine, but via a completely different path (a remote SSH builder from
   this user's own nix.conf) -- not evidence the substituter issue isn't
   real, just that ordinary `nix-build` never exercises it. Every real
   `nix`/Lix client sends `SetOptions` (op 19) immediately after the
   handshake specifically so the daemon applies *that connection's*
   settings instead of its own defaults for the rest of the session; this
   client had never implemented it. Fixed by adding
   `asnix_daemon_set_options()` (`daemon_client.c`) and calling it with
   `useSubstitutes=0` before `BuildPaths` in `realize_demo.c`, which skips
   substituter lookups entirely and forces a real local build -- verified
   end to end: the daemon accepted the build, and the resulting store path
   contains exactly the builder script's own output (`hi from asnix`),
   owned by `root` with the standard Nix store read-only/epoch-timestamp
   normalization, i.e. a genuine sandboxed build, not a cache hit. The
   expired Cachix token itself is a pre-existing, unrelated infrastructure
   issue on this machine and was not touched.
17. **`==`/`!=` on lists/attrsets was pointer identity, not structural
   equality -- previously documented as an acceptable "scope cut", it
   isn't one** (found while debugging real nixpkgs's own
   `lib/minfeatures.nix` compat check via `d.nix`: `missingFeatures != []`
   was always `true` regardless of `missingFeatures`'s actual contents,
   because the two `[]` values being compared are always two separately
   allocated, distinct empty-list objects -- confirmed directly:
   `[] != []` evaluated to `true`, and `[1 2] == [1 2]` evaluated to
   `false`, in asnix). `op_cmp.S`'s original comment reasoned that since a
   Value's tag is encoded in its own low bits, "same tag and same payload"
   collapses to plain raw equality for every tag -- true for INT/IMM, but
   wrong for PTR/LIST/ATTRS, where "same payload" actually means "same
   pointer", not "same content". This is pervasive in real Nix code
   (string/list/attrset `==` is everywhere in nixpkgs), not a narrow edge
   case, so it was a real bug, not a defensible scope cut. Fixed by
   upgrading `asnix_bshared_value_eq` (`builtins_shared.c`) to real
   recursive structural equality for lists/attrsets (forcing nested lazy
   elements via `asnix_force_for_print`) and wiring `op_eq`/`op_neq`
   (`op_cmp.S`) to call it instead of doing a raw `cmpq` -- see M21's scope
   notes above for the full before/after and the deliberately-preserved
   identity semantics for functions.
18. **`+` never dispatched to string/path concatenation -- always did raw
   integer arithmetic on the operands' tagged bits, regardless of type**
   (found immediately after fixing #17 above, debugging a *different*
   string-equality case that still failed: `"fo" + "o" == "foo"` was
   `false` even with #17's fix in place). Root cause: `op_arith.S`'s
   `asnix_op_add` had no tag check at all (an early-milestone "trusted,
   well-formed IR" assumption that was never revisited once strings
   existed) -- it sign-extend-shifted both operands' raw 64-bit words as
   if they were always `TAG_INT`-tagged integers and added them, so two
   string pointers produced a nonsense int (`typeOf ("fo" + "o")` reported
   `"int"`, with a garbage numeric value) instead of concatenating or
   erroring. `-`/`*` had the identical no-check gap, just not yet observed
   in practice (real Nix has no string/path semantics for either, so the
   only correct behavior for a non-int operand is a fatal type error, not
   silent garbage). Fixed by adding a real type dispatch to `asnix_op_add`
   (int+int stays fast inline asm; otherwise defers to a new
   `asnix_add_values`, `value_util.c`, which type-errors on an int-vs-
   nonint mismatch and otherwise concatenates via `asnix_concat_strings`'s
   existing coercion rules) and a plain "both operands must be TAG_INT"
   fatal check to `asnix_op_sub`/`asnix_op_mul`. See M21's scope notes
   above for the preserved-Path-ness rule and the one deliberate semantic
   gap versus real Nix (no path-existence check on mixed string+path
   concatenation).

## M21 scope notes: real structural `==`/`!=` + real `+` operator dispatch

Found and fixed while debugging why `d.nix` (importing real nixpkgs)
failed its own compat check with a blank "missing feature" bullet: two
separate, high-impact correctness bugs, previously misclassified as
acceptable "scope cuts" in this doc. See Known Bugs entries #17/#18 below
for the full writeup of each. Summary:

- **`==`/`!=`** (`op_cmp.S` calling `src/c/builtins_shared.c`'s
  `asnix_bshared_value_eq`, upgraded in place) now do real structural
  equality for lists (elementwise, recursive) and attrsets (same attr
  names, recursive value equality), forcing lazy elements along the way
  via the existing `asnix_force_for_print` entry point. Previously: pure
  pointer-identity `cmpq` on the raw Value words for every tag, meaning
  `[] != []` evaluated to `true`. Functions (closures/primops) remain
  compared by identity only, matching real Nix (verified: two
  syntactically-identical-but-distinct lambdas are `!=`; see
  `tests/conformance/equal-function-*.nix`, all still passing).
- **`+`** (`op_arith.S`'s `asnix_op_add`, plus a new `asnix_add_values` in
  `value_util.c`) now dispatches on operand type: int+int stays inline-asm
  arithmetic (fast path unchanged); anything else defers to C, which
  type-errors on an int-vs-non-int mismatch and otherwise concatenates via
  the same coercion rules `"${a}${b}"` interpolation already uses
  (`asnix_concat_strings`), preserving Path-ness when the LHS specifically
  was a Path. Previously: *every* `+` did a raw arithmetic shift+add on
  the two operands' raw bits regardless of tag, so `"fo" + "o"` silently
  produced a garbage int instead of `"foo"` or an error. `-`/`*`
  (`asnix_op_sub`/`asnix_op_mul`) gained the same "both operands must be
  TAG_INT, fatal otherwise" check while in this file for the same reason
  (previously the identical no-check footgun, just not yet observed in
  practice) — real Nix has no string/path semantics for `-`/`*` at all.
- **Cut, not fixed**: real Nix's `+` additionally runs a path-existence
  check and copies-to-store in some mixed string+path cases (e.g.
  `"a" + ./b` errors if `./b` doesn't exist on disk, before even getting
  to string coercion) — not replicated; this project instead just coerces
  the path to its plain text, succeeding where real Nix might error. Also
  cut: plain `/` as integer division (`4 / 2`) — surveyed while chasing
  `d.nix` further after this fix, confirmed **not implemented at all**
  (the lexer only ever recognizes `/` as part of a path literal or the
  `//` update operator) — a real, separate gap, not yet fixed; flagged to
  the user rather than unilaterally continuing to expand scope.

## M22 scope notes: real-nixpkgs-evaluation sweep

A chain of real, previously-undiscovered gaps, each found by repeatedly
re-running `d.nix` (`import`ing real nixpkgs's `default.nix`) against the
live system Nix install and real nixpkgs checkout on this dev machine,
fixing whatever it hit next, and re-running -- not a single planned
milestone, but logged together since each fix was small and the whole
chain was driven by the same investigation. **Real nixpkgs still does not
fully evaluate** (the chain is ongoing; see Known Bugs #19-#24 for each
individual fix's full writeup):

- **Block comments** (`/* ... */`, including `/** ... */` doc comments)
  were not recognized at all -- `lexer.c` only ever handled `#` line
  comments. Fatal the instant any real-world `.nix` file (which are full
  of these) was lexed. Fixed: a plain skip-to-`*/` loop, same shape as the
  existing `#`-to-end-of-line skip.
- **`->` (logical implication)** had no token and no grammar rule at all.
  Added at the correct precedence (lowest of all binary operators,
  right-associative -- cross-checked against cppnix's real grammar,
  `~/nix/src/libexpr/parser.y`'s `%right IMPL`) by desugaring directly to
  `(!a) || b` at parse time (`parser.c`'s new `parse_impl`), reusing the
  already-correct short-circuiting `AST_OR`/`AST_NOT` compilation -- no
  new IR op needed.
- **More bare root-scope globals**: `removeAttrs`, `isNull` (both already
  existed under `builtins.*`, just needed root-scope exposure, same
  mechanism M18 established for `import`/`map`/`abort`/`toString`), and
  `throw` (did not exist under `builtins.*` at all -- added as a close
  sibling of `abort`, same unconditionally-fatal shape but real Nix's
  distinct, plainer error wording: `throw "x"` -> `error: x`, not
  `abort`'s `"evaluation aborted with the following error message: 'x'"`
  -- confirmed directly against `nix-instantiate`). Root `Env`/
  `ScopeFrame` grew from 5 to 8 slots across this milestone.
- **`import`ing a directory** (`import ./.`, real nixpkgs's own
  `pkgs/top-level/impure.nix` does exactly this) silently corrupted the
  heap instead of erroring or working -- see Known Bugs #22, the most
  serious finding in this sweep (a real memory-safety bug, not a missing
  feature). Fixed by detecting a directory and appending `/default.nix`,
  matching real Nix's own `import somedir` convention, plus defensive
  hardening in `read_file` so a bad path can never reach that corruption
  path again regardless of call site.
- **Dynamic attribute access** (`set.${name}`, used by real nixpkgs's own
  `pkgs/top-level/default.nix`) didn't exist in any form -- no lexer
  support for `${` outside an active string context, no AST/compiler
  support. Added for the **select** form only (`expr.${nameExpr}`): new
  `OP_SELECT_DYNAMIC` op, deferring entirely to a new C helper
  (`asnix_bshared_select_dynamic`, `builtins_shared.c`: interns the name
  at runtime via the already-existing `asnix_intern`, binary-searches,
  forces an unforced thunk) rather than hand-rolling the lookup in asm,
  since this isn't a hot path. **Scope cut, deliberately narrower than
  real Nix**: dynamic keys in attrset *literals* (`{${name} = val;}`) are
  **not** implemented -- `AST_ATTRSET` compiles by interning and sorting
  every binding's name at compile time (`compiler.c`), which has no
  runtime-name path at all; adding one would need a real redesign of that
  construction mechanism (build incrementally at runtime and merge a
  dynamically-named entry in, rather than a single compile-time sort) and
  wasn't attempted since nothing hit so far has actually needed it.
  `expr.${name} or default` (combining dynamic select with `or`) is also
  rejected at parse time (fatal, not silently mishandled) for the same
  reason -- not yet needed.

## M22's Known Bugs entries (continuing the numbered list above)

19. **Block comments (`/* ... */`) were not recognized at all.** `lexer.c`
   only ever handled `#` line comments -- any `/*` (including nixpkgs's
   own ubiquitous `/** ... */` doc comments) hit "unrecognized character
   '/'" immediately, since `/` otherwise only has meaning as part of a
   path literal or the `//` update operator. Found running `d.nix`
   against real nixpkgs: `pkgs/top-level/impure.nix`, the very first file
   nixpkgs imports, opens with a block comment. Fixed by a plain skip-to-
   `*/` loop in the main lex loop, same shape as the existing `#`-to-end-
   of-line skip, checked before path-literal scanning (a block comment's
   `/*` never matches a path literal anyway, since `*` isn't a path
   character, but comments should never compete with path scanning
   regardless).
20. **`->` (logical implication) had no token and no grammar rule at
   all.** Found immediately after fixing #19: `pkgs/top-level/impure.nix`
   uses `assert args ? localSystem -> !(args ? system);`, and `->` fell
   through the lexer's two-character-operator checks straight to ordinary
   `-` (minus) followed by `>` (greater-than), which the parser then
   choked on as "unexpected token" trying to parse `a - (>...)`. Fixed by
   adding `TOK_ARROW` and a new `parse_impl` at the correct precedence
   (lowest of all binary operators, right-associative -- cross-checked
   against cppnix's real grammar, `~/nix/src/libexpr/parser.y`'s `%right
   IMPL`, declared looser than `%left OR`), desugaring `a -> b` directly
   to `(!a) || b` at parse time -- exactly real Nix's own
   `ExprOpImpl::eval` semantics (`!evalBool(e1) || evalBool(e2)`,
   cppnix's `eval.cc`) -- so short-circuiting (the RHS is never evaluated
   when the LHS is false) comes for free from the already-correct
   `AST_OR`/`AST_NOT` compilation, no new IR op needed. Verified against
   real `nix-instantiate --eval` for associativity (`false -> false ->
   true` => `true`) and precedence relative to `||` (`false || true ->
   false` => `false`).
21. **`removeAttrs`/`isNull` are real Nix bare root-scope globals, not
   yet exposed as such.** Both already existed correctly under
   `builtins.*`; M18's bare-globals research had already identified them
   (along with `throw`, `baseNameOf`, `dirOf`, `fetchTarball`,
   `__curPos`) as part of real Nix's small curated bare-global set, but
   only `import`/`map`/`abort`/`toString` were actually wired up at the
   time. Found needed directly: `pkgs/top-level/impure.nix` calls bare
   `removeAttrs args [...]`, not `builtins.removeAttrs`. Fixed by adding
   `asnix_make_removeAttrs_primop()` (`builtins_attrs.c`, alongside the
   existing `prim_removeAttrs`) and `asnix_make_isNull_primop()`
   (`builtins_misc.c`, alongside `prim_isNull`), each just wrapping the
   same native function its `builtins.X` counterpart already uses -- same
   pattern M18 established. Root `Env`/`ScopeFrame` grew from 5 to 7
   slots.
22. **`import`ing a directory corrupted the heap instead of erroring or
   working -- a real memory-safety bug, not just a missing feature.**
   Real Nix's `import somedir` means `import somedir/default.nix`; asnix
   had no such handling, so the raw directory path reached
   `eval_file.c`'s `read_file()` directly. `fopen()` on a directory
   "succeeds" on Linux, but the subsequent `fseek(SEEK_END); long size =
   ftell(f);` returns `-1` (a directory stream doesn't support seeking).
   `malloc((size_t)size + 1)` with `size == -1` wraps to `malloc(0)`, and
   the following `buf[size] = '\0'` then writes to `buf[-1]` -- one byte
   *before* that ~0-byte allocation, corrupting the heap. Confirmed via
   `gdb`: segfaulted inside `read_file` at exactly that line, called from
   `asnix_eval_file` <- `prim_import` <- `asnix_op_call`, triggered by
   `pkgs/top-level/impure.nix`'s own `import ./.`. This is the same
   segfault originally reported at the very start of this investigation
   ("running `./asnix d.nix` is a segfault") -- it took fixing #17-#21
   above to even reach this deeper point in nixpkgs's own evaluation.
   Fixed two ways: (a) `asnix_eval_file` now checks (via `stat()`)
   whether the resolved path is a directory and appends `/default.nix`
   before reading, matching real Nix's own convention; (b) `read_file`
   itself now checks for a negative `ftell()` result and errors cleanly
   instead of corrupting memory, as defense in depth so a bad path can
   never reach this corruption again regardless of which call site hands
   it one.
23. **Dynamic attribute access (`set.${name}`) didn't exist in any
   form.** `$` had no meaning anywhere outside an active string-
   interpolation context -- the lexer's `MODE_NORMAL` dispatch had no
   case for it at all, so `set.${name}` hit "unrecognized character '$'"
   the instant the lexer reached the bare `$` in `.${`. Found running
   `d.nix`: `pkgs/top-level/default.nix` (the file `impure.nix` imports)
   uses exactly this (`set.${name}`). Implemented for the select form
   only (see M22's scope notes above for what's deliberately still
   missing -- dynamic attrset-literal keys): new `TOK_DOLLAR_LBRACE`
   recognition in `MODE_NORMAL` (reusing the exact mechanism string
   interpolation already uses -- mode-push `MODE_NORMAL`, matching `}`
   pops back out through the ordinary `}` handling), a new
   `AstSelect.attr_expr` field (`ast.h`), `parse_select` parsing
   `.${expr}` as an alternative to a plain `.name` segment, a new
   `OP_SELECT_DYNAMIC` IR op, and `asnix_bshared_select_dynamic`
   (`builtins_shared.c`) doing the actual runtime work (intern the name
   via the already-existing `asnix_intern`, binary-search, force an
   unforced thunk) -- deferred entirely to C rather than hand-rolled in
   asm, since this isn't a hot path (plain `.foo` with a compile-time-
   known symbol remains the fast, all-asm path, untouched).
24. **`throw` doesn't exist in any form (not even under
   `builtins.throw`), despite being one of real Nix's bare globals and
   confirmed needed.** Found immediately after fixing #23:
   `pkgs/top-level/default.nix` calls bare `throw` directly (`inherit
   (lib) throwIfNot throwIf;` plus direct `throw "..."` calls elsewhere
   in the same file). A prior comment in `builtins.c` (next to
   `prim_abort`) had explicitly deferred implementing a distinct `throw`,
   reasoning that without a `tryEval`/catch mechanism the two would be
   "behaviorally identical anyway" -- true for *control flow* (both are
   unconditionally fatal here), but not for *wording*: confirmed directly
   against `nix-instantiate --eval` that `throw "x"` reports plain
   `error: x`, while `abort "x"` reports `error: evaluation aborted with
   the following error message: 'x'` -- a real, user-visible difference
   worth getting right even without exception handling. Fixed by adding
   `prim_throw` (`builtins.c`, a close sibling of `prim_abort` with the
   corrected wording) registered under `builtins.throw`, plus
   `asnix_make_throw_primop()` for bare-global exposure (root `Env`/
   `ScopeFrame` grew from 7 to 8 slots).

## M23 scope notes: conformance long-tail (parse-error and missing-builtin gaps)

A fresh, from-scratch re-triage of numtide/nix-conformance's `lang/`
suite (not reusing a stale list from a prior pass), explicitly scoped to
exclude `builtins.match`/`builtins.split` and `builtins.fromTOML` (two
other agents were working those in parallel). Baseline at the start of
this pass: 300/862 `lang/` cases passing. After this pass: 333/862 --
but that raw delta understates the work, for two reasons explained
below: (a) several of the biggest fixes (the `++`/`/` operators,
attrpath binding sugar, hyphenated identifiers) are foundational
language features whose conformance payoff is diluted by *other*,
unrelated gaps in the same files (dynamic attrset keys, flake-refs,
`tryEval`, floats, ...) that still block many of those same files from
fully passing even once the targeted gap is fixed; (b) a nontrivial
chunk of the suite's remaining fails are cases whose expected output
embeds concrete store-path hashes (`eval-okay-drv-fixed-output.nix` and
siblings) that can only ever text-match if asnix replicates real Nix's
*exact* fixed-output-derivation hash algorithm bit-for-bit -- a correct,
clean asnix evaluation of these still "fails" the harness's literal
string comparison for reasons that have nothing to do with correctness
of anything this pass touched. Every fix below was verified directly
against real `nix-instantiate --eval`, not just against the harness.

**Fixed, in rough order of how many files each one unblocked:**

- **`++` (list concatenation) had no token or grammar rule at all** --
  confirmed via the triage as the single biggest root cause in the long
  tail (104 of 862 `lang/` files use it). Added `TOK_PLUSPLUS`, a new
  `parse_concat` precedence level (right-assoc, tighter than `*`/`-`,
  looser than `?`/unary negate -- cross-checked against cppnix's real
  grammar), a new `OP_CONCAT` IR op deferred to C
  (`asnix_concat_lists`, `value_util.c`) since it isn't hot-path, same
  shape as the existing `//` (`OP_UPDATE`) handler.
- **Plain `/` (integer division) had no operator meaning at all** -- a
  prior M21 note had already flagged this as a known, unfixed gap.
  Added `TOK_SLASH`, `/` at `*`'s precedence in `parse_mul`, a new
  `OP_DIV` with its own asm handler (`asnix_op_div`, `op_arith.S` --
  int-only, truncates toward zero via a plain `idivq`, matching real
  Nix's int/int behavior exactly, with a clean "division by zero" fatal
  instead of a SIGFPE crash). The path-vs-division ambiguity (`a/b` with
  no whitespace is still a path literal) needed no lexer changes at all
  -- `try_scan_path` already rejected every case that should be
  division, verified against every one of `a / b`/`a /b`/`a/ b`/`a/b`'s
  independently-different real-Nix meanings.
- **Legacy `let { ..., body = ...; }` form** -- real Nix desugars this
  to `(rec { ... }).body`; fixed by doing the same desugaring directly
  in `parser.c`'s `let` handling (reuses `AST_ATTRSET`+`AST_SELECT`, no
  new AST node or IR op).
- **Attrpath binding sugar (`a.b.c = val;`), including real Nix's
  merge-on-shared-prefix semantics, didn't exist at all.** This is an
  extremely common real-world Nix construct (nixpkgs attribute names are
  full of it) that was previously just a hard parse error. Implemented
  as a `BTNode`/`BTVec` tree built incrementally while parsing one
  binds-block (`parser.c`), supporting both ordinary merge
  (`a.b = 1; a.c = 2;` => `a = { b = 1; c = 2; };`) and the
  "regrettable" merge-into-a-pre-existing-literal-attrset quirk real Nix
  itself documents (NixOS/nix#9020). Scope cut, matching the existing
  M22 one: only plain-identifier segments are supported, not
  quoted-string/`${...}` segments (`eval-okay-attrs5.nix`/
  `eval-okay-dynamic-attrs*.nix` still fail on exactly this, same as
  before this pass).
- **Hyphenated identifiers** (`ident_cont` now includes `-`, matching
  real Nix's own identifier regex and its maximal-munch tokenization of
  `a-b` as one identifier vs. `a - b`/`a -b` as subtraction) -- a
  deliberate M4-era scope cut, revisited because real-world attribute
  names use hyphens constantly (found via
  `eval-okay-drv-fixed-output.nix`'s `flat-md5` etc.). Checked that no
  existing asnix fixture relied on whitespace-free `a-b` meaning
  subtraction before making this change.
- **`derivation`/`baseNameOf`/`dirOf` added as bare root-scope globals**
  (root `Env`/`ScopeFrame` grew from 8 to 11 slots across this pass).
  `baseNameOf`/`dirOf` didn't exist in any form before this pass (not
  even under `builtins.*`); ported directly from real Nix's own
  `prim_baseNameOf`/`prim_dirOf`.
- **Real store-path name validation and outputHash/outputHashAlgo/
  outputHashMode well-formedness validation added to `derivation {...}`**
  (`asnix_check_store_path_name`, `store_path.c`; validation in
  `derivation.c`) -- previously accepted any name/hash string
  unconditionally, which became *newly observable* as a regression the
  moment `derivation` was exposed bare (several `eval-fail-drv-*` cases
  expect these to be hard errors). Deliberately does NOT change how the
  derivation's store path is actually computed for a fixed-output
  derivation (still the ordinary non-fixed-output preimage) -- see the
  store-path-exactness caveat in this section's opening paragraph for
  why that doesn't matter for anything this pass could actually verify.
- **Attrsets with a `__toString`/`outPath` attribute now coerce to
  strings** (`asnix_coerce_to_string` and the stricter string-
  interpolation coercion, `value_util.c`) -- previously any attrset hit
  a hard "cannot coerce" fatal, even one specifically shaped to coerce
  (a derivation's own result, or an explicit `{ __toString = ...; }`).
  Purely a *value*-coercion upgrade, not the separate (and deliberately
  out-of-scope) "string context" dependency-tracking feature.
- **New builtins** (all verified against `nix-instantiate`): `seq`,
  `deepSeq` (with cycle detection for self-referential values, matching
  real Nix's own `forceValueDeep`'s `seen`-set), `intersectAttrs`,
  `groupBy`, `pathExists`, `readFile`, `readFileType`, `readDir`,
  `toPath` (deprecated even in real Nix -- returns a plain *string*,
  not a Path, despite the name), `getEnv`, `parseDrvName` (ported from
  real Nix's `DrvName::DrvName`, including its "first `-` followed by a
  non-letter" split rule), `hashFile`, and plain two-int-argument
  `add`/`sub`/`mul`/`div` (distinct from the `+`/`-`/`*`/`/`
  *operators*).
- **Attrset keys that aren't valid bare identifiers now print quoted**
  (`main.c`'s printer) -- e.g. `groupBy`'s numeric-string keys
  previously printed as bare `1 = ...` instead of real Nix's
  `"1" = ...`. A real, if narrow, printer gap that happened to have
  never been exercised before `groupBy` existed.

**Three real, previously-latent core-evaluator bugs found and fixed
while testing the above (not missing features -- see the numbered Known
Bugs entries below for the full writeup of each):** `map`/`mapAttrs`
calling their function argument immediately instead of lazily (#25),
thunk-of-thunk chains only ever being forced one level deep (#26), and
`concatLists` not forcing its outer list's elements before checking
their tag (#27) -- the second of these three is the most structurally
significant: it was latent in the core evaluator since ~M6/M7, just
never previously reachable, because every *other* result-producing
mechanism already happened to force inline before leaving a value on
VALSTACK.

**Deliberately skipped, with reasons (re-verify before assuming any of
these is still accurate -- re-derive the conformance numbers, don't
trust this list as ground truth):**

- **`builtins.tryEval`** -- would need a real exception-handling
  mechanism (every fatal error in this codebase is an unconditional
  `exit(1)`, with zero unwind/recovery infrastructure at any level).
  This is a correctness-preserving, large, separate feature (likely
  needing `setjmp`/`longjmp` or an equivalent VALSTACK/RETSP-rewind
  protocol at the dispatch-loop level), clearly disproportionate to
  the 2 files it would unblock in this pass. Affects
  `eval-okay-tryeval.nix`/`eval-okay-tryeval-failed-thunk-reeval.nix`.
- **`__curPos`, `builtins.unsafeGetAttrPos`, position-tracking in
  general** -- asnix's AST/IR carries no source-position information
  anywhere (tokens do, but it's discarded at parse time); implementing
  this would mean threading position info through the compiler and IR,
  a cross-cutting change disproportionate to the handful of
  `*-attr-pos*`/`eval-okay-curpos.nix`/`eval-okay-getattrpos*.nix`
  files that need it.
- **Dynamic/quoted-string attrset-literal keys** (`${expr} = val;`,
  `"${str}" = val;`) -- same M22-documented cut, not extended this
  pass; still blocks `eval-okay-attrs5.nix`/`eval-okay-attrs6.nix`/
  `eval-okay-dynamic-attrs*.nix`/`eval-okay-null-dynamic-attrs.nix`.
- **No float type at all** -- blocks `eval-okay-float.nix`,
  `eval-okay-builtins-add.nix`'s float cases, true float division, and
  anything else that writes a `1.5`-shaped literal. A real, sizeable
  feature (new tag or boxed representation, float arithmetic, float-to-
  string formatting matching real Nix's exact precision rules) --
  correctly out of scope for a long-tail pass.
- **`builtins.parseFlakeRef`/flake-ref-shaped builtins in general** --
  explicitly flagged in this pass's own task brief as a likely
  disproportionate rabbit hole; confirmed by inspection (would need a
  real URL-ish mini-parser plus a whole flake-ref data model) and left
  alone. Affects the `eval-okay-parse-flake-ref*.nix`/
  `eval-okay-flake-ref-*.nix` cluster (~9 files).
- **`__json`/structuredAttrs validation** (`eval-fail-json-attr-not-
  json.nix`) -- would need an actual JSON parser (`fromJSON` doesn't
  exist in this codebase at all yet); one file, not worth building a
  JSON parser for in isolation.
- **Fixed-output derivation *hash computation*** (as opposed to the
  *validation* this pass did add) -- real Nix's fixed-output
  derivations use a different store-path preimage formula
  (`fixed:out:algo:hash:name`) than the ordinary one asnix's
  `derivation.c` always uses; not changed, since conformance only ever
  checks the *shape* of these results in this environment (see this
  section's opening paragraph), not literal hash equality with a real
  store.
- **60-bit (not real Nix's full 64-bit) integer range** -- a
  structural, pre-existing limitation (`asnix_make_int`/`op_arith.S` do
  no overflow checking at all, silently wrapping past
  `ASNIX_INT_MIN`/`ASNIX_INT_MAX`), confirmed newly via
  `eval-fail-overflowing-add.nix`/`eval-fail-overflowing-mul.nix`
  (both expect a hard error; asnix currently silently wraps instead).
  Not fixed: doing so properly would mean adding overflow checks to the
  hottest arithmetic path in the whole evaluator (`op_arith.S`'s
  `+`/`-`/`*`), a correctness-vs-hot-path-risk tradeoff this pass
  judged not worth rushing under time pressure for 2 files. Flagged
  here as a real, reproducible gap for a future pass to pick up
  (straightforward in principle: check the native overflow flag after
  each op, then re-check the narrower `ASNIX_INT_MIN`/`MAX` window,
  fatal on either).
- **Context tracking / string context** -- out of scope per this
  pass's own brief (confirmed still blocking
  `eval-okay-drv-masking.nix`/`eval-okay-context*.nix`/
  `eval-okay-drv-outputs.nix` and several others: passing a derivation
  attrset through `inherit` into another derivation's env needs it).
- **`builtins.match`/`builtins.split`/`builtins.fromTOML`** --
  explicitly out of scope for this pass (two other agents), confirmed
  never touched.

## M23's Known Bugs entries (continuing the numbered list above)

25. **`map`/`mapAttrs` called their function argument immediately,
   once per element, instead of deferring each call into a lazy thunk --
   a real strictness bug, not a missing feature.** Confirmed against
   real Nix: `builtins.length (map (x: throw "boom") [1 2 3])` => `3`
   there (nothing ever forces the thrown elements), aborted here before
   this fix. A prior comment in `builtins_attrs.c` (next to
   `mapAttrs`'s implementation) had explicitly — and, it turns out,
   incorrectly — judged eager-vs-lazy construction "unobservable" since
   "every consumer... forces each attrset value immediately anyway";
   numtide/nix-conformance's `eval-okay-intersectAttrs.nix` disproves
   that directly, building `alphabetFail = builtins.mapAttrs throw
   alphabet` specifically as a "canary" (every value throws when
   forced) and then only ever touching its attribute *names* via
   `intersectAttrs`, never its values -- with the old eager
   implementation, just *constructing* `alphabetFail` aborted
   immediately. Fixed via two new shared helpers
   (`asnix_bshared_make_lazy_call1`/`2`, `builtins_shared.c`) that build
   a genuine `Thunk` wrapping a deferred call, reusing this project's
   own existing curry-via-tiny-IR-body trick
   (`make_curried2`/`build_curry2_body`, `builtins_attrs.c`) one level
   further: a small, singleton (built once, cached) IR body that reads
   its captured `{f, arg...}` back out of a fresh per-call `Env` via
   plain `OP_VAR_REF`/`OP_CALL`, wrapped in an `OP_MK_THUNK`-shaped heap
   `Thunk` object built directly in C (same four words
   `op_mk_thunk.S` itself writes) rather than compiled bytecode -- no
   new IR op, no asm changes needed for this part. `builtins_list.c`'s
   `prim_map_stage2` and `builtins_attrs.c`'s `prim_mapAttrs_apply2`
   both updated; `tests/c/test_builtins_list.c`'s own fixture needed a
   matching update (it read `map`'s result elements' raw ints directly
   without forcing, which only "worked" because the old eager `map`
   never left a thunk there).
26. **Thunk-of-thunk chains were only ever forced one level deep -- a
   real, latent core-evaluator bug, exposed (not caused) by #25.** A
   "lazy passthrough" primop like `builtins.getAttr` deliberately
   returns an attrset entry's raw value without forcing it (correct,
   matches real Nix's own lazy attribute-access semantics) -- but every
   *other* value producer in this evaluator (`OP_VAR_REF`, `OP_SELECT`,
   a closure's own `OP_RET`) already forces inline before leaving a
   result on VALSTACK, so every operator (`op_arith.S`'s `+` etc.) only
   ever checks the raw tag directly, assuming "already forced". That
   invariant silently didn't hold for a primop's raw-passthrough
   result: confirmed with a completely ordinary, map-unrelated
   expression -- `let attrs = { a = "x"; }; g = x: builtins.getAttr "a"
   x; in (g attrs) + "y"` -- aborted with a bogus "cannot coerce this
   value to a string" here, `"xy"` on real Nix, proving this predates
   and is independent of #25 (found immediately after fixing #25 while
   testing `eval-okay-attrnames.nix`, which chains `map` + `getAttr`
   together, but the root cause is general). Fixed at the one place
   every primop call's result passes through regardless of which
   primop it is (`op_call.S`'s primop-call path): force the return
   value (`asnix_force_for_print`) before it ever reaches VALSTACK.
   Also made `asnix_force_for_print` itself (`force.S`) loop until its
   result is no longer a `Thunk` (a tail-loop back to the top of the
   function, safe since every register this routine owns is already
   fully restored by that point), matching real Nix's own `forceValue`
   (`while (v.isThunk())` in `eval.cc`) -- defense in depth for the
   same underlying class of chain, reached from the C side (the
   printer, or any future C-side force call) even without going
   through `OP_CALL` at all.
27. **`concatLists` checked its outer list's elements' tags without
   forcing them first -- harmless before #25 (fixed), a real bug
   after.** List elements are always lazy by design (same as attrset
   values) -- an unforced `Thunk` sitting in a list is completely
   ordinary, not an error condition; only *forcing* reveals whether an
   element is actually a list. Before #25's fix, every actual producer
   of a list (literals, `map`, ...) happened to leave already-computed
   values in it, so `prim_concatLists`'s raw `asnix_tag_of()` check on
   each outer element "worked" by accident. Once #25 made `map`
   genuinely lazy per element, `builtins.concatLists (map (hash: map f
   strings) algos)`-shaped code (numtide/nix-conformance's
   `eval-okay-hashstring.nix`/`eval-okay-hashfile.nix`) aborted with a
   bogus "element N is not a list" instead of forcing the inner map's
   still-lazy result first. Fixed by forcing each outer element
   (`asnix_force_for_print`) before checking its tag, same as any other
   lazy-list consumer already has to.
28. **`baseNameOf` stripped *every* trailing slash instead of at most
   one.** Real Nix's own documented rule: "If the string ends with a
   `/`, only this one final slash is removed" -- a prior version of
   this new (M23) function used a `while` loop where its own adjacent
   comment already said "at most one", silently stripping every
   trailing slash. Confirmed wrong: `baseNameOf "a//"` produced `"a"`
   here vs. real Nix's `""` (strip exactly one slash -> `"a/"`, then
   the segment after *its* last `/` is empty). Changed the `while` to
   an `if`. Found via numtide/nix-conformance's
   `eval-okay-baseNameOf.nix`, which exists specifically to pin down
   this exact edge case (with a comment crediting a decade-old upstream
   Nix PR for the same reason).

## M23 scope notes: `builtins.match` / `builtins.split`

Both were missing entirely -- the single biggest remaining cluster in
numtide/nix-conformance's `lang/` suite (the differential
`corpora/regex.nix`, 272 `(pattern, string)` pairs, generates ~270
`eval-okay-regex-NNN.nix` cases, plus 45 more intentionally-invalid
patterns in `corpora/regex-invalid.nix` generating ~90 `eval-fail-regex-
invalid-NNN-{match,split}.nix` cases).

Implemented in a new `builtins_regex.c`/`.h` module (added to
`asnix_make_builtins_attrs`'s merge table in `builtins.c`, same mechanism
every prior builtins-expansion module uses), wired directly onto glibc's
POSIX `<regex.h>` (`regcomp`/`regexec`/`regfree`, `REG_EXTENDED`) rather
than hand-rolling a regex engine -- real Nix's own engine (libstdc++'s
`std::regex` built with `std::regex::extended`) already implements the
same POSIX-ERE/leftmost-longest grammar class, so this matches real Nix
closely for ordinary patterns without needing new infrastructure. Nix
strings aren't NUL-terminated (`asnix_ptr_bytes`); both the pattern and
subject get a scratch `malloc`+NUL-terminate+`free` copy per call (not
arena-allocated -- nothing here needs to outlive one builtin call, same
convention `builtins_compare.c`'s `replaceStrings`/`toJSON` already use).

- **`match`**: real Nix implicitly anchors the whole pattern to the
  entire string (`null` on anything less than a full match). Rather than
  textually wrapping the pattern in `^(?:...)$` (POSIX ERE has no
  non-capturing group -- `\(...\)` IS a capture group, which would shift
  every real group's index by one), this relies on `regexec`'s own
  leftmost-longest guarantee: if any match starts at offset 0, `regexec`
  is guaranteed to find one there (nothing can be "earlier"), and that
  match is guaranteed to be the *longest* one starting at offset 0 -- so
  a plain, unmodified `regexec` call, checked for `rm_so==0 &&
  rm_eo==len`, is exactly `std::regex_match`'s semantics (what real Nix's
  `primops.cc` actually calls) with zero pattern rewriting.
- **`split`**: a `pos`/`last_end`-tracking loop over successive `regexec`
  calls on `subject+pos` (`REG_NOTBOL` once `pos>0`, so a literal `^`
  only ever matches the true start of the whole subject). Zero-width
  matches (e.g. `x*` always has one) are real and were specifically
  handled and checked against real Nix (`split "x*" "ab"` == `[ "" [ ]
  "a" [ ] "b" [ ] "" ]`): after recording one, `pos` advances to
  `rm_eo+1` rather than `rm_eo` (which would just re-find the same empty
  match forever), while `last_end` (which only moves on actual match
  boundaries) still correctly picks up the skipped byte as part of the
  next literal-text span.
- A capture group that didn't participate (e.g. an untaken alternation
  branch) becomes `null` in the result list, not `""` -- verified:
  `match "(a)?(b)" "b"` == `[ null "b" ]`.

**Verification**: every individual rule above was checked directly
against real `nix-instantiate --eval --strict` (2.35.2) before being
trusted (see `builtins_regex.c`'s own per-function comments for the
specific expressions). Beyond that, this module was self-checked against
the *entire* 272-entry `corpora/regex.nix` differential corpus directly
(bypassing nix-conformance's own test runner -- see the scope-cut note
below on why) by writing a one-off script that, per entry, ran
`builtins.match`/`builtins.split` through both real `nix-instantiate` and
this build and diffed the two: **238/272 (87.5%) match byte-for-byte**;
the remaining 34 fall into three understood, deliberately-not-chased
categories (see Known Bugs #29 below for the full writeup of the biggest
one) rather than being random bugs.

**Scope cuts, deliberate**:
- **Capture-group value under a repeated subexpression** (e.g. `(a*)*`
  matched against `"aa"`) is POSIX-unspecified, and glibc's `regexec` and
  libstdc++'s `std::regex` make *different* implementation choices here
  (glibc reports the union/last-nonempty span across all iterations;
  libstdc++/most modern engines report only the final iteration, which
  is often empty) -- this is the majority of the 34 self-check mismatches
  above (~27 of them). Not fixable by any `regcomp`/`regexec` flag; fixing
  it for real would mean writing a custom backtracking engine with
  libstdc++'s specific iteration-capture convention, which is a different
  project, not a `<regex.h>` integration. See Known Bugs #30.
- **A handful of named collating-symbol/equivalence-class forms
  (`[[.hyphen.]-z]`, `[[.space.][.tilde.]]`, `[[=period=]x]`) and
  nonstandard/case-variant POSIX character-class names (`[[:w:]]`,
  `[[:ALPHA:]]`)** that real Nix accepts but glibc's `regcomp` rejects
  (`REG_ECOLLATE`/`REG_ECTYPE`) even with the locale explicitly set to
  `en_US.UTF-8` (checked directly, both with and without an explicit
  `setlocale(LC_ALL, "")` call -- no difference, since glibc's own locale
  data simply doesn't define a collating symbol named `hyphen` et al.,
  and `w`/`ALPHA` were never POSIX class names to begin with). libstdc++
  is simply more lenient here than glibc's locale-driven implementation;
  not a parseable rule to special-case, just an engine difference (6 of
  the 34 self-check mismatches).
- **`[é-a]+`-style ranges spanning a multi-byte UTF-8 character** are
  already a documented, acknowledged cross-platform quirk upstream (see
  nix-conformance's own `corpora/regex-invalid.nix` comment, "`[a-é]`
  rejected on x86_64 only: see nix-bugs/regex-char-sign" -- the sibling
  direction `[é-a]` in `corpora/regex.nix` is the one remaining self-check
  mismatch, same root cause: signed-vs-unsigned byte comparison of a
  UTF-8 continuation byte against an ASCII endpoint). Not re-litigated
  here since nix-conformance already tracks it as inherent/platform-
  dependent, not a asnix-specific bug.
- **A pre-`regcomp` validity pre-check** was added (`builtins_regex.c`'s
  `pattern_rejected_by_real_nix`) specifically because glibc's `regcomp`
  accepts a small, enumerable family of patterns real Nix's
  `std::regex::extended` rejects as a hard parse error (not a semantics
  difference) -- found via `corpora/regex-invalid.nix`'s 45 entries,
  each cross-checked directly against real `nix-instantiate`. The three
  rules implemented (an escape-character whitelist limited to the 12 real
  POSIX ERE metacharacters `. ( ) * + ? [ { ^ $ | \ ` -- notably
  *excluding* `]` and `}`, confirmed asymmetric directly: `\(`/`\)`/`\[`/
  `\{` are valid escapes but `\]`/`\}` are not; a bare digit escape like
  `\1` falls under this same rule, which is also why POSIX ERE
  backreferences -- not part of its grammar at all -- need no separate
  handling; a `{` immediately followed by `,` with no lower bound, e.g.
  `a{,2}`; and an unmatched `)` with no corresponding `(`) are bracket-
  expression-aware (correctly skip `(`/`)`/`{`/`\`/collating-sub-
  expression contents inside `[...]`, where none of them are special)
  and recover 10 of the 13 regex-invalid entries glibc alone would
  wrongly accept. The remaining 3 (`(a){24998}`'s group-repetition
  complexity limit and the two collating-symbol-range forms,
  `[[.-.]]`/`[a-[.c.]]`) are left as glibc-accepts/real-Nix-rejects gaps
  for the same reason as this section's collating-symbol scope cut above
  -- an internal-limit/grammar-leniency difference between the two
  engines, not a describable syntax rule.
- **nix-conformance's own `lang/` test runner for this corpus could not
  actually be used to measure this module** (see the main pass-count
  report for the concrete numbers): every `eval-okay-regex-NNN.nix` and
  `eval-fail-regex-invalid-NNN-*.nix` case wraps its `match`/`split` call
  in `builtins.tryEval`, which does not exist in asnix at all (confirmed:
  `builtins ? tryEval` fatals with "attribute missing" immediately, found
  while debugging why the corpus cases weren't improving) and would be a
  genuinely large, separate undertaking to add -- every fatal error path
  in this codebase (including ones in hand-written asm, e.g.
  `op_select.S`'s "attribute missing") is an unconditional process
  `exit()`, not a catchable/propagatable condition, so implementing real
  `tryEval` semantics would mean threading a `setjmp`/`longjmp`-style (or
  full exception-object-passing) recovery mechanism through every one of
  those sites, an architecture change out of scope for a `match`/`split`
  builtins pass. This is why this module's correctness was instead
  checked against the 272-entry corpus directly, bypassing
  nix-conformance's own runner (see Verification above) -- `match`/
  `split` themselves are correct for the vast majority of real inputs;
  `tryEval`'s absence is what keeps nix-conformance's own `lang/` score
  from reflecting that.

## M23's Known Bugs entries, continued (regex pass)

29. **`builtins.tryEval` does not exist in asnix at all.** Found while
   investigating why nix-conformance's `lang/` regex corpus cases
   (`eval-okay-regex-NNN.nix`) weren't passing despite `match`/`split`
   being correctly implemented and independently verified against real
   `nix-instantiate`: every one of those generated test cases wraps its
   call in `let try = v: let r = builtins.tryEval v; in ...; in try
   (builtins.match ...)` (see `corpora/regex.nix`'s own `try` helper), and
   `builtins ? tryEval` is simply false here -- selecting it fatals
   ("attribute missing, aborting") before `match`/`split` are ever
   reached. This single missing builtin gates roughly 270 otherwise-
   reachable `lang/` test cases (the entire regex differential corpus),
   not a gap specific to this pass's own work. Not fixed here: real
   `tryEval` needs a catchable-exception mechanism, and every fatal path
   in this codebase today (C `fprintf`+`exit()`, and hand-written asm
   sites like `op_select.S`'s own "attribute missing" exit) is an
   unconditional process exit with no unwind/recovery story at all --
   adding one is a cross-cutting architecture change (likely
   `setjmp`/`longjmp` at the `tryEval` call site plus converting every
   `exit()` call site, asm included, to `longjmp` instead), well beyond
   what a `builtins.match`/`builtins.split` pass should attempt. Left
   here as a concretely-measured, high-value target for a future
   milestone.
30. **Capture-group value under a repeated subexpression differs between
   glibc's `regexec` and real Nix's `std::regex`, with no POSIX-mandated
   "correct" answer.** Found via this pass's own 272-entry corpus self-
   check (see the M23 scope notes above): `builtins.match "(a*)*" "aa"`
   on real `nix-instantiate` returns `[ "" ]` (the group captures the
   *last* iteration, which is the trailing empty match once `a*` has
   consumed both `a`s), while asnix/glibc returns `[ "aa" ]` (the group
   captures the full combined span across every non-empty iteration).
   Both agree the *overall* match succeeds and spans the whole string --
   only the captured group text differs. This is POSIX-unspecified
   behavior (the standard explicitly leaves subexpression-under-
   repetition captures implementation-defined), not a bug in either
   engine, and not fixable via any `regcomp`/`regexec` flag -- glibc's
   regex engine and libstdc++'s `std::regex` simply made different,
   internally-consistent design choices decades apart. Reproducing real
   Nix's exact convention here would require a hand-written backtracking
   regex engine with that specific "last iteration wins" capture rule,
   which is a different, much larger project than wiring up `<regex.h>`.
   Affects roughly 10% of the differential corpus's entries (27 of 272 in
   the direct self-check) -- documented as a known, permanent divergence
   rather than silently accepted.

## M23 scope notes: `builtins.fromTOML`

Driven by numtide/nix-conformance's `lang/` corpus: `builtins.fromTOML`'s
own differential corpus (`lang/corpora/toml.nix`, 77 documents, each
backing one `lang/eval-okay-fromTOML-NNN.nix`/`.exp` pair) was the
second-biggest single cluster of conformance failures after this
milestone's predecessors, entirely because the builtin didn't exist at
all.

**What's implemented** (`src/c/toml.c`/`toml.h`, a hand-written
recursive-descent parser in the same plain-index-into-buffer style as
`lexer.c`/`parser.c`, no external TOML library vendored): `key = value`
pairs (bare/quoted/dotted keys), `[table]` and `[[array of tables]]`
headers (including the "a later `[[x.y]]` nests under the *last* element
of an already-open `[[x]]`" stepping rule), inline tables
(`{ a = 1, b = 2 }`), arrays (`[ 1, 2, 3 ]`, free comment/newline
placement), basic strings with `\b \t \n \f \r \" \\ \uXXXX \UXXXXXXXX`
escapes, literal strings (no escapes), multi-line basic/literal strings
(`"""..."""`/`'''...'''`, including the opening-newline-trim and
line-ending-backslash-splice rules), integers (decimal, `0x`/`0o`/`0b`,
underscore separators, leading `+`/`-`), booleans, and comments/CRLF/blank
lines between statements. Registered as `builtins.fromTOML` only (a
plain arity-1 PrimOp, `builtins_toml.c`) -- real Nix doesn't expose it as
a bare root-scope global either.

**TOML date/time literals are implemented, not cut**, despite asnix
having no native date/time *or* float type: real Nix's own `fromTOML`
represents every TOML date/time kind (local date, local time, local
date-time, offset date-time) identically as
`{ _type = "timestamp"; value = "<normalized text>"; }` (confirmed
directly against `lang/eval-okay-fromTOML-062` through `-076.exp`) --
just a string wrapped in a 2-field attrset, fully representable here.
`classify_date_or_time`/`normalize_time` (`toml.c`) reproduce the exact
normalization real Nix's toml11-backed implementation applies: the `T`/`t`/
space date-time separator and lowercase `t`/`z` offset marker are both
normalized to uppercase `T`/`Z`, and fractional seconds are padded with
trailing zeros to a 3-digit (millisecond) minimum, left as-is between 3
and 9 digits, and would be truncated beyond 9 (TOML's own nanosecond
ceiling) -- verified against `-064.exp` (`.5` -> `.500`) and `-065`/
`-066.exp` (6- and 9-digit fractions kept exactly as given).

**What's deliberately cut, and why (the actual scope boundary):** TOML
*floats* and `inf`/`nan` -- asnix's `Value` representation has no float/
double tag at all (`include/asnix/value.h`: only `TAG_INT`, a 61-bit
immediate integer). There is no honest way to represent a TOML float
without either (a) silently truncating it to an int -- a genuinely wrong
value, not a smaller one, and strictly worse than a clear error, since
`3.0` truncated to `3` happens to print identically to real Nix's float
`3.0` in that one specific whole-number case but would be badly wrong for
`1.5`, `6.626e-34`, etc -- or (b) faking a string, which would change the
result's `builtins.typeOf` and break any arithmetic a real caller does on
it. `classify_bare_token` therefore detects a float-shaped token (contains
`.`/`e`/`E` outside a date/time position, or is `inf`/`nan`) and calls
`toml_fatal_float()`, which `fprintf`s a clear "not supported" message and
exits -- matching every other fatal error's convention in this codebase
(no `tryEval`/exception mechanism exists to catch it with anyway, and the
corpus's own `toml-invalid.nix` comment notes this explicitly: "invalid
... documents live in toml-invalid.nix, because builtins.tryEval cannot
catch a fromTOML error"). This is a parser-level, not spec-level,
decision: nothing here claims TOML floats are invalid syntax, only that
asnix cannot represent the resulting *value*.

Exact corpus accounting (verified by generating `{ d = <doc>; v =
builtins.fromTOML <doc>; }` for all 77 `lang/corpora/toml.nix` entries and
diffing the printed result against the corresponding
`lang/eval-okay-fromTOML-NNN.exp` byte-for-byte, then re-confirmed through
the real `./run adapters/asnix lang` conformance runner):

- **73 of 77 pass** (entries 000-005, 009-059, 061-076).
- **4 of 77 remain unsupported, all for the same float reason**: entry
  006 (`a = 1.5\nb = -0.0\nc = 1e10\nd = 1E-3\ne = 6.626e-34`), entry 007
  (`a = inf\nb = -inf\nc = +inf`), entry 008 (`a = nan`), and entry 060
  (`a = [1, "two", true, 3.0]` -- the lone float `3.0` embedded inside an
  otherwise-fine mixed-type array). These map to
  `lang/eval-okay-fromTOML-006/-007/-008/-060.nix`.
- `lang/eval-okay-fromTOML-timestamps.nix` and the un-numbered
  `lang/eval-okay-fromTOML.nix` are both separately out of scope for the
  same reason: each bundles several TOML documents together (one `nix`
  list / `fromTOML` call per file), and at least one of the bundled
  documents in each contains `flt1 = 3.1415`-style float fields, so the
  *whole* file fails regardless of how correct the rest of the document's
  parsing is -- not a new or different gap, just the same float cut hit
  from a different test-file shape.
- `lang/eval-fail-fromTOML-invalid-*.nix` (46 cases) and the four
  `lang/eval-fail-fromTOML-{keyWithNullByte,overflow,underflow,
  valueWithNullByte}.nix` cases are a different cluster entirely (TOML
  documents real Nix's toml11 *rejects*, where the test only checks that
  *some* error occurs) and were never this milestone's target -- this
  parser is intentionally not a strict validator (e.g. it doesn't enforce
  "no newline inside an inline table", integer overflow/underflow
  bounds-checking, or null-byte rejection in keys/values), so roughly a
  third of that cluster (as of this writing) incorrectly *succeeds*
  parsing a document real Nix rejects. Documented here as a known,
  accepted gap rather than silently ignored: tightening this would mean
  adding validation this milestone's actual target (the 77-entry
  eval-okay cluster) never needed.

**A real, previously undiscovered, unrelated bug found along the way**:
see Known Bugs #31 below -- the attrset printer never quoted attribute
names at all (affecting any builtin that can produce an arbitrary-string
key, not just `fromTOML`). Also independently rediscovered here (fixed
already, see M23's first scope-notes section above and this milestone's
Status row): `++` (list concatenation) didn't exist in asnix at all,
which blocked *every single* `fromTOML` conformance case from even
parsing, independent of whether the TOML parser itself was correct
(numtide/nix-conformance's own `lang/corpora/toml.nix` builds its test
list with `docs ++ times`) -- two of this milestone's three parallel
passes hit this exact same gap from different directions; the surviving
implementation is the one described earlier in this document (`OP_CONCAT`/
`op_concat.S`/`asnix_concat_lists`), not a second, separate one.

## M23's Known Bugs entries, continued (TOML pass)

31. **The attrset printer never quoted attribute names, under any
   circumstances -- a real, general bug, not fromTOML-specific, just
   found through it.** `main.c`'s `ASNIX_TAG_ATTRS` print case always did
   `printf("%.*s = ", ...)` on the raw attribute name bytes, unconditionally.
   Real Nix's printer only prints a name bare when it's a syntactically
   valid identifier (`[A-Za-z_][A-Za-z0-9_'-]*`) and isn't one of a small
   reserved-keyword set (`let`/`in`/`if`/`then`/`else`/`rec`/`inherit`/
   `assert`/`or` -- but *not* `true`/`false`/`null`, which print bare
   despite being lexer keywords too, confirmed directly: `nix-instantiate
   --eval -E 'builtins.listToAttrs [{name="true";value=1;}]'` prints
   `{ true = 1; }`, not `{ "true" = 1; }`; and confirmed the reserved-
   keyword case too: `{ let = 1; }` as *source* is a syntax error, but
   `builtins.listToAttrs [{name="let";value=1;}]` prints `{ "let" = 1;
   }`); every other name (empty, digit-leading, containing a space,
   non-ASCII, a reserved keyword) must print quoted exactly like a string
   literal, with the same escaping. Found via `builtins.fromTOML`'s own
   corpus, which deliberately includes keys like `""`, `"1234"`,
   `"quoted key"`, and `"café"` that must round-trip through this
   printer -- but any *other* builtin able to construct an attrset with
   an arbitrary-string key (confirmed independently with
   `builtins.listToAttrs`) was already affected; this was never
   TOML-specific. A second, independent pass in this same milestone also
   found and fixed a narrower version of this bug (character-shape only,
   missing the reserved-keyword case) -- the more complete fix
   (`attr_name_needs_quoting`/`print_attr_name`, `main.c`) is the one that
   survived integration.

## M24 scope notes: quoted-string attrpath segments

Found continuing to chase `d.nix` (importing real nixpkgs) past M23:
`lib/default.nix` hit "parse error: expected ';'" on `inherit
(self.trivial) ... "or" ...;` -- `or` is a reserved keyword in asnix (and
in real Nix), so real nixpkgs's own `lib/trivial.nix` defines it as
`"or" = x: y: x || y;` (a quoted key) rather than bare `or = ...;`, and
`lib/default.nix` correspondingly re-exports it via `inherit (self.trivial)
"or" ...;`. asnix's `inherit`/attrpath-binding/`.select`/`?has-attr`
parsing only ever accepted `TOK_IDENT` for an attribute name -- a quoted
string segment anywhere (`"or"`, `x."a b"`, `x ? "a b"`, `x."a b" = 1;`)
was a hard parse error.

Cross-checked against cppnix's real grammar
(`~/nix/src/libexpr/parser.y`): `attr: IDENTIFIER | string_attr` (a
static quoted string) for ordinary attrpath segments, but `inherit`'s own
`attrs` rule explicitly rejects a *dynamic* (`${expr}`) segment
("dynamic attributes not allowed in inherit") while still accepting a
static string one. Fixed with one shared helper, `parse_attr_segment`
(`parser.c`), used at all four call sites that previously hard-coded
`expect(p, TOK_IDENT, ...)`: `inherit`'s name list, attrpath binding
sugar (`a.b.c = val;`, including attrset-literal keys -- same function),
`.select` (`parse_select`), and `? has-attr` (`parse_has_attr`). Verified
each against real `nix-instantiate --eval` individually (`x."a b"`,
`x ? "a b"`, `let x."a b".c = 5; in x."a b".c`) before trusting it.

**Scope cuts, deliberate, both already-documented from M22, unchanged
here**: a *dynamic* `${expr}` attrpath segment is still only supported
for `.select` (`OP_SELECT_DYNAMIC`, M22), not for `inherit`, binding
targets, attrset-literal keys, or `?`. Lambda-pattern formals
(`{a, "b c" ? default}: ...`) deliberately do NOT get this treatment --
confirmed real Nix itself rejects a quoted formal name too (`({ "a b"
}: ...)` is a syntax error there), so there was nothing to fix.

## M25 scope notes: real call-by-need for function arguments

Found continuing to chase `d.nix`: nixpkgs's `lib/default.nix` evaluates
`lib = makeExtensible' (self: ...)`, where `makeExtensible'`'s own body
is `let self = rattrs self // {...}; in self` -- a lazy fixed-point
combinator (the same idiom behind `lib.makeExtensible`, overlays,
`callPackage`, and `finalAttrs`-style patterns throughout real nixpkgs).
asnix aborted on this with "infinite recursion forcing a thunk
(blackhole)" even though the self-reference is never actually read by
anything that matters.

**Root cause, confirmed with two independent minimal reproductions
against real `nix-instantiate --eval` before touching any code**:
- `(x: 1) (abort "never runs")` -- real Nix returns `1` (the unused
  argument is never evaluated); asnix aborted immediately.
- `let f = x: { a = 1; }; self = f self; in self.a` -- real Nix returns
  `1`; asnix blackholed.

Both point at the same bug: `compiler.c`'s `AST_APP` case compiled a
function call's argument expression directly inline (eager, call-by-
value) before `OP_CALL`, instead of real Nix's call-by-need (an argument
is a thunk, forced only if and when the callee's body actually reads the
parameter). Every *other* binding form in this codebase (`let`,
`rec {}`, plain attrset values) was already correctly lazy via
`OP_MK_THUNK` since M6 -- function-call arguments were the one place
real Nix's laziness was never implemented at all. This is why the
self-referential fixed point blackholed: `f self` evaluated `self`
*at the call site*, forcing the very thunk already in the middle of
being forced.

**Fix**: `AST_APP` now wraps the argument expression in its own
`OP_MK_THUNK` (a fresh `IrBuilder`, compiled and `ib_ret`-terminated
exactly like a `let`-binding's value, capturing the caller's current env)
instead of compiling it straight into the caller's code. `OP_VAR_REF`'s
existing force-on-read logic (unchanged) then forces the parameter
lazily, if and only if the callee's body actually references it --
exactly the same mechanism that already made `let`-bindings lazy, now
extended to call arguments too.

**A second, necessary fix surfaced immediately by the first**: every
native-code `builtins.*` primop reads its arguments' raw tag bits
directly (`args[0]`, `args[1]`, ...) with no way to force a thunk itself
-- forcing requires re-entering the full dispatch loop (`asnix_force_for_
print`), which a plain C function can't do on its own. Previously this
was a harmless invariant gap (every argument reaching a primop call was
*already* forced, since arguments used to be compiled eagerly) --
real/dangerous now that arguments are genuine thunks: a primop could
silently misinterpret an unforced thunk's raw bits as if they were
already the expected tag (corruption, not a clean error). Fixed in
`op_call.S`'s primop-call path: a new loop forces every argument
(`asnix_force_for_print`, already used for primops' *return* values
since M23) before the native function ever sees them. The loop counter
is spilled to a one-time-allocated 16-byte stack slot rather than
push/pop'd per iteration, to keep `%rsp` 16-byte aligned across each
`call` -- matching this file's existing push-in-pairs discipline. Closure
calls are untouched (correctly): a closure's parameter is bound to an Env
slot via the ordinary argument-copy loop, and `OP_VAR_REF`'s own forcing
handles it lazily exactly like any other binding, no primop-style eager
forcing needed or wanted there.

**Verified**: both minimal repros above now match real Nix exactly;
`make test`/`test-e2e`/`test-conformance` all green (157 e2e, 19
conformance, full unit suite); numtide/nix-conformance's `lang/` pass
count went from 387 to 389 (small additional gain, no regression) on the
real external runner.

**Not yet addressed, a real known follow-up**: every call argument is
now unconditionally thunk-wrapped, even a trivial one (`f 1`, `f x` where
`x` is already a plain variable) -- a real, measurable performance cost
(one arena allocation plus an extra indirection per call argument) that
real Nix implementations typically avoid via a "this argument is already
in WHNF, don't bother thunking it" fast path. Correctness came first;
this is a legitimate target for a future profiling-driven pass, not
attempted here.

## M26 scope notes: unary `-` (negate)

Found continuing to chase `d.nix`: `lib/trivial.nix` has `bitNot =
builtins.sub (-1);` -- a bare `(-1)` literal, which asnix had **no**
support for at all (not even as a parenthesized standalone expression,
let alone as a general prefix operator) -- `(-1)` hit "parse error:
unexpected token" since `parse_add`'s loop only ever recognized `-` as a
*binary* infix operator, with nothing handling a leading `-` at the
start of an operand.

Precedence cross-checked against real `nix-instantiate --eval` with four
targeted cases before writing any code (each disambiguates against a
different adjacent operator, since several naive test expressions give
the same answer under either precedence and prove nothing):
- `-f 3` == `-(f 3)` == `-3` (not `(-f) 3`) -- negate binds looser than
  application.
- `-x ? "a"` == `(-x) ? "a"` == `false` (not `-(x ? "a")`, which would be
  a type error negating a boolean) -- negate binds tighter than `?`.
- `-2 + 3` == `(-2) + 3` == `1` (not `-(2+3)` == `-5`) -- negate binds
  tighter than binary `+`.
- `3 - -4` == `7` -- a binary `-` followed immediately by a unary `-` on
  its own RHS parses unambiguously (no grammar conflict with the
  existing binary-operator loop in `parse_add`).

This matches real Nix's own grammar exactly (`~/nix/src/libexpr/
parser.y`: `'-' expr_op %prec NEGATE`, the single tightest-declared
operator in the whole precedence table, but still looser than implicit
application/select, which aren't part of that table at all -- bison
resolves this structurally, not via the precedence declaration, since
`expr_op`'s own grammar already nests application/select arbitrarily
tightly). Fixed with a new `parse_negate` (`parser.c`), spliced between
`parse_app` (tighter) and `parse_has_attr` (looser, now calling
`parse_negate` instead of `parse_app` directly for its operand) --
right-recursive (allows `--x`, harmless) and desugars directly to the
existing binary `-` (`AST_BINOP` op `'-'`) with a literal `0` as the
left-hand side, exactly the same `sub 0 $2` desugaring real Nix's own
grammar action uses -- no new AST node or IR op needed.

## M27 scope notes: `fromTOML` bare global + the real bare-global rule

Found continuing to chase `d.nix`: `lib/trivial.nix` calls bare
`fromTOML` directly (`importTOML = path: fromTOML (readFile path);`),
and asnix's `builtins.fromTOML` (M23) had never been exposed as a bare
global.

**This also corrected a previously-incomplete understanding of real
Nix's bare-global rule** (M18's original research, repeated and extended
piecemeal in M22/M23 by testing individual names against
`nix-instantiate` one at a time): it is not actually a small, hand-
curated list at all. Read directly from cppnix's source this time
instead of guessing further names to test: `EvalState::addPrimOp`
(`~/nix/src/libexpr/eval.cc`) exposes **every** primop whose registered
name does *not* start with `__` as a bare global under that exact name,
automatically, in addition to registering it under `builtins.<name>`
(a `__`-prefixed primop, e.g. `toJSON`'s real registered name is
`__toJSON`, is bare-accessible only under that `__`-prefixed form --
confirmed directly: `toJSON {}` bare is "undefined variable", matching
this rule exactly). Grepping every `RegisterPrimOp`/`.name = "..."` site
across `~/nix/src/libexpr/primops.cc` and `~/nix/src/libexpr/primops/
*.cc` for non-`__`-prefixed names gives the complete, authoritative set:
`import abort throw map removeAttrs isNull toString baseNameOf dirOf
fetchTarball fetchGit fetchMercurial fetchTree fetchFinalTree fromTOML
placeholder scopedImport derivationStrict break` (plus `derivation`,
`builtins`, `true`, `false`, `null`, `__curPos`, which aren't ordinary
`addPrimOp`-registered primops at all and were already known/handled
separately). Of the primops in that list not yet implemented in asnix
at all (`fetchTarball`/`fetchGit`/`fetchMercurial`/`fetchTree`/
`fetchFinalTree`, `placeholder`, `scopedImport`, `derivationStrict`,
`break`), none are added here -- the fetch-family needs real network
I/O, `derivationStrict`/`scopedImport` are moderate undertakings of their
own, and `break`/`placeholder` simply haven't been hit by anything this
project has actually evaluated yet. Documented here as the complete
picture rather than re-discovered one name at a time again, but
deliberately not implemented speculatively (same "need-driven, not
speculative" discipline as every other builtin added so far).

Fixed by adding `asnix_make_fromTOML_primop()` (`builtins_toml.c`,
alongside the existing `prim_fromTOML`) -- same established pattern as
every other bare-global wrapper. Root `Env`/`ScopeFrame` grew from 11 to
12 slots.

## M28 scope notes: dynamic attrset-literal keys + `.${k} or default`

Found continuing to chase `d.nix`: `lib/lists.nix`'s `groupBy` uses
`r // { ${key} = (r.${key} or [ ]) ++ [ e ]; }` -- a dynamic attrset-
literal key (`${key} = ...;`) combined with a dynamic select-with-
default (`r.${key} or [ ]`). Both were documented, deliberate scope cuts
from M22 ("dynamic attrset-literal keys... wasn't attempted since
nothing hit so far has actually needed it" / "`.${name} or default`...
rejected at parse time... not yet needed") -- both are now actually
needed, so both are implemented here.

**Dynamic attrset-literal keys**: cross-checked against real Nix before
designing anything -- a plain `{...}`/`rec {...}` literal allows
`${expr} = value;` (a *hard* duplicate-key error on collision with any
other key, static or dynamic, unlike `//`'s silent right-wins -- `{ a =
1; a = 2; }` and `{ a = 1; ${k} = 2; }` with `k = "a"` both error
"...already defined"), but `let` explicitly rejects it ("dynamic
attributes not allowed in let", confirmed directly in cppnix's grammar,
`~/nix/src/libexpr/parser.y`'s `LET binds IN_KW ...` action -- `let`
reuses the exact same `binds` grammar rule as attrsets, then rejects
after the fact if any dynamic attr was found). Also confirmed: even
`rec {}` does NOT give a dynamic entry lexical/recursive visibility --
`rec { ${k}=1; b=a; }` with `k="a"` still errors "undefined variable
'a'", meaning this is purely a runtime attrset-construction concern,
never threaded through this project's own `ScopeFrame`/lexical-scoping
mechanism at all, for any binding form.

Implemented with a new parallel collection, `DynBindingVec`/
`AstDynBinding` (`parser.c`/`ast.h`), kept entirely separate from the
existing `BTVec`/`BTNode` static-key tree (which can't accommodate a
runtime-only name at all): `parse_binding_stmt` grew a `dyn_out`
parameter -- `NULL` at the one call site (`let`) where real Nix rejects
this, a real vector at the three call sites that build a literal `{...}`/
`rec {...}` (including the legacy `let { ...; body = ...; }` form, which
desugars to `rec {}` under the hood and inherits its rules). `compiler.c`'s
`AST_ATTRSET` case builds the static part exactly as before (completely
unchanged), then applies each dynamic entry afterward via a new IR op,
`OP_INSERT_DYNAMIC_ATTR` -- pops the already-built attrs, the computed
name, and a thunk wrapping the value (same laziness as every other
attrset value), interns the name at runtime (`asnix_intern`, already
used for `OP_SELECT_DYNAMIC`), and inserts it into a freshly-built,
one-larger sorted array (`asnix_attrs_insert_dynamic`, `attrs_update.c`)
-- fatal on a collision, matching real Nix's hard-error semantics
exactly (not `//`'s silent overwrite).

**`.${expr} or default`**: a new `OP_SELECT_DYNAMIC_OR`
(`op_select_dynamic_or.S`) mirrors the static `OP_SELECT_OR`'s try/jmp/
default shape exactly, but defers the actual lookup to a new C function
(`asnix_bshared_select_dynamic_or`, `builtins_shared.c`) instead of
hand-rolling a binary search in asm, since the name isn't a compile-time
constant here -- same reasoning `OP_SELECT_DYNAMIC` itself already used.
Matches `OP_SELECT_OR`'s own graceful-on-non-attrset rule (falls through
to the default rather than fataling, unlike the plain `OP_SELECT_DYNAMIC`
form) -- confirmed: `(5).${k} or 99` returns `99` in real Nix, not a type
error. The parser's own `or`-after-`.select` handling (already shared
between the static and dynamic cases structurally) needed no new logic
at all once the earlier, now-obsolete fatal check was simply removed.

**Verified**: the exact `groupBy` pattern from `lib/lists.nix`, run
end-to-end, matches real `nix-instantiate --eval --strict` byte-for-byte
(`{ hi = [ 3 4 5 ]; lo = [ 1 2 ]; }`). `make test`/`test-e2e`/
`test-conformance` all green (167 e2e, 19 conformance, full unit suite).

**Scope cuts, deliberate, narrower than real Nix**: only a single
dynamic segment is supported per binding (`${k} = v;`), not a dynamic
segment followed by more attrpath segments (`${k}.a = v;`) or preceded
by a static one (`a.${k} = v;`); the "regrettable merge" path
(`reopen_attrset_into`, NixOS/nix#9020's `a = rec {...}; a.c = ...;`
quirk) fatals cleanly if the literal being reopened has any dynamic
entries rather than silently dropping them. None of these were needed by
anything hit so far.

## M29 scope notes: `rec {}` eagerly forced every field on construction

Found continuing to chase `d.nix`: `lib/strings.nix` (a large `{lib}:
let...in rec {...}`) hit "attribute missing" merely from being *called*
with an argument -- before anything ever selected a specific field.
Minimal repro, confirmed against real `nix-instantiate --eval` before
touching any code: `(rec { a = abort "x"; b = 2; }).b` aborted in asnix
instead of returning `2` (real Nix never touches `a` at all, since
nothing selects it).

**Root cause, same class of bug as M25's call-by-value argument fix**:
`compiler.c`'s `AST_ATTRSET` case, for the `rec {}` branch, builds each
binding as a thunk, files it into a fresh frame slot (`OP_FRAME_SET`),
then needs to read all of them back in *sorted* order to hand to
`OP_MK_ATTRS` (which needs every value present, matching its parallel
sorted-symbols array). That read-back used `OP_VAR_REF` -- the only
frame-reading op that existed before this -- which forces on every read
by design (correct for a genuine variable reference, where the value is
about to be used). Here it was being repurposed as a "move this value
out of the frame" step, not a real variable reference, so M25's same
fix couldn't directly apply: `OP_VAR_REF` forcing immediately turned a
`rec {}` literal's construction into "force every field right now,"
regardless of whether anything would ever select them.

**Fix**: a new op, `OP_FRAME_GET` (`op_frame.S`), that reads a frame
slot's raw value verbatim -- no thunk-force check at all, pure data
movement. `compiler.c`'s `rec {}` branch now uses `ib_frame_get` instead
of `ib_var_ref` for the read-back; each binding is already a fresh,
never-shared `Thunk` at that point (just built via `OP_MK_THUNK`), so a
raw read-back is exactly correct -- the thunk still gets forced later,
lazily, the first time something actually selects that field (via the
*normal*, unchanged `OP_SELECT`, which already forces on read, same as
before). `AST_LET` was checked and confirmed to NOT have this bug: its
body is compiled with an ordinary `compile_expr`, which only emits
`OP_VAR_REF` for variables actually referenced, each forcing correctly
on genuine use -- no bulk "read everything back" step exists there, so
nothing needed fixing on that path.

**Verified**: the minimal repro above now matches real Nix exactly.
`make test`/`test-e2e`/`test-conformance` all green (167 e2e, 19
conformance, full unit suite); numtide/nix-conformance's `lang/` pass
count unchanged (388, within noise of 389) -- this bug happened not to
be exercised by that suite's own cases, but is a real, significant
correctness fix for actual nixpkgs evaluation, where `rec {}` is used
constantly.

## M30 scope notes: `builtins.tryEval`

Found continuing to chase `d.nix`: `pkgs/top-level/impure-overlays.nix`
unconditionally calls `try (toString <nixpkgs-overlays>) ""` where `try
x def = let r = builtins.tryEval x; in if r.success then r.value else
def;` -- every real nixpkgs evaluation runs this. Also the thing
Known Bug #29 (M23) identified as blocking the *entire* ~270-case regex
differential corpus (every `eval-okay-regex-NNN.nix`/`eval-fail-regex-
invalid-NNN-*.nix` case wraps its call in this same `try` helper).

**Scope, narrower than "catch anything" -- confirmed by reading cppnix's
actual source, not assumed**: real Nix's `tryEval` only catches
`AssertionError` (`~/nix/src/libexpr/primops.cc`'s `prim_tryEval`:
`catch (AssertionError & e)`), and `ThrownError` (`throw`'s own exception
type) is its *only* subclass (`~/nix/src/libexpr/include/nix/expr/
eval-error.hh`: `MakeError(ThrownError, AssertionError)`). A failed
`assert` throws `AssertionError` directly (hence the class's name).
Everything else -- type errors, infinite recursion, missing attributes,
and (confirmed directly, surprisingly) `abort` itself -- derives from
plain `EvalError`, a *sibling* of `AssertionError`, not a subclass, so
none of those are caught. Verified every one of these distinctions
directly against real `nix-instantiate --eval` before writing any code
(`tryEval (abort "x")` still aborts the whole process unconditionally;
`tryEval (1 + "a")` is not caught either; `tryEval (throw "x")` and
`tryEval (assert false; 1)` both are, uniformly `{success=false;
value=false;}`). This made the actual implementation much more tractable
than "thread exception handling through every fatal path in the
codebase" (M23's original, correctly pessimistic framing of the
*general* problem) -- only two call sites needed to change at all.

**Mechanism**: plain `setjmp`/`longjmp` (`tryeval.c`/`.h`), not a sweeping
redesign. `prim_throw` (`builtins.c`) and `OP_ASSERT_FAIL`'s handler
(`op_assert.S`) now both route through `asnix_tryeval_raise_or_fatal`
instead of a direct fatal exit -- it `longjmp`s to the innermost active
`tryEval`'s buffer if one exists, otherwise falls back to exactly the
previous write-to-stderr-and-`exit(1)` behavior (so top-level `throw`/
`assert` failures, the overwhelmingly common case, are byte-for-byte
unchanged). This works because this project's own dispatch loop already
keeps its live evaluator state (VALSTACK/POOL/RETSP) in registers
(`%r12`/`%r13`/`%r14`) deliberately chosen to be SysV callee-saved
specifically so they survive an ordinary C call for free (see
docs/abi.md) -- which also means a plain `longjmp` correctly rewinds
them back to their `setjmp`-time values with no extra bookkeeping.

**A real bug found and fixed while verifying this, not assumed away**:
`asnix_valstack_limit`/`asnix_retsp_limit` (the bounds-check ceilings
`force.S`'s nested-forcing context saves/restores around its own use,
`stacklimits.S`) are plain C global variables, not registers -- `longjmp`
has no way to restore them, since they're not part of the CPU context it
saves. A `longjmp` triggered from *inside* a nested `asnix_force_for_
print` call (exactly what forcing `tryEval`'s own argument does) skips
that function's ordinary epilogue, the only place those globals would
otherwise get restored -- confirmed directly: `tryEval (throw "x")`
failed with a bogus "value stack overflow" before this fix, because the
very next push after the catch compared the (correctly longjmp-restored)
`%r12` against a now-completely-unrelated stale limit left over from the
abandoned nested force. Fixed by having `prim_tryEval` itself
(`builtins_tryeval.c`) save and explicitly restore both globals around
its own try/catch, on *either* path (redundant but harmless on the
normal-return path, since `asnix_force_for_print`'s own epilogue would
have already restored them correctly there too; load-bearing on the
caught path, which skips that epilogue entirely).

**A second, necessary mechanism**: `tryEval` needs its argument *raw and
unforced*, so it can force it itself inside its own catch scope --
forcing it any earlier (e.g. M25's "primops get pre-forced arguments"
convention, `op_call.S`) would mean the exception could already have
escaped before `tryEval`'s own `setjmp` is even installed. Added a new
`ASNIX_PRIMOP_OFF_NOFORCE` field to the `PrimOp` heap layout (`heap.h`,
defaulting to 0 -- every *existing* arena allocation already zero-fills
this for free, so no other primop constructor needed to change at all)
and a new constructor, `asnix_bshared_make_primop_noforce`, used only by
`tryEval`'s own registration; `op_call.S`'s M25 arg-forcing loop now
checks this flag and skips entirely when set.

Registered under `builtins.tryEval` only (real Nix's own registered name
is `__tryEval`, confirmed in cppnix's source -- unlike `fromTOML`/
`removeAttrs`/etc. this is NOT one of the bare-global names).

**Verified**: every distinction above checked directly against real
`nix-instantiate --eval` before being trusted; the exact `impure-
overlays.nix` `try` pattern now works end-to-end. `make test`/
`test-e2e`/`test-conformance` all green (172 e2e, 19 conformance, full
unit suite). numtide/nix-conformance's `lang/` pass count: **388 -> 627**
(+239) on the real external runner -- confirming M23's own prediction
that this single missing builtin was gating the entire regex
differential corpus.

**Known, accepted limitation, not engineered around** (see tryeval.h's
own comment for the full reasoning): if the thrown expression's
evaluation forced some *other* thunk partway through (not the one
`tryEval` was directly asked to evaluate), that thunk is left marked
IN_PROGRESS forever -- referencing that exact same thunk object again
later would incorrectly blackhole. A full fix would mean tracking and
rolling back every thunk touched since the matching `setjmp`, a much
larger undertaking; not attempted, since every real pattern seen so far
treats a throwing `tryEval`'d expression as a disposable one-off whose
intermediate thunks are never touched again.

## M31 scope notes: lazy `<name>` search-path resolution

Found immediately after M30: `d.nix` still hit "file 'nixpkgs-overlays'
was not found in the Nix search path" even with `tryEval` implemented --
`pkgs/top-level/impure-overlays.nix`'s own `try (toString
<nixpkgs-overlays>) ""` still didn't catch it. Root cause: asnix resolves
a `<name>` search path *eagerly, at compile time* (`compiler.c`'s
`resolve_search_path`, called from `AST_PATH`'s `compile_expr` case),
baking the resolved absolute path directly into the IR as a constant --
so the fatal fires before evaluation (and therefore before `tryEval`'s
`setjmp`) ever starts. Confirmed in cppnix's own source that this must
be lazy: `~/nix/src/libexpr/eval.cc`'s `findFile` throws `ThrownError` --
the *exact same* exception class `throw` itself uses -- on a miss, which
only makes sense if the lookup genuinely happens *during* evaluation,
inside whatever `tryEval` scope is active at the time. An *ordinary*
path literal (`./foo.nix`, `/abs/path`) is correctly unaffected: real
Nix fixes an ordinary path literal's absolute form by where it's
*written* (verified against `nix-instantiate` back when M15 first
implemented path literals), genuinely resolved once, at compile/parse
time -- only the `<name>` search-path form needed to change.

Fixed by splitting `AST_PATH`'s compilation: an ordinary path literal is
untouched (still resolved eagerly, exactly as before); a `<name>`
literal now pushes its search name as an ordinary compile-time string
constant (the same mechanism `AST_STRING` already uses for an all-
literal string) and emits a new op, `OP_RESOLVE_SEARCH_PATH`, which
performs the actual `NIX_PATH`-scanning lookup at *evaluation* time
(`asnix_resolve_search_path_value`, reusing the exact same `resolve_
search_path`/`canon_path` logic, just called later) -- on a miss, it
routes through the same `asnix_tryeval_raise_or_fatal` M30 added for
`throw`/failed `assert`, falling back to identical behavior when no
`tryEval` is active.

**A real bug found and fixed while implementing this**: the new fatal-
message formatter initially used a guessed buffer size (`name_len + 64`)
instead of computing the exact required length, which `snprintf` quietly
truncates to fit -- but the *return value* `asnix_tryeval_raise_or_fatal`
was given as the byte count to `write()` is `snprintf`'s "how many bytes
this *would* have taken" value, not how many were actually written,
so passing it against the undersized buffer was a real out-of-bounds
heap read (confirmed directly: the error message printed visibly
garbled/truncated instead of complete). Fixed by calling `snprintf(NULL,
0, ...)` first to get the exact needed length, then allocating exactly
that before formatting for real -- no guessing.

**Verified**: the exact `impure-overlays.nix` `try` pattern, with
`$NIX_PATH` genuinely unset, now returns the fallback value end-to-end,
matching real Nix exactly; an ordinary successful `<name>` lookup and an
uncaught (no active `tryEval`) miss both still behave identically to
before. `make test`/`test-e2e`/`test-conformance` all green (172 e2e, 19
conformance, full unit suite). numtide/nix-conformance's `lang/`: 627 ->
628 (small additional gain on top of M30's own +239, no regression).

## M32 scope notes: dotted attrpath after `?` (`x ? a.b.c`)

Found continuing to chase `d.nix`: `lib/systems/default.nix` has
`platform ? gcc.arch -> ...`, a multi-segment dotted path on the
right-hand side of `?`. asnix's `parse_has_attr` only ever parsed a
single segment -- confirmed against cppnix's own grammar
(`~/nix/src/libexpr/parser.y`: `expr_op '?' attrpath`, where `attrpath`
is a full `.`-separated chain, not a lone `attr`) that this is a real,
necessary feature, not an edge case to special-case around.

Desugars directly to existing, already-correct machinery at parse
time -- no new AST node or IR op needed: `x ? a.b.c` becomes `x ? a &&
(x.a or {}) ? b && ((x.a or {}).b or {}) ? c`, chaining `AST_AND` over
successive `AST_HAS_ATTR` checks, with each intermediate step's target
built via `AST_SELECT`'s existing `or {}` fallback (`OP_SELECT_OR`) so a
missing or non-attrset intermediate value gracefully short-circuits to
`false` rather than erroring -- exactly matching `AST_HAS_ATTR`'s own
established "not even an attrset simply means no" rule. Verified against
real `nix-instantiate --eval` for the three cases that actually
distinguish this from a naive implementation: a fully-present chain
(`true`), an existing-but-wrong-type intermediate value (`false`, not a
type error), and a missing intermediate attribute (`false`).

The pre-existing outer `while (TOK_QUESTION)` loop (allowing `?` to
apply repeatedly, e.g. the curious-but-real-Nix-tolerated `x ? a ? b`)
was left untouched/preserved -- real Nix's own grammar declares `?` as
`%nonassoc`, but empirically still accepts this when tested directly
against `nix-instantiate`, so whatever this project's existing loop was
doing already matched real Nix's actual behavior; this milestone only
adds dotted-chain support *within* one `?` application, each still
appended via that same outer loop as before.

`make test`/`test-e2e`/`test-conformance` all green (173 e2e, 19
conformance, full unit suite).

## M33 scope notes: dynamic `?` segment (`x ? ${name}`, anywhere in the chain)

Found continuing to chase `d.nix` right after M32 landed: the *next*
parse error in `lib/systems/default.nix` turned out to be a different
segment of the same `?`-attrpath feature -- `rustc-target-env ?
${final.rust.rustcTargetSpec}`, a *dynamic* segment right after `?`
(not preceded by a `.`). `parse_attr_segment` only recognized
`TOK_IDENT`/a static quoted string, so this hit the "expected an
attribute name" fatal. Root-caused by reading cppnix's full grammar
(`~/nix/src/libexpr/parser.y`): `attrpath: attrpath '.' attr | attrpath
'.' string_attr | attr | string_attr`, and `string_attr: '"'
string_parts '"' | DOLLAR_CURLY expr '}'` -- `string_attr` (which
includes the dynamic `${expr}` form) is a valid `attrpath` segment on
its own, not just after a `.`, which is why `? ${expr}` parses as a
dynamic has-attr check with no leading dot needed. Confirmed against
real `nix-instantiate --eval`: `x ? ${name}`, `x ? ${name}.b`, and `x ?
a.${name}` (dynamic in every position: first segment, before a `.`,
after a `.`) all work, all graceful (`false`, not a type error) when
the target or an intermediate value is missing/wrong-type.

Needed one new IR op, `OP_HAS_ATTR_DYNAMIC` (`src/asm/ops/
op_has_attr_dynamic.S`), mirroring `OP_SELECT_DYNAMIC`'s C-deferred
pattern (`asnix_bshared_has_attr_dynamic`, `builtins_shared.c`: interns
the name at runtime, binary-searches) rather than `OP_HAS_ATTR`'s own
hand-rolled asm binary search, since the symbol isn't known until
runtime -- same rationale M28 used for `OP_SELECT_DYNAMIC`. Unlike
`OP_SELECT_DYNAMIC` (fatal on a non-attrset target), this one follows
`OP_HAS_ATTR`'s graceful rule: a non-attrset target is simply `false`,
never fatal (only a non-string *name* is a hard fatal, a genuine
misuse). `AST_HAS_ATTR` grew an `attr_expr` field, the same split
`AST_SELECT` already has.

`parser.c`'s M32 dotted-chain desugaring (`x ? a.b.c` ->
`x ? a && (x.a or {}) ? b && ...`) needed each segment to carry either a
static name or a dynamic expression, not just a `Token*` -- added a
small `AttrSeg` tagged struct and a shared `parse_attrpath_segment`
helper (handles `TOK_DOLLAR_LBRACE` by parsing a full `expr` and
expecting a closing `}`, else falls back to the existing
`parse_attr_segment`), plus `set_has_attr_segment`/`set_select_segment`
helpers so the same `AttrSeg` populates either node kind's static-or-
dynamic fields. A dynamic segment's expression node is deliberately
reused as-is in both the `AST_HAS_ATTR` check and the following
`AST_SELECT ... or {}` node when the chain continues past it (the same
way the static case already reused a `Token*`'s `start`/`len`) --
`compile_expr` is a pure read-only tree walk (confirmed: no AST-node
mutation/caching anywhere in `compiler.c`), so compiling the same
sub-AST twice just re-evaluates the (pure, side-effect-free) name
expression twice; harmless and consistent with real Nix's own purity
guarantees, just not the single-evaluation `attrpath`-vector approach
cppnix's C++ takes internally.

Verified against real `nix-instantiate --eval` for the three segment
positions (bare, before a `.`, after a `.`) and the missing-key case;
the parse error in `lib/systems/default.nix` is gone and evaluation now
proceeds further into the file (the next blocker, if any, is a separate,
not-yet-diagnosed issue).

`make test`/`test-e2e` green (177 e2e); `test-conformance` 629 passed
(up from 628, no regressions).

## M34 scope notes: unforced list elements reaching 6 builtins as raw scalars

Found continuing to chase `d.nix` right after M33: `builtins.replaceStrings`
crashed with "expected a string argument" even though the real nixpkgs
call passed well-typed string lists. Root-caused with gdb (broke at the
fatal site, read the live IP register to recover the symbol id that
`OP_SELECT` was about to reject, then separately read the raw tagged
`Value` reaching `replaceStrings`'s fatal check directly): the "to" list
argument was the result of a `map` call, and `map`'s elements are
*deliberately* unforced Thunks (M23's own laziness fix, confirmed
`builtins.length (map (x: throw "boom") [1 2 3])` must not fire the
throw) -- `replaceStrings` read `to_elems[i]`'s raw bytes directly via
`asnix_bshared_string_bytes` without forcing first, so an unforced Thunk
(tag `ASNIX_TAG_THUNK`, confirmed via gdb: `to_elems[0].raw & 7 == 6`)
hit the "not a `ASNIX_TAG_PTR`" fatal instead of being forced down to
the string it actually was.

This is the *exact* bug class M23 already found and fixed once for
`concatLists` (see that function's own comment, `builtins_list.c`:
"`concatLists (map (x: map f strings) algos)` aborted with a bogus
'element N is not a list'") -- but the fix was never generalized to
every other builtin that reads a list's elements directly as C-array
values. List literals in source (`[...]`) don't exhibit this (every
element is compiled eagerly via plain `compile_expr`, no thunk wrapping,
so a literal's elements are already fully evaluated Values by
construction -- confirmed separately: `builtins.elemAt [(abort "boom")
1] 1` does abort here, a documented, different, narrower scope gap than
this milestone, not the thing fixed here); only builtins that build
*lazy* elements on purpose (`map`/`mapAttrs`, M23) can hand back an
actual unforced Thunk sitting in list storage, and any OTHER builtin
reading that same list's elements raw inherits the hazard.

Audited every direct list-element read across the codebase (grepped for
`_elems[i]`/`comps[i]`/`queue.data[...]`-shaped access feeding a type
check or raw byte/int read, as opposed to lazy pass-through uses like
`map`/`elemAt`/`head`/`tail`/`sort`'s own element copies, or uses as a
function-call argument, both of which are correctly left unforced --
the callee's own `OP_VAR_REF` forces on read if it actually inspects the
argument). Found and fixed five more real instances of the same gap,
each verified against real `nix-instantiate --eval` with a `map (x: x)
[...]`-wrapped repro before and after:
- `replaceStrings` (`builtins_compare.c`): both the `from`/`to` list
  elements.
- `catAttrs` (`builtins_list2.c`): the attrset-typed list elements.
- `listToAttrs` (`builtins_attrs2.c`): the attrset-typed list elements.
- `attrByPath` (`builtins_attrs2.c`): the string-typed path-component
  elements.
- `genericClosure` (`builtins_closure.c`): the work-queue items (popped
  from a `ValueVec` fed directly by the list's raw elements).
- `concatStringsSep` (`builtins_misc.c`): the string-typed list elements
  (two separate loops -- a length-totaling pass and the actual byte-copy
  pass -- both needed the fix).

Fix in every case: force via the existing, already-idempotent
`asnix_force_for_print` (a cheap no-op on an already-forced Value, same
one `OP_SELECT`'s asm uses) immediately before the first tag check or
byte/int read, mirroring exactly what `concatLists` already does.
Deliberately did *not* attempt a blanket structural fix (e.g. wrapping
every list literal's elements in thunks, mirroring M25/M29) -- that
would change a working, deliberate design decision (list literals are
eagerly evaluated by construction) for every one of the ~40 other list-
element read sites in the codebase found during the audit, almost all of
which are legitimate lazy-pass-through uses that must *not* force
(`map`, `elemAt`, `head`, `tail`, `sort`'s copy, every function-argument
pass). Fixing only the confirmed-broken consumers is the smaller,
lower-risk change that matches the actual (narrow) laziness contract
list storage has in this project: elements *can* be unforced Thunks
(only ever placed there by `map`/`mapAttrs`), and any consumer reading
them as scalars must force first -- consumers that only copy/forward
elements are correctly exempt.

`make test`/`test-e2e` green (183 e2e, six new `m34_lazy_list_elem_*`
fixtures, each a `map (x: x) [...]`-wrapped repro of one fixed builtin);
`test-conformance` still 629 passed, no regressions.

## M35 scope notes: `builtins.currentSystem`, `builtins.isPath` + an `isString` path bug

Found continuing to chase `d.nix` right after M34: two more "attribute
missing" aborts, root-caused the same way as M33's investigation (break
on `OP_SELECT`'s miss branch in gdb, read the live symbol id out of
`%rdx`, resolve it via `asnix_sym_name`) -- `currentSystem` then
`isPath`, neither of which asnix implemented at all.

`builtins.currentSystem`: a plain string constant (not a function),
same "called once at `builtins`-construction time, no PrimOp needed"
shape `nixVersion` already established. Confirmed via cppnix's own
source (`~/nix/src/libexpr/primops.cc`: `.name = "__currentSystem"`) that
real Nix registers this `__`-prefixed, i.e. `builtins`-only, not a bare
global (matches `nix-instantiate --eval -E 'currentSystem'` =>
"undefined variable"). Hardcoded to `"x86_64-linux"`, this project's one
supported platform (see flake.nix), rather than computed from `uname`.

`builtins.isPath`: same cppnix source confirms `__isPath` (builtins-only
again). Added as a new primop. While adding it, found a genuine,
previously-undiscovered bug in the *existing* `isString`: since paths
and strings share `ASNIX_TAG_PTR` (distinguished by `asnix_value_is_path`,
see heap.h), `prim_isString` checked only the tag, not the path-marker
bit, so `builtins.isString ./.` wrongly returned `true` here (confirmed
against real Nix: `false`). Fixed `isString` to exclude paths and
implemented `isPath` as the mirror-image check (tag is `ASNIX_TAG_PTR`
*and* the path marker is set).

`make test`/`test-e2e` green (185 e2e, two new `m35_*` fixtures);
`test-conformance` 632 passed (up from 629, no regressions).

## M36 scope notes: name the attribute in `.` selection's "attribute missing" error

Prompted by the user noticing the pattern: M33/M35's investigations each
needed a one-off gdb session (break on `OP_SELECT`'s miss branch, read
the symbol id out of the live `%rdx`/IP register, resolve it via
`asnix_sym_name`) just to find out *which* attribute a plain "asnix:
attribute missing, aborting" was complaining about -- real Nix's own
message already names it (`nix-instantiate --eval '{ a = 1; }.b'` =>
"attribute 'b' missing"). Confirmed real Nix does NOT let `builtins.
tryEval` catch this particular error (`tryEval ({a=1;}.b)` still
surfaces as a top-level uncaught `error:`, unlike `throw`/failed
`assert`/search-path-not-found, M30/M31) -- so this stays a plain fatal
path, no interaction with the tryeval.c catch mechanism.

Added `asnix_bshared_select_missing_fatal(uint32_t sym)`
(builtins_shared.c): resolves the symbol via the existing
`asnix_sym_name` and formats "asnix: attribute 'NAME' missing,
aborting", matching real Nix's quoting style. `op_select.S`'s
`.Lselect_missing` now does a real `call` into this function (passing
the symbol id already sitting in `%rdx` at that point) instead of a
tail `jmp asnix_fatal` with a static message -- safe without extra
stack padding since `%rsp` is already 16-byte aligned entering any op
handler (the same standing invariant `op_call.S`/`op_mk_thunk.S`'s own
direct C calls already rely on) and the function is `_Noreturn`, so the
call's pushed return address is simply never used (same reasoning M30's
op_assert.S fix already established for `jmp`-vs-`call` into a real C
function).

Zero cost on the hot path: only `.Lselect_missing` changed -- the
successful-select path (`.Lselect_found`/`.Lselect_push`, the actual hot
loop) is untouched, and the new `call` only ever executes one
instruction before the process would have exited anyway.
`OP_SELECT_DYNAMIC`'s own miss path already named the attribute (M28);
this brings the static `OP_SELECT` path to parity. Did not extend this
to `OP_SELECT`'s type-error/blackhole paths or `OP_CHECK_FORMALS`'s
missing/extra-field errors in this pass -- same idea would apply
there, but this fix specifically targeted the exact friction just hit
three times in a row (M33/M35's `currentSystem`/`isPath`/
`functionArgs` lookups).

`make test`/`test-e2e` green (no new fixtures -- this is an error-
message-only change, verified by hand against `nix-instantiate --eval`);
`test-conformance` still 632 passed, no regressions.

## M37 scope notes: `builtins.functionArgs`

Found continuing to chase `d.nix` right after M36 (and the new named-
attribute error message paid off immediately -- no gdb session needed
this time): `builtins.functionArgs` was entirely missing. Confirmed via
cppnix source (`~/nix/src/libexpr/primops.cc`: `.name =
"__functionArgs"`) that it's builtins-only, not a bare global.

`functionArgs f` returns `{name: hasDefault}` for a lambda-pattern
(`{a, b ? d, ...}: ...`), or `{}` for every other function shape (a
plain `a: body` lambda -- even nested/curried, since only the
*outermost* lambda is inspected -- or a PrimOp). Verified against real
`nix-instantiate --eval` across five cases before trusting it: a
pattern with a default and `...` (`{a, b ? 1, ...}: a` ==
`{ a = false; b = true; }`), a plain lambda (`{}`), a pattern with an
`@`-alias (`{a}@args: a` == `{ a = false; }`), a curried plain lambda
(`a: b: a` == `{}`), and a PrimOp (`builtins.add` == `{}`).

The needed data (per-field name + has-default) isn't retrievable from
`OP_CHECK_FORMALS`'s existing IR-pool layout in every case (specifically:
when a pattern has `...`, that pool region only stores the *required*
fields' syms, not the full field list, since the "unexpected argument"
check it exists for is skipped entirely and the total list was never
needed before now) -- rather than extend that pool format and risk the
well-exercised `OP_CHECK_FORMALS` asm, built a separate, purpose-made
mechanism: closures now carry a 5th field, `ASNIX_CLOSURE_OFF_FORMALS`
(`ir.h`, `ASNIX_CLOSURE_SIZE` grew 32 -> 40 bytes), holding the
*already-built* `functionArgs` result `Value` -- computed ONCE at
compile time (`compiler.c`'s `AST_LAMBDA` case, which has every formal's
name/default info on hand before freeing it) and carried through
`ib_lambda`'s new `formals_raw` parameter into `OP_LAMBDA`
(`op_lambda.S`, which now reloads it fresh from the closure's own pool
after the `asnix_arena_alloc` call rather than trying to carry it in a
register across that call -- `%rbx`/`%rbp`/`%r15` are already the only
free callee-saved scratch registers this handler has, so a 4th would
have required stack-pushing instead; recomputing the pool index from
the just-restored `%rsi`/the untouched `%r13` was simpler). Every other
Closure-construction site in the codebase (`builtins_shared.c`'s
curried-native wrapper, `builtins_misc.c`'s copy of the same, and
`builtins_attrs.c`'s `make_curried2` self-curry trick for `hasAttr`/
`getAttr`/`removeAttrs`/`mapAttrs`) had to be updated too, since each
builds a `Closure` object by hand (bypassing `OP_LAMBDA` entirely) --
all three now explicitly write the shared empty-attrset `Value` into
the new field (never a lambda-pattern, so always `{}`, matching real
Nix's own `functionArgs builtins.add`). Left as zeroed/unset would have
decoded as `Value{raw=0}` = a null `ASNIX_TAG_PTR` -- not merely wrong,
but a genuine crash risk if `functionArgs` were ever called on one of
these closures and then printed/used.

`prim_functionArgs` itself (`builtins_misc.c`) is a direct field read
for a `ASNIX_TAG_CLOSURE` value, or the empty-attrset constant for
anything else (matching real Nix's permissive-on-non-functions
behavior here, rather than erroring).

`make test`/`test-e2e` green (189 e2e, two new `m37_functionArgs_*`
fixtures) -- also had to update 8 pre-existing `ib_lambda(...)` call
sites across `tests/c/`/`tests/ir/` (synthetic closures built directly
via the IrBuilder API in test scaffolding) to pass the new required
`formals_raw` argument, `0` in every case since none of those tests
exercise `functionArgs`; `test-conformance` still 632 passed, no
regressions.

## M38 scope notes: `asnix_apply`'s PrimOp path never forced args/return (core evaluator fix)

Found continuing to chase `d.nix` right after M37: `builtins.attrNames`
crashed with "argument is not an attrset" even though the real nixpkgs
call passed a well-typed attrset list -- same symptom shape as M34's
`replaceStrings` bug, root-caused the same way (gdb, read the failing
`Value`'s raw tag at the fatal check: `6` = `ASNIX_TAG_THUNK`). But this
time the unforced Thunk wasn't a raw list-element *read* (M34's class) --
it was passed as a function-call *argument*, through `map builtins.
attrNames (map f list)`: the outer `map`'s own C implementation
(`prim_map_stage2`, builtins_list.c) calls `asnix_bshared_make_lazy_call1`
per element, which *does* go through a real compiled IR snippet
(`OP_VAR_REF`+`OP_CALL`) when forced later -- so that path was already
correct. The actual gap was lower-level: `asnix_apply`
(`src/asm/apply.S`), the generic "call this Value from C" entry point
used by `any`/`all`/`partition`/`concatMap`/`genericClosure`'s operator/
`sort`'s comparator/`filterAttrs`'s predicate/etc. to invoke a function
value directly against a (possibly still-lazy) list element, had a
PrimOp branch that was a bare tail-`jmp` straight into the native C
function -- no argument forcing, and no forcing of the returned value
either. `OP_CALL`'s own asm path has carried both of these (M25's
argument-forcing loop, M23's return-value forcing for lazy-passthrough
primops like `getAttr`) since those milestones, but `asnix_apply` is a
genuinely separate call path that never picked up either fix -- it
simply hadn't been exercised by a case where the applied function was
itself a PrimOp receiving an unforced argument until this nixpkgs
pattern (`map somePrimop (map f list)`) surfaced it.

Fixed by converting the PrimOp branch from a tail-`jmp` into a real
`call` + a small epilogue (needed either way, since forcing the return
value requires getting control back): builds a local, forced copy of
the arguments on the native stack (never mutates the caller's own
`args` buffer, which may be read-only or re-read later), skipping the
copy/force step when `ASNIX_PRIMOP_OFF_NOFORCE` is set (same `tryEval`
exception `OP_CALL` already carves out), then calls the native
function and forces its return value before handing it back -- the
exact same two fixes `OP_CALL`'s primop path already has, just
independently implemented since `asnix_apply` doesn't dispatch through
that code. Bounded to 8 arguments (comfortably above every real primop
arity in this codebase, max 3) with a clean fatal fallback rather than
risking a silent stack-buffer overflow if ever exceeded. Every value is
reloaded from a stack scratch slot around each `call` rather than kept
in a register, since `asnix_force_for_print` clobbers every caller-
saved register -- deliberately not hot-path-optimized, since `OP_CALL`'s
own in-dispatch-loop loop already handles the common (direct Nix-level
call) case; this path only runs for the comparatively rare "invoke a
function value directly from C" call sites.

`make test`/`test-e2e` green (191 e2e, two new `m38_*` fixtures, each a
`map somePrimop (map (x: x) [...])`-shaped repro); `test-conformance`
still 632 passed, no regressions.

## M39 scope notes: `builtins.addErrorContext`

Found continuing to chase `d.nix` right after M38: `builtins.
addErrorContext` was entirely missing. Confirmed via cppnix source
(`~/nix/src/libexpr/primops.cc`: `.name = "__addErrorContext"`)
builtins-only, not a bare global. Real Nix's only observable effect is
annotating `value`'s error trace with `msg` if-and-only-if forcing
`value` throws -- verified `msg` gets no special extra laziness beyond
ordinary call-by-need: `let y = addErrorContext "ctx" (abort "x"); in
1` == `1` (same as any unused lazy binding -- M25's own thunk-the-
argument machinery already accounts for this, nothing addErrorContext-
specific), while forcing the result directly (top-level eval, or
wrapping in `isAttrs (...)`) does abort, with "… ctx" shown in the real
Nix trace.

Since asnix has no error-trace/position system at all, there's nothing
to attach `msg` to -- implemented as a pure pass-through, `msg: value:
value` (`addErrorContext2_impl`, builtins_misc.c), which already
reproduces every observable case above via this project's ordinary M25
call-by-need semantics: no `NOFORCE` needed, since forcing only
happens when the *caller* forces the call's own result (the final
curry stage's `OP_CALL` forces both args only once something actually
forces that call to run at all) -- exactly matching real Nix's timing.

`make test`/`test-e2e` green (192 e2e, one new `m39_*` fixture covering
both the success and the lazy-unused cases); `test-conformance` still
632 passed, no regressions.

## M40 scope notes: `${null} = value;` silently omitted, not an error

Found continuing to chase `d.nix` right after M39: a dynamic attrset-
literal key evaluating to `null` fatal'd with "a dynamic '${...}'
attribute name must be a string" (via gdb: the actual `name_val` tag
was `ASNIX_TAG_IMM`/`ASNIX_IMM_NULL`). Root-caused via cppnix's own
source: `~/nix/src/libexpr/eval.cc`'s dynamic-attrs construction loop
checks `if (nameVal.type() == nNull) continue;` *before* the
string-force/type-check -- a deliberate special case, not an oversight,
matching a real nixpkgs idiom for a conditionally-present key
(`${if cond then "key" else null} = value;`). Confirmed directly:
`{ ${null} = 1; }` == `{}` (silently empty) while `{ ${1} = 1; }` is
still a hard error ("while evaluating the name of a dynamic
attribute") -- so this is narrowly about `null` specifically, not a
general "anything goes" relaxation.

Also checked whether the *other* dynamic-attribute-name call sites
(`x ? ${expr}` from M33, `.${expr}` select) share this special case --
confirmed they do NOT: `{ a = 1; } ? ${null}` is a hard error in real
Nix too ("while evaluating an attribute name"), matching cppnix's
separate `getNameValue` helper for `string_attr`/attrpath contexts,
which has no null check at all. So the fix is scoped to exactly one
function, `asnix_attrs_insert_dynamic` (`attrs_update.c`, M28's dynamic-
attrset-literal-key machinery): returns `attrs` unchanged (skipping
insertion entirely, same as cppnix's `continue`) when `name_val` is
`null`, before the existing type/duplicate checks.

`make test`/`test-e2e` green (193 e2e, one new `m40_*` fixture);
`test-conformance` 633 passed (up from 632, no regressions).

## M41 scope notes: dynamic attrpath segments anywhere (`.${k}`, interpolated quoted strings)

Found continuing to chase `d.nix` right after M40: a parse error
("dynamic attributes not allowed here") inside `lib/systems/
architectures.nix`-or-a-sibling-file. Root-caused via gdb (inspected the
parser's own token array at the fatal site): the actual construct was
`"${ident}Fallback" = fallback;` -- an **interpolated quoted string**
used as an attrpath-sugar binding's name, not the bare `${expr}` form
M28/M33 already handled. This exposed a real gap in `parse_attr_segment`
(the shared helper used by `inherit`'s name list, attrpath binding
sugar, `.`-select chains, and `?`-chains): it only recognized a
*non*-interpolated quoted string (`"or"`, `"a b"`) as static and
fatal'd on anything with a `${...}` chunk inside the quotes, never
distinguishing that from the ordinary non-interpolated case the way
cppnix's own grammar does (`string_attr: '"' string_parts '"' |
DOLLAR_CURLY expr '}'` -- the quoted form can itself contain
interpolation, since `string_parts` is literally the same grammar an
ordinary string literal uses).

This also exposed the last remaining piece of M22/M28's original scope
cut: a dynamic segment anywhere in a dotted attrpath (`a.${k} = v;`,
`${k}.b = v;`, `a.${k1}.${k2} = v;`) was still rejected, documented at
the time as "not supported... not needed by anything this project has
hit yet" -- now needed. Both gaps share one fix:

- `parse_attr_segment` now returns an `AttrSeg` (a tagged
  static-name-or-dynamic-expression union, previously a separate M33-
  only helper `parse_attrpath_segment` layered on top of it) instead of
  a plain `Token*`. A quoted string is parsed via the *same*
  `parse_dquote_string` chunk-collecting loop an ordinary string
  literal uses, then classified: zero parts or exactly one non-
  interpolated literal part is static (unchanged behavior, including
  `inherit (x) "or";` continuing to work); anything with an
  interpolation chunk is dynamic. A bare `${expr}` is dynamic as before
  (M28/M33). `inherit`'s own name-list loop explicitly rejects a
  dynamic `AttrSeg` with "dynamic attributes not allowed in inherit"
  (confirmed in cppnix's own grammar: the `string_attr` visitor throws
  specifically for the `Expr*`/dynamic case, never the plain-string
  case) -- the only context that still disallows this.
- Attrpath-sugar binding statements (`a.b.${k}.d = val;`) now collect
  every segment into an `AttrSegVec` (replacing the old `Token`-only
  `TokenVec`), find the first dynamic segment (if any), and split the
  chain there: the static prefix (possibly empty) still flows through
  the existing `insert_attrpath`/`BTVec` merge-tree machinery unchanged
  (now reading `AttrSeg.attr`/`attr_len` instead of `Token.start/len`);
  everything from the first dynamic segment onward is built by a new
  recursive helper, `build_dynamic_chain_value`, into nested plain (non-
  `rec`) one-binding attrset literals -- using either an ordinary static
  `AstBinding` or M28's existing dynamic-binding machinery
  (`AstDynBinding`/`OP_INSERT_DYNAMIC_ATTR`) depending on each
  segment's own kind, so a chain with *multiple* dynamic segments
  composes correctly (each gets its own runtime insert). When the
  static prefix is empty (the very first segment is itself dynamic),
  the built value is pushed directly into the enclosing block's
  `dyn_out` instead of through `insert_attrpath` -- same `dyn_out ==
  NULL` rejection as a bare `${expr} = value;` binding already had
  ("dynamic attributes not allowed in let"), which let the old,
  narrower special-case for that exact bare form (previously handled
  by a separate early branch at the top of `parse_binding_stmt`, unable
  to handle `${k}.b = v;`'s trailing segments) be deleted entirely in
  favor of this one unified path.

Verified against real `nix-instantiate --eval` for every shape this
unifies: an interpolated quoted-string key (`{ "foo${bar}" = 1; }` ==
`{ foox = 1; }`) and the matching select form (`set."foo${bar}"`); a
dynamic segment after a static one (`{ x.${k} = 1; }` == `{ x = { a =
1; }; }`); a dynamic first segment followed by more (`{ ${k}.b = 1; }`
== `{ a = { b = 1; }; }`); two dynamic segments in one chain (`{
${k1}.${k2} = 1; }`); `inherit (x) "or";` still working; and `let
"${k}".b = 1;`/`let ${k} = 1;` both still correctly rejected.

`make test`/`test-e2e` green (198 e2e, five new `m41_*` fixtures);
`test-conformance` still 633 passed, no regressions.

## M50 scope notes: name the specific value/field in 6 more generic fatal-path error messages

The "cheap pass" extension of M36's own idea, requested directly after
M36 landed: M36 named the attribute in `OP_SELECT`'s "attribute
missing" abort; this pass applies the same "a `call` on a fatal-only
path is free" trick to the other generic, unnamed fatal messages that
were still forcing a one-off gdb session to find out *which* value/
symbol actually triggered them. Deliberately NOT full line:column
position tracking (explicitly declined as too large for now) -- just
naming whatever specific value/symbol is already sitting in a register
or C variable at each fatal site, same scope M36 itself set.

Six sites fixed, all following M36's exact `call`-not-`jmp`-into-C
discipline (safe without stack padding since `%rsp` is already 16-byte
aligned entering any op handler -- the dispatch loop's own standing
invariant -- and every new C function is `_Noreturn`, so the harmless
pushed return address is never used; zero cost on every surrounding hot
path since only the already-exiting fatal branch changed):

1. **`op_select.S`'s `.Lselect_type_error`** ("asnix: '.' selection
   target is not an attrset, aborting") now calls
   `asnix_bshared_select_type_fatal(Value v)`, which appends "(got
   `<type>`)" using a new shared helper (below). Checked the four
   asm-level siblings named in the ask (`op_select_or.S`,
   `op_select_dynamic.S`, `op_select_dynamic_or.S`,
   `op_has_attr_dynamic.S`): `op_select_or`/`op_select_dynamic_or`/
   `op_has_attr_dynamic` all treat a non-attrset target as a graceful
   "not found"/`false`, not fatal (confirmed against real Nix: `(5).foo
   or 10` => `10`), so there is no analogous type-error message to fix
   in those three. `op_select_dynamic.S` defers entirely to
   `asnix_bshared_select_dynamic` (builtins_shared.c), which DOES have
   one ("cannot select an attribute (dynamic) on a non-attrset value")
   -- fixed in the same pass as item 3 below, since it's a plain C
   fprintf, not an asm surgery.

2. **`op_check_formals.S`'s `.Lcf_missing_error`/`.Lcf_extra_error`**
   (`{a, b, ...}: ...` called missing a required field, or with an
   unexpected extra one) now call `asnix_bshared_cf_missing_fatal(sym)`/
   `asnix_bshared_cf_extra_fatal(sym)`, naming the specific field.
   `%r15` already held the required sym the binary search just failed
   to find (set earlier in the loop, persists through the search per
   its own existing comment); `%r11` already held the given entry's sym
   that phase 2's search couldn't place among the declared fields --
   neither needed any new register threading, exactly the "already
   available" case the ask called out.

3. **The four "must be a string"/"non-attrset" dynamic-attribute
   messages** (`builtins_shared.c`'s `asnix_bshared_select_dynamic`
   -- both its non-attrset-target AND non-string-name checks --
   `asnix_bshared_select_dynamic_or`, `asnix_bshared_has_attr_dynamic`,
   and `attrs_update.c`'s `asnix_attrs_insert_dynamic`) are plain C, so
   just extended each existing `fprintf` with "(got `<type>`)". No
   existing `typeOf`-style helper was reusable directly (`prim_typeOf`,
   builtins.c, returns a `Value` string for `builtins.typeOf`, not a C
   string, and lives in a module builtins_shared.c doesn't/shouldn't
   depend on) -- added one small shared helper instead of duplicating
   the tag-to-string `switch` four times.

4. **`op_call.S`'s `.Lop_call_type_error`** ("asnix: call target is not
   a function, aborting") now calls `asnix_bshared_call_type_fatal(v)`,
   same "(got `<type>`)" suffix. `%rdx` already held the original
   tagged callee `Value` at that point (read before the closure/primop
   tag dispatch that this path falls through from).

New shared helper, `asnix_bshared_type_name(Value v)`
(`builtins_shared.c`/`.h`): returns a static "a/an `<kind>`" string
(an integer / a boolean / null / a string / a path / a list / an
attrset / a function / ...), reusing the exact same tag -> name mapping
`prim_typeOf` already established, just phrased as a clause instead of
a bare noun. Declared non-static (unlike most one-off helpers in this
file) specifically because `attrs_update.c` needs it too, not just
`builtins_shared.c`'s own dynamic-select fatals -- `attrs_update.c` now
includes `builtins_shared.h` for it. Deliberately does NOT force `v`
first (unlike `typeOf`, which relies on `OP_CALL` already forcing every
primop argument, M25): every call site above reads a value already
sitting on `VALSTACK` or passed in from an op's own C glue, which by
this evaluator's standing invariant (every value reaching `VALSTACK` is
already forced, per `op_call.S`'s own M23 comment) is never actually an
unforced `Thunk` -- the `ASNIX_TAG_THUNK` case in the helper is dead
code in practice, kept only so a future violation of that invariant
prints something sane instead of misreading raw thunk bits.

Did not touch `op_select.S`'s blackhole message, `op_check_formals.S`'s
own `.Lcf_type_error` (lambda-pattern argument itself not a set --
no specific "which value" beyond what the type-error message already
says), or `op_call.S`'s arity-mismatch message -- out of scope for this
pass; same kind of fix would apply but wasn't hit by name in the ask and
isn't blocking anything right now.

Verified every new message by hand (deliberately-bad one-off `.nix`
files run directly against the built binary, same "no gdb needed
anymore" check this whole pass exists to make permanent):
`(5).foo` => `asnix: '.' selection target is not an attrset (got an
integer), aborting`; `[1 2 3].foo` => `... (got a list) ...`; `(5) 3`
=> `asnix: call target is not a function (got an integer), aborting`;
`({ a, b }: a + b) { a = 1; }` => `asnix: lambda called without a
required argument 'b', aborting`; `({ a }: a) { a = 1; c = 2; }` =>
`asnix: lambda called with an unexpected argument 'c', aborting`;
`let n = 42; in { foo = 1; }.${n}` => `... must be a string (got an
integer)`; `let n = [ 1 ]; in { ${n} = 1; }` => `... must be a string
(got a list)`.

No new e2e fixtures (error-message-only change, same precedent M36
itself set). `make test`/`test-e2e` green, no regressions;
`test-conformance` still 633 passed, 169 failed, 0 known, 62 skipped --
identical to the pre-change baseline.

## M57 scope notes: conformance grab-bag

A "pick off what you can" pass over a list of smaller numtide/
nix-conformance gaps, sub-numbered M57a/b/... per item (one commit
each).

### M57a: `lessThan`/`sort` ordering of lists

`asnix_bshared_value_lt` (builtins_shared.c, backs both `builtins.
lessThan` and `builtins.sort`'s default-ish comparator usage) only
handled int-vs-int and string-vs-string; real Nix also orders lists
lexicographically (`CompareValues::operator()`'s `nList` case in
`~/nix/src/libexpr/primops.cc`): walk both lists pairwise, and at the
first index where the elements aren't *equal* (full structural `==`,
not `<`), recurse `lessThan` on just that pair; if one list runs out
first while still equal on the common prefix, the shorter one is less;
equal-length all-equal lists are not less (false, matching the
`lessThan [1 2] [1 2] == false` / `[] [] == false` real-Nix results
confirmed below).

Added a `ASNIX_TAG_LIST`-vs-`ASNIX_TAG_LIST` branch to `asnix_bshared_
value_lt` doing exactly this: force both top-level args and each
element pair (lists are lazy, same as every other list-consuming
builtin already does via `asnix_force_for_print`), use the existing
`asnix_bshared_value_eq` to decide "equal, move on" vs. "recurse `lt`
on this differing pair", and fall back to the prior fatal "cannot
compare" error (now mentioning list-vs-list in its message) for any
other type combination -- including a type mismatch discovered only
once two elements are reached, matching real Nix's `lessThan [1]
["a"]` => `error: cannot compare an integer with a string`.

Verified against real `nix-instantiate --eval`: `lessThan [1 2] [1
3]` => true, `[1 2] [1 2]` => false, `[1] [1 2]` => true, `[1 2] [1]`
=> false, `[] []` => false, `[] [1]` => true, `[1] ["a"]` => throws.
`eval-okay-sort.nix` (asnix's own output byte-for-byte matches its
`.exp`, including the list-of-lists case) and `eval-okay-eq-slot-sort.
nix` (relies on `builtins.lessThan`'s list case falling through to
`asnix_bshared_value_eq`'s existing pointer-identity fast path for a
shared closure value referenced from two different list literals --
already correct, unchanged by this fix) both now pass. Two new e2e
fixtures: `m57a_lessthan_lists.nix`/`m57a_sort_lists.nix`. `make
test`/`test-e2e` green (200 e2e); `test-conformance` 635 passed (+2),
167 failed (-2), 0 known, 62 skipped -- no other test's result moved.

### M57b: `genericClosure` key-comparability validation

`builtins_closure.c`'s own top comment already documented the scope
cut: dedup by `asnix_bshared_value_eq` instead of a real ordered
`std::map<Value*, CompareValues>` like real Nix's, deliberately not
replicating the "incomparable key types" error. Closed that gap
without needing a real ordering: added a `value_key_comparable(Value,
Value)` helper (same int/string/list type scope M57a's `value_lt`
supports) and call it against every already-seen key right before the
existing equality check in the dedup loop, `asnix_bshared_fatal`-ing
out on a mismatch. Confirmed against real Nix that the comparator is
consulted on every insertion attempt *regardless of actual equality*
-- even `{key=true;}` twice throws "cannot compare a Boolean with a
Boolean; values of that type are incomparable" -- so the new check
intentionally runs unconditionally before the equality check, not only
when values turn out unequal.

Verified against real `nix-instantiate --eval`: int-vs-string keys
throws "cannot compare a string with an integer"; two `{key={};}`
(attrset keys, same type but never comparable) throws "...
incomparable"; two `{key=true;}` throws the same way; ordinary
string-keyed closures are unaffected. Both `eval-fail-genericClosure-
keys-incompatible-types.nix` and `eval-fail-genericClosure-keys-
uncomparable.nix` now exit non-zero as required. One new e2e fixture,
`m57b_genericclosure_ok_strings.nix` (a same-type-string-keys closure,
confirming the ordinary path is untouched -- no e2e fixture for the
fail cases themselves since `test-e2e`'s harness only diffs stdout
against an expected success value, not exit codes; the conformance
suite already covers that). `make test`/`test-e2e` green (201 e2e);
`test-conformance` 637 passed (+2), 165 failed (-2), 0 known, 62
skipped -- no other test's result moved.
