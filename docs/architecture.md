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
| M42 — `__functor` protocol (calling an attrset via a `__functor` attribute) | done (see below) |
| M43 — lambda-pattern-vs-attrset disambiguation miscounts `${` in default values | done (see below) |
| M44 — `builtins.placeholder`/bare `placeholder` | done (see below) |
| M45 — `builtins.toFile` | done, **scoped** (see below) |
| M46 — `<nix/fetchurl.nix>` virtual-corepkgs special case | done, **scoped** (see below) |
| M47 — list-to-string coercion (`toString`/derivation env vars) | done, **scoped** (see below) |
| M48 — `outputHashAlgo = ""` wrongly rejected as an unknown hash algorithm | done (see below) |
| M49 | not started |
| M50 — name the specific value/field in 6 more generic fatal-path error messages | done (see below) |
| M51 — float literals: lexer, heap-boxed Value kind, `+`/`-`/`*`/`/`/comparison promotion, printer, `floor`/`ceil`/`isFloat`, `toJSON`/`fromJSON`/`fromTOML` float support | done, **scoped** (see below) |
| M52 — fix `with`-frame leaking into a sibling expression (core evaluator fix) | done, **scoped** (see below) |
| M53 — `foldl'`/`replaceStrings` forced values real Nix never would (plus a core list-literal laziness fix) | done (see below) |
| M54 — a batch of missing builtins: `bitAnd`/`bitOr`/`bitXor`, `trace`, `zipAttrsWith`, `toXML`, `convertHash`, `getContext`/`appendContext`, `scopedImport` | done, **scoped** (see below) |
| M55 — regex engine gaps triage (`builtins.match`/`builtins.split`, 39 conformance failures) | investigated, **no fix** (see below) |
| M56 — integer-overflow detection (`+`/`-`/`*`/`/`, `builtins.add/sub/mul/div`) + `fromTOML` validation gaps (bare/incomplete time literals, calendar-range checks, duplicate table headers, leading-zero/double-underscore integers, invalid bare tokens, embedded NUL bytes, integer-literal overflow) | done (see below) |
| M57 — conformance grab-bag (9 sub-items a-i): `lessThan`/`sort` on lists, `genericClosure` key-comparability, `__overrides` propagation, `toJSON` UTF-8 validation, `import`'s `__toString` coercion, `compareVersions`' int-overflow quirk, path-concat canonicalization, import symlink resolution, CLI `--arg`/`--argstr`/`-A` | done, **scoped** (see below; function/derivation-equality, `-I`, and context-attr-name skipped) |
| M58 — `derivation` is a real Nix-level wrapper around `derivationStrict`, giving self-referential multi-output objects (`drv.out.outPath`, etc.) | done (see below) |
| M59 — `builtins.unsafeGetAttrPos`: a deliberately narrow, approximate implementation (no Attr-record growth, no lexer changes) | done, **scoped** (see below) |
| M60 — `builtins.unsafeDiscardStringContext` (no-op, matching M54's getContext/appendContext scope cut) | done (see below) |
| M61 — real string-context tracking (`getContext`/`appendContext`/derivation-input propagation) | investigated, **no fix** (see below) |
| M62 — `derivationStrict` generalized to real multi-output derivations (`outputs = [ "out" "bin" ... ]`), one distinct store path per declared output | done, **scoped** (see below) |
| M63 — `asnix_apply`'s PrimOp path didn't populate `args[-1]` ("self"), crashing any of this codebase's own self-capturing curried builtins (`elemAt`, `map`, `filter`, `foldl'`) when invoked through it rather than an ordinary `OP_CALL` | done (see below) |
| M64 — `~`-prefixed ("home") path literals, interpolated path literals (`./${foo}`, `~/${foo}`, ...); `builtins.path` | done, **scoped** (see below; `builtins.path` itself not implemented) |
| M65 — `import`-value caching (a second `import` of the same resolved path re-evaluated from scratch instead of sharing the first's result) | done (see below) |
| M66 — `builtins.parseFlakeRef`/`builtins.flakeRefToString`: flake-ref URL-like syntax (github/gitlab/indirect/path/git+/tarball+/file+) | done, **scoped** (see below) |
| M67 — external-conformance grab-bag: derivation `==` by `outPath`, dynamic-attrs scope fixes (rec-sibling visibility, attrpath-merge-into-a-dynamic-key, `${"literal"}` in `let`), `or` as an ordinary identifier, path-interpolation existence check, `fromJSON` deep-nesting stack fix, printer `<PRIMOP>`/`<LAMBDA>` | done, **scoped** (see below; function/slot-identity equality, `__curPos`, `tryEval` thunk re-eval, multi-segment `or default`, and `<PRIMOP-APP>` skipped) |
| M68 — a hand-written POSIX-ERE backtracking engine (`src/c/regex_engine.c`) to fix the 33 real behavioral gaps M55 triaged-but-didn't-fix between glibc's `<regex.h>` and real Nix's libstdc++ `std::regex` | done, **scoped** (see below; the 6 already-known-unfixable Class C patterns from M55 are still unfixed, deliberately) |
| M69 — `derivation`/`derivationStrict`'s `__structuredAttrs = true;` mode (JSON-serialized input attrs instead of per-attribute stringified env vars) | done, **scoped** (see below) |
| M70 — multi-segment `a.b.c or default`: every segment of a dotted chain now shares ONE `or` guard, matching real Nix's `ExprSelect::eval` (M67's documented residual gap, blocking `./asnix d.nix` on real nixpkgs) | done (see below) |
| M71 — `builtins.storeDir` (plain constant, `"/nix/store"`) | done (see below) |
| M72 — bumped `derivation`'s fixed attribute-count cap from 64 to 256; **`./asnix d.nix` now evaluates to completion with no crash** (the standing bug-chase's own goal) | done (see below) |
| M73 — fixed-output derivations now compute their real output/`.drv` paths via real Nix's actual `makeFixedOutputPath` formula (prerequisite for M74's `hashDerivationModulo` work) | done (see below) |
| M74 — real `hashDerivationModulo`: input derivations' own hashes are now substituted into a dependent derivation's `InputDrvs` before hashing, instead of every output/drvPath being computed from each derivation's own attributes in isolation | done, **scoped** (see below; `./asnix d.nix`'s `hello.outPath` still doesn't match real Nix bit-for-bit — a separately tracked, still-open gap, see below) |
| M75 — real store-copying (NAR-hash + content-addressed path) for local (non-`/nix/store`) Path literals coerced via genuine string interpolation or inside a `derivation {...}` attribute | done, **scoped** (see below; `./asnix d.nix`'s `hello.outPath` STILL doesn't match real Nix — the remaining gap is real Nix's *string context*, a materially larger feature, see below) |
| M76 — real string context: an invisible dependency-provenance set attached to string Values, surviving concatenation/coercion, so a derivation reference reaching an attribute only through prior string interpolation (not a bare attrset/Path) is now tracked in `inputDrvs`/`inputSrcs` too; also fixed `__ignoreNulls` (found while verifying) | done, **scoped** (see below; `./asnix d.nix`'s `hello.outPath` now has the exact right `inputDrvs`/`inputSrcs`/`__json` *shape*, but still doesn't match bit-for-bit — the remaining gap is the FULL nixpkgs bootstrap chain's own hash parity, a much larger, open-ended undertaking, see below) |
| M77 — `make test-nixpkgs-hash`: a real-nixpkgs derivation-hash correctness regression test (non-gating, like `test-nix-conformance`), filling a gap the user flagged directly — M58-M76's entire bug-chase had zero automated coverage of its own actual target; found and fixed `builtins.toFile` never attaching self-referential string context (a second, independent M76-class gap) and a `curl.outPath` crash (registered as a known-failing case, not yet root-caused) | done (see below) |
| M78+ | not started |

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
- **`__curPos`, real per-attribute position-tracking in general** --
  asnix's AST/IR carries no source-position information anywhere
  (tokens do, but it's discarded at parse time); implementing this
  FULLY would mean threading position info through the compiler and IR
  into the runtime Attr record, a cross-cutting change disproportionate
  to the handful of `*-attr-pos*`/`eval-okay-curpos.nix`/
  `eval-okay-getattrpos*.nix`/`eval-okay-compare-operand-order.nix`
  files that need it. `builtins.unsafeGetAttrPos` ITSELF is no longer
  in this skipped list as of M59 (see that section): it now exists,
  with an exact null-vs-not-null distinction and an explicitly
  approximate (placeholder `line`/`column`, uniform-not-per-binding
  `file`) non-null case -- narrower than, but no longer identical to,
  the gap this bullet still describes for `__curPos` and any future
  need for real per-token position data flowing anywhere else.
  `eval-okay-curpos.nix`/`eval-okay-getattrpos*.nix`/
  `eval-okay-compare-operand-order.nix` still fail (they need exact,
  not approximate, position values -- `__curPos`'s own magic value and
  the files' exact expected line/column numbers).
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

## M42 scope notes: `__functor` protocol (calling an attrset via a `__functor` attribute)

Found via conformance triage: `eval-okay-callable-attrs.nix` and
`eval-okay-functor-args.nix` both call an attrset directly as if it
were a function. Real Nix supports this when the attrset has a
`__functor` attribute (confirmed in `~/nix/src/libexpr/eval.cc`'s
`callFunction`: `vCur.type() == nAttrs && (functor = ...get(s.functor))`
-> `callFunction(*functor->value, {self_copy, args[0]}, ...)`) -- i.e.
`f arg` desugars to `f.__functor f arg` (the attrset itself as
`__functor`'s first argument, the real argument second), looping so a
`__functor` that itself returns another attrset-with-`__functor` keeps
working through successive arguments. A plain attrset with no
`__functor` at all still gets the ordinary "not a function" error.

Added `asnix_bshared_call_functor(Value attrs_val, Value arg)`
(builtins_shared.c): looks up `__functor`, forces it, then chains two
ordinary `asnix_apply` calls (`apply(functor, self)` then
`apply(step1, arg)`) -- reusing existing call machinery rather than
duplicating it, and getting the "returns another functor" recursive
case for free, since `asnix_apply`'s own ATTRS branch (below) re-enters
this same check. Wired into both call paths that can reach a non-
Closure/non-PrimOp callee:
- `OP_CALL` (`op_call.S`): a new `.Lop_call_is_attrs` branch, reached
  only after the existing Closure/PrimOp checks fail (zero cost on the
  ordinary-function-call hot path). Requires `argc == 1` (every
  ordinary Nix-level call site is arity-1 by construction, per
  `parser.c`'s `parse_app` always desugaring multi-arg application into
  nested arity-1 calls) -- falls through to the ordinary type-error
  fatal otherwise. A real `call` (not a tail `jmp`), since this handler
  needs control back afterward to push the result onto VALSTACK and
  continue the dispatch loop, same shape every other op handler here
  already uses.
- `asnix_apply` (`apply.S`, the generic "call this Value from C" entry
  point used by `any`/`all`/`map`'s lazy-call thunks/etc.): a new
  `.Lapply_is_attrs` branch, also `argc == 1`-only. Uses a tail `jmp`
  into `asnix_bshared_call_functor` instead of `call`+`ret` -- safe and
  correct because `%rsp` is still exactly as aligned at this point as
  it was at `asnix_apply`'s own entry (nothing pushed yet on this
  path), which is exactly what the callee expects to see as if it had
  been `call`'d directly; its eventual `ret` pops straight back to
  `asnix_apply`'s *own* caller, skipping this frame entirely (same
  reasoning the original, pre-M38 primop branch relied on for its own
  tail-jmp).

Verified against real `nix-instantiate --eval`: the exact
`eval-okay-callable-attrs.nix` expression (chained `__functor` plus a
`//`-updated attrset) and a `map (self: self 1) [ { __functor = ...; } ]`
case specifically exercising the `asnix_apply` path, not just `OP_CALL`;
also confirmed a plain attrset with no `__functor` still gets "call
target is not a function (got an attrset), aborting" (M50's message,
unchanged).

`make test`/`test-e2e` green (200 e2e, two new `m42_*` fixtures, one
per call path); `test-conformance` 635 passed (up from 633, +2 as
expected, no regressions).

## M55 scope notes: regex engine gaps triage (39 conformance failures, no fix)

A triage pass over `src/c/builtins_regex.c` (`builtins.match`/`split`),
following up on a conformance triage agent's report of 39 failing
`eval-okay-regex-*`/`eval-fail-regex-invalid-*` tests split into three
classes. Net result: **no code change**. All three classes turned out
to be genuine behavioral differences between glibc's `<regex.h>`
(`regcomp`/`regexec`, what `builtins_regex.c` actually calls) and
libstdc++'s `std::regex` (what real Nix's `primops.cc` calls) -- not
bugs in how asnix drives the library. Each claim below was checked by
calling `regcomp`/`regexec` directly from a standalone C program (not
through asnix), to separate "glibc itself behaves this way" from "asnix
is misusing glibc".

**Class A (27 tests, `(a*)*`-style nested/optional capture-group
ambiguity)**: confirmed a genuine glibc engine limitation, not an
asnix calling-convention bug. `builtins.match "(a*)*" "aa"` should give
`[ "" ]` (POSIX leftmost-longest's tie-break: the empty match on the
final iteration wins) but asnix gives `[ "aa" ]`. Calling `regexec`
directly, bypassing asnix entirely, on `(a*)*` against `aa` reports
group 1 as `[0,2) = "aa"` -- i.e. **glibc's own `regexec` returns the
"wrong" (non-libstdc++-matching) capture**, with no flag combination
that changes this (checked `REG_EXTENDED` is already the only relevant
compile flag; no `regexec` flag affects subexpression tie-breaking).
Reproduced the same divergence directly against glibc for 4 more shapes
spanning the full list's variety (`(a?)*`, `(|a)*`, `((a*)(b*))*`,
`(a*a*)*b`, `((a)*)*`) -- all consistent, all glibc-is-the-source
cases. This is a known class of glibc regex behavior (it does not
correctly implement POSIX leftmost-longest tie-breaking for
subexpressions under repetition/optionality) with no fix available
short of a different regex engine (e.g. vendoring `std::regex`-like
backtracking semantics or writing a custom ERE matcher) -- out of
scope for this pass, left exactly as the existing `match2_impl`/
`split2_impl` comments already describe (no change needed to those
comments; they don't claim correctness here, they describe the POSIX
leftmost-longest *whole-match* property, which does hold and is what
`match2_impl`'s own anchoring trick correctly relies on -- the bug is
specifically in subexpression/capture-group tie-breaking, a different
and narrower property).

**Class B (6 tests, bracket-expression extensions)**: also a genuine
glibc limitation, confirmed per-feature:
- Named collating symbols (`[.hyphen.]`, `[.space.]`, `[.tilde.]`,
  `[.period.]`): `regcomp` itself rejects these with `REG_ECOLLATE`
  ("Invalid collation character") under both the `C` and the
  process's actual `en_US.UTF-8` locale -- glibc's locale collating-
  symbol tables don't define ASCII character names like "hyphen"/
  "space" as named collating elements (these aren't POSIX-mandated;
  libstdc++'s regex traits hardcode a fixed table of such names
  independent of the C library's locale database). Not something
  asnix's call site controls.
- GNU-ish character-class names (`[:w:]`, `[:ALPHA:]`): `regcomp`
  rejects both with `REG_ECTYPE` ("Invalid character class name").
  glibc only recognizes the 12 standard lowercase POSIX names
  (confirmed `[[:alpha:]]` already works correctly in asnix today);
  `w` isn't a POSIX name at all (a Perl/PCRE-ism), and `ALPHA`
  (uppercase) isn't accepted case-insensitively by glibc even though
  it's otherwise a valid name -- libstdc++'s regex traits accept both
  as an extension, glibc's `wctype`-based class lookup does not.
- Equivalence classes (`[=a=]`): syntactically **accepted** by
  `regcomp` (unlike the two above), but the execution semantics still
  diverge: real Nix's `[[=a=]]+` matches "aA" in full (case-insensitive
  equivalence, consistent with `en_US.UTF-8` collation treating case
  variants as equivalent), but calling `regexec` directly on the
  compiled pattern against `"aA"` only matches the first char (`[0,1)
  = "a"`) -- glibc's own equivalence-class matching does NOT fold case
  here even under the process's real `en_US.UTF-8` locale. So this
  isn't a missing-feature gap closable by just not rejecting the
  syntax; the matching behavior itself is different from libstdc++'s.
Per the task's own guidance not to hack around genuine engine
limitations (e.g. preprocessing named collating symbols into literal
characters, or special-casing GNU class-name aliases before calling
`regcomp`), left unfixed and documented here instead.

**Class C (6 tests, invalid patterns silently accepted)**: already
*mostly* fixed in a prior pass -- `pattern_rejected_by_real_nix` (added
before this milestone; see its own long comment above
`compile_or_fatal`) already pre-rejects the escape-whitelist violations,
missing-lower-bound intervals, and unmatched-paren cases that make up
the bulk of `corpora/regex-invalid.nix`. Re-derived the exact mapping
from the conformance test names to corpus entries (`builtins.elemAt
(import ./corpora/regex-invalid.nix) N` with 0-based `N`, confirmed
directly via `nix-instantiate`) since the triage report's example
patterns for indices 34/37/40 (`"a{1,2,3}"`/`"a{99994}"`/`"[a--]"`) do
not match what's actually at those indices in the live corpus
(`(a){24998}`/`[[.-.]]`/`[a-[.c.]]` respectively) -- those three
patterns the report named are, in fact, *already* correctly rejected
(confirmed: `regcomp` itself returns `REG_BADBR`/"Invalid content of
\{\}" for `a{1,2,3}`, `REG_ESPACE`/"Regular expression too big" for
`a{99994}`, `REG_ERANGE`/"Invalid range end" for `[a--]"`, and
`compile_or_fatal` already propagates any nonzero `regcomp` return as
fatal). The three *actually*-still-failing pairs
(`eval-fail-regex-invalid-034/037/040-{match,split}`) are exactly the
two cases the existing comment above `pattern_rejected_by_real_nix`
already calls out as "Deliberately NOT chased": `(a){24998}` compiles
fine under glibc (`regcomp` accepts it, `nsub=1`) -- real Nix's
rejection is libstdc++ hitting some internal NFA-state-count/
complexity threshold for repeated *groups* specifically, well below
glibc's own much larger `RE_DUP_MAX`-class limit, and that threshold
isn't POSIX-specified or otherwise discoverable short of probing
libstdc++'s source/binary search over `nix-instantiate` itself (which
would then be an arbitrary magic number baked into asnix, not a real
rule); `[[.-.]]`/`[a-[.c.]]` (collating symbol used as a bracket-range
endpoint) also compile fine under glibc but are a libstdc++-specific
grammar restriction with no glibc equivalent to inspect. Confirmed via
direct `regcomp` calls (bypassing asnix) that glibc accepts all three
patterns -- this is not a missed error-check in `compile_or_fatal`,
there is no error to check for.

No code changes resulted from this pass. Regression baseline unchanged
(verified `make test`, `make test-e2e` green; conformance count varies
run-to-run due to other agents' concurrent work landing in the shared
`~/asnix` worktree the adapter targets -- saw 635 passed/167 failed/0
known/62 skipped at the time of this pass, vs. the 633/169/0/62 figure
quoted when this milestone was assigned, a +2 drift consistent with
unrelated fixes landing elsewhere, not anything from this pass).

## M43 scope notes: lambda-pattern-vs-attrset disambiguation miscounts `${` in default values

Found continuing to chase `d.nix` right after M42: a parse error
("expected '='") deep inside `pkgs/stdenv/linux/default.nix`. Root-
caused via gdb (conditional breakpoint on the specific `expect(p,
TOK_EQUALS, ...)` call site, printing nearby tokens): the parser had
entered `parse_binding_stmt` trying to parse `lib` as an ordinary
attrset-literal binding name (`lib = ...;`), then choked on a `,`
where it expected `=` -- meaning the disambiguation between "this `{`
starts a lambda pattern" and "this `{` starts a plain attrset literal"
had already picked the WRONG answer for this file's actual top-level
signature, `{ lib, localSystem, config, overlays, bootstrapFiles ?
<huge default expr>, ... }: ...`.

`find_matching_rbrace` (`parser.c`) implements this disambiguation by
scanning forward from the opening `{`, tracking brace depth purely by
counting `TOK_LBRACE`/`TOK_RBRACE`, to find the TRUE matching `}` and
check whether a `:`/`@` follows it (lambda pattern) or not (attrset
literal) -- without actually parsing the formals/default-value
expressions in between. The bug: a string interpolation or dynamic
attribute select (`${expr}`/`.${expr}`) lexes its OPENING `${` as its
own distinct `TOK_DOLLAR_LBRACE` token (confirmed throughout this
project's own string/dynamic-select parsing code, e.g.
`parse_dquote_string`), which `find_matching_rbrace` never counted as
a depth-increment -- but the interpolation's CLOSING brace is an
ordinary `TOK_RBRACE`, which the scan DOES decrement on. Each `${...}`/
`.${...}` inside the scanned range is therefore one spurious decrement
with no matching increment, closing the scan early at the wrong `}` and
corrupting the "what follows it" check. `bootstrapFiles`'s own default
value (`let table = {...}; archLookupTable = table.${localSystem.libc}
or (throw ...); files = archLookupTable.${localSystem.system} or
(...); in (config.replaceBootstrapFiles or lib.id) files`) has three
such dynamic selects before the formals list's real closing `}`,
tripping this exactly.

Fix: `find_matching_rbrace` now also increments depth on
`TOK_DOLLAR_LBRACE`, treating it as an opening brace for counting
purposes (its own closing `}` already decrements normally) --
one-line change, no new token kind, no change to how `${...}` is
parsed anywhere else. Verified with a minimal repro matching the real
shape (a lambda pattern whose default value contains a `let ... in
table.${k} or default` dynamic select) against real `nix-instantiate
--eval --strict`.

`make test`/`test-e2e` green (201 e2e, one new `m43_*` fixture);
`test-conformance` 635 passed, no regressions.

## M44 scope notes: `builtins.placeholder`/bare `placeholder`

Found continuing to chase `d.nix` right after M43: `placeholder` was
entirely missing (an "undefined variable" error, not "attribute
missing" -- confirming it's a bare global, not builtins-only).
Confirmed via cppnix source (`~/nix/src/libexpr/primops.cc`: `.name =
"placeholder"`, no `__` prefix) -- this project's own `eval_file.c`
comment had already flagged `placeholder` by name as a known-but-not-
yet-implemented bare global, left over from an earlier pass.

Computes an output placeholder string for a derivation output name --
ported directly from `~/nix/src/libstore/derivations.cc`'s
`hashPlaceholder`: `"/" + base32(sha256("nix-output:" + outputName))`
(Nix's own base32 variant, no hash-type prefix). Reuses this project's
existing `asnix_hash_sha256`/`asnix_base32_encode` (already relied on
for store-path hashing since M8) rather than adding any new hashing
machinery -- `prim_placeholder` is a ~15-line function. Registered
both ways real Nix exposes every non-`__`-prefixed primop: under
`builtins.placeholder` (added to `ASNIX_BUILTINS_MISC`) and as a bare
root-scope global (`eval_file.c`'s root `Env` grew from 12 to 13 slots;
`compiler.c`'s matching root `ScopeFrame` name list updated in lockstep,
same pattern every earlier bare-global addition in this project's
history has followed).

Verified against real `nix-instantiate --eval`: `builtins.placeholder
"out"` and bare `placeholder "out"` both produce the exact same real
Nix output,
`"/1rz4g4znpzjwh1xymhjpm42vipw92pr73vdgl6xs1hycac8kf2n9"`.

`make test`/`test-e2e` green (202 e2e, one new `m44_*` fixture);
`test-conformance` still 635 passed (the one conformance test that
exercises this, `eval-okay-drv-placeholder.nix`, needs real derivation/
content-addressing support beyond just this builtin to pass in full --
a separately tracked, larger scope cut -- so no conformance-count
change from this fix alone, but it's still a real, verified gap this
project needed fixed for `d.nix`), no regressions.

## M52 scope notes: `with`-frame leaking into a sibling expression (core evaluator fix)

A background triage agent investigating numtide/nix-conformance's
`eval-okay-eq-slot-identity.nix` bisected its failure by hand and found
the real cause had nothing to do with equality: two **sibling** (not
nested) `with` expressions corrupt each other. Minimal repro, verified
against `nix-instantiate --eval --strict` first:

```nix
let foo = { bar = 1; }; baz = { qux = 2; }; in
[ (with foo; bar) (with baz; qux) ]
```

Real Nix: `[ 1 2 ]`. Pre-fix asnix: `asnix: \`with' value is not an
attrset, aborting`. Each `with` alone (`with foo; bar`, or `with baz;
qux`) worked fine in isolation -- only having two side-by-side `with`s
in the same enclosing expression (e.g. both elements of one list)
triggered it.

**Root cause.** `compiler.c`'s `AST_WITH` case compiles `with expr;
body` as: build `expr`'s thunk against the *outer* scope, emit
`OP_WITH` (allocates a new 1-slot `Env` frame tagged
`ASNIX_ENV_WITH_TAG`, installs it as the current `ENV`, binds the
with-expr's thunk into slot 0 -- see `op_with.S`), then compile `body`
against a `ScopeFrame` chain that has this with-frame on top. That part
is correct. What's missing: unlike `AST_LET` (which calls
`ib_frame_exit()` right after compiling its body, emitting
`OP_FRAME_EXIT` to restore `ENV = ENV->up` once the `let` is done being
consumed -- see `op_frame.S`'s own header comment, "a non-tail use of
`let`/`rec {}` doesn't leak its frame into whatever depth-relative
`OP_VAR_REF` the compiler emitted next assuming the *original*
(pre-frame) scope depths"), `AST_WITH` compiled `OP_WITH` + body with
**no matching exit op**. `OP_WITH`'s own continuation logic inlines
straight into the body's bytecode and never restores `ENV` -- so once
a non-tail `with`'s body finishes, `ENV` is permanently left one frame
too deep for whatever the compiler emits next in the same linear IR
stream.

Concretely, for the repro above: compiling list element 2
(`with baz; qux`) builds `baz`'s attrs-thunk via `OP_MK_THUNK`, which
captures *whatever `ENV` currently is* at that point in the bytecode.
Compile-time, element 2's `baz` reference is correctly resolved as
depth 0 relative to the enclosing `let` frame (`ScopeFrame`s are a
separate, correctly-scoped C-stack structure, never corrupted). But at
*runtime*, by the time element 2's `OP_MK_THUNK` executes, `ENV` is
still element 1's leftover with-frame (from `with foo; bar`, never
exited) -- one real frame deeper than the `let` frame the compile-time
depth assumed. The captured thunk's `env` is therefore wrong, and when
forced later (`OP_VAR_REF(depth=0, slot=1)` expecting the `let`
frame's `baz` slot) it instead reads past the 1-slot with-frame's
single value, producing garbage that happens to fail the `TAG_ATTRS`
check in `OP_WITH_TRY` -- hence the "`with' value is not an attrset"
abort. (A second repro from the same bucket, using closures through
equality instead of plain values, aborted with "undefined variable"
for the same underlying reason -- see the fixture list below.)

**Fix.** Added a new op, `OP_WITH_EXIT` (`ir.h`/`irbuild.h`/
`irbuild.c`/`dispatch_table.c`/`op_with.S`'s `asnix_op_with_exit`),
emitted by `compiler.c`'s `AST_WITH` case right after compiling the
body -- the exact `OP_FRAME_EXIT` pattern `AST_LET` already uses,
specialized for `with`: `ENV = ENV->up` **with `ASNIX_ENV_WITH_TAG`
masked off** (`andq $-2, %rdi`). The mask is the one real difference
from `OP_FRAME_EXIT`: a with-frame's own `up` field has the tag bit
OR'd in (see `env.h`), so reusing plain `OP_FRAME_EXIT` (which never
masks, correctly, since ordinary `let`/`rec{}`/lambda frames are never
tagged) would leave `ENV` holding a tagged, odd-valued pointer --
masking a never-tagged pointer elsewhere is a no-op, so one op handles
both the with-exit and (if ever reused) the ordinary case safely.

**Verified against `nix-instantiate --eval --strict`**, both the
crash going away and the *values* being correct (not just "doesn't
abort" -- a leaked frame can accidentally read the right slot by luck,
e.g. a disabled first cut of this fix order-of-magnitude-tested clean
on the plain-`let`-sibling case but not on `with`):

- The two repros in the task (sibling `with`s over plain attrsets, and
  over closures compared with `==`) now run instead of aborting.
- `let a = { x = 1; }; b = { y = 2; }; in with a; with b; [ x y ]` --
  true *nested* `with` (one chain, not siblings) -- still `[ 1 2 ]`,
  unaffected by the new exit op.
- `let a = {x=1;}; b={y=2;}; c={z=3;}; in [ (with a; with b; [x y])
  (with c; z) ]` -- nested-*then*-sibling -- `[ [ 1 2 ] 3 ]`, matching
  real Nix: confirms `OP_WITH_EXIT` only undoes exactly its own
  `OP_WITH`, not the whole chain.
- `let a = 1; b = 2; in [ (with { x = 99; }; a) (with { y = 100; }; b)
  ]` -- the disambiguating case that would catch a leak even if it
  didn't crash: element 2's body reads the *outer* `let`-bound `b`,
  not anything from either `with`. Pre-fix this returned garbage
  (reading into the first with-frame's single slot); post-fix,
  `[ 1 2 ]`, matching real Nix.

**Scope cut: did NOT fix (confirmed separate root causes, not forced
into one fix).**

- `[ foo.bar ] == [ foo.bar ]` evaluates to `true` in asnix but `false`
  in real Nix (confirmed with no `with` involved at all, i.e.
  independent of this bug). Real Nix's `==` on functions/closures
  distinguishes "the exact same `Value` slot referenced twice" (a bare
  variable like `[ id ] == [ id ]`, or `with`-fallback-independent
  direct refs -- these ARE `true`) from "two independently-constructed
  copies of an equivalent closure" (via `.`-select or `with`-fallback,
  which copy the attribute's value out rather than alias its storage
  -- these are `false` even though the underlying closure is
  identical). asnix's `OP_EQ` doesn't implement this copy-vs-alias
  distinction at all, so this milestone's fix does NOT make
  `eval-okay-eq-slot-identity.nix` pass (its expected array mixes both
  patterns); that gap is a separate, deeper equality-semantics
  milestone, out of scope here.
- `eval-okay-delayed-with.nix` (self-referential lazy `with` through a
  `pkgs // packageOverrides pkgs` pattern) still aborts with "infinite
  recursion forcing a thunk (blackhole)", and `eval-okay-delayed-with-
  inherit.nix` still aborts with "attribute 'b' missing" -- both
  byte-for-byte the same errors as before this fix. Traced far enough
  to confirm it's unrelated to the sibling-leak above (no sibling
  `with`s involved; it's a single self-referential `with pkgs;` whose
  value thunk ends up re-entered while still `IN_PROGRESS`, where real
  Nix's shallow, keys-only `//` forcing avoids re-entering it) --
  genuinely a separate laziness bug, left as a documented, uninvestigated-
  further scope cut.

**Fixtures:** `tests/e2e/m52_with_siblings.nix` (the minimal plain-
attrset repro), `m52_with_siblings_outer_ref.nix` (the disambiguating
outer-reference case), `m52_with_nested_then_sibling.nix` (nested-
*then*-sibling regression guard). All three generated via
`nix-instantiate --eval --strict` and green.

**Regression:** `make test` (unit) and `make test-e2e` (now 201 e2e
fixtures) both green, no regressions. `test-conformance` (`cd
/tmp/nix-conformance && ./run adapters/asnix lang`, run against this
fix twice for stability, and against a stashed pre-fix rebuild for an
exact diff): **633 passed, 169 failed, 0 known, 62 skipped both before
and after** -- byte-identical `FAIL`/`SKIP` name lists; this fix
changes behavior (crash -> correct value, or crash -> still-wrong-but-
no-longer-crashing value per the scope cut above) only on cases not
otherwise exercised by the numtide/nix-conformance `lang` corpus as a
pass/fail flip, so the aggregate numbers don't move even though the
underlying bug is real and independently verified against
`nix-instantiate`.

## M53 scope notes: `foldl'`/`replaceStrings` forcing values they shouldn't -- plus the list-literal laziness gap actually blocking one of them

Found via numtide/nix-conformance triage: three failures all shaped like
the M23/M29/M34 laziness-bug family, but forcing *too much, too early*
(the opposite direction from M34's "forced too little" builtins) --
`eval-okay-foldlStrict-lazy-elements.nix`,
`eval-okay-foldlStrict-lazy-initial-accumulator.nix`, and the last
expression of `eval-okay-replacestrings.nix`, each a canary `throw`
that must never fire.

**Bug 1 -- `builtins.foldl'`'s initial accumulator (`builtins_list.c`)**.
Minimal repro, verified against real `nix-instantiate --eval` before
touching anything: `builtins.foldl' (_: x: x) (throw "never forced")
["a" 42]` => `42`, no throw -- `op`'s first parameter is never read, so
real Nix's own `prim_foldlStrict` (primops.cc) never forces the initial
accumulator merely because it was passed in (it only forces it in the
*empty-list* case, where the accumulator directly becomes the result).
Root cause: `foldl'` is implemented as three chained curried native
PrimOp stages (`prim_foldl_prime` -> stage2 -> stage3, builtins_list.c's
own long header comment explains the staging mechanism), and
`op_call.S`'s M25 fix ("force every argument before handing it to a
native primop function") is a *blanket* rule applied to every primop
call, including the internal call `(foldl' op) acc` that merely hands
`acc` to stage2 for capture -- stage2 never inspects `acc`'s tag, only
stores it, so forcing it there was always wrong, not merely redundant.
Fix: a new `make_primop1_noforce` (mirrors `make_primop1`, just also
sets `ASNIX_PRIMOP_OFF_NOFORCE`, the M30 flag `tryEval` already uses for
the same reason) used *only* for the stage2 object `prim_foldl_prime`
returns. Stage3 (the call supplying `list`) deliberately keeps ordinary
forcing -- it genuinely needs the list's own `ASNIX_TAG_LIST` tag/count/
elems (`check_list`), matching real Nix's own
`state.forceList(*args[2], ...)`. Disambiguated against "fixing this
doesn't silently make genuinely-needed strictness disappear": `builtins.
foldl' (acc: x: acc + x) (throw "used") [1 2 3]` still throws (`op`
actually reads `acc`), confirmed identical to real Nix.

**Bug 1b (the one that actually blocked `-lazy-elements.nix`) -- list
literals were never lazy per-element at all**. The accumulator fix
alone did not make `eval-okay-foldlStrict-lazy-elements.nix` pass;
minimal repros bypassing `foldl'` entirely proved the real root cause
is far more basic: `builtins.head [ 42 (throw "boom") ]`,
`builtins.length [ 42 (throw "boom") ]`, and even `builtins.foldl' (x:
y: x) 999 [ 42 (throw "boom") ]` (an `op` that reads *neither*
argument) all aborted on this build with "boom" -- proving the throw
fired merely from *constructing* the list literal, before any consumer
touched a specific element. `compiler.c`'s `AST_LIST` case compiled
every element inline via plain `compile_expr` (no thunk wrapping), so
the moment anything forced the list value itself to WHNF (needed just
to read its own `ASNIX_TAG_LIST` tag/count/elems pointer -- an
unavoidable, correct thing for `foldl'`/`head`/`length` to do), the
list's own construction bytecode ran to completion, which unconditionally
executed *every* element's code, including calling `throw`. This was a
known, already-documented gap (see M34's own scope notes: `builtins.
elemAt [(abort "boom") 1] 1` was already confirmed to abort, "a
documented, different, narrower scope gap... not the thing fixed
here") -- but it is exactly the thing blocking this milestone's test.

Fix: `AST_LIST` now wraps each element in its own thunk (`ib_mk_thunk`)
before `ib_mk_list`, the *exact* pattern the non-`rec` `AST_ATTRSET`
branch already uses for binding values (compiler.c, a few hundred lines
above) and `AST_APP` already uses for call arguments (M25) -- reused
machinery, no new op. `OP_MK_LIST` (`op_mk_list.S`) itself needed no
change: it already just moves `count` already-evaluated `Value`s off
`VALSTACK` into a list object, oblivious to whether each one is a
forced `Value` or (now, also) a `Thunk`.

This *broadens* which lists can hold unforced `Thunk` elements (previously
only `map`/`mapAttrs` results, M23; now every list literal in source,
`[...]`), so re-audited every direct list-element-reading site project-wide
(same audit M34 already did for the narrower map/mapAttrs-only case) by
running the full regression suite and checking every diff: one more
genuine gap turned up, `removeAttrs`'s name-list argument
(`builtins_attrs.c`'s `prim_removeAttrs_apply2`, e.g. `removeAttrs { a=1;
b=2; } [ "b" ]` -- a bare list literal, now exercising the new thunk-
wrapped-element path for the first time) read `list_elem(namelist, i)`
raw without forcing; fixed the same way M34 fixed its six builtins,
`asnix_force_for_print` immediately before the tag check. Every other
direct list-element-reading site audited (`elem`/`any`/`all`/`concatMap`/
`sort`/`partition`/`foldl`/`foldr`/`catAttrs` in builtins_list2.c,
`elemAt`/`concatLists`/`tail`/`head`/`++` in builtins_list.c,
`genericClosure`, `listToAttrs`/`attrByPath`, `toJSON`,
`asnix_bshared_value_eq`) was already either a correct lazy pass-through
(function-argument position, or copied verbatim into a new list/attrs
without inspecting the raw tag) or already force-before-read from M34 --
confirmed by the full regression suite (below) showing zero new
failures anywhere else.

**Bug 2 -- `builtins.replaceStrings` (`builtins_compare.c`'s
`replaceStrings3_impl`)**. `eval-okay-replacestrings.nix`'s last case:
`replaceStrings [ "oo" "XX" ] [ "u" (throw "unreachable") ] "foobar"` =>
`"fubar"`, no throw -- `"XX"` never occurs in `"foobar"`, so its paired
`to` entry must never be read. The previous implementation force-read
*every* `to[i]`'s bytes upfront, in the same loop that reads every
`from[i]` (a loop M34 already fixed to force -- a *different* bug in
the same function, replaceStrings's `to`/`from` list elements
themselves being unforced Thunks from a `map` call). `from[i]` genuinely
needs reading upfront (every entry is tested at every scan position),
but `to[i]` is only ever needed at the one position (if any) where
`from[i]` actually matches. Fix: stopped precomputing `to_bytes`/
`to_lens` arrays entirely; `to_elems[found]` is now forced and its bytes
read lazily, right at the `memcpy` call site inside the `found >= 0`
branch, only for the specific index that actually matched. Disambiguated
both directions against real Nix: `replaceStrings [ "oo" "XX" ] [ "u"
(throw "reachable") ] "fooXXbar"` still throws (`"XX"` *does* occur), and
`replaceStrings [ "oo" "XX" ] [ (throw "unreachable-first") "u" ]
"fooXXbar"` still throws too (`"oo"` occurs, so `to[0]` -- the throwing
one -- *is* read).

**Verification**: every repro and disambiguating case checked directly
against `nix-instantiate --eval --strict` before and after. Four new
e2e fixtures (`m53_foldl_prime_lazy_elements`,
`m53_foldl_prime_lazy_accumulator`, `m53_replacestrings_lazy_to`,
`m53_list_literal_lazy_elements` -- the last one locks in the general
list-literal fix independent of any specific builtin:
`[elemAt [1 (throw) ] 0, head [42 (throw)], length [1 (throw)]]` =>
`[1 42 2]`). `make test` green (full unit suite); `make test-e2e` green
(202 e2e, up from 198). `test-conformance`: ran a clean before/after
diff using this worktree's own binary both times (not the shared
`~/asnix/asnix` the stock adapter points at, which may be mid-edit from
other concurrent milestones' worktrees) -- before: 633 passed / 169
failed / 0 known / 62 skipped, identical to the stated baseline; after:
636 passed / 166 failed / 0 known / 62 skipped. Diffing the two FAIL
lists directly: exactly the three targeted tests flip from FAIL to
passing, zero other tests change in either direction -- no regressions.

## M45 scope notes: `builtins.toFile`

Found continuing to chase `d.nix` right after M44: `builtins.toFile`
was entirely missing. Confirmed via cppnix source (`~/nix/src/
libexpr/primops.cc`: `.name = "__toFile"`) builtins-only, not a bare
global.

Computes the store path a `name`/`contents` pair would get -- ported
directly from `~/nix/src/libstore/store-dir-config.cc`'s
`makeFixedOutputPathFromCA`'s `TextInfo` case: `makeStorePath(makeType
(store, "text", refs), sha256(contents), name)`. `makeType` only
appends anything to the `"text"` type string when `contents` has
references to OTHER store paths (via string context) -- this project
tracks no string context at all (confirmed: no context/dependency
machinery anywhere in this codebase, a separately tracked, larger
scope cut), so every `contents` string asnix can represent is
necessarily context-free, making the type string always exactly
`"text"`. This is the *exact same* algorithm `derivation.c` already
uses for a `.drv` file's own path (also a `"text"`-type hash) --
`prim_toFile` reuses `store_path.h`'s public preimage/path-assembly
helpers directly rather than duplicating `derivation.c`'s file-local
`compute_store_path` wrapper. Registered via `make_curried_native`
(arity 2, same curry-chain convention every other 2+-arg builtin in
this file uses -- a direct 2-arg `make_primop_value` would be
uncallable from ordinary `toFile "a" "b"` syntax, since `parser.c`'s
`parse_app` always desugars that into nested arity-1 calls).

Deliberately does NOT perform real Nix's actual filesystem side effect
(writing `contents` into `/nix/store/...`) -- this project has no real
content-addressed store at all; only the resulting path *string* is
computed, matching what every reachable use in `d.nix`'s own
evaluation (string manipulation on the result, never an actual build)
needs.

One self-inflicted bug caught before trusting this: initially called
`free()` on the `char*` returned by `asnix_string_value_to_cstr`,
which crashed with "free(): invalid pointer" -- that helper allocates
from this project's own never-free arena (`asnix_arena_alloc`), not
`malloc`, so it must never be `free()`'d (same convention every other
arena-sourced pointer in this codebase already follows). Verified
against real `nix-instantiate --eval` for two different name/content
pairs: `builtins.toFile "hello.txt" "Hello World"` and `builtins.toFile
"builder.sh" "#!/bin/sh\necho hi"`, both byte-for-byte matching real
Nix's computed store paths.

Known, accepted regression: `eval-fail-drv-tofile-output-reference.nix`
(numtide/nix-conformance) flips from a coincidental PASS to a genuine
FAIL. That test expects `builtins.toFile "t" "${derivationOutput}"` to
throw ("files created by builtins.toFile may not reference
derivations") -- detecting this requires string-context tracking,
which doesn't exist in this project at all; it was only "passing"
before because `toFile` itself didn't exist yet, so *any* call failed
for an unrelated reason ("attribute missing") that happened to also
satisfy the harness's "exit code != 0" check for an `eval-fail-*` test
(confirmed by conformance triage: that check never compares error
text). A real builtin that's actually usable for `d.nix`'s own
evaluation is worth this one coincidental-pass loss.

`make test`/`test-e2e` green (203 e2e, one new `m45_*` fixture);
`test-conformance` 637 passed (net -1 vs. the 638 baseline right
before this change, entirely accounted for by the one documented,
accepted flip above -- not a regression in `toFile`'s own
correctness).

## M56 scope notes: integer-overflow detection + `fromTOML` validation gaps

Two related "accepts invalid input silently" bug classes found by
conformance triage, both about asnix not detecting error conditions real
Nix detects.

### Part 1: integer overflow

`include/asnix/value.h` already documented asnix's `TAG_INT` as a
**61-bit** immediate (`ASNIX_INT_MIN`/`MAX` = ±2^60), narrower than real
Nix's full 64-bit `int64_t` -- and M23's own "Known Bugs" list had
already flagged this exact gap by name (`asnix_make_int`/`op_arith.S`
did *no* overflow checking at all, confirmed via
`eval-fail-overflowing-add.nix`/`-mul.nix`), with the fix sketched out
almost verbatim: "check the native overflow flag after each op, then
re-check the narrower `ASNIX_INT_MIN`/`MAX` window, fatal on either."
This pass implements exactly that, plus one thing the sketch didn't
anticipate: **the literal itself** can already be the overflow. `let a =
9223372036854775807; in a` (real Nix's `INT64_MAX`) was silently
round-tripping to `-1` at the `OP_PUSH_INT` handler -- `salq
$ASNIX_TAG_BITS` dropping the literal's own top 3 bits with no check at
all, *before* any arithmetic ever ran. Confirmed against
`nix-instantiate --eval`: real Nix accepts that literal fine (it fits
int64) but errors on `a + 1`; asnix was corrupting the literal itself,
unrelated to the `+`.

Fixed at every point a raw `int64_t` becomes a `TAG_INT` Value:

- **`include/asnix/value.h`**: `ASNIX_INT_MIN`/`MAX` moved above the
  `__ASSEMBLER__` guard and respelled as plain decimal literals
  (`1152921504606846975` / `-1152921504606846976`) instead of
  `INT64_C(1) << 60`-style C syntax, specifically so GAS's own
  expression parser can read them as an immediate operand after cpp
  substitution (GAS chokes on C integer-suffix syntax). `cmpq` can't
  take a 64-bit immediate directly (`cmp r/m64, imm32` only
  sign-extends a 32-bit immediate) -- every check below loads the bound
  into a scratch register first (`movq $ASNIX_INT_MAX, %r9; cmpq %r9,
  %rax`), confirmed against `as`+`objdump` to assemble as `movabs`+`cmp`.
- **`src/asm/ops/op_push.S`** (`asnix_op_push_int`): range-checks the
  literal's raw `int64_t` against `ASNIX_INT_MIN`/`MAX` *before*
  shifting, fatal otherwise -- fixes the literal-corruption case above
  directly. (Unary `-` compiles to `0 - x`, M26, so a negated huge
  literal like `eval-fail-overflowing-sub.nix`'s `-9223372036854775807`
  already fatals here on the un-negated literal, before `op_sub` ever
  runs.)
- **`src/asm/ops/op_arith.S`** (`asnix_op_add`/`sub`/`mul`/`div`'s
  all-int fast paths): `+`/`-` only need the narrower range re-check
  (operands are always already within ±2^60, far inside int64, so a
  64-bit `addq`/`subq` of two such operands can never trip the CPU's
  own `OF` flag -- only the *narrower* window catches e.g. `2^59 + 2^59
  = 2^60` landing one past `ASNIX_INT_MAX`). `*` is the opposite case:
  two ±2^60 operands genuinely can overflow a 64-bit `imulq` (confirmed
  by `eval-fail-overflowing-mul.nix`'s `4294967297^2` alone, let alone
  cubed), so `jo` right after `imulq` is load-bearing and checked
  *first* -- a tripped 64-bit multiply leaves garbage in the low 64
  bits, which the range check alone could coincidentally pass. `/`'s
  quotient can only ever exceed the ±2^60 window in the
  `ASNIX_INT_MIN / -1` edge case (never a genuine `idivq` CPU trap,
  since both operands are already bounded), so it only needs the range
  re-check, no `jo`.
- **`src/asm/value.S`** (`asnix_make_int`): the shared C-callable
  reference implementation gets the same ±2^60 range check. This is the
  single highest-leverage fix in the whole pass -- every C call site
  that builds an int `Value` (`builtins.add/sub/mul/div` in
  `builtins_m23.c`, `fromTOML`'s own integer-literal parsing in
  `toml.c`, `asnix_add_values`'s int+int fallback in `value_util.c`,
  etc.) funnels through here, so **`eval-fail-fromTOML-overflow.nix`/
  `-underflow.nix` started passing with zero changes to `toml.c`** --
  reusing this one check instead of duplicating overflow logic in the
  TOML integer parser, per the task brief's own suggestion.
- **`src/c/builtins_m23.c`** (`mul2_impl`, `builtins.mul`): the one spot
  needing a *separate* fix beyond `asnix_make_int`'s after-the-fact
  check, because the overflow there happens *before* that check ever
  runs -- `asnix_int_value(args[0]) * asnix_int_value(args[1])` as plain
  C `int64_t` multiplication is genuine signed-overflow **undefined
  behavior** the moment the mathematical product exceeds int64 (e.g.
  `4294967297 * 4294967297 ≈ 1.8e19 > INT64_MAX`), regardless of what
  any later range check would say. Rewritten with
  `__builtin_mul_overflow` (computes the product without the UB and
  reports whether it overflowed), still followed by `asnix_make_int` for
  the narrower-window check. `add2_impl`/`sub2_impl`/`div2_impl` did NOT
  need this: operands are always already bounded to ±2^60, so plain
  `int64_t` `+`/`-`/`/` of two such values can never itself overflow
  int64 -- `asnix_make_int`'s own check is sufficient there.

Verified against `nix-instantiate --eval`: `let a = 9223372036854775807;
b = 1; in a + b` errors in real Nix ("integer overflow in adding ...");
ordinary arithmetic at/near the ±2^60 boundary (`maxInt = 2^60 - 1`,
`1000000 + 2000000`, etc., both via operators and
`builtins.add/sub/mul/div`) still matches real Nix exactly --
`tests/e2e/m56_int_arith_bounds.nix`.

### Part 2: `fromTOML` validation gaps

M23's own scope notes already flagged real Nix's `fromTOML` (built on
`toml11`) as the ground truth, cross-checked entry-by-entry against
`lang/corpora/toml.nix`/`toml-invalid.nix`. This pass worked through the
16 still-failing `eval-fail-fromTOML-*` cases, each mapped to its exact
`corpora/toml-invalid.nix` entry by running the failing test directly
(`./asnix lang/eval-fail-fromTOML-invalid-NNN.nix`) to see what was
wrongly accepted, then finding the matching corpus line -- all in
`src/c/toml.c`:

- **Entries 15/19/20** (`"a = 07:32"`, and the same shape nested in an
  inline table / array): a bare `HH:MM` local-time (missing the
  required seconds) isn't a valid TOML value at all, but
  `classify_bare_token` had no way to tell it apart from a malformed
  number -- it fell straight through to `parse_decimal_or_prefixed_int`,
  whose `strtoll` simply stopped at the first `:` and silently returned
  the truncated leading digits (`07:32` -> the int `7`). Fixed with a
  `:` guard in `classify_bare_token`: a colon can only ever appear in a
  *complete* `HH:MM:SS` local-time (which `looks_like_date_or_time`
  would already have claimed above), so reaching the bare-value
  dispatch with a `:` still in the token is unambiguously invalid --
  fatal there instead of falling through to the integer parser.
- **Entries 16/17/18** (`"1979-05-27T07:32"`[`Z`/`+07:00`]): a
  date-*time* whose time half is truncated below `HH:MM:SS`.
  `classify_date_or_time`'s call into `normalize_time` did an
  *unconditional* `memcpy(out, s, 8)` with no length check at all -- an
  out-of-bounds read on the 5-byte `"07:32"`, silently fabricating a
  seconds field from whatever memory followed. Fixed by validating the
  time half (new `valid_time_value` helper, shape *and* range) before
  ever calling `normalize_time`, for both the date-time case and the
  bare-local-time case (`classify_date_or_time`'s `else` branch).
- **Entries 34/35** (`"1979-13-01"`, `"25:00:00"`): calendar/clock
  values were shape-checked (correct digit/separator positions) but
  never range-checked at all. New `valid_date_value` (month 1-12, day
  1-31) and `valid_time_value` (hour 0-23, minute 0-59, second 0-60,
  allowing a leap second) helpers, called from `classify_date_or_time`
  for both the date and time halves.
- **Entry 26** (`"[a]\n[a]"`): TOML forbids declaring the same table
  twice. `tnode_enter_table` happily re-returned the existing `TN_TABLE`
  on a second `[a]` header with no check. Added a `declared_as_table`
  flag to `TNode`, set only when a `[table]` header *itself* targets a
  node (not when `tnode_get_or_create_table_child` merely steps through
  it as an ancestor while navigating a dotted/header path, which stays
  legal per TOML's own implicit-table rules) -- fatal if already set.
- **Entry 32** (`"a = 01"`): decimal integers may not have a leading
  zero (except `"0"` itself); hex/octal/binary are exempt (`"0x01"` is
  fine). Checked in `parse_decimal_or_prefixed_int` only when `base ==
  10`, after underscores are stripped.
- **Entry 33** (`"a = 1__0"`): digit separators must be single and
  strictly between two digits, never leading/trailing/doubled. The old
  code just unconditionally skipped every `_` while stripping them with
  no adjacency check. New `validate_digit_separators` scans the *raw*
  digit run (before stripping, the only point the original separator
  positions are still visible) and fatals on any `_` that isn't flanked
  by digits on both sides -- shared by all four integer bases (decimal
  and the three prefixed ones all funnel through the same function).
- **Entry 38** (`"a = tru"`, a truncated `true`): fell through every
  other bare-value case to `parse_decimal_or_prefixed_int`, whose
  `strtoll` converted zero digits and silently returned `0`. Fixed by
  capturing `strtoll`'s `endptr` and fataling when `endptr == buf`
  (nothing at all was consumed as a number).
- **`eval-fail-fromTOML-{key,value}WithNullByte.nix`**: a backslash-u0000
  escape (or a literal NUL byte in the document) decodes to an embedded
  NUL with no complaint, in either a quoted key or a string value (both
  paths run through the same `parse_toml_string`) -- but real Nix
  strings can't carry one. Fixed with one `memchr` check at the end of
  `parse_toml_string`, covering both keys and values in one place.
- **`eval-fail-fromTOML-{overflow,underflow}.nix`**: as noted in Part
  1, these needed **zero changes to `toml.c`** -- `asnix_make_int`'s new
  range check (shared with `builtins.add/sub/mul/div`) already rejects
  any TOML integer literal outside asnix's representable range, which a
  true `int64_t` overflow/underflow trivially always is (asnix's window
  is ±2^60, real Nix's is ±2^63-ish) -- exactly the "reuse, don't
  duplicate" the task brief suggested.

Every fix re-verified against the *valid* corpus too (not just the
invalid one it was meant to reject), both the full differential sweep
(`lang/eval-okay-fromTOML-*`, unchanged pass/fail set) and by hand
(`nix-instantiate --eval`): `0`, `-0`, `0x01`/`0o01`/`0b01` (leading
zero fine for prefixed bases), `1_000_000` (single underscores fine) --
`tests/e2e/m56_fromtoml_valid_edges.nix`. The float/inf/nan-related
`fromTOML` cases (`eval-okay-fromTOML-006/007/008/060.nix`,
`-timestamps.nix`, the un-numbered `eval-okay-fromTOML.nix`) are the
same pre-existing, documented M23 scope cut (no float type at all) --
untouched, still failing exactly as before.

No e2e fixtures for the fatal paths themselves -- same reason M36 set
the precedent: `make test-e2e`'s harness only diffs `stdout`, and
`set -e` means a fixture whose `./asnix` invocation exits non-zero
aborts the *entire* `test-e2e` run rather than reporting one failure.
Added two fixtures instead that lock in the *successful* paths this
pass's checks run through on every call (`m56_int_arith_bounds.nix`,
`m56_fromtoml_valid_edges.nix`), verified by hand against
`nix-instantiate --eval` otherwise.

`make test`/`test-e2e` green, no regressions. `test-conformance`
(`lang/`): 653 passed, 149 failed, 0 known, 62 skipped -- up from the
633/169 baseline, +20 (4 overflow + 2 fromTOML over/underflow + 14
fromTOML invalid-input cases), confirmed via a fail-list diff to be
*exactly* those 20 and nothing else.

## M46 scope notes: `<nix/fetchurl.nix>` virtual-corepkgs special case

Found continuing to chase `d.nix` right after M45: `<nix/fetchurl.nix>`
("file 'nix/fetchurl.nix' was not found in the Nix search path")
imported from `pkgs/build-support/fetchurl/default.nix`. Root-caused
via cppnix source (`~/nix/src/libexpr/eval.cc`): `<nix/...>` is NOT an
ordinary `NIX_PATH` search-path lookup at all in real Nix -- it's
special-cased to a tiny in-memory virtual filesystem (`corepkgsFS`)
that has exactly ONE file ever added to it, `fetchurl.nix`, whose
bytes are embedded directly into the `nix` binary at compile time
(`#embed "fetchurl.nix"`) -- there is no real `corepkgs` directory on
disk anywhere this project could point `NIX_PATH` at, confirmed by
checking the real installed binary's own closure.

Ported the file's content verbatim from cppnix's own source
(`~/nix/src/libexpr/fetchurl.nix`, byte-for-byte, confirmed via `diff`)
rather than reconstructing it, to avoid any behavioral drift.
`asnix_resolve_search_path_value` now special-cases the exact name
`"nix/fetchurl.nix"` (checked before the ordinary `resolve_search_path`
`NIX_PATH`-walking logic ever runs, matching real Nix's own ordering)
by writing the embedded content out to a fixed, stable temp path
(`/tmp/.asnix-corepkgs-fetchurl.nix`, overwritten idempotently every
time -- content never changes, so a concurrent-process race is
harmless) and returning THAT as the resolved Path -- reuses 100% of
the existing Path/import machinery rather than teaching it about
virtual, non-filesystem content. Verified this resolves and imports
correctly even with `NIX_PATH` entirely unset (matching real Nix
exactly, since the virtual-FS special case bypasses `NIX_PATH` lookup
altogether) and that the written file is byte-identical to cppnix's
source.

Documented scope cut: `toString <nix/fetchurl.nix>` would print the
real temp path instead of real Nix's special `"<nix" .. ">"` display
convention -- not hit by anything this project has evaluated so far
(every real use just `import`s it and calls the result).

`make test`/`test-e2e` green (204 e2e, one new `m46_*` fixture);
`test-conformance` still 657 passed, no regressions (not exercised by
the numtide/nix-conformance `lang` corpus, which has no filesystem
fixture tree shaped like this).

## M54 scope notes: a batch of missing builtins

A conformance-triage pass found these entirely missing (`attribute 'X'
missing, aborting`): `bitAnd`/`bitOr`/`bitXor`, `trace`, `zipAttrsWith`,
`toXML`, `convertHash`, `getContext`/`appendContext`, `scopedImport`.
All confirmed against cppnix's own source (`~/nix/src/libexpr/
primops.cc`/`primops/context.cc`): every one is registered `__`-prefixed
(builtins-only) EXCEPT `scopedImport`, which -- like `import`/`map`/
`abort`/etc -- has no `__` prefix and is therefore also a bare root-
scope global (M27's bare-global rule). Kept in one new translation unit,
`builtins_m54.{c,h}` (plus two small new standalone modules, `base64.
{c,h}` and a decode-only addition to the existing `base32.{c,h}`), all
registered through a new `ASNIX_M54_BUILTINS[]` table merged into
`asnix_make_builtins_attrs()` (`builtins.c`) the same way every prior
builtins-expansion pass's table is.

**`bitAnd`/`bitOr`/`bitXor`** (`__bitAnd`/`__bitOr`/`__bitXor`): plain
arity-2 integer bitwise ops, curried via the established
`asnix_bshared_make_curried_native` idiom. Verified against
`nix-instantiate --eval`: `bitAnd 6 3 == 2`, `bitOr 6 3 == 7`, `bitXor 6
3 == 5`.

**`trace`** (`__trace`): forces and prints `msg` to stderr, returns
`value` completely unforced (same "lazy pass-through" idiom as
`getAttr`, M23) -- real Nix's own `prim_trace` forces `msg` but never
touches `value`. This needed its own small twist on the curry-chain
machinery: the REAL native call (the one that actually executes `trace`'s
body) is reached through an ordinary `OP_CALL` into a `PrimOp`, and
`OP_CALL`'s primop branch unconditionally FORCES every argument before
calling the native function (M25/M38) unless `ASNIX_PRIMOP_OFF_NOFORCE`
is set -- without that flag, `value` would get forced (and could already
abort/throw) before `msg` is even printed, backwards from real Nix's
timing. Fixed by building `trace`'s real 2-arg native primop via
`asnix_bshared_make_primop_noforce` instead of the forcing default (a
small local `make_curried_native_noforce`, parallel to
`builtins_shared.c`'s own curry-chain builder) -- `OP_CALL`'s
UNCONDITIONAL post-call force of the RETURN value (M23/M38, independent
of `NOFORCE`) still forces the returned `value` to WHNF once the overall
`trace msg value` application is itself forced, exactly mirroring
`getAttr`'s existing "lazy passthrough" shape. Verified against 4
separate `nix-instantiate --eval` cases, including the one genuinely
subtle formatting rule: a top-level STRING `msg` prints its raw bytes
unquoted (`trace: hi`), while every other type (including a string
*nested* inside a list/attrset) uses an ordinary quoted/structural
printer (`trace: { a = "x"; }`) -- confirmed this is the exact rule
cppnix's own `prim_trace` uses (`if (args[0]->type() == nString) ...
string_view() ... else ... printValue(...)`). The nested-container
printer (`trace_print_value`, `builtins_m54.c`) is a best-effort
fallback, not fully faithful to real Nix's own repl-style printer (no
attrset-name quoting/reserved-keyword handling, no `<PRIMOP>`/`<LAMBDA>`/
cycle-detection notation) -- not worth more fidelity here since all 4 of
this builtin's actual conformance callers only ever trace a plain
top-level string.

Of the 4 conformance tests named as needing `trace`, only
`eval-okay-sort-peeksort.nix` now fully passes. The other 3 reach
evaluation successfully (no longer hard-abort on the missing builtin)
but still fail for separate, pre-existing, unrelated reasons:
- `eval-okay-inherit-from.nix` needs the top-level printer's own
  self-reference/cycle-detection notation (`«repeated»`) for a `rec {}`
  that refers to itself through an `__overrides`-merge pattern -- a
  printer gap that predates this pass (same class of gap `toXML`'s own
  scope notes below hit for `<LAMBDA>`/`<PRIMOP>`).
- `eval-okay-print.nix`'s own primary subject is an even fancier printer
  representation (`<PRIMOP>`, `<PRIMOP-APP>`, `<LAMBDA>`, cycle
  detection on a genuinely self-referential list `x = [x]`) that asnix's
  printer has no path to at all -- unreachable regardless of `trace`.
- `eval-okay-tryeval-failed-thunk-reeval.nix` surfaced a genuine,
  previously-undiscovered bug one layer down, in `tryEval` (M30) itself,
  not in `trace`: the test's `let foo = trace "throwing" throw "nope";
  in tryEval foo; ...; tryEval foo` expects a failed thunk's forcing
  attempt to be safely *re-triable* (real Nix 2.34+ memoizes the
  failure so the second `tryEval foo` is a no-op, printing `trace:
  throwing` exactly once) -- confirmed `trace` itself does exactly the
  right thing (`trace: throwing` prints once, matching the expected
  `.err.exp`), but asnix's own thunk-forcing protocol marks a thunk
  `IN_PROGRESS` (blackhole) before running its body and only clears
  that back to `FORCED` via `OP_RET`'s normal return path (`op_call.S`)
  -- a `longjmp` out of a caught `throw` (`tryeval.c`'s setjmp/longjmp
  mechanism) skips `OP_RET` entirely, so the thunk is left stuck
  `IN_PROGRESS` forever. The second `tryEval foo` then hits the
  existing (correct, M23-era) blackhole-detection fatal path and aborts
  the whole process instead of returning `{success=false;...}` again.
  Fixing this properly needs an actual `Failed`-thunk state (real Nix's
  own memoized-failure representation) that doesn't exist anywhere in
  this codebase yet -- a separate, non-trivial core-evaluator feature,
  not a "missing builtin"; documented here as a genuine, newly-found gap
  rather than attempted as part of this batch.

**`zipAttrsWith`** (`__zipAttrsWith`): for each distinct key across all
attrsets in the given list, collects every value present under that key
(in the LIST's own order) into a list, then calls `f key thatList`
lazily per key (`asnix_bshared_make_lazy_call2`, same M23 laziness
convention as `mapAttrs`/`map`). Verified against `nix-instantiate
--eval`: `zipAttrsWith (n: v: v) [{a=1;b=2;} {a=3;c=4;}] == { a = [ 1 3
]; b = [ 2 ]; c = [ 4 ]; }`, and the empty-list case `== {}`. Fully
passes its own conformance test, `eval-okay-zipAttrsWith.nix`.

**`toXML`** (`__toXML`): ported field-for-field from real Nix's own
`printValueAsXML`/`XMLWriter` (`~/nix/src/libexpr/value-to-xml.cc`,
`~/nix/src/libutil/xml-writer.cc`) -- 2-space indent per level, a
self-closing `<tag attr="val" />` for leaves, alphabetical attrset-key
order, and the exact `<?xml version='1.0' encoding='utf-8'?>` header.
Verified byte-for-byte against both `eval-okay-toxml.nix`/
`eval-okay-toxml2.nix`'s expected output -- both now fully pass.
Deliberately scoped out (neither target test needs them, and each would
need infrastructure this project doesn't have): the `isDerivation`-
special-cased `<derivation>` element (asnix's own derivation support is
itself a narrower M8 scope cut), position attributes (`prim_toXML`
always passes `location=false` in real Nix too, so nothing is lost), and
a fully faithful `<function>`/`<varpat name="...">` (a plain,
non-pattern lambda's single argument NAME isn't tracked anywhere in this
project's `Closure` representation, only pattern-style formals are, see
M37's `functionArgs`) -- closures print an empty `<varpat name="" />`
and primops print `<unevaluated />`, matching real Nix's own "FIXME:
Serialize primops and primopapps" fallback, just applied to lambdas too.

**`convertHash`** (`__convertHash`): ported from real Nix's own
`Hash::parseAny`/`Hash::to_string` (`~/nix/src/libutil/hash.cc`) --
`hash` may be bare (format detected from decoded length against the
algorithm's known digest size), `algo:body`-prefixed, or SRI
(`algo-base64body`); `hashAlgo` is required unless the algo is readable
from one of those two prefixes (and the two must agree if both given).
`toHashFormat` is `base16`/`nix32`/`base32` (alias)/`base64`/`sri` (the
only one that's ever algo-prefixed in the OUTPUT, with `-`, confirmed
directly against real Nix). Needed two new small decode routines not
previously in this codebase: `asnix_base32_decode` (added to the
existing `base32.c`/`.h`, the mirror image of its own `asnix_base32_
encode`) and a brand-new standard-RFC-4648 `base64.c`/`.h` (encode AND
decode -- nothing in this codebase had ever needed base64 before,
`hashString`'s own output is always hex). Verified against
numtide/nix-conformance's own `eval-okay-convertHash.nix`, which
exercises all 4 hash algorithms x all 5 output formats x bare/colon-
prefixed input in one file -- passes byte-for-byte, including the two
real-Nix doc examples quoted in `__convertHash`'s own docstring
(`hash="e3b0c4..."` sha256 base16 -> sri, and the reverse).

**`getContext`/`appendContext`** (`__getContext`/`__appendContext`):
asnix has NO string-context tracking anywhere in this codebase at all
(confirmed: grepping every C source file for "context" turns up only
unrelated hits -- `addErrorContext`'s error-trace sense, M39; parser/
lexer "context"; register-save "context") -- derivation-output string
context (which attrset keys a string's value transitively depends on)
is a substantially larger, separate feature this pass does not attempt.
Scoped to exactly the one case realistically supportable without it: a
value with no actual context. `getContext` always returns `{}`;
`appendContext msg ctxAttrs` validates its second argument is an
attrset (matching real Nix's own type check) but otherwise ignores it,
returning `msg` completely unchanged -- since nothing in this evaluator
ever tracks OR consults context, this already reproduces every
observable plain-string case (`getContext "plain" == {}`, `appendContext
"plain" {} == "plain"`, verified against `nix-instantiate --eval`). The
three conformance tests naming these (`eval-okay-context-builtins.nix`,
`eval-okay-substring-context.nix`, `eval-okay-context-inputs.nix`) each
fundamentally depend on real derivation-output context propagating
through `${drv}`/`drv.outPath`/`builtins.toFile`/`builtins.
unsafeDiscardStringContext`/etc -- confirmed `eval-okay-substring-
context.nix` still fails here, but for a DIFFERENT, equally out-of-scope
reason (`unsafeDiscardStringContext` itself is entirely missing, not
just context-unaware) -- expected to keep failing regardless of this
pass, documented rather than chased further.

**`scopedImport`** (bare global AND `builtins.scopedImport`, confirmed
`.name = "scopedImport"` with no `__` prefix in `~/nix/src/libexpr/
primops.cc`): `scope`'s attribute NAMES become extra bare identifiers
inside `path`'s own file, taking priority over (shadowing) any bare
global/`builtins.X` of the same name -- confirmed directly in real
Nix's own doc comment ("Variables from scope shadow built-ins with the
same name") and cppnix's actual implementation (`scopedImport`,
`primops.cc`): it builds a brand-new, INNERMOST `StaticEnv` seeded from
`scope`'s own keys, whose `up` is the ordinary base/global `StaticEnv` --
a plain LEXICAL binding (closer to `let` than `with`), not a `with`-style
runtime fallback. This distinction matters: a `with`-shaped
implementation would have given `scope`'s names the WRONG priority here
(this codebase's own `with` semantics, confirmed in `op_with.S`'s header
comment, deliberately let a lexical/named-frame binding win over any
enclosing `with`'s fallback regardless of nesting depth -- exactly
backwards from what `scopedImport` needs).

Implemented by actually mirroring cppnix's own mechanism, not faking it:
`compiler.c` gained `asnix_compile_scoped(root, base_dir, extra_names,
extra_lens, extra_count)`, which splices one more NAMED (not `is_with`)
`ScopeFrame` in between the ordinary root `builtins_frame` (factored out
into a new shared `init_builtins_frame` helper, also used by the
ordinary `asnix_compile`) and the file's own top-level expression.
`eval_file.c` gained the matching runtime half, `asnix_eval_file_scoped`
(path, scope)`: reads `scope`'s attribute NAMES (via the already-
existing `asnix_bshared_attrs_*` accessors) to build that extra
`ScopeFrame`, then builds a matching extra, innermost `Env` frame (its
own field VALUES, left completely unforced -- same laziness as any
other attrset field) wrapped around the ordinary root `Env` (grown to
14 slots to add this bare global -- 13 immediately before this
milestone, since M44's `placeholder` had just grown it from 12;
`compiler.c`'s `init_builtins_frame`/`eval_file.c`'s `make_root_env`
list the same 14 names in the same order, as the existing convention
requires -- both files needed hand-merging when this milestone's
worktree and M44/M45's were integrated, since both independently
claimed the same next slot index). `scopedImport`'s own native half
(`scopedImport2_impl`, arity-2 curried like every other true-arity-2
builtin here) accepts the same two path shapes `builtins.import` already
does (a `Path`, or an absolute-path `String`), reusing that exact
rejection logic for a relative-path string.

Verified against numtide/nix-conformance's own `eval-okay-import.nix`
(which exercises both basic name-shadowing AND a recursive `scopedImport
(overrides // s) fn`-via-overridden-bare-`import` pattern) -- passes
byte-for-byte; also checked directly against `nix-instantiate --eval`
with a minimal two-file repro (`scopedImport { extra = 41; } ./target.
nix` where `target.nix` is just `extra + 1`) before trusting the
mechanism at all.

One off-by-one bug found and fixed while wiring this up: the new 13th
root-scope name's LENGTH was initially hardcoded wrong (`13`, an
actual-string-length miscount -- `"scopedImport"` is 12 characters, not
13) in `compiler.c`'s `init_builtins_frame` -- since name resolution
requires an EXACT length match (`name_eq`, `compiler.c`), this silently
made `scopedImport` entirely unresolvable as a bare identifier (compiled
straight through to "undefined variable", never a crash) until caught
by this pass's own `scopedImport`-specific e2e fixture failing. Fixed
by simply correcting the length constant.

**Skipped entirely, as scoped by the task**: `flakeRefToString`/
`parseFlakeRef`/`derivationStrict`/`outputOf`/`placeholder`/`builtins.
path` (flake/derivation-system features needing infrastructure this
project doesn't have, left for a separate pass) and `unsafeGetAttrPos`/
`__curPos` (this codebase has zero position tracking anywhere, confirmed
again here -- a stub `null` return was considered but not attempted,
since nothing in this batch's actual target tests exercises either one;
left for the same future, larger position-tracking effort M31's own
notes already flag).

`make test` green (22/22 binaries); `make test-e2e` green (205/205,
7 new `m54_*` fixtures: `m54_bitops`, `m54_trace`, `m54_zipAttrsWith`,
`m54_toxml`, `m54_convertHash`, `m54_context`, `m54_scopedImport` --
the last with its own `m54_scopedimport_fixtures/target.nix`, same
sibling-file convention `m11_import_fixtures`/`m18_bareglobals_
fixtures` already established). `test-conformance` (run against this
worktree's own built binary, via a scratch adapter pointed at it rather
than the shared `~/asnix/asnix` checkout the repo's default adapter
targets -- see the isolation note at the top of this pass's own task
brief) 639 passed (up from 633), 163 failed (down from 169), 0 known, 62
skipped -- no regressions; the +6 is `eval-okay-import.nix`,
`eval-okay-zipAttrsWith.nix`, `eval-okay-toxml.nix`,
`eval-okay-toxml2.nix`, `eval-okay-convertHash.nix`, and
`eval-okay-sort-peeksort.nix`. `eval-okay-arithmetic.nix` still fails
despite `bitAnd`/`bitOr`/`bitXor` now being correct and verified -- that
file *also* exercises `"aa" < "ab"` (string ordering via `<`), a
separate, pre-existing, already-documented scope cut (this codebase's
`<`/`>` operators are INT-only, see `asnix_bshared_value_lt`'s own
comment) that this batch does not attempt to lift.

## M47 scope notes: list-to-string coercion (`toString`/derivation env vars)

Found continuing to chase `d.nix` right after M46: `<nix/fetchurl.nix>`
imports fine now, but calling the resulting function aborts with
"cannot coerce this value to a string" -- root-caused via gdb (broke
at the fatal, read the value's raw tag: `ASNIX_TAG_LIST`) to
`derivation.c`'s generic per-attribute env-var coercion hitting
`urls = [ url ];` (fetchurl.nix's own derivation call sets this).
`asnix_coerce_to_string` had no `ASNIX_TAG_LIST` case at all --
confirmed via cppnix source (`~/nix/src/libexpr/eval.cc`'s
`coerceToString`, `coerceMore` branch) that real Nix recursively
coerces each element and joins with a single space. Verified against
real `nix-instantiate --eval`: `builtins.toString [1 2 3]` ==
`"1 2 3"`, `builtins.toString [ "a" "b" ]` == `"a b"`, `builtins.toString
[]` == `""`. Does not replicate cppnix's own "skip the separator
before an empty nested list" quirk (that function's own comment calls
it "not quite correct") -- not hit by anything this project has
evaluated so far. Confirmed this is a distinct function from string
*interpolation*'s own narrower coercion (`coerce_to_string_for_interp`,
same file, already documented as deliberately NOT accepting a list --
verified `"${[1 2]}"` is still a real Nix error and unaffected by this
change, since that function has its own separate tag-dispatch, never
delegating to `asnix_coerce_to_string`'s new list case).

Known, accepted regression (same pattern M45's `toFile` fix already
established): three numtide/nix-conformance tests --
`eval-fail-drv-outputs-duplicate.nix`, `eval-fail-drv-outputs-empty.nix`,
`eval-fail-drv-fixed-two-outputs.nix` -- flip from a coincidental PASS
to a genuine FAIL. All three set a derivation's `outputs` attribute to
an invalid list (duplicate names / empty / multiple outputs combined
with a fixed-output hash); `derivation.c` has no `outputs`-specific
validation logic at all (confirmed: no `"outputs"` handling anywhere in
that file), so these were only "passing" because `outputs`, like any
other list-valued attribute, used to hit the exact same generic
"cannot coerce" crash `urls` did -- an accidental error for the wrong
reason, not real `outputs` validation. Implementing multi-output
derivation support (the actual missing feature) is a separately
tracked, larger scope cut -- not needed by `d.nix`'s own single-output
`fetchurl` usage, which is what motivated this fix in the first place.

`make test`/`test-e2e` green (205 e2e, one new `m47_*` fixture);
`test-conformance`: confirmed via a before/after fail-list diff (not
just the aggregate count) that exactly these 3 tests are the only
change -- 663 passed before this fix, 660 after, net -3, entirely
accounted for by the documented `outputs`-validation gap above, not a
regression in list-to-string coercion's own correctness.

## M48 scope notes: `outputHashAlgo = ""` wrongly rejected as an unknown hash algorithm

Found continuing to chase `d.nix` right after M47: once `fetchurl.nix`'s
own derivation call evaluates far enough to reach
`validate_output_hash_fields` (`derivation.c`), it fataled with "unknown
hash algorithm ''" -- `<nix/fetchurl.nix>`'s own default for
`outputHashAlgo` is literally `if hash != "" then "" else ...`, i.e. an
empty string, not absent/NULL, whenever `outputHash` is set and
`outputHashAlgo` is not (the common real-world case for a plain SRI
hash). Verified directly against real Nix: `derivation { ...;
outputHashAlgo = ""; outputHash = "sha256-...";
}` evaluates fine there -- the empty string means "unspecified, infer
the algorithm from `outputHash`'s own SRI/legacy prefix instead," which
`validate_output_hash_fields` already had logic for (the
`strchr(output_hash, '-')`/`:'` prefix-stripping block later in the same
function) -- it just never got a chance to run, since the
unknown-algorithm check fired on the empty string first.

Fix: normalize `output_hash_algo[0] == '\0'` to `NULL` right before the
unknown-algorithm check (`derivation.c`'s
`validate_output_hash_fields`), so an empty string is treated exactly
like "not given at all" and falls through to the existing
SRI-prefix-resolution logic. Verified both directions against real
Nix: `outputHashAlgo = ""` with a valid SRI `outputHash` now succeeds
(matches `nix-instantiate --eval --strict` exactly); `outputHashAlgo =
"bogus"` still correctly fatals with "unknown hash algorithm 'bogus'"
on both asnix and real Nix (a genuinely-unknown non-empty algorithm
name is unaffected).

`make test`/`test-e2e` green (one new `m48_*` fixture); `test-conformance`:
688 passed, 114 failed, 0 known, 62 skipped -- identical to the
pre-fix count (no regressions; not directly exercised by that corpus).

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

### M57c: `__overrides` type-checking + propagation into sibling self-references

`__overrides` (a legacy `rec {}`-only feature) had no handling in asnix
at all before this pass: `grep -r overrides src/c` turned up nothing
except an unrelated daemon-protocol field. Ported real Nix's own
mechanism (`ExprAttrs::eval`, `~/nix/src/libexpr/eval.cc`): when a
`rec {}` literal has a field literally named `__overrides`, evaluating
the rec forces that field's value, requires it's a set, and for every
`(sym, value)` pair in it that matches one of the rec's OWN field
names, swaps that field's ENTRY to the override's value -- critically,
including the slot in the rec's OWN `Env` frame, which is what makes
every sibling binding that references the overridden name (even ones
whose thunk was already built and filed before the swap runs) see the
new value once they're eventually forced, since nothing has forced
anything yet at this point (`rec {}` fields are lazy). An override key
with no matching field is just appended to the output as a new,
non-self-referenceable entry (no frame slot exists for it). A non-`rec`
`{ __overrides = ...; }` is NOT special -- confirmed against real Nix
(only the `recursive` branch of `ExprAttrs::eval` even looks for it).

Implemented as one new op, `OP_REC_OVERRIDES_MERGE` (ir.h/irbuild.h/.c,
new `src/asm/ops/op_rec_overrides_merge.S`, C half `asnix_bshared_
rec_overrides_merge` in builtins_shared.c), emitted by compiler.c's
AST_ATTRSET `rec` branch INSTEAD of the usual `OP_FRAME_GET*n +
OP_MK_ATTRS` pair, but only when a binding named `__overrides` is
detected at compile time (a plain string-name check on the AST, no
runtime cost for every OTHER `rec {}` in existence). The op takes no
VALSTACK input (unlike every other "defer to C" op in this directory)
-- its job is entirely reading/mutating the current Env's own slots,
which the preceding `OP_FRAME_SET` loop already filled; its pool
operand carries each binding's symbol id in original (= Env slot)
order (so slot i's sym is pool entry i -- no separate translation
table needed) plus which slot is `__overrides` itself. The C half
forces `__overrides`, fatals if it's not a set (`asnix_bshared_type_
name` reports the actual type, matching this project's own message
style elsewhere), then builds the merged (sym, value) list and calls
the existing `asnix_bshared_make_attrs` (already does its own
insertion sort by sym) to finish it off -- reusing that helper meant
the override-introduces-a-new-key case (append, no slot) came for
free, not just the common "matches an existing field" case the two
`eval-okay-overrides*` fixtures exercise.

Verified against real `nix-instantiate --eval`: `(rec { __overrides =
{ a = 2; }; x = a; a = 1; }).x` => `2` (was `1` before this fix); the
full `eval-okay-overrides-thunks.nix` scenario (four sub-cases: a
matched override with a nested non-`rec` `{ inherit a; }` also seeing
the override through the shared frame, an override feeding a string
interpolation, an empty `__overrides = {}` no-op, and a `let` -- not
`rec` -- `__overrides` confirmed to have NO effect) matches its `.exp`
byte-for-byte; `rec { __overrides = 1; }` now exits non-zero
("'__overrides' must be a set (got an integer)"); a hand-written extra
check (an override introducing a brand-new key not in the original
rec, `tests/e2e/m57c_overrides_extra_key.nix`) also matches real Nix
exactly, including that the new key has no effect on sibling bindings
that don't explicitly reference it.

Two new e2e fixtures (`m57c_overrides_propagate.nix`/
`m57c_overrides_extra_key.nix`); `make test`/`test-e2e` green (203
e2e); `test-conformance` 641 passed (+4), 161 failed (-4), 0 known, 62
skipped -- no other test's result moved (checked the full fail list
for anything override/rec-attrset-related; none).

(In the same pass, touched up `asnix_bshared_value_lt`'s own header
comment in builtins_shared.h, which had gone stale after M57a added
list ordering to it but before this M57c commit -- a documentation-
only fix, no behavior change.)

### M57d: `toJSON` rejects invalid UTF-8

`builtins_compare.c`'s `json_encode`'s string case passed raw bytes
straight through unescaped/unvalidated (its own header comment even
documented this as deliberate for *valid* UTF-8: "this project has no
Unicode awareness anywhere else either"). Real Nix's `toJSON` goes
through `nlohmann::json`, which validates every string byte-for-byte
and throws (confirmed: a single stray `0xFF` byte, `eval-fail-toJSON-
non-utf-8.nix`'s exact fixture, throws "JSON serialization error: ...
invalid UTF-8 byte at index 16: 0xFF"). Added a structural UTF-8
validator (`utf8_is_valid`: leading-byte class + the right count of
`0x80`-`0xBF` continuation bytes, truncation-safe) called on every
string value `json_encode` emits, fatal on a hit -- deliberately not
as strict as nlohmann's decoder (doesn't additionally reject overlong
encodings or out-of-range/surrogate codepoints), but every genuinely
malformed byte (a lone continuation byte, or `0xF8`-`0xFF`, never valid
UTF-8 leading bytes at all) is still always caught, which is what the
one real test here exercises.

Verified: real fixture now exits non-zero; valid multi-byte UTF-8
(`builtins.toJSON { s = "héllo"; }`) still round-trips byte-for-byte
identically to real Nix's own output. One new e2e fixture
(`m57d_tojson_unicode_ok.nix`, a regression check for the valid-UTF-8
path -- no fixture for the fail case itself, same `test-e2e`-can't-
check-exit-codes reason as M57b). `make test`/`test-e2e` green (205
e2e, see M57e below too); `test-conformance` 642 passed (+1), 160
failed (-1), 0 known, 62 skipped.

### M57e: `import` tries `__toString` before rejecting

Real Nix's `import` argument goes through the same general
`coerceToString` every other path/string consumer does (`eval.cc`),
trying `__toString` (then `outPath`) on an attrset before giving up --
confirmed: `import { __toString = _: toString ./foo.nix; }` works in
real Nix. asnix's `prim_import` (builtins.c) only ever accepted a Path
or an absolute-path String, rejecting everything else outright. Fixed
by falling back to the project's own existing `asnix_coerce_to_string`
(value_util.c -- already implements the identical `__toString`-then-
`outPath` protocol for `toString`/string interpolation/`toJSON`'s Path
case) whenever the argument is neither already a path nor a string,
before the absolute-path check runs; `asnix_coerce_to_string` always
either returns a string/path or exits(1) itself on a genuinely
incoercible value, so the old "argument must be a path or an absolute-
path string" branch stays as an now-unreachable-but-harmless fallback.

Verified against real Nix: `eval-okay-import-tostring.nix` (both the
plain-`__toString` case and the `__toString`-before-`outPath`-priority
case) now matches its `.exp` byte-for-byte. One new e2e fixture pair
(`m57e_import_tostring.nix` + a helper target under the new
`m57e_import_fixtures/` subdirectory -- NOT directly glob-matched by
`test-e2e`'s `tests/e2e/*.nix`, same "helper file, not a standalone
case" pattern M11's own `m11_import_fixtures/` already established).
`make test`/`test-e2e` green (205 e2e); `test-conformance` 643 passed
(+1 over M57d's 642), 159 failed (-1), 0 known, 62 skipped -- no other
test's result moved across either of these two sub-items.

### M57f: `compareVersions`' int-overflow-to-string quirk

Real Nix's `nextComponent`/`componentsLT` (`~/nix/src/libstore/names.cc`)
parses a numeric version component with `string2Int<int>` -- `int`, not
`int64_t` -- so a component whose numeric VALUE exceeds `INT32_MAX`
(2147483647) fails that parse and falls back to being compared as a
plain string for that round, not as a (correctly-valued) big integer.
asnix's `component_is_int` (builtins_compare.c, used only by
`compareVersions`'s `components_lt`, not by plain `splitVersion`) just
accumulated into an `int64_t` with no bound at all. Added the same
32-bit check upstream has, inline in the existing digit-accumulation
loop (bail to "not a number" the instant the running value exceeds
2147483647) -- checking the accumulated VALUE rather than e.g. the
component's string length matters: confirmed `"0000000000000000000002"`
(23 digits, value 2) is still numeric, only `"2147483648"` (2^31, over
the boundary despite being a *shorter* string) isn't.

Verified against real `nix-instantiate --eval`, all five
`compareVersions` cases in `eval-okay-versions-int-range.nix`:
`"2147483648"` vs `"9"` => -1 (over the boundary, loses to numeric `9`
by the existing non-numeric-sorts-before-numeric rule); `"2147483647"`
vs `"9"` => 1 (`INT32_MAX` itself still parses, compares numerically,
correctly wins); `"0.0.0-20191109021931"` vs `"0.0.0-100"` => -1;
leading-zeros case => 1; `"1.0-4294967296"` vs `"1.0-5"` => -1 -- all
now match. `splitVersion`/`parseDrvName` (same fixture's other two
sub-cases) were already correct, confirming the bug was scoped exactly
to `components_lt`'s numeric check as expected. One new e2e fixture
(`m57f_compareversions_int_overflow.nix`). `make test`/`test-e2e`
green (207 e2e); `test-conformance` 644 passed (+1), 158 failed (-1),
0 known, 62 skipped.

### M57g: `path + string` canonicalizes `.`/`..` segments

Path *literals* (`./a/../b`) were already lexically canonicalized at
compile time by a `canon_path` helper local to compiler.c -- but a
RUNTIME `path + string` concatenation (`asnix_add_values`, value_util.c)
just concatenated the raw text and re-wrapped it as a Path verbatim, no
canonicalization at all. Confirmed against real Nix this is wrong:
`toString (/foo/bar + "/../xyzzy/./foo.txt")` == `/foo/xyzzy/foo.txt`,
not the literal uncollapsed concatenation -- `eval-okay-string.nix`
exercises exactly this (and, as a contrast case in the same fixture,
confirms plain STRING + STRING concatenation involving `/../`-looking
text is NOT canonicalized, since it's never a Path value to begin
with: `"/../foo" + toString /x/y` stays literally `/../foo/x/y`).

Fixed by promoting `canon_path` from a compiler.c-local `static` to a
shared `asnix_canon_path` (value_util.c/.h) -- compiler.c keeps every
one of its existing call sites working unchanged via a same-file
`#define canon_path asnix_canon_path` alias, rather than touching each
call site -- and having `asnix_add_values`'s Path-result branch run
the concatenated text through it (NUL-terminating into a temp buffer
first, since `asnix_canon_path` expects a C string but asnix strings
aren't NUL-terminated) before building the final Path Value.

Verified: `eval-okay-string.nix`'s full expected output now matches
byte-for-byte, including the contrast case confirming plain strings
are correctly left untouched. One new e2e fixture
(`m57g_path_canon_concat.nix`). `make test`/`test-e2e` green (208
e2e); `test-conformance` 645 passed (+1), 157 failed (-1), 0 known, 62
skipped.

### M57h: import path resolution no longer over-resolves symlinks

`asnix_eval_file` (eval_file.c) resolved every import target with
`realpath()`, which does two things at once: makes the path absolute,
AND fully resolves every symlink on disk along the way. The second
part is wrong for computing the BASE DIRECTORY used to resolve a
*subsequent* relative import inside that file: `eval-okay-symlink-
resolution.nix`'s fixture tree has `symlink-resolution/foo/overlays ->
../overlays` (a symlink) with `symlink-resolution/foo/overlays/
overlay.nix` (the real file, reached THROUGH that symlink) containing
`import ../lib`. Real Nix resolves that `..` against the file's path
AS WRITTEN (`.../foo/overlays/overlay.nix`'s own directory,
`.../foo/overlays`), landing on `.../foo/lib` (exists) -- confirmed
directly against `nix-instantiate --eval` on this exact fixture tree.
`realpath()` instead fully resolved the `foo/overlays` symlink FIRST,
landing the base directory on the symlink's physical TARGET
(`symlink-resolution/overlays`, since `overlays -> ../overlays`), so
the same `../lib` resolved to the nonexistent `symlink-resolution/lib`
-- "asnix: cannot resolve path" (one level down, inside the nested
`import`, not the top-level file itself).

Fixed by replacing `realpath()` with a new `resolve_import_path`:
makes the path absolute (prepending `getcwd()` if relative) and runs
it through the now-shared `asnix_canon_path` (M57g) -- pure text-level
`.`/`..`/doubled-slash collapsing, no filesystem access, so a symlink
anywhere in the chain is left exactly as named. Nothing downstream
needed `realpath`'s symlink-following for its own sake: `stat()`
(the `import somedir` => `somedir/default.nix` directory check) and
`fopen()` (actually reading the file) both already follow symlinks
transparently at the OS level regardless of whether this project
pre-resolves them itself. Lost, deliberately: `realpath()`'s own
"does this path exist at all" check -- a nonexistent import now fails
one step later, inside `read_file`'s `fopen` ("asnix: cannot open
'...'"), which was already a graceful, non-crashing exit(1) path, just
with a slightly different message.

Verified: `eval-okay-symlink-resolution.nix` now returns `"test"`,
matching its `.exp`; `eval-okay-readDir-symlinked-directory.nix` (a
different symlink case, `readDir` not `import`) was already passing
and still is, confirming this change didn't touch anything it didn't
need to; `eval-okay-import-follows-symlink.nix` still fails, but for an
unrelated reason (`scopedImport`/`__curPos`, not implemented at all --
not a regression, not touched by this fix). Diffed the full
conformance fail list before/after: only the five tests from M57d
through M57h moved from fail to pass, nothing else changed. One new
e2e fixture (`m57h_symlink_resolution.nix` + a `m57h_symlink_fixtures/`
tree with a real symlink committed to the repo, mirroring numtide's
own fixture shape). `make test`/`test-e2e` green (209 e2e);
`test-conformance` 646 passed (+1), 156 failed (-1), 0 known, 62
skipped.

### M57i: CLI flags -- `--arg`/`--argstr`/`-A`

`src/c/main.c` had zero flag parsing at all (hard-required exactly one
argument, the file path). Added real Nix's `nix-instantiate --eval`
three most load-bearing flags:

- **`--argstr name str`**: builds a plain String Value directly, no
  parsing needed.
- **`--arg name EXPR`**: `EXPR` is an arbitrary Nix expression (real
  Nix's own CLI treats it as one too, not a literal string) -- run
  through the exact same lex/parse/compile/eval pipeline a top-level
  file gets, via a new `asnix_eval_expr_source` (eval_file.c, factored
  out of `asnix_eval_file`'s existing `eval_source` so both share one
  fresh-root-env/`builtins` setup), with the process's cwd as the base
  directory any relative path literal INSIDE the expression resolves
  against (confirmed against real Nix: a `--arg`/`--argstr` value has
  no "containing file" of its own the way an ordinary import does).
- **`-A attrpath`**: force-and-select one dotted segment at a time into
  the final result after evaluation (and after any `--arg`/`--argstr`
  auto-call below), same semantics as a chain of plain `.` selections
  written in source, fatal on a non-attrset or missing key partway
  through (naming the specific segment).
- **`--arg`/`--argstr` also trigger real Nix's `autoCallFunction`**
  (`eval.cc`): if the evaluated file is a lambda-PATTERN function
  (`{a, b ? d, ...}: ...` -- introspected via the same `functionArgs`-
  shaped `ASNIX_CLOSURE_OFF_FORMALS` M37 already precomputes per
  closure, not new machinery), build an argument attrset containing
  only the formals a CLI arg actually matched by name, calling the
  closure with it; an unmatched formal is fine if it has a default
  (the lambda's own `OP_CHECK_FORMALS` default machinery supplies it,
  exactly as if that formal were simply omitted from an ordinary call
  site) but fatal ("you must specify a value for the argument 'X'") if
  required. A plain single-identifier lambda (`functionArgs` == `{}`)
  is left uncalled, matching real Nix.

Deliberately NOT implemented: **`-I`** (search-path entries feeding
`<name>` resolution) -- would need a first-class `builtins.findFile`/
`builtins.nixPath` (real Nix represents the search path as an actual
Nix-level list of `{prefix;path;}` attrsets queryable from source),
which is a separate, much larger feature this pass doesn't build; and
no generic "skip any other flag real `nix-instantiate` accepts" logic
-- an unrecognized flag is a hard usage error in `main.c`, by design
(see below for why).

Also updated `/tmp/nix-conformance/adapters/asnix` (a scratch file
outside this repo, no commit needed for it) to actually forward a
case's `.flags` sidecar -- it previously ignored `.flags` entirely, so
nothing in the whole corpus ever exercised any CLI flag regardless of
what asnix itself supported. Critically, it does NOT forward `.flags`
blindly: a corpus-wide scan found 12 distinct flag names across ~140
`.flags` files, only 3 of which asnix implements -- the rest
(`--extra-experimental-features X`, three different `--lint-*-
literals X` variants, `--max-call-depth N`, and bare boolean
`--eval`/`--strict`/`--no-show-trace`/`--impure`) are real-Nix-only
concepts (experimental-features gating, path-literal linting) asnix
has no notion of at all. Since `main.c` treats any unrecognized flag
as a hard usage error (by design, see above), blindly forwarding
these would have turned every one of those ~140 OTHER cases (mostly
`eval-{okay,fail}-fromTOML-*`, previously passing on their own merits
since the adapter never even looked at `.flags` before) into a crash
instead of a pass -- the adapter's flag-forwarding loop explicitly
whitelists `--arg`/`--argstr`/`-A` (forwarded) and explicitly,
individually drops every other flag name found in this exact corpus
(both the value-taking ones, consuming their one argument, and the
bare boolean ones, consuming none) -- a `*) shift ;;` catch-all at the
end is pure defense-in-depth for any single-token flag not already
named above, not the primary mechanism.

Verified against real `nix-instantiate --eval`: `eval-okay-autoargs.
nix` with its exact `.flags` string (`--arg lib import(lang/lib.nix)
--argstr xyzzy xyzzy! -A result`) now produces `"xyzzy!xyzzy!foobar"`,
matching its `.exp` byte-for-byte; a hand-run missing-required-argument
case (`{a}: a` with only `--argstr b x` supplied) fatals with "you must
specify a value for the argument 'a'", matching real Nix's own
rejection class. Diffed the full conformance fail list before/after
this adapter change: only `eval-okay-autoargs.nix` moved from fail to
pass, confirming the other ~140 `.flags`-having cases were correctly
unaffected by now actually reading (and selectively dropping from)
their `.flags` file. No new e2e fixture: `test-e2e`'s harness
(Makefile) invokes `./asnix "$f"` with no flags at all, so it
structurally can't exercise `--arg`/`--argstr`/`-A` -- covered by the
manual verification above and the conformance fixture instead. `make
test`/`test-e2e` green (209 e2e, unchanged); `test-conformance` 647
passed (+1 over M57h's 646), 155 failed (-1), 0 known, 62 skipped.

### M57: items investigated and explicitly skipped

- **Function/derivation equality quirks** (item 3 in the original
  ask): confirmed real Nix's surprising `let f = x: x; in f == f` =>
  `false` is genuine (`EvalState::eqValues`, eval.cc: functions are
  unconditionally incomparable, `case nFunction: return false;`), BUT
  also confirmed the reason `eval-okay-eq-slot-sort.nix` (M57a) still
  passes without this fix is a SEPARATE, unrelated quirk: `eqValues`
  short-circuits on C++ pointer identity (`if (&v1 == &v2) return
  true;`, a documented-as-a-hack fast path) BEFORE the type switch,
  and `ExprOpEq::eval` evaluates each side into a fresh *stack-local*
  `Value v1; Value v2;`, so `f == f` never actually hits that fast
  path (two different stack addresses) even though a shared heap
  object (e.g. the same closure referenced from two different list
  elements) does. Implementing real function-non-self-equality in
  asnix would require replicating this exact "was this Value freshly
  copied into a fresh temporary, or dereferenced from existing
  storage" distinction, which has no clean analog in asnix's
  architecture (variable references return the underlying heap Value
  directly, never a fresh copy) -- skipped as genuinely more involved
  than the task's own "skip if more involved than expected" allowance
  anticipated, not from lack of trying to understand it. Derivation-
  equality's `isDerivation`-style eager `type`-forcing during `==` was
  not investigated in depth given the above; left for a future pass.

- **CLI flag plumbing** (item 8): `--arg`/`--argstr`/`-A` WERE
  implemented (see M57i above); `-I` specifically was not, since it
  needs a first-class `builtins.findFile`/`builtins.nixPath` this pass
  doesn't build (a separate, much larger feature -- see M57i's own
  scope note). Both conformance fixtures that need `-I`
  (`eval-okay-search-path.nix`, `eval-okay-search-path-prefix-slash.
  nix`) also depend on things well beyond just the flag itself:
  `eval-okay-search-path.nix` asserts against `<nix/fetchurl.nix>`
  (real Nix's bundled `corepkgs`, which doesn't exist in asnix's world
  at all) and `length __nixPath == 5`; `eval-okay-search-path-prefix-
  slash.nix` needs `builtins.findFile`/`builtins.nixPath` directly as
  first-class builtins, not just `<name>` literal resolution. Neither
  would pass even with `-I` itself wired up, so building just the flag
  without the builtins it feeds would have had zero conformance payoff
  -- left undone for a future pass that tackles the search-path
  builtins properly.

- **Context-attr-name throwing on a string-with-context dynamic
  attribute name** (item 5's third sub-item): confirmed string-context
  tracking does not exist in this codebase at all -- `value_util.c`'s
  own `asnix_coerce_to_string` comment is explicit about it ("the
  separate, larger 'string context' feature (dependency tracking
  through string ops) this project deliberately doesn't implement"),
  and a repo-wide grep for any context-tracking data structure turned
  up nothing beyond that one acknowledging comment and unrelated
  daemon-protocol/error-context mentions. Not fixable without that
  missing infrastructure, which is well out of this grab-bag's scope --
  skipped per the task's own explicit "skip if the infrastructure
  doesn't exist" instruction.

## M51 scope notes: float literals (lexer through builtins)

**Root cause, minimal repro**: `echo '1.5 + 2' > /tmp/t.nix && ./asnix
/tmp/t.nix` -> `asnix: parse error: expected an attribute name`. asnix's
lexer had no `FLOAT` token at all (`src/c/lexer.c`'s digit-scanning
branch only ever produced `TOK_INT`), so `1.5` lexed as `TOK_INT(1)
TOK_DOT TOK_INT(5)` and the parser tried (and failed) to treat `.5` as
attribute selection on `1`. This single gap was blocking at least 9
numtide/nix-conformance files (every one contains a bare decimal
literal somewhere, including several whose OWN right-hand side comparison
value is a plain Nix float, not just their `builtins.fromJSON`/
`fromTOML` argument string): `eval-okay-float.nix`,
`eval-okay-float-range.nix`, `eval-okay-floor-ceil.nix`,
`eval-okay-types.nix`, `eval-okay-tojson.nix`,
`eval-okay-builtins-add.nix`, `eval-okay-compare-operand-order.nix`,
`eval-okay-fromjson.nix`, `eval-okay-nan-to-string.nix`.

**Lexer**: a new `TOK_FLOAT` (`src/c/lexer.c`/`lexer.h`), matching real
Nix's exact grammar (grepped directly from `~/nix/src/libexpr/lexer.l`):
`(([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?` -- a float
literal always needs a literal `.` somewhere (there is no bare-exponent
float like `1e10`; confirmed directly, `nix-instantiate --eval` on that
input errors "undefined variable 'e10'", i.e. it lexes as `INT 1` then
`IDENT e10`). Two call sites share one tail-scan+`strtod` helper
(`scan_float_tail_and_parse`): the ordinary digit-leading case (extended
into the existing digit-scanning branch, since `10.`/`0.5`/`00.5` all
need slightly different leading-digit-run rules to match the grammar's
two alternatives exactly) and a new bare-`.`-leading case (`.5`, the
`0?` in `0?\.[0-9]+` being genuinely optional). `strtod`'s `errno`
(`ERANGE`) is checked exactly the way real Nix's own lexer.l does,
which turned out to matter for conformance: `0.0e-400` parses to
`0.0` with no error (mantissa already exactly zero, no real underflow),
while `1.0e-400` (genuine underflow), `5.0e-324` (nonzero subnormal
result, glibc's `strtod` still sets `ERANGE` for this), and `1.0e400`
(overflow to `HUGE_VAL`) all correctly remain parse failures --
verified against conformance's own `parse-fail-float-{overflow,
underflow,subnormal}.nix` and `eval-okay-float-range.nix`'s `0.0e-400`/
`0.0e5` cases, all of which still pass after this change.

**Value representation**: the task brief's own suggested starting point
(check whether `ASNIX_TAG_RESERVED`/`0x7` is genuinely free) turned out
to be a dead end on inspection -- `include/asnix/value.h` already spends
all 8 tag slots, `0x7` having been claimed back in M7 as
`ASNIX_TAG_PRIMOP`. So there is no spare tag bit at all, and a double
needs its full 64 bits regardless (no immediate encoding can hold one
losslessly in the 61 payload bits an `INT`-shaped tag would offer). The
only honest option, and the one this project already has a direct,
working precedent for, is heap-boxing: a Float is a THIRD kind sharing
`ASNIX_TAG_PTR` with String and Path (`include/asnix/heap.h`), told
apart by its own reserved negative marker in the leading word, exactly
mirroring M15's Path-vs-String precedent:
`Float{int64_t marker == ASNIX_FLOAT_MARKER (-2); double value;}`.
Unlike Path (which just shifts its own same-shape payload out one slot
after the marker, no extra allocation), this is a genuine new 2-word
heap object, since a double can't share a word with anything else
losslessly. `asnix_make_float_value`/`asnix_value_is_float`/
`asnix_float_value` (`value_util.h`/`.c`) are the only sanctioned way to
touch one. A compile-time float literal is heap-allocated once, at
compile time, by `compiler.c`'s new `AST_FLOAT` case, and referenced via
`ib_push_const` -- the exact same "resolve once, not per-eval" mechanism
`AST_PATH`/`AST_STRING` constants already use.

Every existing place that assumed "`ASNIX_TAG_PTR` means String-or-Path"
and would misread a Float's marker+double bytes as a bogus string
length needed the same kind of dispatch update M15 made for Path:
`asnix_coerce_to_string`/`toString`, the string-interpolation coercion
(a bare float in `"${...}"` is correctly REJECTED, same as a bare int --
verified: `"${1.5}"` errors "cannot coerce a float to a string" on real
Nix, `"${toString 1.5}"` works), the top-level value printer
(`main.c`), `typeOf`, `isString`/`isPath`/new `isFloat`,
`asnix_bshared_type_name` (M50's error-message helper), structural
`==`/`!=` (`asnix_bshared_value_eq`), `toJSON`, and every "this dynamic
attribute name/key must be a string" check that already explicitly
excluded Path (`attrs_update.c`, `builtins_shared.c` x3,
`builtins_m23.c`'s `groupBy`/`pathExists`, `builtins.c`'s
`hashString`) -- each extended with `|| asnix_value_is_float(...)`
alongside its existing `|| asnix_value_is_path(...)` check, same
mechanical, non-speculative extension of an already-established pattern.

**Arithmetic (`+`/`-`/`*`/`/`)**: real Nix promotes int to float
whenever the OTHER operand is already a float, for all four operators
AND for the separate `builtins.add`/`sub`/`mul`/`div` primops (verified
directly in `~/nix/src/libexpr/primops.cc`'s `prim_add`/`sub`/`mul`/
`div` -- these are not independently-scoped features, the operators and
the builtins share the identical promotion rule and, for `/`, the
identical zero-divisor check). A C fallback per operator
(`asnix_add_values` extended, plus new `asnix_sub_values`/
`asnix_mul_values`/`asnix_div_values`, `value_util.c`) handles every
int/float and float/float combination; the existing all-int fast path in
`op_arith.S` is completely unchanged and still never calls into C, so
plain-int arithmetic pays zero cost for this milestone. One genuine
real-Nix quirk replicated exactly rather than "fixed": `1.0 / 0.0` is
STILL a hard "division by zero" error on real Nix, not an IEEE-754
infinity -- `prim_div` checks the float-coerced divisor against zero
unconditionally, before ever looking at whether either operand is
actually a float (confirmed directly against `nix-instantiate --eval`).
`builtins.add`/`sub`/`mul`/`div` (`builtins_m23.c`, previously
INT-only -- a documented scope cut from before this milestone, when
there was genuinely no honest way to do otherwise) now delegate to the
exact same value_util.c helpers.

**Comparisons (`<`/`>`/`<=`/`>=`)**: same promotion rule, verified via
real Nix's `CompareValues` (`primops.cc`). While chasing
`eval-okay-compare-operand-order.nix` (one of the 9 target files, and
NOT actually about floats at its core -- see below), found and fixed a
second, genuinely separate pre-existing bug this file also happened to
exercise: real Nix's parser desugars `a > b` to `__lessThan b a` and `a
<= b` to `!(__lessThan b a)` (`~/nix/src/libexpr/parser.y`), meaning the
RIGHT operand is always forced first for exactly these two operators
(`lessThan`'s curried application forces its first-applied argument
first) -- `<` and `>=` desugar to the plain, unswapped `__lessThan a b`
/ `!(__lessThan a b)`, so they need no such swap. asnix's compiler
previously compiled every `AST_BINOP`'s operands left-to-right
unconditionally, for every comparison operator alike (verified directly
against `nix-instantiate --eval` with `abort`/`throw` canaries on both
sides of `>` and `<=`: real Nix's right-operand-first order lets a
`tryEval`-caught `throw` on the right short-circuit before ever forcing
a never-catchable `abort` on the left; asnix's old left-to-right order
hit the left side's `abort` immediately instead). Fixed in
`compiler.c`'s `AST_BINOP` case: `>` now compiles as "push rhs, push
lhs, `OP_LT`" (computing `rhs < lhs`, i.e. exactly `lhs > rhs`, while
forcing rhs first) and `<=` as the same swap plus `OP_NOT` -- reusing
the existing `OP_LT`/`OP_NOT` IR ops rather than adding a new one.
`OP_GT`/`OP_LE` (`ib_gt`/`ib_le`, `op_cmp.S`'s `asnix_op_gt`/
`asnix_op_le`) are consequently never emitted by the compiler anymore --
left in place as harmless dead code (no unit test exercised them
directly) rather than ripped out, out of scope for this pass. Discovered
in the SAME file: `eval-okay-compare-operand-order.nix` also has
`("a" > "b")` in its expected-`false` list, which exposed a THIRD
pre-existing gap -- the comparison *operators* (unlike the separate
`lessThan`/`sort` builtins, which already supported `string<string` via
`asnix_bshared_value_lt`) were scoped to `TAG_INT` only, with no string
case at all. Extended `asnix_cmp_lt_values`/`gt`/`le`/`ge_values`
(`value_util.c`) to also accept a `string<string` pair (same
lexicographic-byte-comparison rule `asnix_bshared_value_lt` already
uses, reimplemented locally rather than shared -- no existing import
path between the two files) -- a real, if narrow, correctness fix
*found via* this milestone's float work but not actually a float
feature itself.

**Printer, `toString`, `toJSON` -- three genuinely different float
formatting rules, all verified independently against `nix-instantiate
--eval`**:
- The top-level value printer (`main.c`'s `print_value_inner`) uses
  `"%.6g"` -- real Nix's own `print.cc` just does `ostream << double`,
  which is bit-for-bit glibc's `%.6g` (6 significant digits, "general"
  notation, no forced trailing `.0`): `1.5` -> `"1.5"`,
  `2.2250738585072014e-308` -> `"2.22507e-308"`, `0.0` -> `"0"`,
  `builtins.add 2.0 2` -> `"4"` (not `"4.0"`). `nan`/`-nan`/`inf`/`-inf`
  fall out of glibc's `%g` for free, no special-casing needed (checked
  directly: `printf("%.6g", -NAN)` prints `"-nan"`).
- `toString`/`"${...}"`-after-`toString` (`asnix_coerce_to_string`) uses
  `"%f"` -- real Nix's `coerceToString` does `std::to_string(v.fpoint())`
  for a float, which is glibc's `%f`: always 6 digits after the decimal
  point (`toString 1.5` -> `"1.500000"`), and nan/inf still spell
  correctly (no fractional part to pad).
- `toJSON` (`builtins_compare.c`'s `json_encode`) approximates
  `nlohmann::json`'s shortest-round-trip double formatting: try `"%.*g"`
  at increasing precision until the result `strtod`s back to the exact
  same bit pattern, then append `".0"` if the match has neither a `.`
  nor an `e`/`E` (nlohmann always keeps a float's JSON text
  distinguishable from an int's, even for an integral value) --
  verified against `builtins.toJSON 1.0` == `"1.0"` (not `"1"` the
  printer's way, not `"1.000000"` toString's way) and
  `builtins.toJSON 1.7976931348623157e308` == full-precision
  `"1.7976931348623157e+308"`.

**`builtins.floor`/`builtins.ceil`** (`builtins_misc.c`, previously
entirely absent from this codebase): accept int or float, always return
an INT -- an int argument passes through completely unmodified (never
round-tripped through a double); a float is floored/ceiled via plain
casts (`(int64_t)d` truncates toward zero; floor/ceil are that plus a
one-off adjustment for the sign that needs it), not libm's `floor()`/
`ceil()`, so this needed no new link dependency.

**`toJSON`'s `__toString`/`outPath` attrset coercion** (found needed by
`eval-okay-tojson.nix`, NOT itself a float feature -- a second
pre-existing gap this pass's float fix unmasked by getting the file
past its earlier parse failure): real Nix's `printValueAsJSON`
"peels" an attrset via `__toString`/`outPath` BEFORE ever treating it as
a JSON object (confirmed: `builtins.toJSON { outPath.foo = true; }` ==
`"{\"foo\":true}"` there -- the whole peeled value, not a nested
object). `json_encode`'s `ASNIX_TAG_ATTRS` case now checks for either
attribute first and, if present, recurses on `asnix_coerce_to_string`'s
result instead of calling `json_encode_attrs`.

**`builtins.fromJSON`** (`src/c/builtins_json.{c,h}`, new file):
previously didn't exist in this codebase AT ALL -- explicitly flagged in
this doc's own long-tail-pass notes as "not worth building a JSON
parser for in isolation" for the sake of one unrelated conformance file
(`eval-fail-json-attr-not-json.nix`). That calculus changes here:
`eval-okay-fromjson.nix` is one of the 9 files THIS milestone's float
work was asked to unblock, and getting past its parse failure reveals
`fromJSON` is also needed to actually evaluate it (it calls
`builtins.fromJSON` on a literal document and compares the result
against a literal Nix value containing floats) -- so writing a real,
if deliberately small, hand-written recursive-descent JSON parser (same
style as the existing `toml.c`: plain index-into-buffer scanning, no
external dependency, fatal on malformed input) became in-scope rather
than out. Not exposed as a bare global (confirmed against
`nix-instantiate --eval`: unlike `fromTOML`, `fromJSON` is
`builtins.fromJSON`-only). Implements objects, arrays, strings (`\"
\\ \/ \b \f \n \r \t \uXXXX` escapes including UTF-16 surrogate pairs),
numbers (a `.` or exponent makes a Float Value, matching real Nix's own
int/float split, confirmed against the target file's own expected
output), `true`/`false`/`null`. Two correctness bugs found and fixed
while checking adjacent `eval-fail-fromJSON-*.nix` conformance cases
hadn't flipped from "trivially fails because `fromJSON` didn't exist"
to "wrongly succeeds now that it does": an embedded NUL byte from a
`\u0000` escape (in either an object key or a string value) is rejected
outright, matching real Nix's "cannot be represented as Nix string
because it contains null bytes"; an integer literal that overflows
either a real `int64_t` (checked via `strtoll`'s `ERANGE`) or asnix's
own narrower 60-bit range (`ASNIX_INT_MIN`/`ASNIX_INT_MAX`) is rejected
rather than silently wrapping/saturating.

**`builtins.fromTOML` float/inf/nan support** (`toml.c`): this parser
previously detected and REJECTED any TOML float or `inf`/`nan` on
purpose, with an explicit comment explaining why ("asnix's Value
representation is integer-only... this parser detects them and fails
loudly rather than guessing an approximation") -- a deliberate, now-
obsolete scope cut from before this milestone gave asnix an honest way
to represent one. `eval-okay-nan-to-string.nix` (one of the 9 target
files) specifically needs this: it calls `builtins.fromTOML "nan =
nan\nneg = -nan\ninf = inf"` and checks the sign of the resulting NaN is
preserved from how it was WRITTEN (unlike a NaN produced by arithmetic,
which takes the host's default sign -- negative on x86-64). glibc's own
`strtod` already gets this exactly right (`strtod("nan",0)` has a
positive sign bit, `strtod("-nan",0)` negative), so `parse_float_token`
just forwards the whole sign-prefixed token straight to `strtod` rather
than negating a magnitude afterward. General TOML float literals
(`1.0`, `3.1415`, `5e+22`, etc., with underscore separators stripped the
same way the existing integer parser already does) are supported too,
not just the one `nan`/`inf` case the target test needed -- cheap reuse
of the same `classify_bare_token` dispatch point, not a half-fix.

**Deliberately NOT extended, scope cuts made during this pass**:
- `asnix_bshared_value_lt` (the `sort`/`lessThan` BUILTIN's ordering,
  distinct from the `<`/`>`/`<=`/`>=` OPERATORS fixed above) is still
  `INT<INT` or `string<string` only -- a Float now safely falls through
  to that function's existing generic fatal ("cannot compare these two
  values...") instead of being misread as string bytes, but isn't
  specially supported there. Not needed by any of the 9 target files;
  real Nix's own `lessThan`/`sort` DO support float ordering, so this is
  a known, narrower gap than real Nix, consistent with this function's
  pre-existing, already-documented int/string-only scope.
- Path-plus-float arithmetic/ordering, and float support in
  `builtins.toString`-adjacent path-specific code, were not touched --
  no target file exercises them, and real Nix doesn't give floats any
  special path-related behavior either.
- `eval-okay-compare-operand-order.nix`'s FINAL list element,
  `(let __curPos = 42; in __curPos)`, still does not match (expects
  real Nix's actual `__curPos` position-tracking magic value; asnix
  has no source-position information anywhere in its AST/IR at all,
  and threading it through would be the same disproportionate,
  cross-cutting change this doc's Known-Gaps list already excludes for
  `builtins.unsafeGetAttrPos`/`eval-okay-curpos.nix`). This one file
  therefore remains failing even after every float- and evaluation-
  order-related fix above -- a known, pre-existing, unrelated gap, not
  a regression from this milestone and not attempted here.
- One conformance file flipped from an accidental pass to a genuine,
  unrelated fail: `eval-fail-fromTOML-timestamps.nix` previously
  "passed" only because its document also contains TOML floats, which
  used to abort the whole parse before ever reaching its date/time
  literals -- coincidental, not because asnix's actual (working,
  M23-documented, still fully covered by `eval-okay-fromTOML-062`
  through `-076`) timestamp support agreed with the file's expectation.
  With floats fixed, asnix now parses the ENTIRE document successfully
  (dates included), but real Nix's installed `nix-instantiate`
  (2.35.2) actually rejects TOML date/time literals outright
  ("Dates and times are not supported") -- directly reproduced
  (`builtins.fromTOML "a = 1979-05-27T07:32:00Z"` errors on this
  machine's real Nix). This is a genuine, pre-existing inconsistency
  between asnix's M23 timestamp feature (built against, and still
  matching, a documented differential corpus) and this one file's
  expectation, unmasked rather than caused by this milestone -- fixing
  it would mean ripping out working, separately-tested timestamp
  support and regressing the entire `-062`..`-076` cluster, so it was
  left alone and is flagged here rather than silently absorbed.

**Verified against `nix-instantiate --eval` throughout** (not just
trusted from reading `~/nix`'s source): every arithmetic/comparison
promotion case, every formatting rule (printer/`toString`/`toJSON`,
each independently, since they genuinely differ), `floor`/`ceil`'s
negative-number rounding direction, the `>`/`<=` operand-evaluation-
order fix (via `abort`/`throw` canaries), and the TOML nan-sign-
preservation rule.

**Regression**: `make test` (22 unit binaries, including a rewritten
`test_toml.c` float/inf/nan assertion that replaces the now-obsolete
"a TOML float is fatal" check it superseded) and `make test-e2e` (four
new `m51_*` fixtures: float arithmetic, float/string comparisons,
`floor`/`ceil`/`isFloat`/`typeOf`, and `toJSON`/`fromJSON`/`toString`
float formatting, each generated via `nix-instantiate --eval --strict`)
both green, no regressions. `test-conformance` (`lang/`): 646 passed,
156 failed, 0 known, 62 skipped, up from a 633/169 baseline measured on
this exact worktree before this milestone's changes (+13 net: the 8 of
9 target files that don't need `__curPos`, plus 6 bonus files this
pass's `fromTOML`/`fromJSON` fixes also happened to unblock
(`eval-okay-fromTOML-006/-007/-008/-060.nix`, the main
`eval-okay-fromTOML.nix`, and `eval-okay-fromjson-escapes.nix`), minus
the one `eval-fail-fromTOML-timestamps.nix` flip documented above).

## M58 scope notes: `derivation` is a real Nix-level wrapper around `derivationStrict`

Found continuing to chase `d.nix` right after M48: once nixpkgs' bootstrap
chain got far enough to evaluate `pkgs/os-specific/linux/minimal-bootstrap/`
(reached indirectly as a sibling dependency, not something `d.nix` asks
for directly), it aborted with "'.' selection target is not an attrset
(got a string), aborting". Root-caused with gdb (the by-now-standard
technique: break on `asnix_bshared_select_type_fatal`, read `%rsi`'s
IrOp operand for the symbol id *before* the jump that skips loading it,
resolve via `asnix_sym_name`): the selection was `.outPath`, on a value
whose string content was a derivation's own store path
(`mescc-tools-extra-1.9.1`). Chasing further (another gdb session,
this time printing the calling primop's own argument attrset) showed
the attrset in question was `mescc-tools-extra/default.nix`-style
`callPackage` auto-args, and the specific Nix-level culprit was
nixpkgs' `lib.extendDerivation` (`lib/customisation.nix`, used by
every `mkDerivation`-built package and by minimal-bootstrap's own
`derivationWithMeta` helper), which does
`inherit (drv.${outputName}) type outputName;` and
`drv.${outputName}.outPath` -- both require `drv.out` (for the
single-output case) to itself be attrset-shaped, so its OWN `.outPath`
can be selected a second time.

Confirmed directly against real Nix that this is exactly right:
`builtins.typeOf (derivation {...}).out` is `"set"`, not `"string"`.
asnix's `derivation.c` (`asnix_prim_derivation`) was setting
`out = outPath` as a bare string -- simply wrong, inherited unchanged
all the way from M8. The real reason: `derivation` is **not a primop**
in real Nix at all -- it's a short Nix-level function
(`~/nix/src/libexpr/primops/derivation.nix`, embedded into the `nix`
binary, the same `#embed`-at-compile-time mechanism M46 already found
for `<nix/fetchurl.nix>`) that calls the actual primop,
`derivationStrict`, and then builds a self-referential "nice" object
on top: for each output name (single `"out"` is all this project
supports), `commonAttrs // { outPath; drvPath; type; outputName; }`,
where `commonAttrs` already contains the original input attrs
(`name`/`system`/`builder`/...), an `all` list, and `drvAttrs` --
and, crucially, `commonAttrs` itself is built via
`drvAttrs // (listToAttrs outputsList) // {...}`, so `commonAttrs.out`
(and therefore `drv.out`) ends up pointing back at the very attrset
being constructed. A `let`-bound mutual recursion between
`commonAttrs` and `outputsList`, which asnix's `let` already supports
correctly (confirmed directly:
`let a = {x=1;} // b; b = {y = a.x + 1;}; in a.y` → `2` on both
engines) -- the fix needed none of the "disproportionate" infra the
M8-era design doc worried about.

**Implementation**: ported `derivation.nix` verbatim (its leading doc
comment dropped, no semantic effect) into a new
`ASNIX_DERIVATION_NIX_SOURCE` string constant in `derivation.c`,
written to a fixed temp path once per process and evaluated via
`asnix_eval_file`, exactly mirroring M46's `<nix/fetchurl.nix>`
pattern. The existing C primop (`asnix_prim_derivation`, unchanged)
is now bound as `derivationStrict` (both the bare global and
`builtins.derivationStrict` -- confirmed real Nix exposes it as a bare
global too: `builtins.isFunction derivationStrict` is `true` there).
`derivation` itself (both bare and `builtins.derivation`) is now
`asnix_make_derivation_wrapper_value()`'s result: the wrapper source
lexed/parsed/compiled/evaluated exactly once and cached as a process-
wide static `Value`, since it's a pure function with no caller-specific
state -- reusing one across every root Env's `derivation` binding is
behaviorally identical to rebuilding it fresh per file-import (which
every *other* root-env slot still does) without paying the parse/compile
cost repeatedly.

One real wrinkle: bootstrapping the wrapper creates a self-referential
construction problem, not just a self-referential *value* -- building
ANY root Env eagerly constructs all fifteen bare-global slots
(including `derivation` itself) and the whole `builtins` attrset
(including its own "derivation" entry), so evaluating the wrapper
file's OWN root Env would try to build the wrapper again, forever.
Fixed with a narrow, explicit escape hatch rather than making
construction lazy everywhere: `eval_file.c`'s `make_root_env` became
`make_root_env_ex(up, skip_derivation_wrapper)` and
`builtins.c`'s `asnix_make_builtins_attrs` became
`asnix_make_builtins_attrs_ex(skip_derivation_wrapper)`; passing 1
substitutes the plain `derivationStrict` primop for slot 8 (and for
`builtins.derivation`) in that one bootstrap evaluation only -- correct
because the wrapper source never references bare `derivation`, only
`derivationStrict`. `asnix_eval_file_for_derivation_bootstrap` is the
dedicated entry point that passes 1; every other caller still goes
through the ordinary `asnix_eval_file`/`make_root_env` (`skip=0`) path
unchanged.

Root env/builtins-name-list bookkeeping: the root `Env` grew from 14 to
15 slots (`derivationStrict` is new, slot 14); `compiler.c`'s
`init_builtins_frame` scope-name list grew to match.

**Known, pre-existing limitation surfaced (not caused) by this fix, fixed
immediately after as a small follow-up (same milestone, no new number)**:
asnix's printer had no cycle/sharing detection at all (real Nix prints
`«repeated»` for a value it's already printed higher up the same
structure, see `nix-instantiate --eval`'s own output on
`(derivation {...}).all`). Since `derivation`'s result is now
genuinely self-referential (`drv.out.out == drv.out`, matching real
Nix exactly), printing the WHOLE structure (`d.all`, or bare `d`
itself) recursed forever and segfaulted on asnix, where real Nix
produces a finite, `«repeated»`-marked printout. Not hit by `d.nix`
(which only ever selects `.outPath`, a plain string leaf, never prints
the whole derivation object) or by anything in `test-e2e`/
`test-conformance` -- found verifying this milestone by hand, not by
either test suite.

Fixed in `main.c`'s `print_value_inner`: a small fixed-size stack
(`g_print_active`, 4096 entries, same "small bounded cap" convention
this file's attrset printer already uses) of currently-active
ATTRS/LIST base pointers (an actual DFS-ancestor check, not a broader
"seen anywhere" dedup -- pushed on entering that case, popped on
leaving it), printing `«repeated»` instead of recursing when a value
already on that stack comes back around. Verified directly: both a
genuine list self-cycle (`let a = [ 1 a ]; in a`) and M58's
derivation self-reference now print a finite result with no crash on
asnix. Does NOT byte-for-byte match real Nix's exact repeat points in
every case -- confirmed real Nix's own dedup is keyed by `Value*`
*slot* identity (whether two attribute bindings happen to share the
literal same `Value` cell after a `//`-merge), not by the underlying
heap object's identity the way this fix does, so a value reachable via
two *different* Value slots that both resolve to the same shared heap
list/attrs can print as fully expanded on real Nix (one such case:
`derivation {...}`'s `commonAttrs.all` field) where asnix's simpler,
purely-structural check correctly collapses it to `«repeated»` one
level earlier -- never produces a wrong *value*, no crash either way,
just a cosmetic difference in exactly where the ellipsis lands,
undetectable by anything that doesn't print a self-referential value
in full. `make test`/`test-e2e`/`test-conformance` all still green,
zero regressions (nothing in either suite constructs a genuinely
cyclic value today).

Verified directly against real Nix (`/tmp/m58_check.nix`-style
fixture): `builtins.typeOf d.out == "set"`, `d.out.outPath == d.outPath`,
`d.out.outputName == "out"`, all `true` on both engines. New
`tests/e2e/m58_derivation_self_ref_out.nix` fixture added (`true`).
`make test`/`test-e2e` both green, zero regressions. Before/after
`git stash` comparison on a quick custom harness over
`/tmp/nix-conformance/lang`'s `eval-okay-*.nix`/`eval-fail-*.nix` files
(not the same methodology as this file's other `test-conformance`
numbers above, which run this project's own small mined
`tests/conformance/` subset -- noted here as a distinct, one-off check,
not a new standing baseline): 553 passed/105 failed/148 skipped before
this fix, 556/102/148 after -- +3 net, zero regressions.

## M59 scope notes: `builtins.unsafeGetAttrPos` -- a deliberately narrow, approximate implementation

**Why this exists despite the standing scope cut.** This file's Known
Gaps list (and the M54/M51 notes above) has long documented
`builtins.unsafeGetAttrPos`/`__curPos`/position-tracking in general as
skipped because "asnix's AST/IR carries no source-position information
anywhere ... implementing this would mean threading position info
through the compiler and IR, a cross-cutting change disproportionate to
the handful of files that need it." That judgment was, and still is,
correct for doing this *fully* -- real file/line/column for every
individual attribute would mean growing the runtime Attr record
(`ASNIX_ATTR_SIZE`/`ASNIX_ATTR_OFF_SYM`/`ASNIX_ATTR_OFF_VALUE`,
`include/asnix/heap.h`), a change dozens of call sites across both C
and asm (`op_select.S`'s binary search, `op_mk_attrs.S`,
`derivation.c`, `attrs_update.c`, `builtins_attrs2.c`'s `listToAttrs`,
...) would have to agree on. This pass implements something narrower
and explicitly approximate instead, per a direct instruction to do so
in place of that larger feature: get the function to exist, return the
right *shape*, get the null-vs-not-null distinction exactly right, and
document plainly what's approximated -- **without** growing the Attr
record and **without** touching the lexer (`src/c/lexer.c`/`lexer.h`)
at all.

**Verified against real Nix first** (`nix-instantiate (Determinate Nix
3.22.5) 2.35.2`, matching this repo's existing reference version):
`unsafeGetAttrPos name attrset` returns `{ file; line; column; }` for an
existing attribute (confirmed exact: a `let a = { x = 1; }; in
builtins.unsafeGetAttrPos "x" a` fixture file has `x` at column 11 on
line 1, and real Nix reports exactly `{ column = 11; file =
"/tmp/t.nix"; line = 1; }`; a four-line fixture with `bar` on line 4
reports `line = 4; column = 5` exactly matching the source), or `null`
for a missing one -- and it's `builtins`-only, confirmed by
`nix-instantiate --eval --strict -E 'builtins.isFunction
unsafeGetAttrPos'` failing with "undefined variable 'unsafeGetAttrPos'"
(unlike `scopedImport`, M54, which real Nix DOES expose as a bare
global). Also notable: real Nix's own position tracking is disabled
for `-E`/`--expr` string-sourced expressions (reports `null` even for
an attribute that exists) but works for real files -- not reproduced
here, irrelevant to asnix's own CLI shape (no `-E` flag exists).

**What's exact:** the null-vs-not-null distinction. `unsafeGetAttrPos`
(new, `src/c/builtins_m54.c`, added to the existing `ASNIX_M54_BUILTINS`
`{name,build()}` table alongside `scopedImport`/`trace`/etc., curried
via the same `asnix_bshared_make_curried_native` two-arg pattern every
other arity-2 builtin in this codebase uses) does a real membership
check (`asnix_bshared_attrs_find`, the same helper `hasAttr`/`getAttr`
use) and returns real `ASNIX_NULL` when `name` isn't one of
`attrset`'s attributes -- exactly matching real Nix, with no
approximation at all on this half of the contract. It's wired into
`asnix_make_builtins_attrs()` only (via the M54 table, which only ever
feeds `builtins.X`), with no bare-global root-Env/`compiler.c`
counterpart the way `scopedImport` has one -- matching the
confirmed-above real-Nix behavior that this is `builtins`-only.

**What's approximate, and why, for the non-null case:**

- **`file`** is the resolved absolute path of the OUTERMOST file asnix
  was originally invoked on (argv's `<file.nix>` in `main.c`), reported
  *uniformly* for every attribute that exists, regardless of which
  actual file/import actually wrote that specific binding. This needed
  one small new piece of plumbing, deliberately kept to the minimum:
  a new file-scoped global in `eval_file.c`, `g_top_level_file`
  (exposed via a new getter, `asnix_eval_top_level_file()`,
  `eval_file.h`), set once -- guarded, so a nested `import`/
  `scopedImport` re-entering `asnix_eval_file`/`asnix_eval_file_scoped`
  never overwrites it -- to a `strdup`'d copy of the resolved path (a
  copy, not the `resolved` pointer `asnix_eval_file` itself frees before
  returning, since a value `unsafeGetAttrPos` built can still be
  forced later, e.g. during `main.c`'s post-return `print_value()`,
  after that free already happened). This mirrors an existing,
  narrower precedent already in the codebase: `compiler.c`'s own
  `g_compile_base_dir` global, used for `AST_PATH` resolution, is the
  same "one file-scoped global tracking which file is currently
  relevant" idea, just scoped to directories-during-compile rather than
  the one top-level file-during-the-whole-run. Real per-binding file
  tracking (the file a *specific* attribute's value-literal was
  actually written in, which could be any file reached via any chain of
  `import`/`//`/`inherit` before the attrset reaches this call) would
  require the attrset itself to carry that information -- which is
  exactly the Attr-record growth this pass is scoped to avoid. `""` if
  no file has been evaluated yet (shouldn't happen via `main.c`'s normal
  entry point; only reachable in principle via `asnix_eval_expr_source`,
  the `--arg`/`--argstr` CLI path, which has no file of its own).
- **`line`/`column`** are **fixed placeholders, always `1`/`1`** --
  not read from anywhere, and not even an attempt at "the position of
  the file's top-level expression" (computing even that much would
  still require correlating an AST node back to a Token's `pos`, then
  computing a line/column by scanning the source buffer, none of which
  exists anywhere in the current pipeline). The one piece of real
  per-token position data that does already exist in this codebase
  without any lexer change -- `lexer.h`'s `Token.pos`, a `const char *`
  into the source buffer, present on every token -- is read by
  `parser.c` only long enough to build `AstBinding`/`AstDynBinding`
  (`src/c/ast.h`), which keep NO position field at all, and is
  discarded once the `Ast` tree is built; by the time any value reaches
  a runtime `Attrs` object, there is no path left from "the attribute
  that exists" back to "the token that named it" without adding a field
  somewhere compile-time AND runtime both reach -- i.e. growing either
  `AstBinding` (compile-time only, insufficient by itself) *and* the
  Attr record (what this pass is explicitly scoped to avoid). A fixed
  `1`/`1` was chosen over any fancier guess specifically to avoid
  implying false precision -- it reads as an obvious placeholder rather
  than a plausible-but-wrong real position.

**Fixtures:** `tests/e2e/m59_unsafegetattrpos_exists.nix` (checks
`builtins.isAttrs`/`isString pos.file`/`isInt pos.line`/`isInt
pos.column` and the exact `{column;file;line;}` attribute-name shape
via `attrNames`, deliberately NOT asserting the approximate `file`
string's exact value or the placeholder `line`/`column` numbers, per
the above), `tests/e2e/m59_unsafegetattrpos_missing.nix` (asserts
`unsafeGetAttrPos "y" a == null` exactly, no approximation on this
side). Both generated against, and matching, real `nix-instantiate
--eval --strict` first.

**Regression:** `make test` (unit, all green) and `make test-e2e` (238
fixtures, up from 236, the two new `m59_*` files; all green, no
regressions) both clean. `test-conformance`: this worktree's own small
curated `tests/conformance/` subset (19 files) unchanged byte-for-byte
before/after. The full external numtide/nix-conformance `lang/` corpus
(`cd /tmp/nix-conformance && ./run adapters/asnix-m59 lang`, a scratch
adapter pointed at this worktree's own build, same convention as every
prior pass's own scratch adapter) reports **687 passed, 115 failed, 0
known, 62 skipped both before and after** this change, byte-identical
pass/fail lists -- expected and unsurprising: no `lang/` corpus file
exercises `unsafeGetAttrPos` (it needs `__curPos`-adjacent exact
position tracking to be a meaningful differential test, which is
exactly the feature this pass does not attempt).

## M60 scope notes: `builtins.unsafeDiscardStringContext`

Found continuing to chase `d.nix` right after M58/M59: once nixpkgs'
bootstrap chain evaluates far enough (M58 having fixed `derivation`'s
self-reference), the next blocker was "attribute
'unsafeDiscardStringContext' missing" -- real nixpkgs code (e.g.
`pkgs/build-support/fetchurl/default.nix`-adjacent helpers) calls this
on `url`/`name`-shaped strings before using them as plain env-var
values. Same situation M54's `getContext`/`appendContext` already
documented: asnix has no string-context tracking at all anywhere in
this codebase, so with nothing ever attached to any string, discarding
context is already a no-op. Implemented as a straight identity
function (`builtins`-only, confirmed via `nix-instantiate --eval
--strict -E 'builtins.isFunction unsafeDiscardStringContext'` failing
with "undefined variable" -- not a bare global), added right next to
`getContext`/`appendContext` in `builtins_m54.c`'s existing table.
Verified against real Nix: `builtins.unsafeDiscardStringContext "foo"
== "foo"`.

`make test`/`test-e2e` green (one new `m60_*` fixture), `test-conformance`
(mined subset) unchanged. `d.nix` progresses past this exact blocker to
the next one.

## M61 scope notes: real string-context tracking -- investigated, no fix

A follow-up triage pass, prompted by a cluster of `numtide/nix-
conformance` failures, on whether asnix can add *real* (non-empty)
string-context tracking -- the Nix feature where a string's own
construction carries forward an invisible set of store-path
dependencies (`"${drv}/bin/foo"`'s context includes `drv`'s output) --
as opposed to the no-op `getContext`/`appendContext`/
`unsafeDiscardStringContext` M54/M60 already ship (correctly, those
comments say, "given nothing is ever tracked"). **Net result: no code
change**, same category of outcome as M55's regex triage.

**Categorizing the 14 named candidate files** (read every `.nix` +
`.exp`/`.err.exp` pair, cross-checked live against
`nix-instantiate --eval --strict`, not just trusted against the
corpus's own `.exp`, per this pass's own brief -- corpora do drift):
7 of the 14 are unrelated to context at all and were left alone:
`eval-fail-drv-outputs-duplicate.nix`/`eval-fail-drv-fixed-two-
outputs.nix` (derivation multi-output *validation*, squarely another
concurrent agent's `derivation.c` territory -- not touched, to avoid
collision); `eval-fail-elem-derivation-type-throws.nix`/`eval-fail-eq-
derivation-type-throws.nix` (an unrelated `isDerivation`/`type`-
forcing-during-equality exception-propagation bug, nothing to do with
strings); `eval-fail-json-attr-not-json.nix`/`eval-okay-json-attr-
normalised.nix` (JSON-parsing/canonicalization of `__json`, `fromJSON`
already exists and is the relevant code, not context); `eval-fail-bad-
string-interpolation-2.nix` (a nonexistent path literal failing at
path resolution, before context would ever enter the picture).

The other **7 genuinely do need real context tracking**:
`eval-okay-context.nix`, `eval-okay-context-builtins.nix`, `eval-okay-
context-inputs.nix`, `eval-okay-context-introspection.nix`, `eval-okay-
substring-context.nix`, `eval-fail-context-attr-name.nix` (a context-
carrying string used as a dynamic attribute name must throw -- M57's
own notes already flagged this exact file as skipped), and `eval-fail-
drv-tofile-output-reference.nix` (M45's own notes already flagged this
exact file as a known, accepted regression once `toFile` existed,
"detecting this requires string-context tracking, which doesn't exist
in this project at all").

**Why this is disproportionate, not just "more work than one pass"**:
this is the third time this codebase's own history has independently
reached for this conclusion (M45, M54, M60 each separately call it "a
substantially larger, separate feature" / "a substantially larger
scope cut") -- this pass re-verified *why*, concretely, rather than
taking that on faith:
- **Representation** is the easy part and does fit the project's
  established precedent: a fourth marker-tagged `ASNIX_TAG_PTR` kind
  (`ContextString`, alongside Path/Float) carrying a trailing list of
  context-element strings, dispatched in `asnix_ptr_bytes`/
  `asnix_value_is_*` exactly the way Path/Float already are. Not where
  the size is.
- **Propagation is the real surface area.** `eval-okay-context-
  builtins.nix` alone enumerates, by name, which string builtins must
  *keep*, *drop*, or *transform* context: string interpolation,
  `substring` (including the "empty substring keeps context" case),
  `replaceStrings` (both as source AND when a replacement value itself
  carries context), `concatStringsSep`, `toJSON`, `toString`,
  `baseNameOf`, `dirOf`, `hashString` (drops), `match`/`split` (drops,
  "on purpose"), plus `unsafeDiscardStringContext`,
  `unsafeDiscardOutputDependency`, and `addDrvOutputDependencies` --
  the latter two don't exist in this codebase at all yet (confirmed:
  zero hits), and real Nix's semantics for them require a genuine
  tagged-union context-ELEMENT type (`Built{drvPath,outputs}` /
  `DrvDeep` / `Opaque{path}`), not just "a set of strings" --
  `appendContext`'s own test constructs exactly these three shapes
  (`{outputs=[...];}`, `{allOutputs=true;}`, `{path=true;}`) and
  expects them preserved distinctly. `asnix_ptr_bytes`/
  `asnix_make_string_value` alone have 101 call sites across
  `src/c/*.c`; every one that *builds* a new string from pieces would
  need an explicit context-union decision, verified correct per-
  builtin against the list above, not just "doesn't crash."
- **Origination reaches into derivation.c's hashing, which is live,
  contended territory.** `eval-okay-context-inputs.nix` requires a
  derivation's `drvPath`/`outPath` to change depending on which
  context a string *attribute value* passed to `derivation {...}`
  carries (context flows into `inputDrvs`/`inputSrcs` in the `.drv`
  ATerm, changing its hash) -- i.e. the fix's biggest single piece
  lands directly inside `derivation.c`'s output-hash computation,
  which another agent is concurrently rewriting for multi-output
  support in this same batch. Doing this properly here risks either
  duplicating or directly conflicting with that work.

Per this task's own step 4 ("if this needs a genuinely large change
disproportionate to the handful of files it would fix -- STOP and
report"): stopping here. A correct narrow slice (e.g. "only track
context through plain `${drv}` interpolation, nothing else") was
considered and rejected -- `eval-okay-context-builtins.nix` would still
fail on effectively every other line, since it checks *all* the
builtins above in one file, so a narrow slice buys close to nothing
against the one richest test while still adding a new heap-object kind
and an `asnix_ptr_bytes` dispatch path that every one of those 101
call sites now has to be re-reasoned about for correctness (silently
dropping or silently retaining context wrongly is worse than today's
honest, documented no-op). No files changed; `eval-fail-context-attr-
name.nix` and `eval-fail-drv-tofile-output-reference.nix` remain
exactly the documented, accepted gaps M57/M45 already called out.
`make test`/`test-e2e`/`test-nix-conformance` all unchanged (no code
touched).
## M62 scope notes: `derivationStrict` generalized to real multi-output derivations

Found continuing to chase `d.nix` right after M60: the next blocker was
"attribute 'bin' missing, aborting". Root-caused directly with gdb
(breaking on `asnix_bshared_select_dynamic`'s fatal path, dumping the
target attrset's keys): some package deep in nixpkgs' dependency graph
declares `outputs = [ "out" "bin" ... ]` (an ordinary multi-output
derivation -- most compilers/libraries split `out`/`bin`/`dev`/`lib`/
`man`). The failing lookup was `strict.${outputName}` inside M58's
`derivation.nix` wrapper, with `outputName = "bin"`, and `strict` (the
result of `asnix_prim_derivation`) ALWAYS had exactly 4 keys
(`outPath`, `drvPath`, `type`, `out`) regardless of the caller's
`outputs` -- confirmed `asnix_prim_derivation` never even read the
`outputs` attribute at all, it hardcoded a single output named `"out"`.

Verified real Nix's actual behavior first, directly (not just from
reading `primops.cc`/`derivations.cc`):
`nix-instantiate --eval --strict` on `derivation { ...; outputs =
["out" "bin"]; }` confirms `d.bin.outPath` is a genuinely different
store path from `d.outPath`/`d.out.outPath`, and that
`derivationStrict`'s own result (not the `derivation` wrapper's) is
just `{ drvPath; <outputName> = <that output's outPath string>; ... }`
-- a plain string per output, no nested attrset, no separate
`outPath`/`type`/`out` keys (confirmed via
`~/nix/src/libexpr/primops.cc`'s `mkOutputString`). Also confirmed the
output-path `name` component rule directly: output `"out"` uses the
derivation's own `name` verbatim, every other output uses
`name + "-" + outputName` (`outputPathName` in
`~/nix/src/libstore/derivations.cc`) -- e.g. `x` and `x-bin` for the
two outputs above.

**Algorithm, read directly from real Nix's
`derivationStrictInternal`/`processDerivationOutputPaths`
(`primops.cc`/`derivations.cc`) before implementing**: the existing
masked/real two-pass hashing scheme generalizes with exactly ONE
masked hash shared across every declared output (not one per output):
pass 1 blanks every output's own env var to `""` AND lists every
output in the ATerm's `Outputs` section as `(name,"","","")`, hashes
that once; pass 2 computes each output's own distinct store path as
`makeStorePath("output:"+outputName, maskedHash,
outputPathName(drvName, outputName))` -- same masked digest, only the
preimage `type` string and store-path `name` component vary per
output -- fills every output's env var in with its own real path, and
re-hashes the now-fully-filled-in ATerm once for the `.drv` itself
(unchanged from M8: `type = "text"`, name `<drvName>.drv"`).

**Implementation**: `derivation.c`'s `asnix_prim_derivation` now parses
an optional `outputs` attribute (list of strings, default `["out"]` --
same default `ASNIX_DERIVATION_NIX_SOURCE`'s own `outputs ? [ "out" ]`
already assumed) into a small fixed-size array
(`ASNIX_DRV_MAX_OUTPUTS`, 16, same "small bounded array, fatal if
exceeded" convention as `ASNIX_DRV_MAX_ARGS`/`ASNIX_DRV_MAX_ENV`) --
rejecting duplicates and the reserved name `"drvPath"`, matching real
Nix's own `handleOutputs`. Unlike `args`, parsing `outputs` does NOT
skip the generic env-var path: real Nix's own multi-output `.drv`
files genuinely have an `outputs = "out bin"` space-joined env var too
(the existing list-to-string coercion from M47 already produces this
for free), consumed separately by build-time setup hooks. The masked/
real two-pass hashing loop generalized directly per the algorithm
above (new `output_path_name` and `atermoutput_cmp` helpers); the
result attrset now has `1 + noutputs` keys (`drvPath` plus one string
per output) instead of the old hardcoded 4, matching real Nix's
`derivationStrict` shape exactly (the old `outPath`/`type`/`out` keys
were dropped -- re-confirmed by re-reading `ASNIX_DERIVATION_NIX_SOURCE`
that the wrapper only ever reads `strict.drvPath` and
`strict.${outputName}`, never `strict.outPath`/`strict.type`).
Fixed-output derivations (`outputHash` given) remain restricted to a
single `"out"` output, explicitly rejected otherwise, matching real
Nix's own restriction there -- multi-output fixed-output/content-
addressed derivations are out of scope (same pre-existing scope cut
`validate_output_hash_fields`'s header comment already documents).

`aterm.c`'s `asnix_build_derivation_aterm` previously hardcoded a
single `"out"` entry in the ATerm's `Outputs` section; generalized to
take a caller-supplied, pre-sorted `AtermOutput{name,path}` array of
arbitrary length (new type in `aterm.h`), writing one `(name,path,"",
"")` tuple per entry -- the other two call sites (`tests/c/
test_aterm.c`, `tools/realize_demo.c`) updated to the new signature
(both single-output; a new `test_multi_output` case added to
`test_aterm.c` covering the 2-output case, verified shape against real
Nix's `~/nix/src/libstore/derivation/aterm.cc` unparse logic directly).

Verified directly against real Nix (`nix-instantiate --eval --strict`
on the exact repro above): `d.bin.outPath` and `d.outPath` are
different store paths, `d.out.outPath == d.outPath` still holds (M58's
self-reference property, specifically for the `"out"` output), both
captured byte-for-byte in two new `tests/e2e/m62_derivation_multi_
output*.nix` fixtures (one boolean-property check, one exact-store-
path check against the real paths `nix-instantiate` computed for `x`/
`x-bin`).

`./asnix d.nix`: confirmed the exact "attribute 'bin' missing,
aborting" blocker is gone (baseline reproduced first: dies in ~1.3s
with that message; after this fix, runs ~1.7s longer before hitting a
new, unrelated segfault deep in `foldl'`/`elemAt` -- a later blocker in
this same standing chase, not chased further here per this task's
explicit scope).

`make test`/`test-e2e` both green, zero regressions (two new `m61_*`
e2e fixtures, one new `test_aterm` case). `make test-nix-conformance`
(full numtide/nix-conformance `/tmp/nix-conformance/lang` sweep):
557 passed/101 failed/148 skipped before this fix, 562/96/148 after --
+5 net, zero regressions. The five newly-passing cases: `eval-okay-drv-
outputs.nix`, `eval-okay-drv-output-named-drv.nix`, `eval-okay-drv-
placeholder.nix`, `eval-fail-drv-outputs-duplicate.nix`, `eval-fail-
drv-fixed-two-outputs.nix`. Explicitly out of scope, still failing,
and needing other infrastructure this task doesn't cover:
`eval-okay-drv-masking.nix` (derivation-to-derivation input masking --
needs real string-context tracking, a `hashDerivationModulo` over
actual input derivations, not just blanked outputs of the SAME
derivation) and `eval-okay-derivation-legacy.nix`/`eval-okay-drv-
structured-attrs.nix` (`__structuredAttrs`, a wholly separate JSON-
based attribute-passing mode this project has no support for at all).

## M63 scope notes: `asnix_apply`'s PrimOp path didn't populate `args[-1]` ("self")

Found continuing to chase `d.nix` right after M62: `./asnix d.nix`
dumped core. gdb's backtrace (unreliable past 1-2 frames in this
threaded-dispatch evaluator, as usual, but the top frame was solid)
showed a segfault inside `prim_elemAt_stage2`
(`builtins_list.c:290`, `Value list = self_captures(args)[0];`),
called from `asnix_apply` (`apply.S`), called from somewhere inside
`foldl'`'s own C loop. `self_captures()` (`builtins_list.c`) recovers
a curried stage PrimOp's own trailing captured Value(s) by reading
`args[-1]` -- a real invariant `op_call.S` guarantees for ordinary
in-language calls (the callee's own tagged Value sits in the VALSTACK
slot immediately before its arguments), which this module's own
header comment already documents, including an explicit (but, it
turned out, incomplete) argument for why `asnix_apply` never needs to
satisfy it: "this module itself only ever uses asnix_apply to call the
*user-supplied* function ... never its own curried stages".

That argument missed one case: the user-supplied function CAN, after
being partially applied, evaluate to one of THIS module's own self-
capturing PrimOps. Minimal repro (verified identical result on real
Nix, `30`, vs. a clean segfault on asnix before this fix):
```
builtins.foldl' (acc: builtins.elemAt [10 20 30]) 0 [0 1 2]
```
`op`'s first application (`asnix_apply(op, &acc, 1)` inside
`builtins_list.c`'s `apply2`) is an ordinary Closure call -- fine, no
issue -- and returns `elemAt [10 20 30]`, itself a curried, self-
capturing `prim_elemAt_stage2` PrimOp instance. `apply2`'s SECOND call
(`asnix_apply(step1, &elem, 1)`) invokes that PrimOp directly through
`asnix_apply`'s own PrimOp branch, which built its own scratch args
buffer with nothing meaningful before it -- `self_captures`'s
`args[-1]` read garbage off `asnix_apply`'s stack frame, segfaulting.
Same risk applies to `map`'s `f`/`filter`'s `pred` (both also
self-capturing via this exact mechanism), not just `foldl'`'s `op`;
only `foldl'` happened to be what `d.nix` hit first.

**Fix** (`src/asm/apply.S`): the PrimOp branch's scratch buffer gained
a fifth header slot ("self", holding the original tagged PrimOp Value
straight from `%rdi`, which is never clobbered before this point) at
offset 32, with the forced-argument copy shifted from offset 32 to 40
to make room -- `ASNIX_APPLY_PRIMOP_SCRATCH_BYTES` stays 104 bytes
unchanged (40-byte header + 8 Value slots, still satisfying the
existing `%rsp ≡ 0 (mod 16)` requirement with no new padding needed).
`args[-1]` (i.e. offset 32, one slot before the offset-40 forced-args
copy handed to `fn`) now holds exactly what `op_call.S` would have put
there, so `self_captures` works identically regardless of which path
invoked the PrimOp. Only fixes the FORCING path; the narrower
`NOFORCE` path (`foldl'`'s stage2 specifically, M53) still hands `fn`
the caller's own raw buffer with no slot before it (that buffer isn't
owned by this function, so there's nothing to retroactively add a slot
to) -- not reproduced, not fixed here, a consistent, narrower instance
of the exact same pre-existing limitation, not a new gap.

Verified: the minimal repro above now returns `30` on asnix, matching
real Nix exactly; `builtins.foldl' (acc: i: acc ++ [(builtins.elemAt
[10 20 30] i)]) [] [0 1 2]` and `builtins.map (builtins.elemAt [10 20
30]) [0 1 2]` (two shapes that happened NOT to trigger the bug, since
their intermediate values go through an ordinary lambda/lazy-thunk
path instead) still both give `[ 10 20 30 ]`, confirming no regression
in the already-working cases. `./asnix d.nix` progresses past this
exact segfault to whatever comes next in the standing chase.

`make test`/`test-e2e` both green (one new `m63_*` fixture, verified
against real `nix-instantiate --eval --strict` first), zero
regressions. `make test-conformance` (mined subset) unchanged.
## M64 scope notes: `~`/interpolated path literals; `builtins.path` (scoped out)

`make test-nix-conformance` triage: `eval-okay-home-path-not-normalised.nix`
and `eval-okay-path-string-interpolation.nix` both failed with `asnix:
unrecognized character '~' in source` -- this lexer had no support at all
for `~`-prefixed ("home") path literals. `eval-okay-path.nix` failed
separately with `attribute 'path' missing, aborting` (`builtins.path` not
implemented at all).

**`~` semantics, verified against real `nix-instantiate --eval --strict`
with `HOME=/fake-home` set** (cppnix's own `src/libexpr/parser.y`,
`path_start: HPATH { ... }` action): a home literal's text (after the
leading `~`) is concatenated onto `$HOME` *literally* -- no `canonPath`
call at all, unlike an ordinary `./`/absolute path literal (which this
project's `resolve_path_literal`/`asnix_canon_path` already collapses
`.`/`..`/doubled slashes in at compile time). This is exactly what the
fixture's name ("not normalised") is about: `~/a/../b` stays
`$HOME/a/../b`, never collapsed to `$HOME/b`.

**The harder discovery**: fixing just `~` wasn't enough for
`eval-okay-path-string-interpolation.nix` -- that file also exercises
`./${foo}`, `/${foo}`, `./a-${foo}-b`, etc., and interpolated path
literals (independent of `~`) turned out to be *entirely unimplemented*
already (`let foo = "x"; in toString ./${foo}` gave a parse error before
this milestone, with no `~` involved at all). Since
`eval-okay-home-path-not-normalised.nix` itself has one interpolated-home
case (`~/${"a"}/../b`), there was no way to scope this down to "just `~`,
no interpolation" and still fix either target file. Implemented general
path-literal interpolation as the real underlying feature, with `~`
support riding on top of it.

**Grammar, ported from cppnix's `src/libexpr/lexer.l`/`parser.y`
directly** (verified line-by-line against both, then against
`nix-instantiate`): a path literal's lexing works exactly like a
double-quoted string's (`TOK_STR_START`/`TOK_STR_PART`/
`TOK_DOLLAR_LBRACE`/`TOK_STR_END`, already in this codebase since the M5
interpolation work) -- `./${foo}` lexes as an initial `PATH`-equivalent
chunk ("./"), `TOK_DOLLAR_LBRACE`, the embedded expr, `TOK_RBRACE`, then
a final chunk + a new `TOK_PATH_END` boundary marker (mirrors real Nix's
own `PATH_END` token, which fires unconditionally after *every* path
literal, interpolated or not -- this project only emits it when
interpolation is actually present, to avoid touching the existing
single-token `TOK_PATH` path for the common case). One real-grammar
subtlety ported faithfully: real Nix's `PATH`/`HPATH` regex requires at
least one full `/PATH_CHAR+` group, which can't match a bare `/`
immediately followed by `${` (zero chars between); a separate
`PATH_SEG`/`HPATH_START`-before-`${` lookahead rule handles exactly that
case. Ported as `scan_path_groups` (lexer.c): its group-loop only
special-cases a `/` with zero following path-chars when it's *directly*
followed by `${` -- gated this way specifically so an in-progress `//`
(update operator) is never miscounted as a 1-or-2-char path (verified:
a blind "always consume a trailing bare slash" version of this function
broke `a // b` parsing by eating both slashes as separate paths before
`scan_path_groups`'s `saw_group`-gating was added).

**Compiler/runtime, mirroring real Nix's `ExprConcatStrings::eval`
exactly** (`~/nix/src/libexpr/eval.cc`, verified directly): the FIRST
chunk of an interpolated path resolves to a genuine Path *constant* at
compile time (`resolve_path_chunk` in compiler.c) -- for the `~` case,
the same no-canon concat-onto-`$HOME` formula as the plain literal case
(same formula either way, confirmed in cppnix's source: the HPATH parser
action doesn't distinguish "complete literal" from "first chunk before
interpolation", it's the same grammar rule either way); for the plain/
absolute case, canonicalized same as an ordinary literal, but with ONE
difference -- a trailing `/` is preserved (real Nix: "add back in the
trailing '/' to the first segment") rather than rejected, since it's
exactly what lets the next part glue on with the right separator (`./`'s
resolved form must end in `/` so concatenating `"foo"` after it reads
`.../foo`, not `.../dirfoo`) -- EXCEPT when the whole chunk is just a
bare `/` (`literal.size() > 1` in real Nix's own condition; ported
verbatim as `len > 1`), since `CanonPath("/")` is already `"/"` and a
second slash would wrongly produce `//foo`. Every subsequent chunk/part
pushes as an ordinary String constant or compiles as an ordinary
expression; a new op, `OP_PATH_CONCAT` (`src/asm/ops/op_path_concat.S`,
`asnix_concat_path` in value_util.c), concatenates everything exactly
like the existing `OP_STR_CONCAT`/`asnix_concat_strings` (string
interpolation's runtime half) does, then *unconditionally*
re-canonicalizes the whole result and rewraps it as a Path -- matching
real Nix's own unconditional re-canonicalization of the full
concatenated string whenever the first operand was a Path, regardless of
that first operand's own text having been deliberately left
un-canonicalized (real Nix's comment: "skip canonization of first path,
which would only be not canonized in the first place if it's coming from
a `./${foo}` type path"). This is why `~/${"a"}/../b` canonicalizes to
`$HOME/b` even though a bare `~/a/../b` (no interpolation at all) stays
`$HOME/a/../b` uncollapsed -- same `~` formula for the first chunk either
way, the difference is entirely in whether anything downstream of it
ever re-canonicalizes. `canon_wrap_as_path`, the inner canon+rewrap
helper, is shared with `asnix_add_values`'s existing `path + string`
handling (M57g) rather than duplicated.

Verified end-to-end against real `nix-instantiate --eval --strict` with
`HOME=/fake-home` set (sandboxed `HOME=` assignment required routing
through a tiny wrapper script rather than an inline shell prefix, purely
a tooling quirk of this session, not relevant to the fix itself): both
target fixtures now match byte-for-byte --
`eval-okay-home-path-not-normalised.nix`'s `[ "/fake-home/a/../b"
"/fake-home/./a" "/fake-home/a/.." "/fake-home/../x" "/fake-home/a/b"
"/fake-home/b" ]` and `eval-okay-path-string-interpolation.nix`'s `{
absolute = /foo; expr = /pwd/lang/foo/bar; home = /fake-home/foo;
notfirst = /pwd/lang/bar/foo; simple = /pwd/lang/foo; slashes = /foo/bar;
surrounded = /pwd/lang/a-foo-b; }` (the `/pwd` placeholder is
`test-nix-conformance`'s own repo-root substitution, added below, not
baked into asnix's output).

**`make test-nix-conformance` harness gap, fixed alongside**: the target
didn't set `HOME` or do nix-conformance's own `/pwd` repo-root
substitution (`run`'s `printf '%s\n' "${out//$root//pwd}"`), both part of
the corpus's own documented adapter protocol (`README.md`'s "Writing an
adapter": `HOME=/fake-home` is one of the fixed env vars every adapter is
invoked with). Without both, neither target fixture's `.exp` could ever
byte-match no matter how correct asnix's own resolution was. Added both
to the `Makefile` recipe, matching the upstream protocol exactly rather
than inventing a separate convention -- safe for the same reason in both
cases: a `.exp` file can never itself contain this clone's real `$HOME`
or local checkout path, so the substitution/env-var is a no-op on every
comparison that wasn't already destined to fail. (Bonus, not required by
this milestone: a handful of other already-failing, already-documented
"not mechanically comparable yet" cases -- `eval-okay-dotslash-path-
fatal.nix` and siblings -- are now comparable too, though none happened
to flip to passing on this pass.)

**`builtins.path`: scoped out entirely, not implemented.** Real Nix's
`builtins.path { path; name; filter; recursive; sha256; }` needs to walk
a real directory tree, build a NAR serialization of it (calling the
user's `filter` callback per entry when given), hash that with SHA-256,
then assemble a content-addressed store path from the result (or, for
`recursive = false`, hash the raw bytes of a single real file instead) --
genuine filesystem content-addressing infrastructure this project has
already, repeatedly decided not to build (same documented precedent as
`derivation.c`'s fixed-output-derivation validation, M48: "this... does
NOT change how the derivation's store path is actually computed... which
would need actual content fetching infra this project doesn't have", and
`<nix/fetchurl.nix>`'s M46 notes). Unlike `builtins.toFile` (M45), which
computes a *real*, correct store-path hash because its content is an
ordinary Nix string already in memory (no filesystem read needed),
`builtins.path` has no such shortcut -- every one of its three
real-Nix-shaped behaviors (`recursive = true` NAR hashing with a filter
callback, `recursive = false` flat-file hashing, and the implicit
default-is-recursive case) needs the real file's bytes, and two of the
three additionally need the NAR directory format and a Nix-callback-
per-entry filter mechanism that don't exist here at all. A narrower
"flat-file-only" slice was considered (tractable in isolation: this
project already has SHA-256, base32, and store-path-assembly
primitives, see `store_path.h`) but was rejected as not worth the risk
here -- `eval-okay-path.nix`'s own `.exp` is one 3-element list compared
as a single string, and two of its three elements are the `recursive`
(NAR) case that's out of scope regardless, so even a byte-perfect flat
implementation couldn't flip this specific conformance file to passing.
`builtins.path` continues to fail exactly as before ("attribute 'path'
missing, aborting").

Two new e2e fixtures: `tests/e2e/m64_home_path.nix` (`~`-prefixed
literals, with and without interpolation; self-contained via
`builtins.getEnv "HOME"` rather than a baked-in path, so it isn't tied to
whatever `$HOME` happens to be wherever this suite runs) and
`tests/e2e/m64_path_interp.nix` (plain/absolute interpolated paths,
self-contained via comparing against an equivalent non-interpolated
literal rather than printing an absolute path, so it isn't tied to this
clone's checkout location). Both verified against real `nix-instantiate
--eval --strict` first.

`make test`/`make test-e2e` both green, zero regressions (239 e2e cases,
two new `m59_*` fixtures). `make test-nix-conformance`: 556 passed/102
failed/148 skipped before this milestone, 558/100/148 after -- the two
target fixtures flip to passing, nothing else in the 806-file total
changes (totals match exactly both times; the net shift is entirely and
only those two files moving fail->pass). `eval-okay-path.nix` remains in
the fail bucket, as documented above.

## M65 scope notes: `import`-value caching

Found investigating a user-reported concern that `./asnix d.nix` looked
like it was stuck in an infinite loop under `strace -e trace=newfstatat`
(the same handful of paths -- `/tmp/.asnix-corepkgs-fetchurl.nix`,
individual `pkgs/os-specific/linux/minimal-bootstrap/*` files --
appearing tens of thousands of times in the trace). Confirmed it's NOT
an infinite loop (the same run without `strace` completes in ~1.5s;
`strace`'s own enormous per-syscall overhead, applied to hundreds of
thousands of real syscalls, is what made it look like a hang) -- but
it IS a real, severe, self-inflicted amount of redundant work, with a
clear root cause: asnix had no `import`-value cache at all. Real Nix
memoizes `import <path>` by the resolved path, so re-importing the same
file anywhere else in the program returns the exact same (possibly
still-unforced) Value rather than re-evaluating from scratch. Minimal
repro, verified directly against real Nix: a file containing
`builtins.trace "evaluated!" 1`, imported twice and summed, prints
`trace: evaluated!` exactly ONCE on real Nix, but TWICE on asnix before
this fix (confirming the file's own top-level expression -- and
everything reachable from forcing it -- really was being computed
independently per import site, not shared). nixpkgs' own `pkgs/by-name`
overlay and bootstrap chain import the same files (`lib/default.nix`,
`<nix/fetchurl.nix>`, individual package files reached through more
than one alias/overlay path) from many different places, so this
compounded badly.

**Fix** (`eval_file.c`): a simple path -> Value cache
(`g_import_cache`, a plain linear-scan dynamic array -- the number of
distinct files one process imports is small relative to the cost of
each entry, a full parse+compile+eval, so a linear `strcmp` scan's own
cost is negligible next to what it saves). `parse_file`'s path-
resolution step (`resolve_import_path` + the `import somedir` =>
`somedir/default.nix` directory rule) was split out into its own
`resolve_file_path` helper so `asnix_eval_file` can check the cache
*before* paying for a read+lex+parse+compile+eval it might not need --
only a cache MISS falls through to the expensive path, which then
populates the cache with the freshly computed result. Deliberately
scoped to plain `import` only: `asnix_eval_file_scoped`
(`builtins.scopedImport`) is NOT cached, since its extra `scope`
argument can legitimately differ between calls on the same file path,
so caching by path alone would silently return the wrong (first-caller's)
result to a later call with a different scope -- correct per real Nix,
which also does not cache `scopedImport`.

Verified: the `builtins.trace` minimal repro above now prints the trace
line exactly once, matching real Nix. `./asnix d.nix` wall-clock dropped
from ~1.5s to ~0.43s (roughly 3.5x), same exact next blocker as before
this fix (unaffected, unrelated). `make test`/`test-e2e` both green,
zero regressions. `make test-nix-conformance`: 558/100/148 before this
fix (M64's own final numbers), 561/97/148 after -- +3 net, zero
regressions (verified via the failing-file list, not just the
aggregate count) -- some of numtide/nix-conformance's own corpus
apparently already exercises observable import-caching behavior the
same way the `trace` repro does.

## M66 scope notes: `builtins.parseFlakeRef`/`builtins.flakeRefToString`

**Root cause.** `make test-nix-conformance` had 10 failures named after
flake refs (`eval-okay-parse-flake-ref*.nix`,
`eval-okay-flake-ref-to-string*.nix`,
`eval-okay-flake-ref-round-trip.nix`) because `builtins.parseFlakeRef`/
`builtins.flakeRefToString` didn't exist at all. Confirmed by reading
every one of those `.nix`/`.exp` pairs first (plus their
`eval-fail-parse-flake-ref-*`/`eval-fail-flake-ref-to-string-*`
siblings, for validation-rule hints even though they aren't in the
required set) that this is pure string<->attrset syntax -- no flake
resolution, no registry lookup, no filesystem/network access anywhere
in any of them, confirmed directly: `cat eval-okay-parse-flake-ref.nix`
is literally `builtins.parseFlakeRef "github:NixOS/nixpkgs/23.05?dir=lib"`.

**Minimal verified repro.** `nix-instantiate --eval --strict -E
'builtins.parseFlakeRef "github:NixOS/nixpkgs/23.05?dir=lib"'` (real
Nix, Determinate Nix 3.22.5/2.35.2, same reference version this repo's
other scope notes already use) returns `{ dir = "lib"; owner =
"NixOS"; ref = "23.05"; repo = "nixpkgs"; type = "github"; }` -- asnix
previously aborted with "attribute 'parseFlakeRef' missing".

**The spec, reverse-engineered.** The flake-ref grammar is NOT
documented anywhere nix-conformance ships; it was derived field-by-
field from the 10 required `.nix`/`.exp` pairs (every scheme: bare
`nixpkgs`-style indirect ids, `github:`/`gitlab:`, `flake:`, bare `/...`
absolute paths and explicit `path:`, `git+https`/`git+ssh`/`git+file`/
bare `git://`, `tarball+`/`file+`/bare `http(s)://`/bare `file:`), then
cross-checked directly against real `nix-instantiate --eval` for every
case that was genuinely ambiguous from the fixtures alone -- in
particular: the exact percent-encoding "safe" charset (three distinct
ones, found by sweeping every printable ASCII byte through a round-trip
and comparing which survive bare vs. get escaped: a path *segment*
escapes `/` and `%` and most sub-delims, a generic URL *path* additionally
keeps `/` bare, a *query value* additionally keeps `?` bare too); that a
raw, unescaped space in a query silently drops that one key/value pair
("dubious URI query ... missing equal sign, ignoring" on stderr, not an
error); that duplicate query keys use first-occurrence-wins
(`std::map::emplace` semantics); that `github`/`gitlab`/`path` have a
*strict* unknown-query-key error while `git`/`tarball`/`file` are
*permissive* (unknown keys just ride along in the leftover `url`
query) and `indirect` silently *drops* unknown keys; that a literal
`url` query key is always dropped outright for git/tarball/file
(`parseFlakeRef "https://x.org/a.tar.gz?dir=d&url=u"` keeps `dir` but
drops `url=u` entirely); that `indirect` lowercases a hex-shaped `rev`
at *parse* time but `github`/`git` only normalize (lowercase, or decode
a `sha1-<base64>` SRI string to hex) at `flakeRefToString` time -- this
asymmetry is real, confirmed by `eval-okay-flake-ref-round-trip.nix`'s
own `"github:a/b/ABCDEF0123...` entry being `same = false`; and that an
empty `narHash` string is substituted with the fixed all-zero sha256
SRI dummy value (`sha256-AAAA...=`) for every type that keeps `narHash`
except `path` (confirmed: `flakeRefToString { type="path"; path="/a";
narHash=""; }` keeps `narHash=` literally empty, while every other type
substitutes the dummy).

**The fix.** New, self-contained module `src/c/builtins_flakeref.c`/`.h`,
following the established `{name,build()}`-table-plus-integration-step
pattern (`ASNIX_FLAKEREF_BUILTINS`, wired into `builtins.c`'s
`asnix_make_builtins_attrs_ex`'s `build_tables`-adjacent list the same
way M54's own table is, right after it -- a tiny, additive diff there).
Both builtins are single-argument native primops
(`asnix_bshared_make_primop`). Internals: a shared percent-decoder, the
three-charset percent-encoder described above, a generic
"`scheme://[authority]path[?query]`" URL parser/rebuilder (used by
git/tarball/file, including port normalization -- `:080` -> `:80`,
empty port dropped) and six scheme-specific
parse/toString function pairs (forge-style `owner/repo/[ref|rev]`,
`indirect`, `path`, `git`, `tarball`, `file`). All intermediate scratch
buffers are plain `malloc`/`free` (freed before returning), the same
pattern `builtins_m54.c`'s `toXML` already uses for its
`open_memstream` buffer -- final results are always copied into a
fresh arena-backed `Value` via `asnix_make_string_value`/
`asnix_bshared_make_attrs` before returning.

**Scope cuts, documented rather than silently approximated:**
- No actual flake resolution/fetching/registry lookup for bare
  `indirect` refs (`nixpkgs` alone never becomes a real nixpkgs
  checkout) -- confirmed unnecessary by reading
  `eval-okay-parse-flake-ref-indirect.nix` itself: it only ever calls
  `parseFlakeRef`, never anything that would need the fetch to
  actually happen.
- `narHash` is NOT validated as a real SRI hash (real Nix errors "hash
  'x' is not SRI" for a malformed one) -- every fixture this module
  must match only ever gives `narHash` as `""` or an already-correct
  SRI string, so it's treated as an opaque string (plus the one
  confirmed `""` -> dummy substitution) rather than porting
  `Hash::parseAny`'s full validation.
- `sourcehut:` is accepted at the dispatch level (free to add, shares
  the forge-style code path) but not independently verified against
  real Nix -- no fixture exercises it.
- The `eval-fail-parse-flake-ref-*`/`eval-fail-flake-ref-to-string-*`
  siblings (ref-and-rev conflicts, a relative-path reference, a
  negative int in a flakeRefToString attr) are not in the required set
  and aren't graded on exact error text (`test-nix-conformance` only
  checks `eval-fail-*` for a nonzero exit) -- several of the easy ones
  (both-ref-and-rev, negative-int, "./foo" not being a valid absolute
  path) are implemented anyway (cheap, and one -- the path-ref-and-rev
  variant -- was caught as an actual regression during this pass, see
  below), but this wasn't pursued exhaustively.
- `builtins.getFlake` (actual fetching) is untouched -- out of scope by
  design, not attempted.

**A regression caught before committing.** The first working version
only checked `ref`-in-path-vs-`ref`-in-query and `rev`-in-path-vs-`rev`-
in-query conflicts separately, missing the cross case (`ref` from the
path position, `rev` from the query, or vice versa) --
`eval-fail-parse-flake-ref-github-path-ref-and-rev.nix`
(`github:a/b/c?rev=aaaa...`, ref `c` from the path, `rev` from the
query) went from "fails because `parseFlakeRef` doesn't exist" (an
accidental pass) to "fails because it genuinely IS ambiguous" needing a
real check -- caught by diffing the full conformance failure list
before/after rather than trusting the net pass count alone. Fixed by
checking `(has_ref) && (has_rev) && !(both came from the same
multi-segment path position)` as one combined rule.

**Final numbers.** All 10 target files now pass byte-for-byte against
their `.exp` sidecars. `make test-nix-conformance`: pass=557/fail=101
-> pass=567/fail=91 (skip=148 unchanged) -- a clean +10/-10, nothing
else regressed (verified by diffing the full failed-file list
before/after, not just the aggregate counts; one apparent extra
regression, `eval-okay-toxml.nix`, turned out to be a one-off flake --
it passes standalone and on a repeat full sweep). `make test`/`make
test-e2e` stay green, plus 6 new fixtures under `tests/e2e/`
(`flakeref_parse_github`/`_indirect`/`_path`/`_git`,
`flakeref_to_string`, `flakeref_round_trip` -- the last one exercises
`flakeRefToString (parseFlakeRef s) == s` across all five scheme
families on inputs already in canonical form), each verified against
real `nix-instantiate --eval --strict` before being trusted.

## M67 scope notes: external-conformance grab-bag

A "pick off what you can" pass over a batch of unrelated small-to-medium
numtide/nix-conformance gaps handed out as a single list (no shared
root cause), sub-numbered M67a/b/... per item. Baseline before this
pass: `make test-nix-conformance` `pass=557 fail=101 skip=148`; after:
`pass=571 fail=87 skip=148` (net **+14**, confirmed stable across two
repeat runs after an initial run showed 4 extra passes that didn't
reproduce -- `eval-okay-overrides.nix`/`eval-okay-regex-014/113/114.nix`
are flaky under load, unrelated to anything touched here, not counted).
Every claim below was checked directly against real `nix-instantiate
--eval --strict` (Determinate Nix 3.22.5 / 2.35.2) before writing code,
per this project's standing convention.

### M67a: derivation equality by `outPath`

`eval-okay-eq-derivations.nix`/`eval-okay-eq-slot-derivations.nix`.
`asnix_bshared_value_eq`'s `ASNIX_TAG_ATTRS` case only ever did a full
structural walk (count + per-attr recursive `==`), which is wrong for
two reasons real Nix's own `eqValues` (`~/nix/src/libexpr/eval.cc`)
special-cases: (1) two derivations built from identical arguments are
two different heap attrsets (different `drvA1`/`drvA2` objects) that
should still be `==`, and (2) `derivation {...} // { dummy = 1; }`
changes the attribute count/shape without changing what the derivation
*means*, so a plain structural walk wrongly says unequal. Real Nix's
rule (confirmed reading `eqValues`'s `nAttrs` case directly): if both
sides' `isDerivation` (a `type` attr that forces to the string
"derivation") is true, compare their `outPath` attribute's *value*
(itself a recursive `eqValues`, i.e. an ordinary string compare once you
get there) instead of walking every attribute -- NOT `drvPath` (a
plausible-sounding guess the task handed down that turned out wrong
once actually read from source).

Added `value_is_derivation` (builtins_shared.c) and a new branch in
`asnix_bshared_value_eq`'s `ASNIX_TAG_ATTRS` case, checked right after
the existing `a.raw == b.raw` pointer fast path and before the
structural walk: if both sides are derivations, compare `outPath`
values and return, skipping the structural walk (and critically,
skipping forcing any OTHER attribute -- `eval-okay-eq-slot-
derivations.nix`'s `e = d // { f = x: x; t = throw "t"; }` relies on
`e == d` never forcing `t`). Two conformance-mined-for-free bonus fixes
fell out of this for free: `eval-fail-eq-derivation-type-throws.nix`/
`eval-fail-elem-derivation-type-throws.nix` (confirming `isDerivation`'s
own forcing of `type` lets a `throw`/`abort` inside it propagate
uncaught, exactly like real Nix) now pass too, since `value_is_derivation`
forces `type` unconditionally with no try/catch around it.

Verified against real Nix: `derivation {name="a";...} == derivation
{name="a";...}` (same args, different call) is `true`; the same with an
extra `// {dummy=1;}` on one side is still `true`; same name but
different `builder` is `false`. One new e2e fixture,
`m67_derivation_eq.nix`.

### M67b/c: dynamic (`${expr} = v;`) attrpath scope/merge gaps

Three related but distinct gaps in `${expr}`-keyed attrset bindings,
all found by actually reading `compiler.c`/`parser.c`'s own existing
comments and then checking whether what they claimed was still true --
two of the three claims turned out to be either wrong or narrower than
how they were being used.

**M67b -- a dynamic `rec {}` binding's value couldn't see its own static
siblings** (`eval-okay-dynamic-attrs.nix`/`-bare.nix`:
`rec { "${bString}" = a; a = true; }.b`, `bString = "b"`). `compiler.c`'s
`AST_ATTRSET` case compiled every `${expr} = v;` entry's name/value
against the *enclosing* `scope`, never the `rec`'s own frame, citing
ast.h's own comment that "real Nix also doesn't give these lexical/
recursive visibility even in `rec {}`" (confirmed by one real test:
`rec { ${k}=1; b=a; }` with `k="a"` IS still "undefined variable 'a'").
But that comment answers a different question than the one actually
needed here -- confirmed directly: `let bString="b"; in (rec {
"${bString}"=a; a=true; }).b` is `true` in real Nix. The two directions
are genuinely different: a *static* sibling can never reference a
*dynamically-named* attribute (no compile-time-known frame slot exists
for it, unaffected by this fix), but a *dynamic* binding's own name/
value expression CAN read (never write) the frame, same as any other
`rec {}` binding. Fixed by compiling a `rec {}`'s dynamic bindings
against `&frame` (the same `ScopeFrame` static bindings use) instead of
`scope`, and moving their `OP_INSERT_DYNAMIC_ATTR` emission to run
*before* `ib_frame_exit`, not after -- the attrs value sits on the
VALSTACK throughout either way, only the *scope* dynamic bindings
compile against changed. Non-`rec` `{}` is unaffected (never recursive,
confirmed unchanged: `{ ${k}=a; a=1; }`, no `rec`, is still "undefined
variable 'a'").

**M67c -- merging an attrpath into (or a sibling attrpath statement
alongside) a literal that itself had a dynamic key fataled unconditionally**
(`eval-okay-dynamic-attrs-2.nix`: `{ a."${"b"}"=true; a."${"c"}"=false;
}.a.b`; `eval-okay-merge-dynamic-attrs.nix`: `set1={a=1;};
set1={"${"b"+""}"=2;};` and `set3.a=1; set3."${"b"+""}"=2;` shapes).
`parser.c`'s static-attrpath-merge tree (`BTVec`/`BTNode`,
`reopen_attrset_into`) only ever carried static bindings; its own
comment called the dynamic-key case "not hit by anything this project
has evaluated so far" and fataled rather than silently dropping data --
turned out to actually be hit by this exact conformance batch, not just
a hypothetical. Gave `BTVec` its own `dyn: DynBindingVec` field;
`reopen_attrset_into` now folds a reopened literal's `dyn_bindings` into
`dest->dyn` instead of fataling; `finalize_bt_node` re-attaches a
branch's absorbed `dyn` bindings to its synthesized nested attrset; the
three top-level `{...}`/`rec {...}` call sites fold `bt.dyn` into the
block's own explicit `${...}=...;` `DynBindingVec` before finalizing.
`let` (which has no `dyn_bindings` field on `AST_LET` at all) gets a
loud guard instead -- fatal with the same "dynamic attributes not
allowed in let" message, same spirit as the pre-existing convention,
for a shape (`let a.${k}=1; a.b=2; in ...`) not believed reachable
today and not exercised by anything in this project's own fixtures or
numtide/nix-conformance.

Verified against real Nix: both files' every listed case matches
`.exp` exactly (`{ b=true; c=false; }`-shaped merges, `set3={a=1;b=2;}`-
shaped attrpath-into-dynamic-sibling merges). Two new e2e fixtures,
`m67_dynamic_attrs_rec_sibling.nix`/`m67_dynamic_attrs_merge.nix`.

### M67d: `${"literal"}` is a compile-time-constant attr name, not a dynamic one

`eval-okay-dynamic-attrs-3.nix`: `let ${"a"}=1; attrs=rec{b=c;
${"c"}=d; d=a;}; in {inherit ${"a"}; inherit attrs; inherit (attrs)
${"b"} ${"c"} d;}`, whose own comment says "dynamic attrs are not
generally allowed in `let`... but they are if they only contain a
string". `parse_attr_segment` already folded a *quoted-string* segment
(`"a"`/`"foo${x}"`) into a static name when it had no actual
interpolation inside, but the separate bare-`${expr}` branch never did
the equivalent check on ITS `expr` -- so `${"a"}` (a dollar-brace
wrapping a plain string-literal expression) stayed classified as
genuinely dynamic, which is exactly what `let` unconditionally rejects.
Real Nix does this same general fold, not just for `let`: confirmed in
`~/nix/src/libexpr/include/nix/expr/parser-state.hh`'s
`ToBeStringyExpr::visit`, "consistently handle simple StringExpr
${"string"} as non-dynamic attributes" (NixOS/nix#14642), used for
AttrPath components generally. Added the identical `AST_STRING`-with-
no-real-interpolation check to the bare-`${expr}` branch of
`parse_attr_segment`. Verified against real Nix: `let ${"a"}=1; in a`
is `1`; `let ${"a"+""}=1; in a` still fails "dynamic attributes not
allowed in let" (confirming the fold is specifically about a *literal*
string, not merely a `${...}` that happens to be computable). One new
e2e fixture, `m67_let_dollar_brace_string_literal.nix`.

### M67e: `or` as an ordinary (if deprecated/"cursed") identifier

`eval-okay-attrs5.nix` (`let or = x: y: x || y; in ...`) and
`eval-okay-deprecate-cursed-or.nix` (`(x: x) (x: x+1) or;`-shaped bare-
variable uses) both hit "parse error: expected a binding name"/failed
to parse `or` as an atom at all -- `or` is lexed as its own keyword
token (`TOK_OR`, for `.attr or default`), and nothing in `parser.c`
ever accepted that token anywhere an ordinary identifier is legal. Real
Nix's own grammar still accepts `or` as a plain identifier in any
position that can't be confused with the select-default form --
deprecated there with a warning (NixOS/nix#11121) but still accepted,
not rejected (confirmed: `let or=1; in or` is `1`, warning on stderr
only). Made `starts_atom`/`parse_atom` treat a bare `TOK_OR` exactly
like `TOK_IDENT` (a plain `AST_VAR`), and `parse_attr_segment`'s
unquoted-name fallback accept it too (binding/attribute/inherited
names) -- the existing `had_select && TOK_OR` check (the real
`.attr or default` grammar) runs first at its own call site regardless,
so there's no new ambiguity: `or` only becomes a plain identifier where
nothing already claims it as the select-default keyword.

This exactly fixed `eval-okay-deprecate-cursed-or.nix` (stdout "0",
which is all the lightweight `test-nix-conformance` sweep checks --
the warning text on stderr is not compared). `eval-okay-attrs5.nix`
parses further now but still fails on an unrelated, pre-existing,
already-documented gap (see "Skipped" below) -- not reverted, since the
`or`-as-identifier fix itself is correct and unblocks real syntax. One
new e2e fixture, `m67_or_identifier.nix`.

### M67f: string-interpolation of a nonexistent `Path` must fail

`eval-fail-nonexist-path.nix` (`"${./fnord}/xyzzy"`) and
`eval-fail-bad-string-interpolation-2.nix` (`"${./fnord}"`) both
*succeeded* in asnix, printing the plain path text, where real Nix
fails with `error: path '...' does not exist`. Root cause named by
`value_util.c`'s own pre-existing comment on `coerce_to_string_for_
interp`'s Path branch: real Nix's `"${path}"` triggers a "copy to
store" coercion (which obviously requires the source to exist), not
implemented here (no daemon `AddToStore`) -- but the *existence check*
that gates it is cheap and independently addable without the rest.
Confirmed the scope precisely against real Nix first: `builtins.toString
./nonexistent` succeeds (no existence check at all -- plain text
formatting); only *bare interpolation* of a Path requires existence.
Added an `lstat` check (dangling symlinks still count as "existing",
matching `pathExists`'s own convention) right there.

**Regression found and fixed in the same pass**: this coercion function
is shared by TWO call sites, not one -- `OP_STR_CONCAT` (genuine
`"${...}"` interpolation) AND `asnix_add_values`'s path-`+`-string case
(`/foo/bar + "/x"`), which reuses the exact same string-coercion rules
for an entirely unrelated reason (confirmed against real Nix: `+` on a
Path never requires existence, `toString (/foo/bar + "/x")` succeeds
even when `/foo/bar` doesn't exist). The existence check was initially
added unconditionally and broke `tests/e2e/m57g_path_canon_concat.nix`
(`/foo/bar + "/../xyzzy/./foo.txt"`, a deliberately-nonexistent path
used for canonicalization testing) -- caught by `make test-e2e`, not
missed. Fixed by threading a `check_path_exists` bool through
`coerce_to_string_for_interp`/a new shared `concat_strings_impl`, with
the public `asnix_concat_strings` (used by `OP_STR_CONCAT`) passing 1
and `asnix_add_values`'s own call passing 0. Also found and fixed a
second, pre-existing instance of the exact same confusion in
`tests/ir/test_strings_fixtures.c`'s own `fixture_concat_coerces_path`
unit test, which used a deliberately-nonexistent fixed path string
through the SAME (now existence-checking) interpolation path -- changed
to `"/"` (portable, always exists) since that test is about the
coercion logic, not existence.

Verified against real Nix: `"${./fnord}"` on a nonexistent `./fnord`
fails "path '...' does not exist"; `toString (path + string)` on a
nonexistent path still succeeds. One new e2e fixture (the happy path),
`m67_path_interp_exists.nix` -- note its own exact output isn't
byte-identical to what real Nix would do for an interpolated path that
*does* exist (real Nix still attempts an actual store-copy at that
point, which is its own separate, already-documented, unimplemented
gap -- this fix only adds the existence *gate* in front of the
existing, unchanged "just the text" approximation).

### M67g: `fromJSON` on a very deeply nested document segfaulted

`eval-okay-fromjson-deep.nix` (`deep 20000` and `deep 100000`, nested
`[[[...]]]]` built by string repetition) expects `[ 1 1 ]` on real Nix
(whose nlohmann-backed parser documents no nesting limit); asnix's own
hand-written recursive-descent parser (`builtins_json.c`) uses one
native C stack frame per nesting level, which already fit `deep 20000`
but segfaulted on `deep 100000` (confirmed: the default 8 MiB thread
stack is the limiting factor, not an asnix-specific bug -- running the
*same* binary under `ulimit -s 65536` made both cases pass with zero
code change).

**First attempt, reverted**: globally raising the process's own
`RLIMIT_STACK` once at startup (`setrlimit`) fixed this file but broke a
*different*, already-passing one: `eval-fail-toJSON-stack-overflow.nix`
deliberately relies on hitting a stack limit to prove a *controlled*
failure (real Nix has an explicit, interpreter-level `max-call-depth`
counter completely independent of native stack size; asnix has no such
counter anywhere, so its only "stack overflow" at all, here, is a
native one) -- deepening the *whole process's* stack let `builtins.
toJSON`'s own, unrelated recursive serializer (a different function
entirely) run deep enough to actually finish instead of crashing,
flipping that test from a (pre-existing, already-documented-as-
coincidental, per this Makefile target's own `eval-fail-*` comment)
"pass via native crash" to an outright fail. Found by `make
test-nix-conformance`'s own before/after diff, not missed.

**Fix that stuck**: scope the bigger stack to ONLY `fromJSON`'s own
recursion, via a `ucontext.h` side stack (`getcontext`/`makecontext`/
`swapcontext`) allocated fresh per call and freed after -- not a new OS
thread (no actual concurrency: the caller is suspended for the
`swapcontext` call's duration, same single-threaded execution this
whole codebase, e.g. the global arena allocator, already assumes). This
leaves `toJSON`'s own call path completely untouched, so it still
crashes at the same native-stack-limited depth as before, while
`fromJSON` gets a dedicated 64 MiB stack for its own parse.

Verified against real Nix (already known from the task's own write-up,
confirmed again here): `deep 20000`/`deep 100000` both `1`. `make
test-nix-conformance` before/after confirms both files land correctly
(`fromjson-deep` now passes, `toJSON-stack-overflow` still passes, no
net change to the latter). One new e2e fixture (a smaller `deep 50000`,
to keep the test suite fast), `m67_fromjson_deep.nix`.

### M67h: value printer shows `<PRIMOP>`/`<LAMBDA>` instead of `<unprintable...>`

`eval-okay-print.nix` (`trace [...] [null toString (deepSeq "x") (a:a)
(let x=[x]; in x)]`) exposed that `main.c`'s `print_value_inner` had no
case at all for `ASNIX_TAG_PRIMOP`/`ASNIX_TAG_CLOSURE` -- both fell to
the generic `default:` branch and printed `<unprintable value, tag=N>`.
Real Nix's printer shows `<PRIMOP>` for an unapplied builtin, `<LAMBDA>`
for a bytecode closure, and `<PRIMOP-APP>` for a *partially*-applied
multi-arg builtin. Added the first two unconditionally: `ASNIX_TAG_
PRIMOP` is always correct as `<PRIMOP>` (every multi-arg builtin
visible from Nix source is pre-curried into a chain of ordinary
closures, `asnix_bshared_make_curried_native`, precisely BECAUSE Nix's
call syntax is always unary -- a bare arity-N `ASNIX_TAG_PRIMOP` value
only ever exists fully unapplied, so "0 args given" is the only
reachable state). `ASNIX_TAG_CLOSURE` -> `<LAMBDA>` is NOT always
correct for the same reason: a partially-applied native builtin (e.g.
`deepSeq "x"`) is ALSO one of those synthetic curry-stage closures,
bit-for-bit indistinguishable from a genuine user lambda at this tag (no
spare field anywhere in the closure heap layout currently marks "I am a
curry stage, not user code" -- its `formals`/`up` fields can legitimately
take the same values either way) -- reliably telling them apart would
need a dedicated marker threaded through the closure heap layout
(`heap.h`) and every `.S` site that builds one, out of proportion for
this pass. So `<LAMBDA>` is a known, documented approximation, same
spirit as this project's other scoped gaps.

This file ALSO exposed a second, independent gap in the *list* printer:
real Nix's `[ (let x=[x]; in x) ]` case prints `[ [ «repeated» ] ]`
(TWO levels before the cycle marker), not the one level asnix's
existing cycle-detector gives (`[ «repeated» ]`) -- traced to the exact
same underlying real-Nix quirk that also blocks the equality cluster
(see "Skipped" below): real Nix's own cycle detection keys off the
literal memory address of the *Value struct currently being printed*,
and a *non-trivial* list-element expression (this one, a `let`) gets a
FRESH such address via `maybeThunk`'s bit-copy-on-force semantics,
bit-for-bit indistinguishable in CONTENT from the original but a
different ADDRESS -- so the outer, "borrowed" copy inserts into `seen`
once, then recurses into the SAME underlying list object's own
(original) address, which inserts a SECOND time before finally hitting
the already-seen entry. asnix's Value representation has no equivalent
indirection (a Value already IS the heap pointer, there's no separate
"box" to re-copy) -- not fixable without the same disproportionate
architecture change the equality cluster needs, see "Skipped" below.

Net: `eval-okay-print.nix` is closer (no longer `<unprintable...>`) but
still not byte-exact, for the two reasons above -- stays in the failed
list, documented rather than silently left as a worse-looking crash.
One new e2e fixture testing what IS now correct outside a trace (direct
top-level printing, not just inside `builtins.trace`), `m67_print_
primop_lambda.nix`.

### M67i: `TEST_VAR=foo` -- a `test-nix-conformance` harness gap, not an asnix bug

`eval-okay-getenv.nix` (`getEnv "TEST_VAR" + (if getEnv "NO_SUCH_VAR" ==
"" then "bar" else "bla")`, expecting `"foobar"`) "failed" only because
this project's own lightweight `test-nix-conformance` Makefile target
never set `TEST_VAR` before running -- confirmed this is a test-harness
gap, not an asnix bug, by running real `nix-instantiate --eval` on the
exact same file with `TEST_VAR` unset too: it ALSO doesn't give
"foobar" then (gives "bar"). The real numtide/nix-conformance `run`
script exports `TEST_VAR=foo` (plus `NIX_PATH`/`HOME`, not replicated
here -- nothing else in the current failure list needs them) before
running any case (`run:173`/`README.md:84`). Added `export TEST_VAR=foo`
to the Makefile target. `builtins.getEnv` itself was already correct
(confirmed: `TEST_VAR=foo asnix eval-okay-getenv.nix` already gave
`"foobar"` before this fix -- only the sweep script's own environment
was missing the variable).

### Skipped (investigated, no fix)

- **`eval-okay-eq-slot-elem.nix`/`eval-okay-eq-slot-identity.nix`/
  `eval-okay-equal-function-direct-identical.nix`** -- all three need
  real Nix's exact "Value-struct-address identity" hack
  (`eqValues`'s `if (&v1 == &v2) return true` fast path, PLUS `nFunction`
  unconditionally `return false` otherwise): a BARE VARIABLE reference
  used as a list element/attrset value is pointer-SHARED with its
  original binding's storage (`maybeThunk`'s trivial-expression case,
  no copy), while almost every OTHER expression kind gets a FRESH
  Value address via force-and-bit-copy even when the CONTENT is
  identical -- so `f == f` (direct comparison) is `false` (two fresh,
  non-address-equal copies of the same closure content, `nFunction`
  dispatch always loses) but `[f] == [f]` (list-wrapped) can be `true`
  (both list literals' elements point at the exact same original
  storage, pointer check wins before `nFunction` dispatch ever runs).
  Traced through by hand against each test's every sub-case (documented
  in-session, not re-derived here) and confirmed this is genuinely,
  precisely what's being tested. asnix's Value representation (a
  tagged word that for any heap type already IS the pointer -- no
  separate "box"/"slot" indirection a `let`/list-literal/thunk-force
  could re-copy into a fresh address while leaving the content
  unchanged) has no notion to hang this on at all; replicating it would
  mean adding exactly that extra indirection layer for every value in
  the system, pervasively touching the compiler's codegen for variable
  references, list literals, attrset literals, and thunk-forcing alike
  -- the same class of "infrastructure genuinely out of proportion"
  this task's own instructions anticipate skipping. (Current behavior:
  closures compare by CONTENT pointer identity unconditionally, which
  is MORE permissive than real Nix in some of these cases and less in
  others -- not a worse approximation than before this pass, just an
  unchanged one.)
- **`eval-okay-compare-operand-order.nix`** -- its last element,
  `let __curPos = 42; in __curPos`, needs `__curPos` to be a true
  parser-level magic identifier (rewritten to a position-literal AST
  node at every occurrence, ignoring any user `let __curPos = ...;`
  shadow) -- not implemented in asnix AT ALL (distinct from M59's
  `unsafeGetAttrPos`, confirmed: `grep -rn curPos src/c` before this
  pass found nothing). Per this task's own instructions, `__curPos`/
  `unsafeGetAttrPos` is M59's area (another agent's); not deepened
  here. Every OTHER element of this file already evaluates correctly
  (verified by hand against the real-Nix-expected array with the
  `__curPos` element dropped).
- **`eval-okay-tryeval-failed-thunk-reeval.nix`** -- exactly, word for
  word, the limitation M30's own scope notes ALREADY document under
  "Known, accepted limitation, not engineered around": a thunk that
  throws partway through `tryEval`'s `longjmp`-based catch is left
  permanently marked `IN_PROGRESS` (blackholed), since the unwind skips
  every normal "mark forced" step with no record of which thunk(s) were
  mid-force at the time; re-forcing that SAME thunk a second time
  (exactly what this file does, "Since Nix 2.34, errors are memoized")
  incorrectly reports infinite recursion instead of re-raising. M30's
  own text already calls a full fix "a much larger undertaking", citing
  the need to track and roll back every thunk touched since the
  matching `setjmp` -- confirmed this file is exercising precisely that
  already-known, already-documented gap, not a new bug; not re-attempted
  here, consistent with this task's "tryEval work disproportionate"
  warning.
- **`eval-okay-attrs5.nix`** (remaining failure after M67e's `or`-as-
  identifier fix) -- `as.x.y.bla or bs.bar.foo or "xyzzy"` needs a
  missing-attribute failure at ANY point along a MULTI-segment attrpath
  (`bs.bar.foo`, where `bs.bar` itself is missing) to still be caught by
  that chain's OWN trailing `or default`. `parser.c`'s `parse_select`
  already documents this exact scope cut in its own comment ("only the
  last segment's failure is caught, not an earlier one in a longer
  chain") -- each `.`-segment becomes its own single-segment
  `AST_SELECT` node, with `default_` attached only to the OUTERMOST
  (last) one; a failure selecting an INNER segment happens while
  evaluating that outer node's unprotected `target`, escaping the
  default entirely. Fixing this properly means restructuring
  `AST_SELECT` to hold a whole attrpath array with ONE default at the
  end (matching real Nix's own `expr_simple '.' attrpath OR_KW
  expr_select` grammar) instead of a chain of single-segment nodes --
  a real AST/compiler restructuring, not a narrow fix, and an
  orthogonal, pre-existing, already-documented gap this pass's `or`
  fix merely got far enough to actually reach (confirmed: before M67e,
  this file failed at the PARSE stage, before this semantic gap was
  even reachable).

## M68 scope notes: a hand-written POSIX-ERE backtracking engine to actually fix M55's 33 regex gaps

M55 triaged 39 `nix-conformance` regex failures into three classes and
shipped **no fix**, concluding the first two (27 + 6 = 33 tests) were
genuine glibc-`<regex.h>`-vs-libstdc++-`std::regex` engine differences
with "no fix available short of a different regex engine". This pass
built that engine: `src/c/regex_engine.c`/`.h`, a small hand-written
POSIX ERE parser + continuation-passing backtracking matcher, used
alongside (not instead of) the existing glibc-based
`builtins_regex.c`. Net result: all 33 of M55's Class A/Class B
failures now pass; M55's Class C (6 tests, `(a){24998}`/collating-
symbol-as-range-endpoint patterns real Nix rejects for libstdc++-
internal-complexity reasons with no discoverable rule) remains
unfixed, exactly as M55 already concluded was correct to leave alone.

**Root cause, restated precisely.** M55 already correctly identified
*that* glibc and libstdc++ disagree; this pass needed to pin down
*exactly how*, since "a different engine" only works if it reproduces
libstdc++'s specific algorithm, not some other (equally "valid")
backtracking convention. Verified directly against real
`nix-instantiate` (2.35.2) dozens of small hand-picked cases before
writing a line of matcher code (see `regex_engine.c`'s own header
comment for the final, citable summary) to separate three
independent behaviors that all look superficially similar but turned
out to need different handling:

1. **Alternation is ordered, not "pick the longest branch"**:
   `(aa|a)(aa|a)` on `"aaa"` gives `[ "aa" "a" ]` (first-listed "aa"
   wins simply because it's tried first and happens to work), while
   `(a|aa)(a|aa)` on the same string gives `[ "a" "aa" ]` (first-listed
   "a" wins, also just because it's tried first) -- neither result is
   about which branch is longer, both are "try branches in the order
   they're written, first one whose entire remaining match eventually
   succeeds wins".
2. **Repetition has one specific, well-known backtracking-engine quirk**:
   a `*`/`+`/`{m,}` loop, once its real (non-empty) iterations are
   exhausted, still greedily attempts exactly one more iteration even
   if the loop body can only match empty there, and *that* iteration's
   capture (even though zero-width) overwrites whatever the last real
   iteration had recorded, before the loop finally gives up (confirmed:
   `(a*)*` on `"aa"` → `[ "" ]`, not `[ "aa" ]`; the subtler
   `((a)*)*` on `"aa"` → `[ "" "a" ]`, confirming a repetition that
   runs its body *zero* times leaves an enclosing group's capture
   completely untouched, as opposed to resetting it -- a different and
   narrower thing than "overwritten by an empty iteration").
3. **Unanchored search (`builtins.split`) doesn't always find the
   same match *span* as glibc, not just different captures for the
   same span** -- this is the one M55 didn't catch, because M55 only
   tested `builtins.match` (implicitly anchored) by hand: `builtins.split
   "(x*)(xy)?" "xy"` gives real Nix 3 matches (`"x"`, then two
   zero-width ones) because the greedy `x*` commits to consuming "x"
   and libstdc++ never backtracks that choice just because the
   optional `(xy)?` after it could have matched more -- but glibc's
   `regexec`, being actually-POSIX-compliant here, *does* find the
   longer 2-character `"xy"` span instead. Confirmed by `eval-okay-regex-184`
   through `-193`'s `sp` fields: `m` (match) was already correct
   without any of this milestone's split-specific fix (match's
   whole-string anchoring already forces the right answer, by luck of
   how anchoring interacts with greedy backtracking), but `sp` needed
   the custom engine to find the match *boundary* itself, not just
   recompute captures for a boundary glibc had already decided on.

**Implementation.** `regex_engine.c` parses the ERE subset
nix-conformance's regex corpus and nixpkgs actually use (literals,
`.`, anchors, `*`/`+`/`?`/`{m,n}` with stacking, `()` capturing groups,
`|` alternation, `[...]` bracket expressions including POSIX class
names, named collating symbols, and equivalence classes -- no
lookaround/backreferences/lazy quantifiers, none of which ERE has
anyway) into a small AST, then matches via continuation-passing-style
backtracking (`m`/`run_cont`/`match_rep` -- see the file's own header
comment for the full design writeup). Two matching modes share nearly
all the same code: *anchored* (`asnix_regex_match_anchored`, fixed
target length, ordinary accept-first-success backtracking -- this
alone reproduces both quirks 1 and 2 above, since fixing the required
end position is exactly what forces backtracking into an earlier
branch/rep-count choice when needed) and *search* (`asnix_regex_search`,
no fixed length -- same accept-first-success everywhere **except**
`RN_ALT` nodes with more than one branch, which explicitly try every
branch to its own completion and keep whichever reaches furthest, the
mechanism that reproduces quirk 3 without over-applying "prefer
longest" to repetition's own rep-count choice, which quirk 2's own
test cases prove must NOT happen).

`builtins_regex.c`'s `match2_impl`/`split2_impl` route by how much a
given pattern actually needs this: a pattern using none of the three
bracket-expression extensions (M55's Class B) and having zero capture
groups never touches `regex_engine.c` at all -- glibc's regexec is
already both correct and faster for that case, unchanged. A pattern
using a Class B extension bypasses glibc entirely (it often can't even
`regcomp` these). Everything else with at least one capture group gets
captures recomputed by the custom engine for the span glibc's regexec
already found (`groups_to_list_recomputed`); `split2_impl` additionally
re-derives the match *boundary* itself via the custom engine (not just
recomputing captures for glibc's boundary) whenever there's a capture
group and the subject is short enough to be safe (see "performance"
below) -- this is the fix for quirk 3.

**Two real pitfalls found and fixed during verification, not just
theorizing**:

- **Native-recursion stack depth, not time complexity.** This CPS
  matcher's `*`/`+` handling recurses once per repetition (not an
  iterative loop), so a repetition matched over N real characters
  costs O(N) C call-stack depth. A dedicated probe harness (not part
  of this project's test suite) found this segfaults somewhere in the
  low thousands of matched characters even for a trivial pattern like
  `(a|b)*`, confirmed live by nix-conformance's own
  `eval-okay-regex-264` (`(a|b)*` against a >10000-character string)
  crashing the instant the custom engine's capture-recompute was wired
  in unconditionally. Converting repetition to use an explicit
  heap-allocated backtracking stack instead of C recursion would be
  the real fix; out of scope for this pass given the actual target
  patterns are all tiny. Mitigated instead with
  `ASNIX_REGEX_CUSTOM_MAX_SPAN` (2000 characters, comfortably below the
  measured crash threshold with margin for this process having less
  free stack than the standalone probe did, and comfortably above
  every pattern this milestone actually targets): above that span,
  `groups_to_list_recomputed`/`split2_impl` fall back to glibc's own
  (unimproved, but also unregressed) results.
- **Exponential blowup from exploring every alternative of a*
  *repeated* ambiguous alternation.** Quirk 3's fix (explore every
  `RN_ALT` branch to completion in search mode) is, by itself,
  genuinely exponential when the repeated body *is* an ambiguous
  alternation -- confirmed by three more previously-passing
  nix-conformance long-input tests (`(a|aa)*b`, `((a|b)(c|d))+`,
  `(ab|a)*b`, all well under the 2000-character span cap, none of them
  anywhere near stack-depth territory) hanging past the harness's
  10-second timeout the instant the search-boundary fix landed. Fixed
  with a shared, decrementing step budget (`MCtx.budget`,
  `ASNIX_REGEX_SEARCH_BUDGET` = 300000 node visits) that makes `RN_ALT`
  degrade from "explore every branch" back to plain
  accept-first-success once exhausted -- bounded-but-possibly-
  degraded beats exponential, and for every pattern this engine is
  actually meant to handle (nix-conformance's regex-NNN corpus is all
  single/low-double-digit-character inputs), the budget is never
  remotely close to exhausted, so this never actually changes behavior
  for anything in scope.

One more gap found and fixed along the way, inside M55's own already-
"mostly fixed" Class C detection: `[[=a=]-c]` (an equivalence class
used as a bracket-range *endpoint*) newly round-tripped as
successfully-compiling-and-matching once the custom engine started
handling Class B patterns, where it previously hit glibc's own
rejection first. Confirmed real Nix also rejects this (same family as
M55's already-documented collating-symbol-as-range-endpoint
rejections, just for equivalence classes instead) and added the
matching rejection to `regex_engine.c`'s own bracket parser
(`eval-fail-regex-invalid-041`).

**Verified** against real `nix-instantiate --eval --strict` (2.35.2)
for every behavioral claim above before trusting it (dozens of small
hand-picked `match`/`split` probes, not just the corpus's own `.exp`
files -- same discipline M55's own investigation used). New
`tests/e2e/m68_regex_*.nix` fixtures (6 files) cover the specific
shapes fixed: nested/optional capture under repetition, the
zero-reps-leaves-group-untouched case, split's greedy-quantifier-vs-
optional boundary divergence, and all three bracket-expression
extensions. `make test` (including a new `tests/c/test_regex_engine_scratch.c`
unit battery exercising ~38 hand-verified match/capture shapes
directly against `regex_engine.c`, bypassing the Nix layer entirely)
and `make test-e2e` both green.

`make test-nix-conformance` before/after (this shared worktree sees
concurrent unrelated work from other agents landing throughout, same
caveat M55's own numbers carried, and the host was also under heavy
concurrent load while these were taken -- load average in the 20s-40s
-- which sporadically times out unrelated, individually-fast
(millisecond-scale, confirmed by timing several directly) tests under
the harness's 10-second-per-file budget; the numbers below are from a
clean, re-confirmed run, not a lucky one): **556 passed/102 failed**
before this milestone (39 of those 102 were regex: the 33 this
milestone targets plus M55's 6 already-known-unfixable Class C ones)
→ **589 passed/69 failed** after (6 of those 69 are regex: exactly
M55's Class C, untouched). All 33 targeted regex tests now pass; the
non-regex failure lists before and after are identical -- zero
regressions anywhere else in the corpus.
## M69 scope notes: `derivation`/`derivationStrict`'s `__structuredAttrs = true;` mode

**The blocker.** This milestone picked back up the `d.nix` standing bug-
chase right after M62/M63/M64/M65's work had each cleared an earlier
blocker in turn: `./asnix d.nix` (`(builtins.import /home/tbereknyei/
nixpkgs/default.nix {}).hello.outPath`) aborted with "asnix: cannot
coerce this value to a string". Root-caused directly with gdb,
breaking at `value_util.c:200` (the fatal path inside
`asnix_coerce_to_string`) and inspecting the calling frame -- the
caller was `derivation.c`'s `asnix_prim_derivation`, mid-way through
its per-attribute loop, trying to coerce an attrset value to a string.
Walked up one more frame to the actual input attrset and confirmed
directly (forcing the offending attribute's own `__structuredAttrs`
sibling, tag IMM/subtag TRUE) that some package deep in nixpkgs' real
dependency graph sets `__structuredAttrs = true;` on its `derivation
{...}` call.

**Minimal verified repro** (matches real Nix, which succeeds and
prints a `{ ...; outPath = "..."; ... }` attrset; asnix previously
aborted):
```
derivation {
  name = "x";
  system = "x86_64-linux";
  builder = "/bin/sh";
  __structuredAttrs = true;
  env = { foo = "bar"; };
}
```

**Real Nix's actual behavior, read directly from source before
implementing anything** (`~/nix/src/libexpr/primops.cc`'s
`derivationStrictInternal`, cross-checked against a real `.drv` file
produced by `nix-instantiate` on the repro above, not just the source
read): `__structuredAttrs`'s own value is looked up with a dedicated
`attrs->get(...)` call *before* the per-attribute loop even starts
(the attribute can appear anywhere in the input attrset, not
necessarily first) -- if true, the loop's generic "stringify every
attribute into its own env var" branch is replaced with "accumulate
every attribute's own `printValueAsJSON`'d value into one in-memory
JSON object", for every key except `args` (handled identically either
way, never part of the JSON) and `__structuredAttrs` itself (skipped
entirely, but -- confirmed by producing a real `.drv` for
`__structuredAttrs = false;` and reading it -- ONLY when the mode is
actually active; an explicit `false` falls through to the ordinary
per-attribute path like any other key, becoming a literal
`__structuredAttrs=""` env var, since `coerceToString false == ""`).
`name`/`system`/`builder`/`outputHash`/`outputHashAlgo`/
`outputHashMode` are *also* still individually recovered as plain C
strings inside this same branch (real Nix's own nested `switch` inside
its `jsonObject` case still calls `forceString`/`forceStringNoCtx` on
exactly these keys) -- they just don't ALSO become their own env var
the way they do outside structured-attrs mode. The masked/real two-
pass output-path hashing scheme (M8/M62) is completely untouched: every
declared output still gets its own blanked-then-filled env var exactly
as before, confirmed by reading the real `.drv` file byte-for-byte
(`nix-instantiate` on the repro above, with `nix-instantiate
--eval --strict` to get `drvPath`, then `cat`-ing that store path
directly):
```
Derive([("out","/nix/store/85k3fnwmzp95ccbwmn2ixisp0y8gdqjj-x","","")],
[],[],"x86_64-linux","/bin/sh",[],
[("__json","{\"builder\":\"/bin/sh\",\"env\":{\"foo\":\"bar\"},\"name\":\"x\",\"system\":\"x86_64-linux\"}"),
("out","/nix/store/85k3fnwmzp95ccbwmn2ixisp0y8gdqjj-x")])
```
-- confirming directly that structured-attrs mode's `env` section ends
up with EXACTLY the per-output vars plus one `__json` entry, nothing
else, and that `__json`'s own content is just an ordinary alphabetical-
key-order JSON object (`~/nix/src/libstore/derivation/aterm.cc`'s
`unparse`: `drv.structuredAttrs->unparse()` returns a single
`("__json", <nlohmann dump>)` pair merged into a copy of `drv.env`
right before ATerm serialization -- the exact same serialization used
for both the masked-hash pass and the final `.drv`, since nothing in
this project's scope makes the JSON content itself depend on output
paths). `aterm.c`'s `asnix_build_derivation_aterm` needed **zero**
changes: `__json` is just one more ordinary `(key,value)` env pair to
it, and its existing escaping (`\n \r \t \" \\`) already handles a
JSON string's embedded quotes/backslashes correctly.

**The fix.** `derivation.c`'s `asnix_prim_derivation` gained a
dedicated pre-scan for `__structuredAttrs` (mirroring real Nix's own
`attrs->get` lookup before the main loop) setting a `structured_attrs`
flag, then the main per-attribute loop's generic tail was restructured
so that: (1) every key reaching that point (except `args`, excluded
earlier via its own `continue`) is unconditionally collected as a raw
`(sym, forced-Value)` pair into a new `json_syms`/`json_vals` scratch
array when `structured_attrs` is set; (2) the six scalar fields
(`name`/`system`/`builder`/`outputHash*`) are *always* coerced to C
strings regardless of mode (needed internally either way); (3) the
*push into the ordinary `env` array* only happens when NOT in
structured-attrs mode. After the loop, if structured-attrs mode is
active, the collected pairs are folded into one attrset
(`asnix_bshared_make_attrs`) and handed to a *new* exported function,
`asnix_toJSON_cstr` (`builtins_compare.c`/`.h`) -- the exact same
recursive `json_encode` that backs `builtins.toJSON` itself, now
factored out into its own entry point rather than duplicated, per the
task's own hint that `builtins_json.c` (which turned out to only
implement `fromJSON`; `toJSON` actually lives in `builtins_compare.c`)
shouldn't need a second serializer written from scratch. Its result
(arena-copied, the malloc'd scratch freed immediately -- matching
every other env value in this function) becomes the single `__json`
env var, added to the `env` array before the existing masked/real
two-pass hashing runs, completely unchanged otherwise.

**Verification methodology.** Because the masked/real two-pass hashing
machinery (M8/M62) is a byte-for-byte port of real Nix's own ATerm
format, and `json_encode` was already verified byte-for-byte against
`nlohmann::json::dump()` (M19/M51/M57d), a correct structured-attrs
implementation should reproduce real Nix's EXACT store-path hashes,
not just the right shape -- used this as the primary verification
signal rather than only checking "does it crash": the minimal repro
above produces `drvPath = "/nix/store/3g9b3db4qr9ybd2mjhwxmf3d51xn4rbl-
x.drv"` and `outPath = "/nix/store/85k3fnwmzp95ccbwmn2ixisp0y8gdqjj-x"`
on asnix, matching real `nix-instantiate --eval --strict` on the same
file exactly, byte-for-byte, including the hash. Also caught one real
bug this way during development: an initial version used `strlen
("__structuredAttrs")` miscounted as 18 (it's 17) in both the pre-scan
`asnix_intern` call and the loop's own `continue` check, silently
leaving `structured_attrs` false in every case -- caught immediately
because the minimal repro kept hitting the exact same "cannot coerce"
crash it was supposed to fix, not some subtler wrong-output bug.

**Scope cuts, explicitly out of scope:**
- `__ignoreNulls`, `__contentAddressed`, `__impure` (real Nix features
  that interact with structured-attrs mode's own per-attribute switch)
  remain unimplemented -- pre-existing gaps, not touched or widened by
  this milestone.
- Byte-for-byte matching real Nix's exact `nlohmann::json::dump()`
  whitespace/ordering was NOT separately re-verified here beyond what
  M19/M51/M57d already established for `builtins.toJSON` itself (this
  milestone just reuses that same code path) -- the exact store-path
  hash matches found above are themselves strong evidence it's still
  correct.
- No support for a `__json`-named user attribute colliding with the
  synthesized one (real Nix has a dedicated `StructuredAttrs::
  checkKeyNotInUse` rejection for this; asnix simply lets the
  synthesized `__json` env entry stand -- not reproduced, no fixture
  exercises it).
- Does not implement any build-time behavior for structured-attrs mode
  (writing an actual `.attrs.json`/`.attrs.sh` file, `NIX_ATTRS_JSON_
  FILE`/`NIX_ATTRS_SH_FILE` env vars) -- out of scope by the same
  pre-existing "this project doesn't build anything" convention
  `derivation.c`'s own header comment already documents; the `__json`
  env var's *content* (and therefore the resulting store-path hashes)
  is still computed correctly, which is everything this project's own
  scope needs.

**`./asnix d.nix`**: confirmed the exact "cannot coerce this value to a
string" blocker is gone (baseline reproduced first via the minimal
repro above, independent of the actual nixpkgs checkout); `d.nix`
itself now progresses to a new, later blocker ("attribute 'stdenv'
missing, aborting") -- the next iteration of this standing chase, not
pursued further here per this task's explicit scope.

`make test`/`make test-e2e` both green, zero regressions, plus two new
`tests/e2e/m69_structured_attrs_*.nix` fixtures (one matching the
minimal repro above, one with a deeper `env` nesting -- an attrset
containing an int, a list with a bool, and a doubly-nested attrset with
an empty list), both checking exact `drvPath`/`outPath` store-path
strings verified against real `nix-instantiate --eval --strict` first.
`make test-nix-conformance`: pass=574/fail=84 -> pass=576/fail=82
(skip=148 unchanged) -- a clean +2/-0, verified by diffing the full
failed-file list before/after rather than trusting the aggregate counts
alone (the sweep has a small amount of pre-existing, unrelated flake --
a different single file, never the same one twice, intermittently
failed/passed across repeated runs both before and after this change;
re-running isolated the genuine, stable delta). The two newly-passing
files: `eval-okay-drv-structured-attrs.nix` (exact store-path match
across four structured-attrs shapes: empty, several JSON-typeable
values including a nested attrset, multi-output, and a string needing
JSON escaping) and `eval-okay-derivation-legacy.nix` (`__structuredAttrs`
alongside the now-structured-attrs-only `allowedReferences`/
`disallowedReferences`/`allowedRequisites`/`disallowedRequisites`/
`maxSize`/`maxClosureSize` attributes, via `builtins.derivationStrict`
directly rather than the `derivation` wrapper). A third related file,
`eval-fail-derivation-structuredAttrs-stack-overflow.nix` (a 100,000-
deep nested linked-list attrset under structured attrs, expecting "a
controlled stack overflow error rather than a segfault"), was already
counted as passing before this change (via the unrelated "cannot
coerce" crash) and still counts as passing after (verified: it now
genuinely evaluates/serializes most of the way through before a real
native stack overflow in the recursive JSON encoder/printer, exit code
139) -- this project's conformance harness only checks "`eval-fail-*`
exits nonzero and didn't time out", not *which* error occurred, so
this isn't a behavioral claim that asnix now has real recursion-depth
guards anywhere in this path (it doesn't; same pre-existing gap as
every other unbounded-recursion C function in this codebase).

## M70 scope notes: multi-segment `a.b.c or default` -- every segment of a chain now shares ONE `or` guard

The live blocker on `./asnix d.nix`
((`builtins.import /home/tbereknyei/nixpkgs/default.nix {}).hello.outPath`):
`asnix: attribute 'stdenv' missing, aborting`. Root-caused directly with
gdb: broke at `asnix_bshared_select_missing_fatal`, dumped the target
attrset's keys -- it's `pkgs/top-level/config.nix`'s settings attrset,
not a package set -- then grepped real nixpkgs for the exact selection:
`pkgs/stdenv/generic/make-derivation.nix:191`, `userHook = config.stdenv.userHook
or null;`. Minimal repro, verified directly against real Nix:
```
let a = { b = { c = 1; }; }; in a.x.y or "default"
```
Real Nix: `"default"`. Pre-fix asnix: `asnix: attribute 'x' missing,
aborting` -- crashed on the FIRST missing segment instead of falling
through to the default.

This is exactly the gap M67e's own notes already flagged as the
remaining `eval-okay-attrs5.nix` failure (see that section, and
`parser.c`'s `parse_select` comment at the time): real Nix's grammar
(`parser.y`: `expr_simple '.' attrpath OR_KW expr_select`) attaches
`or default` to the WHOLE preceding dotted chain, and `ExprSelect::eval`
(`~/nix/src/libexpr/eval.cc`) walks EVERY segment of that one
multi-segment attrpath in a single loop, falling through to `default`
the instant ANY segment is missing or the current value isn't an
attrset -- not just the last one. asnix's parser instead built a chain
of nested single-segment `AST_SELECT` nodes (one per `.segment`,
`target` pointing at the previous node), attaching `default_` only to
the OUTERMOST (last) node after the loop. So `a.x.y or "default"`
compiled as: a hard, unguarded select of `x` on `a`, feeding a guarded
select of `y` on that -- the first hard select had nothing to fall back
to, hence the crash.

**Fix.** Two coordinated halves, since fixing only one in isolation is
actively dangerous (see the double-select risk noted below):

- **Parser (`parse_select`, `src/c/parser.c`)**: when the trailing `or`
  is seen after the segment-collecting loop, instead of stamping
  `default_` only on the outermost node, walk back down the just-built
  chain via `target` while it's itself a freshly-built `AST_SELECT`
  with `default_ == NULL` (true for every node of *this* chain, and
  only this chain, by construction -- an earlier chain's `or` would
  already have set its own nodes' `default_`) and stamp the SAME
  `default_` `Ast*` onto every node. Sharing one `Ast*` subtree across
  multiple parent nodes is safe here: `compiler.c` only ever READS
  `default_` to emit a fresh, independent copy of its bytecode at each
  compile site, never mutates the AST.
- **Compiler (`AST_SELECT` case, `src/c/compiler.c`)**: a non-NULL
  `default_` no longer means "lone single-segment select" -- it now
  collects the WHOLE chain sharing that one `default_` pointer (walking
  `target` the same way, stopping at the first node whose target isn't
  an `AST_SELECT` with the identical pointer, which is the chain's true
  base expression), compiles the base once, then emits one
  `OP_SELECT_OR`/`OP_SELECT_DYNAMIC_OR` per segment in source order
  (innermost first -- the chain is built innermost-first as nested AST
  but must execute `a`, then `.x`, then `.y`, so the collected,
  outermost-first array is walked in reverse), with every segment's
  miss-jump delta patched to the SAME shared landing pad. One
  `OP_JMP` right after the last segment skips the landing pad on a full
  hit; the landing pad itself compiles `default_` exactly once and
  falls through to the shared end, which the hit-path jump targets --
  same two-site patch-after-the-fact shape `AST_IF` already uses for
  then/else, generalized from one try-site to N. No new IR ops needed:
  this is entirely a reuse of the existing `OP_SELECT_OR`/
  `OP_SELECT_DYNAMIC_OR` "jump offset to the default code" mechanism
  against one shared target instead of one-per-node.

  The reason both halves had to move together: stamping `default_` on
  every chain node in the parser while leaving the compiler's old
  per-node-independent-wrap logic in place would have been WORSE than
  the original bug, not better -- a node whose `target` is itself
  select-or-guarded would, under the old compiler logic, first let its
  target's own wrap catch a miss and substitute the *whole default
  value* in place of the target, then go on to apply its OWN select to
  THAT value (`.y` selected from `default`, not `default` itself used
  directly) -- silently wrong whenever `default` happens to have a
  same-named attribute, and a second, independent evaluation of
  `default_`'s bytecode (visible as a doubled side effect) whenever it
  doesn't. Confirmed this exact failure mode never occurs with the
  combined fix.

Verified against real `nix-instantiate --eval --strict` beyond the one
minimal repro: a 3-segment chain missing at each possible position
(`a.x.y.z`/`a.q.y.z`/`a.x.q.z`/`a.x.y.q or "d"`), a dynamic segment
mixed with a static one (`a.${k}.y or d`, both hit and miss), nested
`or` chains (`a.b or c.d or e`, both the first- and both-miss cases),
and a `builtins.trace`-wrapped default (`a.x.y or (builtins.trace
"eval-default-once" "d")`) confirmed to fire the trace exactly once,
matching real Nix exactly in every case.

**`./asnix d.nix`**: confirmed the exact "attribute 'stdenv' missing"
blocker is gone; `d.nix` now progresses to a new, later blocker
(`asnix: attribute 'storeDir' missing, aborting`) -- the next iteration
of this standing chase, not pursued further here per this task's
explicit scope.

`make test`/`make test-e2e` both green, zero regressions, plus four new
`tests/e2e/m70_or_chain_*.nix` fixtures (all verified against real
`nix-instantiate --eval --strict` first): the 2-segment minimal repro,
a 3+-segment chain with a miss at every position, a mixed static/
dynamic-segment chain, and a case (`builtins.attrNames (a.x.y or d)`
with `d = { y = 5; w = 6; }`) that would return the WRONG value
(`d.y` = `5` instead of `attrNames d` = `["w" "y"]`) under exactly the
double-select regression described above, making that failure mode
automatically checkable via stdout diffing rather than only via manual
trace-count inspection.

`make test-nix-conformance`: `pass=623 fail=35 skip=148` ->
`pass=624 fail=34 skip=148` (net **+1**, confirmed via a full
before/after diff of the failed-file list, not just the aggregate
counts, that the one flip is exactly `eval-okay-attrs5.nix` -- the file
M67e's own notes predicted this fix would resolve -- and nothing else
regressed or newly failed).

## M71 scope notes: `builtins.storeDir`

Found continuing to chase `d.nix` right after M70: the next blocker was
"attribute 'storeDir' missing, aborting" -- the same gdb technique as
every prior milestone (break on `asnix_bshared_select_missing_fatal`,
read the symbol id) named the missing attribute directly, no deeper
investigation needed this time. `builtins.storeDir` is a real Nix
builtin -- a plain constant (`"/nix/store"` on this dev machine,
confirmed via `nix-instantiate --eval -E 'builtins.storeDir'`), not a
function -- that wasn't implemented at all. Grepping nixpkgs confirmed
several `pkgs/by-name` packages reference it directly. `builtins`-only,
same M27 bare-global rule `nixVersion`/`currentSystem` already follow
(confirmed: bare `storeDir` is also "undefined variable" on real Nix).

Implemented identically to the existing `nixVersion`/`currentSystem`
precedent in `builtins_misc.c` (a `build()` slot returning a plain
string Value, no `PrimOp` needed since it's not callable). `make
test`/`test-e2e` green (one new `m71_*` fixture, verified against real
`nix-instantiate --eval --strict` first), `test-conformance` (mined
subset) unchanged. `d.nix` progresses past this exact blocker to the
next one.

## M72 scope notes: bumped `derivation`'s attribute-count cap; `d.nix` reaches completion

Found continuing to chase `d.nix` right after M71: the next (and, it
turned out, final) blocker was "asnix: derivation: too many attributes
(max 64)" -- `derivation.c`'s `asnix_prim_derivation` stores every
input attribute into a few fixed-size C stack arrays
(`env[ASNIX_DRV_MAX_ENV]`, `json_syms`/`json_vals[ASNIX_DRV_MAX_ENV]`,
M69's `__structuredAttrs` support), bounded at a hardcoded 64 -- never
hit by any fixture or conformance case so far, but a real
`stdenv.mkDerivation` call (the structuredAttrs-era
`make-derivation.nix`, M67/M69's own area) unconditionally sets a
couple dozen attributes (`enableParallelBuilding`, `strictDeps`,
`depsBuildBuild`, `depsHostHostPropagated`, `__structuredAttrs`, `env`,
`outputChecks`, ...) before the package itself contributes any of its
own, exceeding 64 easily once a few package-specific attributes are
added on top.

Fix: bumped `ASNIX_DRV_MAX_ENV` from 64 to 256. These are plain fixed-
size stack arrays, not an arena allocation -- the only cost of a larger
bound is a few more KB of stack per `derivation {...}` call, negligible
at this size (`ASNIX_DRV_MAX_ARGS`, the separate, unrelated `args`-list
cap, left at 64 -- not what tripped this). Verified with a synthetic
100-attribute `derivation {...}` call (new `tests/e2e/m72_*` fixture)
that previously hit the cap and now succeeds on both engines.

**`./asnix d.nix` now evaluates to completion with no crash** --
the exact standing goal this whole `d.nix`-driven bug-chase (every
milestone from M33 through this one, across many single-session passes
and seven parallel background agents) was aimed at. It prints a real-
looking Nix store path for `hello.outPath`:
```
asnix:      "/nix/store/z7zqhkm36dgjarhrwn6sf7akzx00zc0p-hello-2.12.3"
real Nix:   "/nix/store/wpr3vw194dl8g7bj94d54rj76v3rp3c0-hello-2.12.3"
```
The STRING SHAPE matches (a valid-looking `/nix/store/<32-char-hash>-
hello-2.12.3` path), but the HASH itself does not match real Nix's --
expected and out of scope, not a bug to chase further: bit-exact
derivation-hash equality across `hello`'s entire real dependency
closure (the full stdenv bootstrap chain, every intermediate
derivation's exact `.drv` content) would require this project's
already-and-repeatedly-documented scope cuts to not exist at all --
real string-context/`hashDerivationModulo` tracking through actual
input derivations (M61, M62's own notes), exact real-Nix-matching
`__structuredAttrs`/`__json` serialization (M69's own documented
scope), fixed-output multi-output derivations, and more. Getting the
hash to match would mean building all of that, a vastly larger project
than "find and fix crashes" -- this milestone's success criterion was
always "evaluates without crashing," which is now met.

`make test`/`test-e2e`/`test-conformance` all green (one new `m72_*`
fixture). `make test-nix-conformance`: `pass=624 fail=34 skip=148`,
unchanged (this fix isn't exercised by that corpus -- no test case
constructs a 65+ attribute derivation).

## M73 scope notes: fixed-output derivations' real `makeFixedOutputPath` formula

Found while scoping M74's `hashDerivationModulo` work (the user asked
for `d.nix`'s computed `hello.outPath` to actually MATCH real Nix's,
not just stop crashing) -- M74's own verification plan calls for a
"fixed-output input" test case (since `<nix/fetchurl.nix>`, M46, is
fixed-output, and `hello`'s own `src` comes from it), which immediately
exposed a pre-existing, **separate** bug: a fixed-output
`derivation {...}` call's `outPath` never matched real Nix at all.
`validate_output_hash_fields`'s own pre-existing comment admitted this
outright ("it does NOT change how the derivation's store path is
actually computed") -- the function only ever validated `outputHash`'s
SHAPE, then let the output path fall through to the ordinary masked-
ATerm-digest scheme every other (non-fixed-output) derivation uses,
which is simply the wrong formula for this case. Confirmed directly:
`derivation { ...; outputHashAlgo="sha256"; outputHashMode="flat";
outputHash="00...00"; }.outPath` gave a different path on asnix than
on real `nix-instantiate --eval --strict`.

**Fix**: ported real Nix's actual `makeFixedOutputPath`
(`~/nix/src/libstore/store-dir-config.cc`) verbatim. Two cases:
- `sha256` + `outputHashMode = "recursive"`: the output path comes
  straight from the content hash itself (`makeStorePath("source", hash,
  name)`) -- no extra hashing step.
- every other combination (the common case: `"flat"` mode, which is
  `<nix/fetchurl.nix>`'s own default, M46): hash a
  `"fixed:out:<r:-or-empty><algo>:<hex-hash>:"` payload string with
  SHA-256, then feed *that* into the ordinary `makeStorePath("output:out",
  ..., name)` formula (`compute_store_path`, unchanged, M8/M62). Renamed
  `validate_output_hash_fields` to `decode_output_hash` (same
  validation, now also returns the decoded raw digest bytes + resolved
  algorithm so the above can actually use them -- a new
  `compute_fixed_output_path` function implements the two-case formula,
  reusing `compute_store_path` in both branches rather than
  reimplementing its preimage/outer-hash/base32 pipeline a third time).

A second, related gap found verifying the "recursive" case specifically:
real Nix's `.drv` ATerm for a fixed-output derivation's `Outputs` entry
carries real `hashAlgo`/`hash` fields (confirmed directly against a
real `.drv`: `("out",path,"sha256","0d5f6015...")`, not `("out",path,
"","")`, which is what every *other* derivation's entry correctly
still has) -- needed because this derivation's own `drvPath` is hashed
from this REAL (unmasked) ATerm text, so a wrong Outputs entry means a
wrong `drvPath` even when `outPath` itself is already correct (exactly
what the "recursive" verification case caught: `outPath` matched,
`drvPath` didn't, until this was fixed). `"recursive"` mode additionally
prepends `"r:"` to that ATerm field specifically (confirmed: `("out",
path,"r:sha256",hash)`) -- a real Nix detail (`makeFileIngestionPrefix`)
distinct from (and in addition to) the path-formula's own `"r:"`
handling above. `AtermOutput` (`aterm.h`/`aterm.c`) gained `hash_algo`/
`hash` fields, NULL for the ordinary (non-fixed-output) case every
other derivation still uses unchanged.

Verified against real `nix-instantiate --eval --strict`, both `outPath`
AND `drvPath`, across: flat+sha256 (hex), SRI-format (`sha256-base64`,
`outputHashAlgo=""`, `<nix/fetchurl.nix>`'s own shape, M48), recursive+
sha256, and flat+md5 -- all four now match exactly.

`make test`/`test-e2e` green (`tests/c/test_aterm.c` and its 4 call
sites updated for `asnix_build_derivation_aterm`'s new `inputDrvs`/
`inputSrcs` parameters, added ahead of M74's own need for them, passed
as empty for now; `test_multi_output`'s unaffected by the new
`AtermOutput` fields since C zero-initializes trailing struct-literal
members). `make test-conformance` (mined subset) unchanged.
`make test-nix-conformance`: `pass=624 fail=34 skip=148` before this
fix, `pass=626 fail=32 skip=148` after -- +2 net, zero regressions.

## M74 scope notes: real `hashDerivationModulo` (input-derivation hashing)

The user explicitly rejected "`d.nix` evaluates without crashing" as
good enough (M72's own stated goal) once it was pointed out that the
computed `hello.outPath` hash didn't actually match real Nix's. Root
cause: `hello`'s derivation depends on several other derivations
(`bash`, `stdenv-linux`, a `fetchurl`-fixed-output source, etc.), and
real Nix's `hashDerivationModulo` requires substituting each input
derivation's *own* hash in place of its literal `.drv` path before
hashing the dependent derivation -- `derivation.c` had no concept of
"this attribute value references another derivation" at all, so it
always string-coerced every attribute and lost that information
entirely, making this substitution impossible.

Two pieces, neither needing real store/file I/O:

- **A narrow, call-scoped input-reference collector**
  (`asnix_collect_input_refs`), *not* a persistent Value-level string-
  context feature (that broader feature -- a 4th tagged Value kind
  touching ~101 call sites -- was declined twice already, M61 and
  implicitly M45/M54/M60, as disproportionate to its own user-facing
  payoff). This one only needs to know, for the duration of one
  `derivation {...}` call, which attribute values are (or contain) a
  derivation-output reference. Walks each already-forced attribute
  value: an attrset with `type`/`drvPath`/`outputName` (every
  derivation Value has these, M58's wrapper) records a `Built` ref;
  a list recurses into every element; a bare Path already under
  `/nix/store/` records an `Opaque` `inputSrcs` ref. Nothing else is
  tracked (scope cut, matching M61's own precedent of naming what's
  deferred): no `DrvDeep` context, no copying arbitrary local files
  into a content-addressed store (no precedent for that anywhere in
  this project; M45's `toFile` explicitly declined the equivalent).
- **A process-global, in-memory masked-hash cache**
  (`g_masked_hash_cache`, modeled directly on M65's `g_import_cache`
  precedent and real Nix's own `derivation::masked::hashes`). Real
  Nix's `nix-instantiate --eval` normally writes real `.drv` files to
  `/nix/store` as a side effect and could read them back, but its
  OWN `--read-only-mode` falls back to exactly this kind of pure
  in-process memoization instead -- since Nix's call-by-need
  evaluation guarantees a referenced derivation was already forced
  (hence already cached) before anything can reference it, asnix can
  replicate read-only-mode's behavior with no real store at all. A
  cache miss is fatal (a real invariant violated), not a silent
  fallback, matching this project's standing convention.

`aterm.c`/`aterm.h` generalized to accept real `InputDrvs`/`InputSrcs`
sections (previously hardcoded to `"],[],[],"`) -- `AtermInputDrv`
is `(drv_path, [output_names...])`, matching real Nix's own ATerm
shape exactly. `derivation.c` groups the collected `Built` refs by
`drv_path` (a small, bounded, linear-scan grouping, same convention as
every other small array in this file), builds TWO parallel
`AtermInputDrv` lists -- `real_input_drvs` (literal `.drv` paths, for
this derivation's own on-disk `.drv` content/`drvPath` hash) and
`masked_input_drvs` (each group's `drv_path` replaced by its cached
masked hash, hex, no algorithm prefix -- the actual substitution) --
and passes each into its own `asnix_build_derivation_aterm` call.

Three distinct, non-obvious bugs found and fixed chasing this through
increasingly complex verification cases (each isolated by comparing
asnix's own ATerm text, via gdb, against a real `.drv` dump, and
cross-checking suspect hash computations by hand in Python):

1. **Wrong cache value for regular (non-fixed-output) derivations.**
   Initially cached `masked_digest` (the `maskOutputs=TRUE` variant,
   with THIS derivation's own outputs blanked) as the value a later
   derivation substitutes in for this one. Wrong: real Nix's
   `hashDerivationModulo`'s `maskOutputs` parameter only ever controls
   whether OUTPUT paths are blanked -- it does NOT affect whether
   `inputDrvs` get hash-substituted (that substitution always
   happens). The correct cached value is a THIRD ATerm variant never
   otherwise built: this derivation's REAL (filled-in) outputs
   combined with the SAME hash-substituted `inputDrvs` the masked pass
   already uses. Fixed by building `cache_aterm` from `real_outs` +
   `masked_input_drvs` specifically for the cache-insert step.
2. **Wrong `type` string for a derivation's own `drvPath`.** Even
   after fix 1, a 2-level chain's dependent derivation's `outPath`
   matched but its `drvPath` didn't, despite the real ATerm text being
   byte-identical (confirmed via `diff`) to real Nix's actual `.drv`
   file. Root cause: a `.drv`'s own store path is a content-addressed
   "text" path whose TYPE string is NOT the bare `"text"` this project
   previously used -- it's `"text:" + every inputSrcs/inputDrvs path,
   sorted` (`infoForDerivation`, `~/nix/src/libstore/derivations.cc`):
   a `.drv` genuinely "references" everything it mentions. Every PRIOR
   fixture had zero references, so this was unnoticed until M74's own
   2-level verification case gave it something to catch. Fixed by
   building `drv_type` from the sorted union of `group_paths`/
   `refs.srcs` before the final `compute_store_path` call.
3. **Wrong `InputDrvs` ordering for the masked pass specifically.**
   The `nativeBuildInputs = [ a b2 ]` (two distinct input derivations)
   verification case still mismatched after fixes 1/2, even though the
   REAL pass's `InputDrvs` section (sorted by literal `.drv` path) was
   byte-identical to real Nix's actual `.drv`. Root cause: real Nix's
   substitution doesn't just swap each `inputDrvs` entry's path for
   its resolved hash in place -- it rebuilds `inputDrvs` as a NEW
   `std::map<string, StringSet>` keyed by the hash STRING itself
   (`~/nix/src/libstore/derivations.cc`), and a `std::map`'s iteration
   (which `unparse` walks) is sorted by ITS OWN key -- so the MASKED
   pass's `InputDrvs` section must be sorted by the substituted hash
   hex string, independently of whatever order the literal paths
   sorted in. Confirmed: `b2`'s masked hash sorts before `a`'s despite
   `a.drv` sorting before `b2.drv`. Fixed by re-sorting
   `masked_input_drvs` by its own (already-substituted) `drv_path`
   field, separately from `real_input_drvs`'s literal-path order.

Two more bugs surfaced only once this was run against real nixpkgs
(`d.nix`, not any of this milestone's own small verification cases --
every prior fixture had too few references/inputs to hit either):
`compute_store_path`'s `preimage` buffer was a fixed 512 bytes, too
small once `type` itself became `"text:" + N references` for a
derivation with many direct inputs/sources; and the companion
`drv_type` accumulation buffer (`derivation.c`) was a fixed 4096
bytes with no overflow check on its own `snprintf` return value (a
silent-truncation risk, not just a size cap). Both are now sized
dynamically (`malloc`, exact required length) instead of guessing a
bigger constant. A third, related issue: `ASNIX_DRV_MAX_INPUT_DRVS`
(the grouping arrays' bound) was 64, too small for `stdenv-linux`
alone (well over 64 distinct direct input derivations in real
nixpkgs) -- bumped to 1024, but moved from the stack to `malloc`,
since resolving one derivation's input refs can recursively force
(hence recursively call `asnix_prim_derivation` for) a dependency
several levels deep in nixpkgs's own derivation graph, and 1024
entries' worth of these arrays on the stack per nested call actually
overflowed the real stack chasing `d.nix` to this point.

Verified against real `nix-instantiate --eval --strict`, all 5 cases
from this milestone's own plan: a plain 2-level chain, a 3-level
chain, a fixed-output input, a multi-output input (referenced by name,
`a.dev` not just `a.out`), and a list of two distinct inputs
(`nativeBuildInputs = [ a b2 ]`, the case that caught bug 3 above) --
every case's `outPath` AND `drvPath` match exactly
(`tests/e2e/m74_input_*`).

**Still open, deliberately out of this milestone's scope** (per the
plan's own Phase 1/Phase 2 split): `./asnix d.nix` now runs to
completion with the real substitution logic in place (no longer
crashing on the input-derivation-count/stack-depth bugs above), but
`hello.outPath` still doesn't bit-for-bit match real Nix's
(`n0m8hzqnp3bj0if010gjial5402xvgym...` vs real's
`wpr3vw194dl8g7bj94d54rj76v3rp3c0...`). This milestone's own scope
boundary explicitly anticipated this: there may be independent gaps
elsewhere in the full nixpkgs/stdenv chain (real Nix's internal
corepkgs builder/setup shell scripts, which this project doesn't
embed, being the leading suspect) that this milestone's narrow,
controlled verification cases can't surface. Chasing that down is a
separately tracked follow-up, using the same gdb/direct-`.drv`-diff
methodology this milestone itself used.

`make test`/`test-e2e` green (5 new `m74_input_*` fixtures, each
verified byte-for-byte against real `nix-instantiate --eval --strict`
before being written). `make test-nix-conformance`:
`pass=627 fail=31 skip=148` -- +1 net over M73, zero regressions.

## M75 scope notes: real store-copying (NAR-hash) for local Path literals

Chasing `d.nix` past M74: `hello.outPath` still didn't match real
Nix's. Root cause, confirmed by diffing real Nix's actual
`hello-2.12.3.drv` byte-for-byte against this project's own computed
ATerm: real Nix's `inputSrcs`/`args` embed `/nix/store/...-source-
stdenv.sh` and `/nix/store/...-default-builder.sh` -- `pkgs/stdenv/
generic`'s own local `.sh` files, referenced as plain Path literals
and copied into the content-addressed store when coerced to a string.
`asnix` had never implemented this at all: `value_util.c`'s
`coerce_to_string_for_interp` had an explicit, long-standing, *known*
comment admitting as much ("real Nix additionally copies the path to
the store... not implemented... produces the plain path text instead"),
and `derivation.c`'s own M74 input-reference collector only ever
tracked a bare Path already under `/nix/store/`, never one that still
needed copying there first.

**Fix**: a new `src/c/nar.c`/`nar.h`, `asnix_copy_path_to_store`,
implementing real Nix's own `addToStore` (method=NixArchive/
"recursive", hashAlgo=sha256) exactly: NAR-serialize the path (file,
directory, or symlink -- format verified byte-for-byte against
`~/nix/src/libutil/archive.cc`/`serialise.cc`: each string field is an
8-byte little-endian length, the raw bytes, then zero-padded to the
next 8-byte boundary; directory entries sorted ascending by name),
SHA-256 the result, then feed that digest through the EXACT SAME
`makeStorePath("source", digest, name)` formula M73's
`compute_fixed_output_path` already implements for its own
sha256+recursive branch -- no new path-computation logic needed, only
the NAR-serialization step was missing. Like `derivation`/`toFile`,
does not perform the real filesystem side effect (no actual
`/nix/store` writes) -- only the resulting path *string* is computed,
same stance this project has taken since M8. Memoized by literal
input path (linear-scan array, M65's import-cache convention) so the
SAME local path being both collected (M74's `asnix_collect_input_refs`)
and coerced (the actual env-var/string value) doesn't get NAR-hashed
twice.

Wiring turned out to need THREE call sites, not one, because real
Nix's `coerceToString` has independent `coerceMore`/`copyToStore`
flags and this project previously only had the two combinations that
happen to share neither:
- `coerce_to_string_for_interp` (genuine `"${...}"` string
  interpolation, `copyToStore=true`, `coerceMore=false`): now actually
  performs the copy where it previously only had the existence check
  (M67f) in front of a documented no-op.
- A brand new `asnix_coerce_to_string_for_derivation`
  (`value_util.c`/`.h`): `derivation {...}`'s own attribute coercion
  needs `coerceMore=true` (plain ints/bools/null become "7"/"1"/""
  like `toString`, verified: ordinary nixpkgs derivation attributes
  are frequently non-strings) **and** `copyToStore=true` together --
  a combination neither `asnix_coerce_to_string` (has the former, not
  the latter) nor `coerce_to_string_for_interp` (has the latter, not
  the former) provides alone. Recurses into itself (not
  `asnix_coerce_to_string`) for nested ATTRS (`__toString`/`outPath`)
  and LIST cases, so a Path nested inside either also gets copied --
  matches real `hello.drv`'s own `args` list, which contains exactly
  two such local-file references. `derivation.c`'s three existing
  coercion call sites (`args`, `outputs`, the generic per-attribute
  path) switched to it.
- `asnix_collect_input_refs`'s bare-Path case (M74, `derivation.c`):
  previously only recorded an `inputSrcs` entry for a path ALREADY
  under `/nix/store/`; now computes (via the same memoized
  `asnix_copy_path_to_store`) and records the COPIED path for anything
  else, so the `.drv`'s `inputSrcs` section agrees with what the
  coercion above actually embedded as the literal env-var/arg text --
  required, since a `.drv`'s `inputSrcs` and its own attribute values
  have to name the same store path.

**Regression found and fixed in the same pass**: `tests/e2e/
m67_path_interp_exists.nix` (`"${/.}"`, M67f) exercised this new copy
path for the first time and tried to NAR-serialize the entire
filesystem root -- which even REAL Nix fails on, on this machine
(`error: opening '/boot/System.map-...': Permission denied`,
confirmed directly). That fixture's choice of `/.`  was always a
placeholder (its own M67 scope notes already flagged "note its own
exact output isn't byte-identical to what real Nix would do," since
the copy wasn't implemented yet at the time) -- not a real scenario
worth preserving now that copying actually happens. Changed it to
self-reference the fixture file itself (always exists, small,
deterministic), and corrected its `.expected` to the real computed
store path (removing the long-standing caveat).

Verified against real `nix-instantiate --eval --strict`: a plain file
interpolated into a string, a whole directory (recursive NAR,
multiple files/subdirectories) interpolated into a string, and a
local file referenced from inside a `derivation {...}`'s `args` --
all three match exactly (`tests/e2e/m75_path_copy_*`).

**Still open**: `./asnix d.nix`'s `hello.outPath` STILL doesn't match
real Nix after this fix (it changed -- confirming this fix did take
effect -- but didn't converge). Diffing asnix's own computed ATerm for
`hello-2.12.3` against real Nix's actual `.drv` found the next gap is
qualitatively different and larger than either M74 or M75: real Nix's
`hello.drv` has FOUR `inputDrvs` (`bash`, `stdenv-linux`,
`version-check-hook`, the `fetchurl` source), but asnix's only has
THREE -- missing `bash` entirely, even though `bash`'s own store path
*is* correctly present as the literal `builder` string. Root cause:
nixpkgs sets `builder = stdenvShell;` where `stdenvShell` ultimately
traces back to `shell = "${stage0.bash}/bin/bash";` (`pkgs/stdenv/
linux/default.nix`) -- a STRING INTERPOLATION of a derivation
reference, concatenated with a literal suffix, evaluated to a plain
string long before `derivation {...}`'s primop ever sees it. By the
time `asnix_collect_input_refs`'s structural walk (M74: looks for a
bare derivation attrset, a list of derivations, or an already-resolved
store Path) sees the `builder` attribute's value, it's just an opaque
string with no trace of "this came from derivation bash's output" --
exactly the information real Nix's actual *string context* (an
invisible, attached-to-the-string-Value dependency set, threaded
through `coerceToString`/concatenation/substring/etc.) exists to
preserve. This is the same broader feature M61 (and implicitly M45/
M54/M60) already explicitly declined as disproportionate to its own
user-facing payoff (`getContext`/`hasContext`/
`addDrvOutputDependencies`) -- M74's plan deliberately scoped around
it with a narrower, call-scoped structural collector specifically
because the full feature is large. That narrower collector is
necessarily blind to the (idiomatically very common in real nixpkgs)
case of a dependency reference flowing through a string
interpolation rather than staying a bare attrset/Path -- which is
exactly what `hello`'s own `builder` attribute does. Separately
(smaller, independent): asnix's `__json` blob also has two EXTRA keys
real Nix's doesn't (`__ignoreNulls`, `userHook`) -- real Nix's
structured-attrs serialization drops any attribute whose value is
`null` when `__ignoreNulls = true` is set (including `__ignoreNulls`
itself), not yet implemented here. Both are tracked as a follow-up,
not blocking this milestone's own (verified, scoped) completion.

`make test`/`test-e2e` green (3 new `m75_path_copy_*` fixtures --
file, directory, and derivation-attribute cases -- plus the
`m67_path_interp_exists` fixture/expected correction above).
`make test-nix-conformance`: `pass=628 fail=30 skip=148` -- +1 net
over M74, zero regressions.

## M76 scope notes: real string context

Chasing `d.nix` past M75: `hello.outPath` still didn't match real Nix.
Diffing asnix's own computed ATerm for `hello-2.12.3` against real Nix's
actual `.drv` found the gap was qualitatively different and larger than
M74/M75: real Nix's `hello.drv` has FOUR `inputDrvs` (`bash`,
`stdenv-linux`, `version-check-hook`, the `fetchurl` source), but asnix's
only had THREE -- missing `bash` entirely, even though `bash`'s own store
path *was* correctly present as the literal `builder` string. Root cause:
nixpkgs sets `builder = stdenvShell;`, which traces back to `shell =
"${stage0.bash}/bin/bash";` (`pkgs/stdenv/linux/default.nix`) -- a STRING
INTERPOLATION of a derivation reference, concatenated with a literal
suffix, evaluated to a plain opaque string long before `derivation
{...}`'s primop ever sees it. M74's structural collector
(`asnix_collect_input_refs`: looks for a bare derivation attrset, a list
of derivations, or an already-resolved store Path) is blind to this by
construction -- once a reference becomes a STRING via interpolation,
there's no structure left to walk. This is the same feature M61 (and
implicitly M45/M54/M60) already declined once, for being disproportionate
to its own user-facing payoff (`getContext`/`hasContext`/
`addDrvOutputDependencies`) -- the user explicitly chose to implement it
anyway, since it turned out to also be load-bearing for hash parity.

**Key design simplification vs. the originally-declined scope**: that
earlier estimate (~101 call sites, a new tagged Value kind touching the
asm layer) assumed context would be baked into the STRING HEAP LAYOUT
itself (like Path/Float's marker-byte trick, `include/asnix/heap.h`).
Instead, a new `src/c/string_context.c`/`.h` attaches context via a SIDE
TABLE keyed by the string Value's own arena pointer (a linear-scan array,
same convention as `eval_file.c`'s import cache / `derivation.c`'s
masked-hash cache) -- ordinary string reads (every asm op: equality,
printing, hashing, `OP_STR_CONCAT`'s own byte-copying) stay COMPLETELY
untouched, since they never need to know a context exists. Only the
specific C-level functions that *construct* a new string from existing
string/attrs/list inputs need to query-and-propagate -- a literal survey
of every such function (an Explore agent cross-referencing both this
codebase and real Nix's own `coerceToString`/`primops.cc`) found ~20 call
sites, not ~101, and zero asm changes.

`AsnixStringContext` (`drvs: {drv_path, output_name}[]`, `srcs: path[]`)
reuses exactly M74's own `BuiltRef`/`InputRefSet` vocabulary.
`asnix_string_attach_context(v, ctx)`/`asnix_string_context_of(v)` are the
only two public entry points -- a no-op/`NULL` for the overwhelmingly
common case (every plain-literal/arithmetic/non-derivation string never
gets a table entry at all).

**Origin points** (where context is *synthesized*): `asnix_prim_derivation`
(`derivation.c`) now attaches `Built(drv_path, output_names[o])` to each
output-path string it builds for the self-referential `strict` attrset --
this is exactly what `pkg.out`/`pkg.${name}` resolve to, which is what
`"${pkg}/bin/x"`-style interpolation reads via the `outPath`-lookup
coercion path (`drvPath` itself stays context-free -- that's real Nix's
separate `DrvDeep` context, referencing a derivation's whole closure,
explicitly out of scope, same as M74's own collector never handled it).
Coercing an already-`/nix/store/...` Path to a string, and M75's NAR-copy
result, both now attach `Opaque(path)` -- this subsumes M74's bare-Path
structural detection entirely.

**Propagation points** (forward/union existing context, no new entries):
`asnix_coerce_to_string`'s ATTRS (`__toString`/`outPath`) and LIST
branches; the same two branches in `asnix_coerce_to_string_for_derivation`
(M75); `coerce_to_string_for_interp`'s Path branch; `concat_strings_impl`
(backs both `"${...}"` interpolation and `+`'s string/path-concat case --
the single highest-traffic site). Each follows the same mechanical
pattern: accumulate each piece's own context via `asnix_string_context_of`,
union into a local `AsnixStringContext`, attach to the result if
non-empty. Scoped OUT of this milestone (matching M74's own precedent of
naming what's deferred, not silently skipping it): propagation through
`substring`/`concatStringsSep`/`baseNameOf`/`dirOf`/`replaceStrings`/
`stringToCharacters`/`toJSON`/`toXML`; `getContext`/`appendContext`'s real
Nix-level attrset shape (currently still hardcoded no-ops);
`unsafeDiscardStringContext` actually stripping an entry (currently still
a harmless identity pass-through, same as before this milestone, since
nothing had context to discard either way); `toFile`'s context-validation-
and-fold-into-NAR-references nuance. None of these are hit by `hello`'s
own attribute shapes -- real but not load-bearing for this milestone's
goal, tracked as a follow-up.

**`derivation.c` rewiring**: `asnix_collect_input_refs` (M74) is extended,
not replaced -- it still recognizes a bare derivation attrset/list
structurally (needed for e.g. `src = a;` where `a` is a derivation
attrset not yet coerced to a string), but now ALSO: (a) reads attached
context off any plain-string leaf it visits (the actual fix -- an
interpolated `builder` string, by the time it reaches here, carries its
`Built(bash's drvPath, "out")` ref from M76's propagation above, with no
structure left to inspect), and (b) recurses into a GENERIC (non-
derivation-shaped) attrset's own nested attribute values too, instead of
treating it as a dead end. (b) was found necessary, not just thorough-for-
its-own-sake: confirmed directly that real `hello.drv` itself uses
`__structuredAttrs = true;`, whose non-special attributes bypass the
normal string-coercion call entirely and reach this collector only as a
raw, uncoerced Value -- matching real Nix's own JSON serializer
(`printValueAsJSON`), which performs the identical recursive walk to
collect context from a structured-attrs blob.

**Found and fixed in the same verification pass, not string-context
itself but directly blocking the same goal**: `__ignoreNulls = true;`
(nixpkgs' `make-derivation.nix` sets this unconditionally) was not
implemented at all -- real Nix's `derivationStrictInternal`
(`~/nix/src/libexpr/primops.cc`) unconditionally skips the
`__ignoreNulls` attribute itself (regardless of its own value or
`__structuredAttrs` mode) and additionally skips every OTHER attribute
whose forced value is `null` when the flag is set. Confirmed directly:
real `hello.drv`'s own `__json` blob has neither `__ignoreNulls` nor
`userHook` (a null-valued attribute nixpkgs sets) at all; asnix previously
emitted both verbatim, a second, independent source of `__json` byte
mismatch on top of the missing `bash` inputDrv.

Verified against real `nix-instantiate --eval --strict`: the exact
`hello`-shaped pattern (`builder = "${a}/bin/sh";`, a derivation reference
reaching `derivation {...}` ONLY through a prior string interpolation,
never as a bare attrset) and a `+`-concatenated variant both match
exactly (`tests/e2e/m76_interp_dep.nix`/`m76_plus_dep.nix`); all 5 of
M74's own verification fixtures re-verified unchanged (the context-based
collection produces IDENTICAL results to the old structural-only walk for
every case it already handled).

**Re-ran `d.nix`**: `hello.outPath` still does NOT match real Nix
bit-for-bit, but the gap changed character entirely. Before this
milestone: 3 of 4 expected `inputDrvs` (missing `bash`), and the `__json`
blob had 2 extra keys real Nix's doesn't. After: all 4 `inputDrvs` present
(sorted correctly), identical `inputSrcs`, and the `__json` blob now has
the EXACT SAME KEY SET as real Nix's (confirmed by direct comparison,
key-by-key) -- every remaining difference is a VALUE mismatch for keys
whose values are themselves derived from upstream packages (`bash`,
`stdenv-linux`, `version-check-hook`, the fetched source tarball) not yet
matching real Nix's own hashes. Dumping real `bash-5.3p15.drv` directly
found it has its OWN 21 `inputDrvs` (`bootstrap-stage4-gcc-wrapper`,
`bootstrap-stage4-stdenv-linux`, `bison`, 15 individual patch derivations,
...) -- this is the full nixpkgs bootstrap chain, not a leaf. Reaching bit-
exact `hello.outPath` parity means reaching bit-exact parity for every
package in that chain, an open-ended, much larger undertaking than any
single milestone so far -- explicitly flagged to the user as such rather
than silently continuing into it; tracked as a separate, indefinite
follow-up to the standing `d.nix` bug-chase, not a blocker on this
milestone's own (verified, scoped) completion.

`make test`/`test-e2e` green (2 new `m76_*` fixtures, each verified
byte-for-byte against real `nix-instantiate --eval --strict` before being
written). `make test-nix-conformance`: `pass=628 fail=30 skip=148` --
unchanged from M75 (this corpus doesn't exercise multi-derivation
hashing), zero regressions.

## M77 scope notes: a real-nixpkgs correctness regression test, plus two bugs it found

The user pointed out, correctly, that M58-M76's entire `d.nix` bug-chase
had zero automated regression coverage of its own actual target --
`hello.outPath` parity against real Nix -- only small, synthetic,
hand-written fixtures (`tests/e2e/m74_input_*`/`m75_*`/`m76_*`). Every
check against the real target was a one-off manual `nix-instantiate`
comparison, re-done by hand each session, with no way to notice a
regression (or measure forward progress) automatically.

New `make test-nixpkgs-hash` (`tests/nixpkgs-hash/attrs.txt` +
`pkgs.nix`): for every nixpkgs attribute path listed, checks
`.outPath`/`.drvPath` byte-for-byte against real `nix-instantiate --eval
--strict`, using `-A` (both tools already support dotted attribute-path
selection -- `main.c`'s own `select_attrpath`, added for exactly this
kind of use, ported from real Nix's own `-A`). Deliberately
**non-gating**, same convention as `test-nix-conformance`: this corpus
is EXPECTED to have real failures for a long time (the standing bug-chase
being open-ended), so a hard-fail-the-build gate would make the target
useless the moment any single upstream package doesn't fully match --
reports pass/fail, a crash and a hash MISMATCH both counted as failures
but labeled distinctly (`(crash)`/`(mismatch)`), a real-Nix eval failure
for a listed attribute silently skipped (typo/doesn't-exist, not an
asnix correctness question).

Building the corpus immediately found two real, fixable bugs, both
before even reaching the harness's final form:

1. **`builtins.toFile`'s result never carried its own store-path as
   context.** `hello.src.drvPath` didn't match real Nix even though
   `hello.src.outPath` did and `hello.src` is a near-leaf fixed-output
   derivation (not deep in the bootstrap chain) -- diffing asnix's own
   computed ATerm against real `hello-2.12.3.tar.gz.drv` found asnix's
   `inputSrcs` was missing a `mirrors-list` entry that WAS correctly
   present (same computed path!) inside the `__json` blob's
   `mirrorsListFile` field. Root cause: `<nix/fetchurl.nix>`'s own
   `mirrorsListFile = toFile "mirrors-list" (...)`-- real Nix's
   `builtins.toFile` (`mkStorePathString`, `~/nix/src/libexpr/eval.cc`)
   attaches `Opaque(selfStorePath)` as the RESULT string's own context
   (NOT forwarded from its `contents` argument, a separate, still-
   unimplemented nuance -- see `prim_toFile`'s own header comment);
   `asnix`'s M45-era `prim_toFile` never attached any context to its
   result at all, so by the time this path reached M76's
   `asnix_collect_input_refs`, it was already an opaque string with no
   trace of being a store reference. Fixed by attaching the same
   `Opaque(out_path)` context `asnix_prim_derivation`'s own M76 origin
   point already does. Verified: `hello.src.drvPath`'s `inputDrvs`/
   `inputSrcs`/`__json` now match real Nix's shape exactly (output
   PATHS still differ -- `bash`/`curl`/`stdenv-linux-no-cc` are
   themselves unresolved bootstrap-chain packages, expected).
2. **`curl.outPath` crashes** (`concatStringsSep: expected a string
   argument`, confirmed via gdb to be a bare ~93-attribute derivation-
   shaped attrset reaching `concatStringsSep` where real Nix would have
   a plain string) -- traced to `pkgs/stdenv/generic/problems.nix`'s
   meta-validation machinery (`concatMapStringsSep ", " (el: el.name)
   kindGroup`-shaped code), several layers into nixpkgs' own
   `checkMeta`/problem-reporting system. NOT root-caused or fixed this
   milestone -- registered in the new corpus as a known-failing
   `(crash)` case instead of silently working around or hiding it,
   since making it VISIBLE or, is the entire point of this milestone;
   a real, separate bug to chase as its own follow-up.

Current `make test-nixpkgs-hash` baseline: `pass=1 fail=7` (`hello`,
`hello.src`, `bash`, `curl`, each `.outPath`/`.drvPath` --
`hello.src.outPath` is the only pass; everything else is either a
genuine upstream-bootstrap-chain hash mismatch, already expected and
documented in M76's own scope notes, or `curl`'s crash above). This
number is the actual, truthful state of the standing bug-chase's own
goal -- expected to only improve from here as the bootstrap-chain work
continues, and any future regression in what little passes now will be
caught immediately instead of discovered by accident.

`make test`/`test-e2e`/`test-conformance`/`test-nix-conformance` all
green, zero regressions (the new target is additive and non-gating).
