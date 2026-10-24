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
