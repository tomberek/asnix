# IrOp compaction proposal

Status: Proposal A has LANDED, as `docs/architecture.md`'s M169 section
(`IrOp` 24 -> 16 bytes), confirmed by independent verify at -0.537%
instructions on the real `d.nix` eval. Proposal B (replacing the handler
pointer with a small index) is still a design exploration only, not a
milestone, and nothing about it should be treated as decided until the
measurements it calls for actually happen. The rest of this document
below was written before M169 landed; treat "Proposal A... in flight"
language as historical.

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
