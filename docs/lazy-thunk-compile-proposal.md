# Lazy thunk-body compilation proposal

Status: PROPOSAL, not yet implemented or measured. This is a design
exploration, not a milestone -- see `docs/architecture.md`'s M180
section for the measured sizing data this proposal is built on, and
`docs/architecture.md` generally for the project's actual shipped
history. Nothing in this document should be treated as decided until
the measurements it calls for actually happen.

**Update (follow-on design pass, read this before the sections below).**
A dedicated design pass worked out a concrete mechanism for the general
`OP_LAZY_THUNK_BODY` stub -- specifically targeting the failure mode
that sank M187's narrower `callPackage`-fusion attempt (the
`asnix_apply` out-of-line call overhead) -- and a concrete lifetime
scheme for the two obstacles this doc already names, plus a THIRD real
hazard this doc did not name, found only by reading the already-shipped
milestone that introduced it (see "A third lifetime hazard" below). No
code was changed on master or any other branch: the dispatch-side
mechanism is concrete enough to specify exactly, register-by-register
and byte-offset by byte-offset, against the real assembly it would hook
into -- but the lifetime fix touches enough call sites across
`parser.c`/`ast.c`/`compiler.c`, and interacts with the freeing logic of
THREE separately-shipped milestones at once, that building and
verifying it correctly was judged to need its own dedicated pass, not a
tack-on to this one -- consistent with this doc's own prior "Recommended
sequencing" section judging the full general opcode larger than one
shot, and with M187's own lesson that the mechanism matters as much as
the idea. See "Sizing the new retention cost" below for exactly what
measurement should happen before any of this is built, and the new
"Four-stage plan, revised" at the end for the recommended next step.

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

## Concrete mechanism: the self-upgrade needs ZERO new branches at the five force-entry sites, not one

Re-reading `force.S`, `op_var_ref.S` (`op_select.S`/`op_select_or.S`/
`op_with.S` all follow the identical shape) against the actual IR
dispatch ABI (`docs/abi.md`, `include/asnix/ir.h`) finds this project's
existing dispatch mechanism is already exactly the hook this design
needs, for free -- no edit to any of the five sites is actually
required, which is a stronger result than this doc's original "exactly
one new branch each" estimate.

**Why.** `asnix` is a direct-threaded interpreter: `IrOp` is `{void
*handler; int32_t a; int32_t b}` (`ir.h:179-183`), and `ir_link()`
rewrites every op's `handler` word in place from a small integer op-id
to the real machine address of that op's handler, once, right when the
owning `IrProgram` is finished (`ib_finish`, `irbuild.c:384`). Every one
of the five force-entry sites enters a thunk's body the exact same way,
confirmed directly in the current code (`op_var_ref.S:77-91`, the
`UNFORCED` branch):

```asm
movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13   // r13 = POOL  (prog->pool)
movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi    // rsi = IP    (prog->ops)
movq ASNIX_THUNK_OFF_ENV(%rax), %rdi     // rdi = ENV   (thunk's captured scope)
...
movq (%rsi), %rax                        // rax = ops[0].handler
jmp  *%rax                               // dispatch into whatever that handler is
```

`%rcx` here is "the clean prog pointer" -- the shared `IrProgram*`
every dynamic `Thunk` cell at one static `OP_MK_THUNK` site points at
(`ASNIX_THUNK_OFF_STATE`, M103). The final `movq (%rsi),%rax; jmp
*%rax` does not care, and has never had to care, WHAT `ops[0].handler`
actually is -- it is already a fully generic computed jump into
whichever handler `ir_link` installed there. This is, by construction,
already "the branch" the original design called for: the existing
dispatch step itself distinguishes stub-vs-real with zero marginal
cost, the same way it already distinguishes `OP_ADD` from `OP_CALL`
from anything else.

**So the entire mechanism is one new op and one new handler, nothing
else.** Add `OP_LAZY_THUNK_BODY` to the `ir.h` enum and
`asnix_handler_table` (`dispatch_table.c`) exactly like every other op.
Its pool entry (`b` = pool index, same convention `OP_MK_THUNK`/
`OP_PUSH_CONST` already use) holds one pointer: a `LazyThunkDescriptor*`
(new struct, `ir.h` or a new small header) with four fields:

```c
typedef struct LazyThunkDescriptor {
    IrProgram *self;       /* the SAME shared IrProgram this stub's own
                             * OP_MK_THUNK pool entry points at -- this is
                             * exactly what gets mutated in place on
                             * upgrade. Sibling of ASNIX_THUNK_OFF_STATE's
                             * pointer, not a new indirection. */
    Ast *body;              /* the deferred subtree -- see the lifetime
                             * sections below for why this is safe to
                             * still be valid when this fires */
    ScopeFrame *scope;       /* persisted (arena-owned, not C-stack) copy
                             * of the scope chain compile_expr would have
                             * been called with */
    const char *file_path;   /* g_compile_file_path at defer time */
    const char *base_dir;    /* g_compile_base_dir at defer time */
} LazyThunkDescriptor;
```

A stub program is built through the EXISTING `IrBuilder` pipeline,
unchanged -- no hand-rolled `IrProgram` construction, no separate
`ir_link` call to remember:

```c
IrBuilder *stub_b = ib_new();
LazyThunkDescriptor *desc = /* allocate + fill, see above */;
size_t pool_idx = ib_pool_add(stub_b, (int64_t)(intptr_t)desc);
ib_emit(stub_b, OP_LAZY_THUNK_BODY, 0, (int64_t)pool_idx);
IrProgram *stub_prog = ib_finish(stub_b);   /* runs ir_link() on this 1-op
                                             * array exactly like any other
                                             * program -- nothing special */
desc->self = stub_prog;                     /* self-reference, set once */
ib_mk_thunk(b, stub_prog);                  /* unchanged -- ib_mk_thunk
                                             * already just pool-adds
                                             * whatever IrProgram* it's
                                             * handed */
```

The new handler, `asnix_op_lazy_thunk_body` (`src/asm/ops/
op_lazy_thunk_body.S`, picked up automatically by the Makefile's
existing `$(wildcard src/asm/ops/*.S)`), is entered with exactly the
same register state every other op handler is entered with (`%rsi` =
this stub's own 1-op `ops` array, `%r13` = this stub's own 1-entry
`pool`, `%rdi`/`%r12`/`%r14` = the calling thunk's ENV / the live
VALSTACK / the live RETSP, all already correctly set up by whichever of
the five force-sites got here -- a RETFRAME pointing back to the real
caller, with `force_target` set to the `Thunk*`, was ALSO already
pushed by that site before the jump, exactly as for any other thunk
body). Mirroring the existing save/restore convention every other
C-calling op handler already uses (`op_mk_thunk.S:14-15`, `docs/
abi.md`'s "Calls to `asnix_arena_alloc`" section: push IP/ENV, call,
pop):

```asm
asnix_op_lazy_thunk_body:
    movl  ASNIX_IROP_OFF_B(%rsi), %ecx
    movq  (%r13, %rcx, 8), %rdi      // rdi = LazyThunkDescriptor*
    pushq %rsi                        // save IP (dead anyway, but mirror
    pushq %rdi                        // the project's 2-push convention)
    call  asnix_compile_lazy_thunk_body   // C bridge, see below -- returns
                                            // the SAME IrProgram* (desc->self),
                                            // now upgraded in place
    popq  %rdi                        // restored (unused below, but keeps
    popq  %rsi                        // the stack-discipline identical to
                                       // every other call site)
    movq  %rax, %rcx                  // rcx = desc->self, now carrying REAL
                                       // ops/pool
    movq  ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq  ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq  (%rsi), %rax
    jmp   *%rax                       // fall into the real first op, in
                                       // the SAME dispatch context -- no
                                       // asnix_apply, no new RETFRAME
```

This is the exact property M187's own "Conclusion" section asked for: a
real fix "would need an inline trampoline reusing the ALREADY-active
dispatch context instead of an out-of-line `asnix_apply` call." Here
that trampoline falls out for free, because unlike the `callPackage`
fusion (which had to turn stored *data* -- a path and an override
attrset -- into a live function application, a genuinely different
runtime shape needing `asnix_apply`'s general call machinery), this
stub upgrades into the literal SAME `IrProgram` an eager compile would
have produced on day one. There is no semantic gap to bridge at
force-time, only a compile call that happens to run late -- so falling
through is just "keep running this thunk's body," identical in shape to
every other force in the interpreter, not a new kind of control
transfer.

**The C bridge**, `asnix_compile_lazy_thunk_body` (`compiler.c`, new,
small):

```c
IrProgram *asnix_compile_lazy_thunk_body(LazyThunkDescriptor *desc) {
    const char *saved_base_dir = g_compile_base_dir;
    const char *saved_file_path = g_compile_file_path;
    g_compile_base_dir = desc->base_dir;
    g_compile_file_path = desc->file_path;

    IrBuilder *b = ib_new();
    compile_expr(b, desc->body, desc->scope);
    ib_ret(b);
    IrProgram *real = ib_finish(b);

    g_compile_base_dir = saved_base_dir;
    g_compile_file_path = saved_file_path;

    desc->self->ops = real->ops;    /* mutate the SHARED struct in place --
    desc->self->pool = real->pool;   * every dynamic Thunk cell from this
                                      * static site already points at
                                      * desc->self, never at `real` */
    free(real);                      /* the 16-byte IrProgram wrapper
                                      * ib_finish handed back is now
                                      * redundant -- its two fields were
                                      * copied out, desc->self IS the
                                      * permanent record */
    return desc->self;
}
```

Grepping `compiler.c` confirms `g_compile_base_dir`/`g_compile_file_path`
(lines 34/40) are the ONLY two file-scope mutable globals `compile_expr`
depends on beyond its explicit `IrBuilder*`/`Ast*`/`ScopeFrame*`
parameters -- so this save/restore pair is a complete fix for obstacle
#2 from this doc's own "Real obstacles" section above (not a partial
one), and `compile_expr`/`ib_new`/`ib_finish`/the symbol table are all
ordinary, reentrant C functions with no other hidden call-scoped state
that would make calling them mid-dispatch (nested inside an active
`asnix_eval`, not from `eval_file.c`'s top-level driver) unsafe. The
bridge touches only compile-time bookkeeping (`malloc`-backed `IrOp`/
pool arrays) -- it never touches the runtime `Env`/`Value` arena, so
there is no reentrancy hazard with the live dispatch loop it is called
from.

One honest residual to verify, not yet done: whether the opt-in
`ASNIX_IR_OPT=1` pass pipeline (`irpass_run`, invoked unconditionally
inside `ib_finish`) tolerates a 1-op/1-pool-entry program whose single
op is an id none of the existing passes recognize. Expected to be inert
(every pass scans for its own specific op shapes and should simply find
none), but this is a "confirm, don't assume" item for whoever
implements this, not a design risk.

## A third lifetime hazard: `src`, the raw source-text buffer

This doc's original "Real obstacles" section named two: M109/M110's
Ast-arena freeing, and `ScopeFrame`'s C-stack lifetime. Reading
`docs/architecture.md`'s M115 section directly (not assumed from this
doc's own earlier framing) found a third, real one neither this doc nor
the task that commissioned this pass had named:

**`Ast` string fields point directly into the file's raw source buffer,
not into independently-owned memory.** M115's own comment states this
explicitly: `AstBinding.name`/`AstFormal.name` (and, by the same
mechanism, `AstVar.name`, string-literal/path-literal text, `AST_SELECT`
`attr` fields, etc.) are byte-range pointers into `src`, the `char*`
`read_file` returns -- never copied out. M115 made this buffer's
lifetime explicit and tied it to exactly one `asnix_compile`/
`asnix_compile_scoped` call, freeing it right after (the same point
M109/M110 resets the `Ast` arena), after measuring the pre-M115 leak at
**~24.41 MB of logical bytes summed across the real `d.nix`/ffmpeg
closure's ~1,057 imported files**.

This means persisting the `Ast` NODES of a deferred subtree (fixing
hazard #1) and persisting the `ScopeFrame` STRUCTS of its scope chain
(fixing hazard #2) is not sufficient on its own: every name/string field
either structure holds is still a dangling pointer into a buffer M115
already frees at the exact same moment. Any design that stops at "keep
the nodes and frames alive" has a latent use-after-free baked in,
invisible until a real multi-file eval reuses the freed `src` buffer's
address for something else (the same "never hit the tiny fixture suite,
only the real workload" shape M109/M110's own prior bug had, per that
section's post-mortem) -- a sharp reminder that `git grep`ing this
doc's own two named obstacles is not the same as re-deriving them
against the CURRENT code.

**Two fix candidates, and why one is clearly better for this specific
workload:**

1. *Extend `src`'s own lifetime* whenever at least one still-unupgraded
   stub might reference it. Simple (no byte-copying needed, since
   existing Ast nodes already just alias into `src` as-is), but far too
   coarse: a single deferred attrset field anywhere in a file keeps that
   file's ENTIRE source buffer alive, and virtually every real nixpkgs
   file has at least one attrset literal. This would give back close to
   the FULL ~24.41 MB M115 measured saving, for nearly all 1,057 files
   in the real closure -- not a marginal cost, a near-total reversal of
   an already-shipped, already-measured win. Rejected.
2. *Deep-copy only the referenced bytes*, at the exact same parse-time
   moment hazard #1's own fix (below) redirects node allocation -- i.e.
   whenever the parser is building a name/string/path field while
   "inside a deferred subtree," copy the underlying bytes into the same
   persistent store instead of aliasing `src`. Bounded by the actual
   identifier/string-literal content length of just the deferred
   subtrees, not by whole-file size -- a small, measurable fraction of
   the ~24.41 MB figure, not comparable to it. Recommended.

Candidate 2 needs one new small helper (`deferred_strdup_or_alias` or
similar) and touches every `parser.c` construction site that currently
captures a `cur(p).start`-based substring destined for a long-lived
`Ast` field (plausibly on the same order as the "46 real construction
sites" M188 already enumerated for a related reason, `asnix_ast_new_at`'s
`src_pos` threading) -- real, bounded surface area, not a single-line
fix, and exactly the kind of multi-site mechanical change this pass's
own conclusion judges worth a dedicated follow-up rather than a
same-shot patch.

## Lifetime mechanism: redirect allocation at parse time, don't deep-copy at compile time

Two ways to keep a deferred subtree's `Ast` nodes alive past M109/M110's
per-file arena reset were considered:

- **(A) Deep-copy the subtree at the moment `compile_expr` decides to
  defer it** (walk `bind->value`'s already-parsed tree, reallocate each
  node into a long-lived arena).
- **(B) Redirect `asnix_ast_new`'s allocation target at PARSE time**,
  for exactly the dynamic extent of parsing one attrset binding's value
  expression, via a single global flag (the same pattern
  `g_compile_base_dir`/`g_compile_file_path` already use for
  call-scoped state) toggled around the relevant `parse_expr` call(s)
  inside `parse_binding_stmt` -- which is shared, unchanged, by both
  `let` and attrset-literal parsing today, so the toggle happens only at
  the attrset-literal call sites, never at the `let` one.

**(B) is the right choice, and the reason is central to this whole
proposal's own stated goal, not a minor implementation preference.**
(A) walks and reallocates the ENTIRE subtree of EVERY attrset field,
unconditionally, the moment `compile_expr` reaches it -- which is
exactly the ~91%-never-forced population this proposal exists to stop
doing eager work for. A node-shaped deep copy is cheaper per byte than
full IR compilation, but it is still `O(subtree size)` eager work paid
for every field whether or not it is ever forced, which only shrinks
the wasted-work problem, not eliminates it. (B) costs nothing extra:
the parser already walks every node exactly once to build it in the
first place; redirecting WHERE that one, already-necessary allocation
lands is free. The same reasoning applies to the string-copy fix for
hazard #3 above -- copy the bytes once, during the parse that was going
to touch them anyway, never a second pass.

**Nesting is handled correctly by construction, not by a special case.**
The toggle flag should be saved and restored around the one call (not
force-set/force-cleared), so an attrset literal nested inside another
deferred field's value stays deferred into the SAME persistent arena --
consistent with the fact that forcing the outer field's stub will need
to recursively compile the inner one too, exactly mirroring how eager
`compile_expr` already recurses into nested attrset literals today with
no extra plumbing. `compile_expr`'s own `AST_ATTRSET` case applies the
"defer this field's value" rule locally and unconditionally wherever it
fires, including on a subtree reached via a now-deferred compile call --
so recursive deferral falls out of the existing code structure, not a
new mechanism.

**Scope of what gets redirected.** The flag's dynamic extent is "while
parsing one binding's value inside `parse_binding_stmt`," which, because
that function is shared with `inherit`-sugar and attrpath-sugar
handling, also harmlessly over-retains a few already-eagerly-consumed
nodes (a dynamic key's own `${expr}`, an `inherit (expr) ...;` clause's
shared `src_expr`) in the persistent arena instead of the per-file one.
This is a correctness no-op (compiler.c still compiles those parts
eagerly, exactly as today, regardless of which arena their nodes
happen to sit in) and a small, bounded memory cost, not the dominant
one -- a later refinement could narrow the toggle to only the exact
value sub-`parse_expr` call if this noise turns out to matter, but
measuring the DOMINANT cost (the field-value subtrees themselves) first
is the right order of operations.

**`ScopeFrame` persistence is a separate, smaller step on top of (B).**
`ScopeFrame` itself is never parser-owned (it's built by `compiler.c`,
not `parser.c`), so it needs its own explicit persist step, run once at
the exact point `compile_expr`'s `AST_ATTRSET` case decides to defer a
field instead of calling `ib_mk_thunk(b, ib_finish(thunk_b))` eagerly:

```c
/* new: deep-copies the chain (walking ->up), allocating each frame AND
 * a fresh copy of its names[]/name_lens[] arrays from a dedicated,
 * non-resettable arena -- critically, copying the STRING BYTES each
 * name points to as well (hazard #3), not just aliasing the Ast-owned
 * pointer, since the enclosing attrset literal node that currently owns
 * those bytes is not necessarily itself part of the deferred subtree
 * and will NOT survive M109/M110's ordinary per-file reset. */
ScopeFrame *scope_frame_persist(ScopeFrame *stack_scope);
```

This chain is short (bounded by real lexical nesting depth, not by
file size) and each frame small, so this step's absolute cost should be
negligible relative to the Ast-retention cost above -- but it is still
a real, currently-unmeasured allocation that should be counted in the
same measurement pass, not assumed free.

## Sizing the new retention cost -- the single most important thing to measure before building this

This proposal's core trade is: stop compiling ~1.93M IR ops (M180) that
are thrown away anyway, in exchange for keeping SOME amount of `Ast`
memory (plus now, per hazard #3, some `src`-buffer bytes, plus
`ScopeFrame` copies) alive for the ~90.7% of deferred thunks that never
force, for the rest of the process -- memory M109/M110/M115 each
already measured as a real, deliberate win to free promptly. This is
exactly the kind of trade-off this project has now hit twice this
session already (the IrOp-compaction lookup-table question, and M187's
own declined `callPackage` fusion): an instruction-count win that can
hide a memory cost an instructions-only measurement would never catch.

**What is already known, as bounds, not as the answer:**
- M110 measured freeing the WHOLE file's `Ast` tree (every node, not
  just attrset-field values) at **~6.2% peak RSS** on `d.nix`/ffmpeg
  (736,920 KB -> 691,780 KB, ~45 MB). The attrset-field-value-only
  subset this proposal would keep alive is a FRACTION of that whole
  tree's node count/bytes -- bounded above by ~45 MB, but the real
  fraction is unmeasured. M180's own framing text ("forcing one package
  forces compilation of every sibling binding in the same file (let/
  attrset scope)") suggests attrset fields plausibly dominate the
  469,859-thunk-body population by count, which would push this
  fraction toward the higher end of that bound, not the lower one --
  a real, continuing to describe this as a wash would be hand-waving.
- M115 measured freeing `src` buffers at **~24.41 MB** summed across the
  real closure. Candidate 2 above (copy only referenced bytes) bounds
  the new cost to a small multiple of total identifier/string-literal
  byte length inside deferred subtrees specifically -- plausibly much
  smaller than 24.41 MB, but again, not measured.
- Against both of these: the ~1.93M-op compile-avoidance itself has its
  own memory-side benefit not yet quantified either -- fewer `IrOp`/pool
  arrays allocated means less `malloc` overhead/fragmentation on top of
  the raw byte count, something M170's own `ib_finish`-shrink investigation
  already showed matters in practice on this exact code path.

**The concrete next step, before writing any of the asm/compiler.c
changes above**: build ONLY the measurement half -- redirect parser.c's
attrset-field-value `Ast` allocation into a separate, deliberately
UN-resettable arena (exactly the mechanism recommended above, minus the
actual `OP_LAZY_THUNK_BODY` opcode/compile-deferral machinery) and
measure peak RSS on the real `d.nix`/ffmpeg eval with THAT alone,
compared against current master. This isolates and directly answers
"how much of M109/M110's ~45 MB win does deferral actually give back,"
with no new opcode, no new dispatch-side risk, and no scope-correctness
risk at all (the file still compiles every binding eagerly in the exact
same order as today -- only WHICH arena temporarily holds some of the
now-permanently-retained `Ast` nodes changes). If that number alone is
already a wash or a regression against the ~1.93M-op savings' own
(separately real, but partial) memory benefit, the general opcode is
not worth building at all, and this proposal should say so plainly
rather than proceeding on hope. If it is comfortably favorable, THEN
the `OP_LAZY_THUNK_BODY` dispatch mechanism above (lower-risk, now fully
specified, no `asnix_apply` cost) is the next real step, followed by the
`src`-buffer byte-copy fix for hazard #3, in that order -- cheapest and
most informative measurement first, highest-risk mechanical change last.

### Stage 1 measured: real, not comfortably favorable -- stopping here, not proceeding to stage 2 unauthorized

Built exactly the measurement-only experiment above: `ast.c` gained a
second, parallel chunk list (`g_ast_retained_chunks`) that
`asnix_ast_arena_reset()` never frees, plus a toggle
(`asnix_ast_enter_retained`/`asnix_ast_leave_retained`, nested-safe --
returns/restores the previous state rather than hard-coding on/off, so
an attrset literal nested inside another already-redirected value stays
redirected). `parser.c`'s `parse_binding_stmt` became a thin wrapper
(`parse_binding_stmt_inner` holds the original unchanged body) that
toggles redirection on exactly when `dyn_out != NULL` -- this file's own
existing signal for "this is an attrset literal, not a `let` block" (the
3 attrset-literal call sites pass `&dyn`; the 1 `let` call site passes
`NULL` specifically to reject dynamic attrs). No opcode, no compiler.c
change, no scope-correctness change at all -- every binding still
compiles eagerly, in the same order, by the same code; only which chunk
list physically holds the nodes differs.

**Correctness**: `make test`, `make test-e2e` (300/300),
`make test-nix-conformance` (`pass=655 fail=3 skip=148`, same 3 known
pre-existing failures), `make test-nixpkgs-hash` (`pass=148 fail=0`),
`./asnix d.nix` output hash unchanged -- confirming this redirect-only
change is behaviorally invisible, as designed.

**Measured** (`/usr/bin/time -v`, 3 interleaved trials, real
`d.nix`/ffmpeg eval, against a from-scratch baseline built at this
proposal's own design-doc-extension commit):

| | peak RSS |
|---|---|
| before (no redirect) | 511,808 KB (avg) |
| after (attrset-field-value `Ast` retained) | 539,146.7 KB (avg) |
| delta | **+5.34%** (+27,338.7 KB, ~+26.7 MB) |

This is a real, non-trivial retention cost -- roughly the same order of
magnitude as the naive memory-avoidance estimate for skipping 1.93M
`IrOp` array allocations alone (1.93M x 16 bytes/op, post-M169, is
~31 MB), meaning the memory side of this trade is genuinely AMBIGUOUS
on this number alone, not "comfortably favorable" the way this
section's own decision rule requires before proceeding to stage 2. It
is also not a clean regression that kills the idea outright (the two
numbers are close enough that a real net memory effect, once the
`IrOp`-array avoidance is also accounted for, could land either way).

**Decision: stopping at stage 1, not proceeding to build
`OP_LAZY_THUNK_BODY` in the same pass.** This isolated number alone
cannot settle the ship/no-ship call -- that requires the INSTRUCTION
side of the trade (the primary, larger motivation: avoiding ~1.93M
wasted IR-op compiles, likely several percent of total eval
instructions), which only the real stub mechanism from stage 2 can
produce. Building stage 2 -- a new opcode, a C bridge mutating a shared
`IrProgram` in place, hooking into all 5 force-entry sites' existing
dispatch, plus the `src`-buffer byte-copy fix for hazard #3 -- is
exactly the kind of correctness-sensitive, multi-file mechanical change
this proposal's own prior sections already judged needs its own
dedicated pass, not a tack-on once stage 1's number turned out
ambiguous rather than clearly favorable. Recorded here as the real
answer to "how much does deferral cost in isolation," per this
section's own stated purpose -- the next step is building stage 2 for
real, as its own dedicated pass, to get the instruction-side number
this ambiguous memory result makes necessary before any ship decision.

No code landed on master -- the arena-redirect experiment lives on its
own disposable worktree/branch (`lazythunk-impl-7bde`), not merged.

## Four-stage plan, revised

The original "Four-stage plan" above is superseded by this ordering,
now that the mechanism and lifetime design are concrete:

1. **Measure retention cost in isolation** (the `Ast`-arena-redirect-only
   experiment just above) -- no opcode, no compiler.c change beyond the
   arena redirect, answers the single open question that determines
   whether anything further is worth building.
2. **If favorable**: implement `OP_LAZY_THUNK_BODY` plus
   `asnix_compile_lazy_thunk_body` exactly as specified above, behind a
   build-time check (this project's standing discipline for fragile
   layout/invariant changes, e.g. M188's variant-build precedent) --
   deferring ONLY attrset field values (not let-bindings, list
   elements, or call arguments, matching this doc's original scoping
   rationale), with two targeted correctness tests: a known-dead
   attrset field's body never runs `compile_expr` at all (instrument
   and assert zero calls), and a field forced through two different
   dynamic `Thunk` instances at the same static site compiles exactly
   once (assert the second force sees `ops[0].handler` already real,
   never re-enters the stub handler).
3. **Fix hazard #3** (the `src`-buffer byte-copy, candidate 2 above) in
   the same pass, not as a follow-up -- shipping without it is a latent
   use-after-free, not a smaller, separately-landable milestone.
4. **Measure for real** on the full `d.nix`/ffmpeg eval with interleaved
   `perf stat` (instructions) AND `/usr/bin/time -v` (peak RSS), plus
   the full verification ladder (`make test`, `make test-e2e` 300/300,
   `make test-conformance`, `make test-nix-conformance` expecting
   `pass=655 fail=3 skip=148`, `make test-nixpkgs-hash` expecting
   `pass=148 fail=0`, and `./asnix d.nix`'s output hash unchanged) --
   both numbers matter equally, per this session's own repeated lesson
   that one without the other is not a real result.
