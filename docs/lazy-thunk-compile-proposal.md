# Lazy thunk-body compilation proposal

Status: PROPOSAL, not yet implemented or measured. This is a design
exploration, not a milestone -- see `docs/architecture.md`'s M180
section for the measured sizing data this proposal is built on, and
`docs/architecture.md` generally for the project's actual shipped
history. Nothing in this document should be treated as decided until
the measurements it calls for actually happen.

## Motivation

M180 measured that asnix's `compile_expr` compiles every binding's
value to IR immediately when its enclosing scope is compiled, whether
or not that binding is ever forced. On the real `d.nix`/ffmpeg eval:
469,859 distinct thunk bodies get compiled, but only 43,640 (9.3%) are
ever forced. The other 426,219 (90.7%) are compiled and never touched --
roughly 1.93M wasted IR ops, about 62% of the eval's entire 3.1M total
compiled IR volume. This is larger than any single lever found so far
this session, including the lexer TokVec-elimination win.

## Proposed design: a self-upgrading stub opcode

Every dynamic `Thunk` cell created from one static `OP_MK_THUNK` site
shares a single compiled `IrProgram` pointer. Today that pointer always
points at a fully compiled body. The proposal changes only what it
points at on day one: instead of real bytecode, it points at a
one-instruction stub -- call it `OP_LAZY_THUNK_BODY` -- whose pool entry
carries the still-uncompiled `Ast` node for the thunk's value expression,
plus the scope information needed to resolve variable names to depths.

The five places in the code that already know how to enter an unforced
thunk -- `op_var_ref.S`, `op_select.S`, `op_select_or.S`, `op_with.S`,
and `force.S` -- need exactly one new branch each: if the entered
program's first op is this stub, call back into the compiler to produce
the real `IrProgram`, overwrite the stub's own `ops`/`pool` fields in
place with the result, then fall through into the now-real code exactly
as today. Because all dynamic instances from one static site share the
one `IrProgram` struct, this self-upgrade happens at most once per
static site, no matter how many separate `Thunk` cells force it later.

Nothing about blackhole detection, memoization of the forced value, or
`letrec` self-reference needs to change, because none of that logic
looks past the thunk's state tag -- it only ever sees "a clean
`IrProgram*` pointer," before and after the upgrade.

## Real obstacles, both from already-shipped work

**M109's Ast-node freeing.** M109 made the project explicitly free `Ast`
nodes once a file finishes compiling, to save memory. A stub that defers
compilation needs its `Ast` subtree to survive past that point, possibly
until long after the rest of the file's AST is gone.

**`ScopeFrame`'s stack lifetime.** The scope-resolution structure the
compiler needs, `ScopeFrame` in `compiler.c`, is a plain C stack
variable today, freed the moment the enclosing `compile_expr` call
returns -- it also will not survive to a later force.

Both problems share the same fix: give the `Ast` node and the scope
chain for any subtree chosen for deferral a longer-lived arena
allocation, and exclude exactly those nodes from M109's freeing pass.
The cleanest way to bound this cost is to defer only attrset field
values, not ordinary let-bindings or call arguments, since the
measurement points squarely at attrset fields -- large nixpkgs-style
package lists -- as the source of the waste, while let-bindings tend to
be forced soon after creation and would gain little from deferral.

**The M94 bytecode disk cache.** A cached file's on-disk form has to be
real bytecode, since raw pointers to `Ast` nodes are meaningless across
process runs. The simplest safe rule: lazy-compile stubs and bytecode
caching do not mix -- a file read through the cache compiles eagerly as
it does today, and the stub only applies on a cold compile. This is not
really a loss, since cold-eval instruction count is the project's actual
target metric, not the cached-eval path.

## Four-stage plan

1. **Before building anything**, extend M180's disposable counters to
   break down the never-forced population by creation kind -- attrset
   field vs. let-binding vs. list element vs. call argument -- to
   confirm the waste really concentrates in attrset fields the way the
   theory predicts. This is cheap and should run before committing to
   the design.
2. Write a short proposal doc (this one, extended once stage 1's data
   is in) covering the new op's exact pool payload (`Ast` node, scope
   chain, plus the file path and base directory the compiler needs for
   relative imports, since those are globals today and must be saved
   per deferred thunk and restored around the deferred compile call),
   the arena-lifetime change for deferred `Ast`/scope data, and the
   small bridge function the assembly calls into the C compiler.
3. Implement behind a build-time safety check (this project's standing
   discipline for fragile layout/invariant changes), plus two targeted
   tests: one proving a known-dead attrset field is never compiled,
   another proving a field forced twice through two different dynamic
   instances only compiles once.
4. Measure for real on the full `d.nix`/ffmpeg eval with interleaved
   `perf stat`, checking both instruction count AND peak memory -- the
   extra per-force branch in five hot paths and the arena cost of
   keeping deferred subtrees alive both cut into the ~1.93M compiled
   ops this measurement suggests are avoidable, and this project has
   already learned once this session (the IrOp-compaction memory
   question) that an instruction-count win can come with a memory cost
   that needs its own separate check, not an assumption.

## Recommended sequencing: a narrower first step

A follow-on pass on this proposal recommends starting smaller than the
general `OP_LAZY_THUNK_BODY` stub above, in order of surgical-ness:

1. **Fuse the `callPackage`-idiom shape at compile time, before touching
   AST/scope lifetimes at all.** A large share of nixpkgs's never-forced
   bindings are almost certainly the plain `name = callPackage path {};`
   idiom -- a fixed three-piece shape: a function value, one small
   override attrset, and a call. This project already has precedent for
   recognizing a specific call shape at compile time and emitting a
   cheaper fused form instead of generic bytecode: M112's peephole fusion
   of `builtins.elem`/`any`/`all`/`match`/`intersectAttrs`, and M113's
   `LazyApp` kind for `map`/`mapAttrs`/`zipAttrsWith`. The same move
   applies here -- recognize the `callPackage`-shaped binding and store
   it as plain data (a path pointer and an override-attrset thunk), zero
   compiled IR, instead of running `compile_expr` on it; the force path
   interprets that data directly. This has none of the general design's
   lifetime problems (no `Ast` node or scope chain to keep alive, nothing
   to free later, no bytecode-cache interaction to design around), at
   the cost of covering only one recognized shape rather than every
   attrset field. Given how much of real nixpkgs is this one shape, it
   could capture a large fraction of the measured 62% for a fraction of
   the risk, and should be measured BEFORE deciding whether the general
   opcode design is still worth its added risk.
2. **Push deferral down past compile into parse itself** (phase 2, once
   stage 1 above is measured and proven): right now an unforced thunk
   still costs a full parse into `Ast` nodes even if its IR never gets
   built. V8 and other JavaScript engines solve the equivalent problem
   with lazy inner-function parsing -- a cheap pre-scan that only finds
   the matching closing delimiter of a function body, skipping full
   parsing until the function is actually called. The same trick applies
   to an attrset field's value: a fast bracket-and-string-aware scan
   records just the byte range and enough lexer state to resume later,
   and the real parse-plus-compile only happens on first force. This
   compounds with the general opcode idea (saving `Ast`-node allocation
   too, not just the IR-compile step) and actually helps M109's
   free-after-compile work rather than fighting it, since there is no
   `Ast` node to free at all for a field never forced. More invasive
   than stage 1 (touches `parser.c`, not just `compiler.c`).
3. **Generalize the stub beyond attrset fields to lambda bodies**: a
   lambda defined in a library file (common in nixpkgs's `lib/` tree) but
   never called in a given eval pays the same eager-compile cost as an
   unforced attrset field. The same lazy-stub mechanism applies to
   `OP_LAMBDA`'s body, deferred to first call instead of first force,
   reusing the same bridge function and arena-lifetime fix. A cheap
   add-on once the attrset-field version exists, not a separate design.
4. **A narrower, `iets`-inspired per-entry descriptor for the
   `callPackage` idiom specifically**, addressing a case the stub/opcode
   plan does not help at all: a field that genuinely does get forced
   still pays the cost of allocating a distinct 16-or-24-byte thunk
   object -- the same problem `iets` found for `mapAttrs` and fixed with
   `THUNK_MAP`. `iets`'s fix only works because `mapAttrs` entries share
   ONE callback body across every entry; the nixpkgs-style attrset case
   does not share a body across bindings, so `THUNK_MAP` itself does not
   transfer directly. But a narrower version might: for the recognized
   `callPackage` idiom from stage 1, the per-entry data is already small
   and uniform (a path plus one override attrset), so it could live in a
   shared descriptor with an 8-byte per-entry cell instead of a full
   generic thunk object -- cutting memory for the ~9% of bindings that DO
   get forced, as well as compile cost for the ~91% that don't.

Checked directly against `iets`'s own architecture (`~/gnix/iets/
ARCHITECTURE.md`, `docs/explanation/evaluation.md`): `iets` has the same
two-stage `parse -> compile -> eval` shape as asnix and pays the same
per-binding eager-compile cost -- its own measured number on a comparable
workload is almost identical to this proposal's (22M `mapAttrs` thunks,
93% never forced). Its `THUNK_MAP` fix targets a different part of the
problem (per-instance thunk *allocation* for a shape that shares one
compiled body across all instances), not the per-binding compile cost
this proposal targets (textually distinct bodies, each needing its own
compile). So this is a genuinely open opportunity, not a case of asnix
catching up to something `iets` already solved -- there is no existing
design to borrow wholesale, only the shared precedent that both
evaluators have independently measured "most thunks are never forced"
as real and worth designing around.

## Open risks

- **Scope correctness.** `let`'s self/sibling visibility, `inherit`
  scoping, and `rec`-attrset self-reference are all delicate areas this
  project's own M146-era comments in `compiler.c` already flag. Deferred
  compilation must resolve variable names to the exact same depths a
  same-shot compile would have produced -- any drift here is a silent
  correctness bug, not a build failure.
- **Bytecode cache interaction.** Must be verified that a file compiled
  once with deferred stubs, then re-read from a cold compile a second
  time (cache disabled, per this project's cold-eval benchmark
  methodology), produces byte-identical final results to the fully
  eager path -- not just "doesn't crash."
- **Scale of the per-force branch cost.** Five hot paths each gain one
  new branch. Given this project's own measured dispatch volume
  (14,656,701 steps on the real eval, from the op-pair histogram
  instrumentation), even a cheap branch multiplies; this needs to be
  measured against the ~1.93M-op savings directly, not assumed to be
  negligible.
