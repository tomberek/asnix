# IrOp compaction proposal

Status: Proposal A has LANDED, as `docs/architecture.md`'s M169 section
(`IrOp` 24 -> 16 bytes), confirmed by independent verify at -0.537%
instructions on the real `d.nix` eval. Proposal B (replacing the handler
pointer with a small index, "lookup-table variant") has been
**implemented in full, measured, and DECLINED** -- see
`docs/architecture.md`'s M170 section for the complete writeup. The
"Real handler size data"/"Addressing schemes considered" sections below
are kept as originally written (the design-exploration analysis they
contain is still accurate background), but the "lookup-table variant is
the better starting candidate" conclusion they build toward did NOT
survive contact with real measurement -- see "M170: real measured
result" right below for why, before reading the rest of this document.
The rest of this document below was written before M169 landed; treat
"Proposal A... in flight" language as historical.

A later follow-up -- "does `b`'s real skew toward small/zero values open
a sub-16-byte `IrOp`?" -- was also investigated directly (a real
candidate was built in full this time, not just reasoned about: a packed
12-byte `IrOp` with the global-symbol-id-carrying ops' operand rerouted
through the pool) and **declined**, for the same real-measured-
instruction-count-regression reason M170 was -- see "Follow-up: does
`b`'s real skew... open a sub-16-byte `IrOp`?" below for the complete
writeup, including the real `perf stat`/RSS numbers.

## M170: real measured result (supersedes this document's own `+14,656,701`/`+1 instruction per dispatch` prediction below)

The lookup-table variant described below was built in full (every op
handler's dispatch tail, `dispatch.S`'s entry dispatch, and -- a
correctness subtlety this document didn't anticipate --
`force.S`/`apply.S`'s synthetic sentinel-return dispatch, which needed
two new reserved op-ids of its own; see `docs/architecture.md`'s M170
section for the full correctness story) and measured with 15 interleaved
`perf stat` trials on the real `d.nix`/ffmpeg eval, `taskset -c 0`,
alternating before/after:

```
PREDICTED (this document, below): +1 instruction/dispatch, +14,656,701 total, +0.237%
MEASURED  (M170, 15 interleaved trials): instructions +38,143,157.1 avg, +0.616%
                                         cycles       +15,570,327.5 avg, +0.446%
```

The measured regression is **~2.6x the predicted one**, and it is
consistent -- every one of the 15 interleaved instruction-count pairs
regressed (range +0.517% to +0.666%, no exceptions). Root cause: this
document's "+1 instruction per dispatch" arithmetic implicitly assumed a
single-instruction table load (`load index, load table[index]`), but
`asnix` builds as a PIE executable, and this codebase's existing asm
uses RIP-relative addressing for every global throughout -- there is no
`movq table(%rip,%rax,8), %rax` form in x86-64 (RIP-relative addressing
can't itself carry a scaled index), so the lookup actually costs **two**
instructions (`leaq asnix_handler_table(%rip), %rcx` to materialize the
table's base address, then `movq (%rcx,%rax,8), %rax`), not one. That
alone predicts roughly `2 x 14,656,701 = 29,313,402` (+0.473%) -- still
short of the measured `+38,143,157` (+0.616%), suggesting the real
per-dispatch volume on current nixpkgs is somewhat higher than the
`14,656,701` figure this document's own op-pair histogram instrumentation
found in an earlier session (plausible given nixpkgs itself grows over
time; not independently re-confirmed by re-running that specific
instrumentation in M170, since the direct `perf stat` measurement already
answered the ship/no-ship question on its own). A non-PIE build would
close most of the PIE-specific half of this gap, but changing the whole
binary's PIE-ness is a separate, independent decision with its own
security/ASLR tradeoffs, well outside a dispatch-mechanism change's scope.

**This resolves the open question "What needs to happen before any of
Proposal B ships" (below) raises**: step 3's conditional ("if real
measurement shows the lookup table's dependent load is NOT well-hidden
...") has been answered directly by measurement, not by the M127/M154
latency-hiding prior it hypothesized about -- the measured cost here is
dominated by raw instruction *count* (a real, retired, every-dispatch
`leaq`+`movq` pair), not by the dependent-load *latency* those priors
were about, so the "probably hidden by OoO execution" reasoning this
document used to prefer the lookup table over the bucketed scheme was
answering a question that turned out not to be the deciding one. Given
this, the bucketed scheme's own cost was never actually measured either
-- it remains exactly as speculative as before, and there's no
indication left in this document that it would actually do better once
someone sits down and builds and measures ITS real generated code too,
rather than reasoning about it in the abstract the way both schemes were
reasoned about before this milestone. Proposal B is DECLINED in its
lookup-table form; the codebase was left unchanged (M169's address-baking
`ir_link()` is still exactly what runs today) rather than carrying this
forward speculatively.

## Follow-up: does `b`'s real skew (mostly small/zero) open a sub-16-byte `IrOp`?

A later question, asked after M170's decline: nearly all `IrOp.b` values
*look* small or zero by inspection -- does that observation open some
angle M170 didn't consider, e.g. narrowing just `b`, or packing `a`+`b`
jointly? This was investigated in two passes within the same session: a
first pass (arithmetic + a real measured value-distribution survey, no
code built) that looked promising enough to justify a second pass
(an actual full implementation, built and measured with the real
perf-stat ladder). The second pass's real measurement is the one that
matters for the ship/no-ship call; the first pass's analysis is kept
below because the arithmetic it establishes is still correct and
relevant background, with one correction noted where the second pass
found it was reasoning about the wrong case.

### Pass 1: arithmetic + real distribution survey (no code built)

**A naturally-aligned `IrOp` cannot shrink below 16 bytes no matter how
narrow `a`/`b` get**, independent of what values they actually hold:
`IrOp` contains a pointer, so its *struct* alignment is 8 (the pointer's
own alignment), meaning `sizeof(IrOp)` must be a multiple of 8. With
`handler` occupying the first 8 bytes, the struct's total raw size is `8
+ X` where `X` is whatever `a`/`b` occupy combined. For *any* `X` with `0
< X <= 8` -- whether `X` is today's 8 (two `int32_t`), or 4 (one packed
`int32_t` holding two 16-bit halves), or even 1 (a single packed byte) --
`8 + X` lands in `(8, 16]`, which rounds up to exactly 16 either way.
**Correction from pass 2**: this arithmetic is only true for a
*naturally-aligned* layout (the compiler inserting no padding but still
rounding the whole struct up to its strongest member's alignment). An
EXPLICIT `__attribute__((packed))` layout sidesteps this entirely --
confirmed directly by compiling `{void *handler; int16_t a; int16_t b;}
__attribute__((packed))` and checking `sizeof()`: 12 bytes, not 16, because
`packed` suppresses the whole-struct alignment rounding, not just
inter-member padding. Pass 1 did not consider this (it reasoned only
about natural alignment); pass 2 did, and it is the one real way to get
below 16 bytes with a pointer-containing struct. The real constraint is
instead the one SoA reasoning below (pass 1) already found: whether `a`/
`b` can actually be narrowed at all.

**Real measured value distribution.** A temporary `ir_link()`-site
instrumentation (every op, every compiled program, including bytecode-
cache deserialization -- the same universal choke point M169's own
survey used) was built, run against the real `d.nix`/ffmpeg eval, and
reverted (same "measure, don't ship the probe" discipline M169's own
original operand-width survey used): 3,116,004 ops tallied (matching
M169's own "3.1 million" figure almost exactly -- a good cross-check that
this instrumentation and M169's counted the same thing). Per-opcode joint
`(a,b)` ranges, `OP_COUNT=57` enum order, selected rows:

| op | count | a range | b range | a fits u16 | both fit u12 |
|---|---|---|---|---|---|
| `OP_SELECT` | 316,410 | 0..77,229 (symbol id) | always 0 | 70.1% | 35.0% |
| `OP_HAS_ATTR` | 284 | 1..77,113 (symbol id) | always 0 | 96.8% | 69.7% |
| `OP_WITH_TRY` | 32,688 | always 32 (jump delta) | 0..58,021 (symbol id) | 100%(b) | 40.9% |
| `OP_SELECT_OR` | 1,136 | 32..96 (jump delta) | 1..76,860 (symbol id) | 95.2%(b) | 65.1% |
| `OP_HAS_ATTR_IF` | 2,083 | 48..1,184 (jump delta) | 1..77,229 (symbol id) | 95.0%(b) | 25.3% |
| everything else (pushes, frame ops, calls, plain jumps, pool indices, etc.) | ~2.76M | mostly 0, max seen 19,437 | mostly 0, max seen 77,229\* | 100% | 94.9%-100% |

\* `OP_MK_ATTRS`'s pool-index `b` tops out at 19,437 (the largest single
compiled program's own op count, same figure M169's survey found), not
a symbol id.

The non-symbol-carrying majority of ops (pushes, frame alloc/set/get,
calls, plain `if`/`jmp`, pool indices) genuinely are tiny -- every one of
them fits a plain `int16_t` with real margin (max observed 31,152 for a
jump byte-delta, 19,437 for a pool index/count, both comfortably under
`INT16_MAX`=32,767), confirming the original "`b` is mostly small or
zero" observation *for those op kinds*. But every op that carries a
**global symbol id directly as a plain immediate** (`OP_SELECT.a`,
`OP_HAS_ATTR.a`, `OP_WITH_TRY.b`, `OP_SELECT_OR.b`, `OP_HAS_ATTR_IF.b`)
tells a different story: the real eval interns at least 77,230 distinct
identifiers (attribute names, `let`/lambda binding names) across the full
nixpkgs/ffmpeg closure -- comfortably past any 16-bit ceiling, and the
single hottest, highest-volume symbol-carrying op in the entire table,
`OP_SELECT` (316,410 compiled occurrences -- about 1 in 10 of all ops in
this eval, and already one of the largest hand-inlined handlers per the
"Real handler size data" table above), has a symbol id exceeding 16 bits
in **almost 30% of its real occurrences**, and exceeding 12 bits in
**65% of them**.

This directly kills the pure-narrowing and 16-bit-common-case-plus-
escape-hatch candidates:

- **"Both `a` and `b` fit in ~12 bits, pack them into a smaller word"**:
  false for `OP_SELECT`'s `a` field specifically -- the single highest-
  volume symbol-bearing op only has *35%* joint-12-bit coverage, not the
  "nearly all" the premise assumed.
- **"16-bit common case + rare escape hatch for outliers"**: an escape
  hatch only pays off when escapes are rare (the same logic that made
  M169's `OP_PUSH_INT` pool-reroute a clean win: 64-bit literals are a
  genuine, tiny minority of all ops). Here the "outlier" case -- a symbol
  id over 12 or even 16 bits -- is 30-65% of `OP_SELECT`'s own real
  occurrences, not an edge case by compiled-op count.

But it leaves one real candidate standing, which pass 1 under-explored:
route *only* the 5 symbol-id-carrying op/field slots through the constant
pool (mirroring `OP_PUSH_INT`'s own M169 escape hatch exactly: `a`/`b`
becomes a pool index -- bounded by the compiled program's own size, same
~19,437 ceiling as every other pool index, safely inside `int16_t` --
with `pool[index]` holding the real, full-width symbol id), while an
EXPLICIT `__attribute__((packed))` layout (not naturally-aligned, see the
correction above) lets every OTHER op/field stay a plain `int16_t`
immediate. This looked, on paper, like a real candidate: the escape only
costs one extra dependent load on the 5 ops' own occurrences (352,601 of
the 3.1M total *compiled* ops, ~0.01%, by this pass's own count), far
cheaper than M170's declined every-dispatch lookup-table cost. Promising
enough on paper to build and measure for real -- see pass 2.

### Pass 2: built in full, measured, declined

Built the candidate pass 1 identified in full: `IrOp` as `{void *handler;
int16_t a; int16_t b;} __attribute__((packed))` (12 bytes, confirmed via
`sizeof()`), `OP_SELECT`/`OP_HAS_ATTR`/`OP_WITH_TRY`/`OP_SELECT_OR`/
`OP_HAS_ATTR_IF` rerouting their symbol id through the pool exactly as
described above, a loud-abort range check in `ib_emit`/`ib_patch_delta`
(irbuild.c) guarding every OTHER op's `a`/`b` against ever exceeding
`int16_t` (mirroring `OP_PUSH_INT`'s own `ASNIX_INT_MIN`/`MAX` precedent --
loud failure, not silent truncation, if a future program's operand ever
grows past this), every `src/asm/ops/*.S` handler's load width narrowed
from `movl`/`movslq` to `movswl`/`movswq` (mechanical, all values are
non-negative and fit 16 bits so sign- vs zero-extend are bit-identical
once extended), the 10 real symbol-read call sites across those 5 files
given the extra `pool[idx]` resolve load, `bytecode.c`'s wire format
updated (`PTAG_OP_SYM_A`/`PTAG_OP_SYM_B` now read/write a pool index, not
a bare id; magic bumped `ASNXBC03` -> `ASNXBC04`), `irdump.c`'s
disassembly updated to resolve the new pool indirection, and three
existing hand-built `IrOp[]` test fixtures (`tests/ir/test_backward_jump.c`,
`tests/asm/test_stacklimits.c`, `tests/c/test_bytecode.c`) fixed where
they either hardcoded a now-stale stride-dependent byte-delta constant or
needed >32,767 pool/operand entries in a single builder purely to drive
an unrelated runtime buffer (VALSTACK/RETSP) past its own, much larger
cap -- rebuilt those two specifically to reach the same runtime depth via
an actual loop/recursion construct instead of a one-pool-entry-per-level
unrolled builder, plus one new genuine `OP_SELECT` bytecode-cache round-
trip fixture (the riskiest untested path: a pool index that must be
re-pointed at a freshly re-interned symbol id on the read side).

**Full verification ladder, all green**: `make test`, `make test-e2e`
(300/300), `make test-conformance`, `make test-nix-conformance
NIX_CONFORMANCE_DIR=/tmp/nix-conformance` (`pass=655 fail=3 skip=148`,
the same 3 known pre-existing position-tracking gaps), `make
test-nixpkgs-hash` (`pass=148 fail=0`). `./asnix d.nix` byte-identical to
the expected store path throughout.

**Measured both axes this candidate's own risk profile demanded** --
15 interleaved `perf stat -e cpu_core/instructions/,cpu_core/cycles/`
trials (instructions/cycles) plus 3 interleaved `/usr/bin/time -v`
trials (peak RSS), `taskset -c 0`, alternating before (master, 16-byte
`IrOp`)/after (this candidate, 12-byte packed `IrOp`), never batched, on
the real `d.nix`/ffmpeg eval:

```
instructions: before avg 5,440,541,479.4   after avg 5,487,274,922.5   (+46,733,443.1, +0.859%)
cycles:       before avg 3,347,265,623.0   after avg 3,347,812,393.0   (+546,770.0, +0.016%)
peak RSS:     before 512,160 KB            after 503,712 KB            (-8,448 KB, -1.65%)
```

Instructions regressed **consistently, in every single one of the 15
interleaved pairs**, same one-directional signal M170's own measurement
had (no exceptions either way). Cycles are flat (within this project's
own documented noise band for this metric). Peak RSS is a real,
deterministic win (identical across all 3 trials), roughly a third of
the size of M169's own 24->16 shrink, as expected from a smaller (16->12,
25%) further reduction -- confirming the memory side of this candidate's
premise was correct.

**But the instruction-count regression is real, consistent, and -- by
this project's own standing practice of trusting instruction count over
cycles on this shared, contended machine -- bigger than the win is worth,
almost exactly reproducing M170's own outcome at a slightly larger
magnitude (+0.859% here vs M170's +0.616%).** Root cause is not fully
resolved to the instruction, but a real contributing factor was directly
measured rather than assumed: pass 1's `+0.01%` escape-hatch cost estimate
used each symbol-id op's STATIC compiled-op count, not its DYNAMIC
dispatch count -- precisely the distinction M170's own writeup flagged as
a risk ("the real measured dispatch volume... may be higher") and which
this pass 1 analysis repeated instead of heeding. A temporary dispatch
counter (reverted after this one measurement) found `OP_HAS_ATTR`,
`OP_SELECT_OR`, and `OP_HAS_ATTR_IF` all dispatch at RUNTIME far more
often than their static compiled-op count suggested (e.g. `OP_HAS_ATTR`:
284 compiled occurrences vs 156,956 real dispatches -- these three ops
evidently live inside commonly-reused `lib`-style helper functions called
across many packages, not used once each): 890,309 total dynamic
dispatches across the 5 rerouted ops, ~2.5x pass 1's static-count-based
352,601 estimate. That alone doesn't fully account for the measured
+46.7M-instruction gap, though -- the loud-abort range check added to
`ib_emit`/`ib_patch_delta` (irbuild.c's own new compile-time safety net,
called once per compiled op and once per patched jump respectively, a
combined ~3.2M calls on this eval) and the extra `ib_pool_add` call now
made for each of the 352,601 statically-compiled symbol-id sites are both
real, plausible contributors to the remainder, but this was not
partitioned further once the `perf stat` instruction-count delta alone
already answered the ship/no-ship question -- consistent with this
project's standing practice (M170: "the `perf stat` instruction-count
delta is the number that actually matters for the ship/no-ship call, and
it was measured directly, not inferred").

**Conclusion: declined.** Fully reverted: `git diff` against the
pre-change tree is empty for every file except this document (no source
files modified) -- `include/asnix/ir.h`, `src/c/irbuild.c`,
`src/c/bytecode.c`, `src/c/irdump.c`, all 29 touched `src/asm/ops/*.S`
files, and the three test-fixture files are all back to their pre-change
state. `IrOp` stays the 16-byte, no-padding layout M169 shipped. This
closes the "`b` looks mostly small" angle the same way M170 closed the
lookup-table angle: a real candidate was identified, built in full,
verified correct against the full ladder, and measured with real
interleaved trials rather than trusted on arithmetic or a static-count
estimate alone -- and the real measurement, not the paper estimate, is
what settled it. The memory win was real (-1.65%) but, exactly as this
project's own standing practice already decided once before (M170), a
consistent real instruction-count regression on this project's primary
trusted metric is not a trade this project is taking.

## Motivation

`IrOp` (`include/asnix/ir.h`) is a fixed 24-byte record:

```c
typedef struct {
    void *handler;  // offset 0, 8 bytes
    int64_t a;       // offset 8, 8 bytes
    int64_t b;       // offset 16, 8 bytes
} IrOp;
```

A fresh instrumentation pass (reverted, no code shipped) counted every
compiled op's `a`/`b` values across the real `d.nix`/ffmpeg eval: 3.1
million ops total (summed across every file compiled during the eval,
including nixpkgs imports), with the largest single compiled program at
19,439 ops. Finding: every field fits comfortably in 32 bits except one —
`OP_PUSH_INT.a`, which carries a Nix integer literal directly, and Nix
integers are real 64-bit values (a literal at exactly 2^32 and a negative
literal were both found in real nixpkgs source). Every other operand —
symbol IDs, lexical depths, frame slots, pool indices, jump byte-deltas —
is bounded by the size of the compiled program or symbol table itself,
not by anything user-supplied, and stays far below the 32-bit ceiling.

At 3.1M total ops, a `24 -> 16` byte shrink saves roughly **23.6 MB**
across the full real eval — a real number, comparable in scale to
milestones this project has already shipped and valued (M80's arena
shrink was 151MB -> 126MB).

**Confirmed by direct measurement after M169 landed**: peak RSS on the
real `d.nix` eval (`/usr/bin/time -v`, 3 interleaved trials each,
`taskset -c 0`) dropped from an average of 542,960 KB (M168, pre-shrink)
to 520,843 KB (M169, post-shrink) — a measured **-21.6 MB (-4.1%)**,
consistent in direction and close in magnitude to the hand-calculated
prediction above. This was not checked at the time M169 was merged (only
instruction count was measured); worth noting as a gap in that
milestone's own verification, closed here after the fact.

## Proposal A: narrow `a`/`b` to `int32_t`

Shrinks `IrOp` 24 -> 16 bytes. `OP_PUSH_INT` is the one exception: its
literal must route through the existing constant pool (the same
mechanism `OP_PUSH_CONST` already uses for string literals) instead of
being carried inline, since a 64-bit value cannot fit in 32 bits. This
needs a dedicated escape hatch, not a narrower encoding, for exactly the
one field that carries user data instead of internal bookkeeping.

This is a real, bounded, well-precedented change (M105 already narrowed
`ASNIX_ATTR_SIZE` 16 -> 12 bytes the same way), but it touches every op
handler in `src/asm/ops/*.S`, the bytecode disk-cache's on-disk format
(needs a version bump so a stale cache written by the old 24-byte layout
is never misread by the new 16-byte one), `irbuild.c`/`compiler.c`'s
`PUSH_INT` emission path, the existing constant-folding pass
(`irpass_cfold.c`, which reads/writes `OP_PUSH_INT.a` directly today and
would need to know it no longer carries the value inline), and `irdump.c`.

**Status: LANDED as M169** (see `docs/architecture.md`). Two independent
implementations were dispatched concurrently without coordination (a
known, already-flagged hazard this session hit before) — the first to
land and pass independent verify (-0.537% instructions, full ladder
green, including adversarial 2^31/2^32-boundary constant-folding cases
and a full bytecode-cache invalidation round-trip in both directions)
was merged. If the second implementation also lands, it is redundant
and should not be merged on top.

## Proposal B: replace the handler pointer with a small index

Separate from Proposal A. `OP_COUNT` is currently 57 — small enough that
an op identity fits in a single byte. The idea: instead of `ir_link()`
baking a full 8-byte handler *address* into every op, store a small
index, and arrange the compiled handler code so that index can be turned
into an address by cheap arithmetic alone (no memory load) at dispatch
time.

### Why a naive index-into-a-pointer-table doesn't work

This is literally the ops' PRE-`ir_link()` representation already
(`handler` starts out holding a small op-id, rewritten to a real address
by `ir_link()` — see `docs/ir-format.md`). `ir_link()` exists specifically
to eliminate the indirection a table lookup would cost: today's dispatch
tail is "advance IP, load handler (already the final address), `jmp`" —
one load. An index into `asnix_handler_table` would make it "advance IP,
load index, load `handler_table[index]`, `jmp`" — two loads, the second
dependent on the first, on the single hottest loop in the entire program
(measured at 14,656,701 dispatch steps on the real eval). This reads as
an obvious thing to avoid at first glance — but see the "review finding"
under "Addressing schemes considered" below, which quantifies it
directly and finds it is not clearly worse than the alternatives once
actually measured on the same basis.

### Real handler size data

Measured directly from the compiled binary (`nm -S --size-sort`) at
master `a37a413` (M168), cross-referenced against `asnix_handler_table`'s
57 entries:

| size range | count | examples |
|---|---|---|
| ≤ 32 bytes | 6 | `op_jmp` (12), `op_frame_exit` (18), `op_with_miss`/`op_assert_fail` (19), `op_with_exit` (22), `op_frame_set` (26) |
| 33–167 bytes | 45 | most ops; median is roughly 65–100 bytes |
| ≥ 296 bytes | 6 | `op_check_formals` (296), `op_var_ref` (416), `op_call` (451), `op_with_try` (494), `op_select_or` (532), `op_select` (545) |

Total natural compiled size across all 57 handlers: ~6,605 bytes. Range:
12 to 545 bytes — a **45x spread**, not a tight cluster. The six largest
handlers are disproportionately the ones that have received the most
hand-inlining work this session (M111/M154/M155/M162/M167 all landed
union-probe and Cell-chain logic directly into `select`/`select_or`/
`with_try`/`var_ref`) — meaning the size spread will keep *growing* as
more of this project's own optimization work lands, not stay fixed.

### Addressing schemes considered, and the arithmetic constraint that rules some out

Any scheme that computes a handler's address via pure arithmetic (no
load) requires the *slot size* to be a function of the index alone —
either one global constant, or selectable by a cheap compare/branch on
the index's own value (a small, fixed number of buckets). **A per-handler
variable slot count (each handler reserving exactly as many slots as its
own real size needs, tightly packed) does NOT support pure-arithmetic
addressing** — finding the N-th handler's start address requires the
cumulative sum of every prior handler's slot count, which is not a linear
function of N unless every handler's slot count is identical. That
requires a lookup table of cumulative offsets, which is exactly the
dependent-load cost this whole exercise is trying to avoid. (An earlier
pass through this design mistakenly treated tight variable-length packing
as compatible with zero-load addressing — it isn't; this is corrected
here rather than carried forward.)

So the real candidates, in order of how much padding waste they accept in
exchange for avoiding a lookup table entirely:

| scheme | total footprint | vs. natural (6,605 B) | extra per-dispatch cost |
|---|---|---|---|
| one global stride (pad every handler to 545 B) | 57 × 545 ≈ 31,065 B | 4.7x | none (pure arithmetic) |
| 3-tier bucketed stride (32 B / 176 B / double-jump for the 6 largest) | ≈ 10,942 B | 1.66x | a small, fixed number of compares/branches per dispatch (no load) |
| lookup table of cumulative offsets (true variable packing, ~8,448 B if slots were sized per-handler) | ≈ 8,448 B | 1.28x | +1 instruction/dispatch (+14,656,701 total) — see review finding below: this is at or below the bucketed scheme's own cost, not worse |

The bucketed scheme is the only one that is both space-efficient *and*
avoids a memory load. It is not free, though: every dispatch pays a
handful of extra retired instructions for bucket selection, multiplied by
the real measured dispatch volume on this eval (14,656,701 steps, from
the op-pair histogram instrumentation already run this session) — even
1-2 cheap compare/branch instructions per dispatch is 14.7-29.3 million
extra instructions, which is larger than several of this session's actual
shipped wins (M167: ~7.5M instructions; M164: ~8.3M instructions).

**Review finding: this makes the lookup-table option look better than
the main text of this proposal initially treated it, not worse.** The
earlier framing dismissed the lookup table on the grounds that it adds
"the exact cost this proposal exists to avoid" without quantifying that
cost on the same basis as the bucketed alternative. Doing that
arithmetic directly: today's dispatch tail is "advance IP, load handler,
`jmp`" (3 instructions). A small-index-plus-lookup-table tail is "advance
IP, load index, load `table[index]`, `jmp`" (4 instructions) — exactly
**+1 instruction per dispatch**, deterministically, or **+14,656,701
instructions total** on this eval. That is at or below the LOW end of
the bucketed scheme's own 14.7-29.3M estimate, while the lookup table
also has the smallest footprint of any scheme considered (8,448 bytes)
and sits well clear of any icache-capacity concern. The table itself
(57 entries, 4-8 bytes each — well under 500 bytes either way) is touched
on literally every dispatch, so it is about as close to "permanently
L1d-resident" as any data structure in this program could be; a cold-cache
miss on it is implausible after the first handful of dispatches.

The one real argument left for the bucketed scheme is latency, not
instruction count: a dependent load has different pipeline characteristics
than a compare-and-branch against a constant (a true data dependency vs.
none). But this project has already measured, twice, that short dependent
chains on this specific hardware are substantially hidden by out-of-order
execution rather than adding their full latency (M127's register-VM
investigation found store-to-load forwarding hid most of a chain's real
cost; M154's "slow it down" experiment found ~85% of a ~105-instruction
chain's injected cost was hidden). A single extra load feeding a `jmp`
is a much shorter chain than either of those experiments tested, so if
anything the prior is that it would be hidden, not exposed. Combined with
this campaign's own standing practice of trusting instruction count over
cycles (cycles are the noisy, untrustworthy signal on this shared
machine, repeatedly), **the simple small-index-plus-lookup-table scheme
is the better starting candidate of the two, not the bucketed one** — it
is simpler to build, has no bucket-boundary bookkeeping to keep in sync
as handler sizes drift, and is not clearly worse on the metric this
project actually trusts. This reverses this document's own earlier,
unquantified instinct mid-design (see the conversation this proposal was
drawn from) that the lookup table was something to design around rather
than a real candidate in its own right.

The one-global-stride option additionally has a separate, independent
problem: at ~31KB, it approaches a typical 32KB L1 instruction cache's
own capacity on its own, before counting any other code resident there
(libc routines, C fallback functions, mimalloc internals) — a real risk
of instruction-cache pressure on the hottest loop in the program. The
bucketed scheme (~11KB) is comfortably clear of that concern.

### Making variable-size handlers safe, not fragile

A legitimate worry with any scheme that reserves fixed slot budgets for
handlers is fragility: what if a future change grows a handler past its
reserved size? The answer is NOT to hand-pin byte offsets and hope the
compiler cooperates (that fails silently — a wrong jump target at
runtime, not a build error, and would be invisible until something
crashed in a confusing way). Instead: declare each handler's slot
allocation explicitly via assembler `.align`/`.space` directives sized
with headroom, and add a build-time check — a test that reads real
compiled sizes (`nm -S`/`objdump`) and asserts none exceeds its allocated
budget. Exceeding it becomes a loud build failure, not silent corruption.
This is a real, buildable safety net, not a hope.

### Dismissed sub-ideas (kept here for the record, not re-litigated)

- **Runtime code generation** (write trampolines/relocated handler bodies
  to `mmap`'d executable pages at startup): removes the static-layout
  fragility, but either (a) still costs an extra `jmp`-through-trampoline
  on every dispatch if real handler bodies stay in place, or (b) requires
  relocating compiled handler code into a dense array, which needs a real
  relocation story (handler code likely contains RIP-relative references
  to C fallback calls/data that are only valid at their original compiled
  address) — effectively building a mini dynamic loader. Not worth it
  unless the underlying fusion/compaction idea is independently proven
  valuable first.
- **Double-jump trampolines for oversized handlers** (small fixed-size
  stub that jumps to the real out-of-line body): avoids forcing all
  handlers to one size, but lands the extra jump specifically on
  `select`/`select_or`/`call`/`var_ref` — which are also among the
  hottest ops by real execution volume (multiple of these appear in the
  top-20 `(op, next_op)` dispatch pairs already measured this session).
  Adding guaranteed overhead to the highest-volume ops to save space
  elsewhere is very likely a net loss; superseded by the in-place
  multi-slot-with-build-check idea once the addressing constraint above
  is accounted for.
- **"Gap" opcode slots doubling as fusion entry points**: clever in
  theory (unused interior bytes of a large handler's reserved span could
  be addressable sub-entry-points), but the payoff is capped by the same
  economics as ordinary op-pair fusion, which has already been measured
  twice this session (a real execution-count histogram, and an
  actually-built `MK_THUNK`->`FRAME_SET` prototype) and found flat/no-win
  (+0.049%) on this real workload. A more fragile encoding mechanism
  doesn't change a payoff that's already been shown not to exist.

## What needs to happen before any of Proposal B ships

1. ~~Proposal A (`IrOp` 24 -> 16 bytes) needs to land and be measured for
   real, on its own, first.~~ **Done: landed as M169, -0.537% instructions,
   independently confirmed.** It showed a measurable win, so step 2 is
   now actionable.
2. Measure real L1-dcache-miss / L1-icache-miss / iTLB-miss
   counts (`perf stat` with those specific events, not just raw
   instruction count) for the *current* 24-byte and proposed 16-byte
   layouts, to establish whether the claimed memory-footprint benefit is
   actually cache-miss-bound on this workload at all, rather than
   assuming it.
3. Only then would a dedicated follow-up investigation into Proposal B be
   justified — building it speculatively on top of an unmeasured
   foundation is not recommended. If it is pursued, start with the
   simple small-index-plus-lookup-table scheme, not the bucketed one —
   per the review finding above, it is simpler to build, has the
   smallest footprint of the options considered, and is not clearly
   worse on instruction count, this project's own trusted metric. Treat
   the bucketed scheme as a fallback only if real measurement shows the
   lookup table's dependent load is NOT well-hidden on this hardware for
   this specific access pattern (contrary to the M127/M154 prior).

## Open risks

- **Redundant concurrent work.** Two independent implementations of
  Proposal A are in flight right now, dispatched by different sessions
  without coordination. This session has already hit a real collision
  once from exactly this pattern (two forks using the same shared
  integration-clone path simultaneously, one overwriting the other's
  in-progress merge) — when both land, compare and keep the
  better-verified one rather than attempting to merge both.
- **Constant-folding interaction.** Proposal A's `PUSH_INT` pool-routing
  change must stay correct against the existing `irpass_cfold.c` constant
  folding pass (M136/M139), which currently assumes `OP_PUSH_INT.a`
  carries the literal directly.
- **Bytecode cache versioning.** Any `IrOp` layout change invalidates the
  on-disk bytecode cache's wire format. A stale cache from the old layout
  must be detected and rejected, never silently misread.
