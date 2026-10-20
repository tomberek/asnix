# asnix M9 benchmarks

Micro-benchmarks comparing asnix against real Nix (`nix-instantiate --eval`)
on the narrow language subset asnix currently supports. **This is not a
nixpkgs-scale or "real world" comparison** — see Caveats below before
reading too much into the numbers.

## Running

```
bench/run              # hyperfine if on PATH, else a shell timing-loop fallback
bench/run --iters 50   # shell-loop fallback repeat count (ignored under hyperfine)
```

Writes a timestamped `bench/results/<UTC-stamp>.md` (+ `.hyperfine/` JSON/txt
detail if hyperfine ran) with a provenance header: git commit + dirty state,
`uname -a`, nproc, `nix-instantiate --version`.

Regenerate the workload corpus (only needed after editing sizes/shapes in
`gen_workloads.sh`):

```
bench/gen_workloads.sh
```

## Workloads (`bench/workloads/*.nix`)

Every workload uses *only* syntax asnix's parser currently accepts — no
`rec`, `with`, `//`, comparisons, booleans, string interpolation, or
self/mutual recursion (see `docs/architecture.md`'s M4/M5 scope notes).

| workload | exercises |
|---|---|
| `baseline` | a single literal — isolates fixed process-startup cost from everything below |
| `arith_deep` | long `+`/`*` chain, no env/frame cost |
| `let_chain` | N nested single-binding `let`s, each referencing the last — env-frame-chain allocation + depth-N lookup walks |
| `lambda_chain` | N sequential curried-lambda calls — call/return + closure capture |
| `list_build_{small,medium,large}` | list literal construction at 3 sizes |
| `attrset_build_{small,medium,large}` | non-recursive attrset literal construction at 3 sizes |
| `select_chain` | one N-field attrset summed via N sequential `.attr` selects |

## Methodology

Each workload runs under both `./asnix <file>` and `nix-instantiate --eval
<file>`, `--warmup 3 --min-runs 15` under hyperfine (shell timing loop at
`--iters` repetitions otherwise). `make release` (`-O2`) builds asnix before
timing — the default dev build (`-O0 -g`) is for inner-loop debugging, not
for anything you'd want to quote a number from.

Every workload's result is independently verified to match `nix-instantiate
--eval` byte-for-byte before being trusted as a benchmark (not just fast,
*correct*) — same discipline as every other milestone in this project.

## Caveats — read before quoting these numbers

- **`nix-instantiate`'s number is dominated by process startup** (loading
  its own evaluator, store connection setup, etc.), not by evaluating these
  tiny expressions. The `baseline` workload (evaluate the literal `1`) makes
  this explicit: asnix and `nix-instantiate` both "evaluate" it instantly,
  but `nix-instantiate`'s wall-clock time is essentially unchanged from the
  more complex workloads. These numbers measure *"time to get an answer from
  a cold process invocation,"* not *"evaluator throughput on hot code"* —
  asnix wins by a wide, consistent margin here largely because it has no
  comparable fixed startup cost (no store connection, no `nix.conf`
  parsing, no plugin loading), not necessarily because its *per-node
  evaluation* is proportionally faster. A fairer per-node-cost comparison
  would need either a long-running daemon-mode asnix or an in-process
  `nix-instantiate` harness — neither exists yet.
- **The workloads are tiny and synthetic**, scoped to exactly what asnix's
  parser currently accepts. They say nothing about how asnix would perform
  on a real nixpkgs expression (`rec` attrsets, `with`, string
  interpolation, `import`, and most of `lib` aren't implemented yet — see
  `docs/architecture.md`'s Status table).
- Treat this as a sanity check on the M3/M4.5 "dispatch-elimination" thesis
  and a smoke test that nothing regressed perf-wise, not as a competitive
  benchmark against cppnix/Lix/iets/fix.
