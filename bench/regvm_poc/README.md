# Register-based-VM POC microbenchmark

Follow-up to docs/architecture.md's M123 cross-evaluator speed investigation
(asnix ~2.35x wall-clock / ~2.07x instructions slower than `iets` on a fair
single-threaded nixpkgs eval) and the step-1 finding that caching just the
top 1-2 VALSTACK slots in spare registers has no clean fit (every callee-
saved register is already owned by `op_call.S`/`op_let.S`/`op_frame.S`;
the general-purpose registers are even more heavily used inside
`op_var_ref.S`). This directory is the bigger step-2 POC: how much would a
genuine register-based evaluation approach save, measured directly, not
estimated on paper?

**Not linked against `libasnix.a` and not wired into the real compiler/
dispatch pipeline** -- by design (see the parent task). This is a
standalone, hand-assembled comparison of two implementations of the exact
same op-handler fast-path instruction sequences real asnix uses
(`op_var_ref.S`'s already-forced/non-Cell path, `op_arith.S`'s all-int
fast path, `op_cmp.S`'s `asnix_op_lt` fast path), copied verbatim, for one
representative chain with NO `OP_CALL`/`OP_LET`/`OP_FRAME_*` anywhere in
it:

```
(a + b) * (c - d) < (e + f) * (g - h)
```

8 `OP_VAR_REF` + 6 arithmetic ops + 1 `OP_LT` -- exactly the slice where
step 1's "every register is already spoken for" blocker does not apply.

## What's compared

- `run_valstack_chain` (`chains.S`): every operand pop/push goes through a
  real memory-resident VALSTACK via `%r12`, byte-for-byte matching
  `op_arith.S`/`op_cmp.S`/`op_var_ref.S`'s own convention (same tag
  checks, same overflow checks, same `ASNIX_CHECK_VALSTACK` bounds check).
- `run_regvm_chain` (`chains.S`): the same 8 variables and 7 intermediate
  results live in a fixed set of real registers (`%r8`-`%r11`) for the
  whole chain -- statically assigned per chain position. This requires
  per-position SPECIALIZED handler code (16 distinct label blocks, not 5
  generically-reused ones) -- a real, unavoidable cost of register
  residency in a non-JIT interpreter: x86 can't address a register by a
  runtime integer index, so a *generic*, reusable register-VM handler
  would have to keep its "registers" in a memory-indexed register file
  anyway, which is a different addressing scheme for the exact same
  memory traffic this POC is trying to eliminate, not a way around it.
  This POC deliberately measures the OPTIMISTIC CEILING (as if a tiny
  template-codegen specialized this one chain shape), not what a
  generic bytecode-dispatch register VM could actually achieve.

Both share the *same* threaded-dispatch mechanism (a 16-entry tape of
`{handler, a, b}` 24-byte records -- `ir.h`'s real `IrOp` layout -- each
handler ending in "advance IP, load next handler, `jmp *reg`"), and
`OP_VAR_REF` still reads its `slot` operand from the tape at runtime in
BOTH versions (not baked in as an immediate in the register version) --
so dispatch overhead and the env-lookup/tag-check cost are identical
between the two. The *only* deliberate difference is whether an
intermediate value takes a real store-then-reload round trip through
memory or stays in a register the whole time.

Both chains fold each iteration's final bool result into a running XOR
checksum across `iters` repetitions, returned to `harness.c` and printed
-- this is the correctness gate (not wired into `make test`): run both
variants with the same `iters`, their printed checksums must be
bit-identical (verified: they are, for both an even and an odd iteration
count).

## Building

Same flags the project's own `Makefile` uses for `.S`/`.c`:

```
cc -std=c11 -Wall -Wextra -g -O2 -Iinclude -Isrc/c -U_FORTIFY_SOURCE -Wa,--noexecstack -c -o chains.o chains.S
cc -std=c11 -Wall -Wextra -g -O2 -Iinclude -Isrc/c -U_FORTIFY_SOURCE -c -o harness.o harness.c
cc -o bench harness.o chains.o
```

## Measuring

This machine is a hybrid Meteor Lake part (P-cores + E-cores show up as
separate `cpu_core`/`cpu_atom` perf PMUs) -- every measurement below pins
to CPU 0 (confirmed a P-core: `cpu_atom` events read `<not counted>`
there) and reads the `cpu_core/.../` PMU explicitly, so P/E-core
migration never contaminates the counts. 7 repeats via `perf stat -r 7`,
200,000,000 iterations per run (~3s/run, long enough that process-startup
noise is negligible):

```
taskset -c 0 perf stat -r 7 -e cpu_core/instructions/,cpu_core/cycles/,cpu_core/branches/,cpu_core/branch-misses/ -- ./bench stack 200000000
taskset -c 0 perf stat -r 7 -e cpu_core/instructions/,cpu_core/cycles/,cpu_core/branches/,cpu_core/branch-misses/ -- ./bench regvm 200000000
```

Load average on this shared machine was 0.4-1.3 (22 cores) at measurement
time -- quiet by this session's own standard for "trustworthy", and the
cross-run spread below (<0.2% on cycles, <1.3% on wall-clock across 7
repeats each) confirms it.

## Results (measured, not projected)

| metric | VALSTACK (stack) | register (regvm) | delta |
|---|---|---|---|
| instructions/iter | 362.1 | 298.1 | **-17.7%** |
| cycles/iter | 55.39 | 51.77 | **-6.5%** |
| wall-clock/iter | ~14.95 ns | ~13.38 ns | **-10.5%** |
| branches/iter | 77.0 | 69.0 | -10.4% |
| IPC | 6.54 | 5.76 | (stack is HIGHER) |

The per-iteration instruction counts above were independently hand-traced
from the two handler chains in `chains.S` (20 instr/`OP_VAR_REF` x 8 +
~28-29 instr/arith-op x 6 + 23 instr/`OP_LT` + 9 instr/loop-control = 362
for `stack`; 15 x 8 + ~25-26 x 6 + 20 + 6 = 298 for `regvm`) and match
`perf`'s measured totals (72,423,393,909 / 200,000,000 = 362.12 and
59,620,891,558 / 200,000,000 = 298.10) to 2 decimal places -- strong
cross-check that nothing about the measurement is confused about what
it's counting.

**The headline finding**: eliminating 100% of this chain's VALSTACK
memory traffic (every intermediate push+pop, for all 15 real ops) cuts
instructions by 17.7% but cycles by only 6.5%. This is NOT a wash --
there is a real, measurable, repeatable win -- but it is far smaller than
the instruction-count story on its own would suggest. `stack`'s IPC is
actually *higher* than `regvm`'s (6.54 vs 5.76): the extra memory
instructions are themselves cheap on this core (store-to-load forwarding
on a predictable, tight VALSTACK access pattern, plus enough
out-of-order window to overlap much of the latency), so most of the
"extra work" `stack` does is nearly free in cycle terms. This directly
confirms (rather than assumes away) the parent task's hypothesis #4: a
modern OOO core hides most, not all, of the stack-memory round trip for
exactly this access pattern.

One honest caveat on the 6.5%/10.5% cycle/wall numbers specifically (NOT
on the 17.7% instruction number, which is a hard, repetition-independent
fact verified by hand-counting): this microbenchmark runs the *same*
chain 200 million times in a tight loop, so the CPU can overlap many
independent iterations' critical paths (only the loop counter and
checksum form a cross-iteration dependency). A single occurrence of this
chain shape embedded once in a large, less-repetitive real program has
less parallel "future work" available to hide behind, so the real
per-occurrence cycle cost of VALSTACK traffic in production code is
plausibly somewhat higher than 6.5% -- treat 6.5%/10.5% as a reasonable
lower bound on the true per-occurrence cost, not an exact production
number. It is very unlikely to approach 17.7%, though: same-address
store-to-load forwarding (the dominant hiding mechanism here) operates
within a single iteration's own dependency chain and does not require
cross-iteration overlap at all.

## How much of the real ~2.07x gap could this plausibly close?

Measured via `irdump` (the project's own IR-dump tool, built with `make
irdump`) against all 532 `eval-okay-*.nix` fixtures in the already-present
`/tmp/nix-conformance/lang` corpus (the same corpus `make
test-nix-conformance` uses) -- a STATIC op-kind histogram, 18,064 total
op records:

| class | ops | share |
|---|---|---|
| strictly chain-safe (`PUSH_*`/`ADD`/`SUB`/`MUL`/`DIV`/`EQ`/`NEQ`/`LT`/`GT`/`LE`/`GE`/`NOT`) | 3,601 | 19.9% |
| `VAR_REF` (ambiguous -- forced-fast-path when already evaluated, a real call/thunk-force boundary otherwise; can't tell statically) | 2,182 | 12.1% |
| everything else (`CALL`/`RET`/`MK_THUNK`/`SELECT`/`FRAME_*`/`LAMBDA`/`WITH*`/`MK_LIST`/`MK_ATTRS`/`JMP`/`IF`/...) | 12,281 | 68.0% |

Even the generous upper bound (count every `VAR_REF` as chain-safe, which
overstates it -- most `VAR_REF`s that matter dynamically in a real eval
*are* forcing something) puts the chain-safe share at 32.0%. This is a
STATIC, per-compiled-program count across small conformance snippets, not
a dynamic, execution-weighted count across a real nixpkgs eval -- the
caveat cuts AGAINST the technique, not for it: nixpkgs's own
attrset/overlay/function-composition idioms are, if anything, more
`CALL`/`MK_THUNK`/`SELECT`-heavy than these small fixtures (those four
ops alone are already 44.2% of this static corpus), so the real dynamic
chain-safe share in a full nixpkgs eval is unlikely to be higher than
this estimate.

Combining the two real measurements (not blindly extrapolating the toy
benchmark's own ratio, per the parent task's explicit instruction):

```
plausible overall cycle reduction
  <= chain-safe op share x this chain's own measured cycle reduction
  <= 0.32 x 6.5%
  ~= 2.1%
```

Against M123's own numbers, asnix needs roughly a 50%+ cycle reduction to
close the ~2.35x wall-clock / ~2.07x instruction gap to `iets` (IPC is
already close between the two: 1.76 vs 1.91). A ~2% cycle reduction, even
taking this POC's results at face value as a fully-realized, zero-
overhead best case, closes on the order of 3-4% of the real gap -- not a
rounding error, but not a lever that moves the needle either.

## Honest conclusion

This is a genuinely different, more fundamental finding than step 1's "no
free registers" blocker -- and arguably more informative. It isn't that
register-based evaluation is architecturally infeasible for this slice
(the POC proves it IS feasible, with a real if modest payoff); it's that
**the VALSTACK memory traffic itself was never the dominant real
bottleneck it looks like on paper**, for two independent, compounding
reasons measured directly here: (1) a modern out-of-order core already
hides most of a predictable stack's store/load round trips (verified: 6.5%
cycle reduction from removing 100% of this chain's memory traffic,
against a 17.7% instruction-count reduction), and (2) only a minority
(~20-32%, measured via a real IR-dump histogram, not guessed) of real op
dispatches are even candidates for this treatment in the first place,
since every `OP_CALL`/`OP_LET`/`OP_FRAME_*`/thunk-force site is
structurally excluded by construction (that's step 1's own finding,
holding regardless of this POC's result).

**Recommendation**: do not pursue a register-based VM (or the full-JIT
step 3) as the lever for closing the asnix-vs-iets gap. M123's own
un-landed findings point at a far larger, already-identified target:
lex/parse/compile is estimated ~8x slower per byte than `iets`'s own
pipeline and accounts for roughly HALF of the entire cycle gap on its
own (43.9% of all cycles in `asnix_eval_file`'s own subtree) -- an order
of magnitude bigger lever than anything available from the dispatch
loop's own memory-traffic pattern, and already flagged in
docs/architecture.md as the single biggest remaining lever worth a
dedicated milestone.
