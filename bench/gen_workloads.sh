#!/usr/bin/env bash
# Generates bench/workloads/*.nix from scratch. Not run automatically by
# `bench/run` -- the generated files are checked in (they're the actual
# benchmark corpus); this script exists purely so the exact construction of
# each workload is reproducible/auditable rather than hand-typed-and-trusted,
# per M9's "correctness corpus" goal. Re-run after editing SIZES below to
# regenerate.
#
# Every workload here uses *only* syntax asnix's parser currently accepts
# (see docs/architecture.md's M4/M5 scope notes and src/c/parser.c):
# int/bool/null literals, `+ - *`, `if/then/else`, `let ... in` (single- and
# multi-binding), single-param curried lambdas, application by juxtaposition,
# list literals, non-recursive attrset literals, `.attr` selection. No
# comparisons, `&&`/`||`, `with`/`rec`/`inherit`, `//`, string interpolation,
# or recursion (`let fact = n: ... fact (n-1) ...` doesn't see itself here --
# M6 scope notes) -- so "repetition" workloads below are flat chains, not
# recursive functions.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p workloads

# ---- arith_deep: one long left-associated +/* chain, no variables ----
# Exercises raw arithmetic-op dispatch depth with zero env/frame cost.
gen_arith_deep() {
    n=$1
    out="1"
    for ((i = 2; i <= n; i++)); do
        if ((i % 2 == 0)); then out="$out * $i"; else out="$out + $i"; fi
    done
    printf '%s\n' "$out" > workloads/arith_deep.nix
}

# ---- lambda_chain: N sequential curried-lambda calls, nested arg-first ----
# No `rec`, so this is the honest substitute for "recursion" the task
# description calls for: `inc` applied N times via N nested one-arg calls,
# not a self-referential function.
gen_lambda_chain() {
    n=$1
    expr="0"
    for ((i = 1; i <= n; i++)); do
        expr="inc ($expr)"
    done
    printf 'let inc = x: x + 1; in %s\n' "$expr" > workloads/lambda_chain.nix
}

# ---- let_chain: N nested single-binding lets, each referencing the last ----
# Exercises env-frame-chain allocation and depth-N variable-lookup walks
# (OP_VAR_REF walks `up` pointers `depth` times) without any function calls.
gen_let_chain() {
    n=$1
    {
        printf 'let x0 = 0;\n'
        for ((i = 1; i <= n; i++)); do
            printf '    x%d = x%d + 1;\n' "$i" "$((i - 1))"
        done
        printf 'in x%d\n' "$n"
    } > workloads/let_chain.nix
}

# ---- select_chain: one N-field attrset, summed via N sequential selects ----
# Exercises OP_SELECT's binary search N times over an N-entry table (built
# once), isolated from attrset *construction* cost as much as this subset
# allows (construction is unavoidable -- there's no way to select without
# first building something to select from).
gen_select_chain() {
    n=$1
    {
        printf 'let s = {\n'
        for ((i = 0; i < n; i++)); do
            printf '  a%d = %d;\n' "$i" "$i"
        done
        printf '}; in\n'
        for ((i = 0; i < n; i++)); do
            if ((i > 0)); then printf ' + '; fi
            printf 's.a%d' "$i"
        done
        printf '\n'
    } > workloads/select_chain.nix
}

# ---- list_build_{size}: list literal of N ints, wrapped in builtins.length ----
# `length` only reads the List header's count field (src/c/builtins.c), it
# doesn't force/print each element -- so this isolates list *construction*
# cost from O(N) stdout formatting, which would otherwise dominate at the
# larger sizes and swamp the construction-cost signal being measured.
gen_list_build() {
    size=$1
    name=$2
    elems=""
    for ((i = 0; i < size; i++)); do
        if ((i > 0)); then elems="$elems "; fi
        elems="$elems$i"
    done
    printf 'builtins.length [ %s ]\n' "$elems" > "workloads/list_build_${name}.nix"
}

# ---- attrset_build_{size}: N-field attrset literal, wrapped+selected ----
# Attrset *values* are lazily thunked (M6) but the N-entry table itself
# (OP_MK_ATTRS) is built eagerly regardless of what's read afterward -- so
# selecting just one field still pays full construction cost while printing
# only a scalar (avoids O(N) print cost the same way list_build does; there's
# no attrset analogue of `builtins.length` in this builtin set to use
# instead).
gen_attrset_build() {
    size=$1
    name=$2
    {
        printf 'let s = {\n'
        for ((i = 0; i < size; i++)); do
            printf '  a%d = %d;\n' "$i" "$i"
        done
        printf '}; in s.a0\n'
    } > "workloads/attrset_build_${name}.nix"
}

# let_chain's depth is capped at 100, well under 128, NOT as a stylistic
# choice: deeper was tried first (2000) and silently produced the wrong
# answer (17592168531160 instead of 2000) rather than erroring. Root cause,
# found while building this harness: forcing x_N's thunk cascades into
# forcing x_{N-1}, x_{N-2}, ... down to the literal base case (M6's "forcing
# enters a thunk exactly like a zero-arg call, pushing a retframe" design),
# so a chain of N *dependent* let bindings keeps N retframes open on RETSP
# simultaneously -- unlike lambda_chain/select_chain below, whose calls/
# forces resolve one at a time and never stack up. RETSP is a fixed
# ASNIX_RETSTACK_BYTES=4096 = 128 32-byte frames (src/asm/dispatch.S) with no
# overflow check: depth 127 is correct, depth 128 segfaults, depth 200+
# silently corrupts and returns a wrong-but-plausible-looking integer. This
# is a real evaluator limitation, not a benchmark-harness bug -- out of
# scope to fix here (see the task's "don't touch src/asm" constraint); flagged
# in bench/README.md and docs/architecture.md instead of worked around by
# editing core files.
gen_arith_deep 3000
gen_lambda_chain 2000
gen_let_chain 100
gen_select_chain 300

gen_list_build 50 small
gen_list_build 500 medium
gen_list_build 5000 large

# attrset_build's "large" size is capped at 2000, NOT matching list_build's
# 5000, for the same reason let_chain got capped above: 5000 was tried first
# and segfaulted. Root cause, found while building this harness: unlike list
# literals (whose elements compile straight to value pushes, confirmed safe
# up to at least 10000 elements), each attrset binding compiles to a
# separate freshly-malloc'd IR sub-program wrapped in OP_MK_THUNK (M6's
# laziness design -- compiler.c's AST_ATTRSET case), and that path breaks
# cleanly in half at exactly 4096 bindings: 4096 evaluates correctly, 4097
# segfaults, every size tried above that (up to 5000) silently returns a
# plausible-looking wrong answer instead of crashing or erroring. 4096 is
# suspicious (it's exactly ASNIX_VALSTACK_BYTES/8 from src/asm/dispatch.S),
# but list literals share the same "N pushes, then one MK_* op" compile
# shape and don't hit it even at 10000 -- so the exact mechanism wasn't
# fully isolated; capping well under the measured boundary (2000, 2x
# margin) was the correct scope here per the task's "redesign the workload,
# don't patch src/asm or src/c" constraint. Flagged in bench/README.md and
# docs/architecture.md as a real limitation worth a follow-up investigation.
gen_attrset_build 50 small
gen_attrset_build 500 medium
gen_attrset_build 2000 large

echo "generated $(ls workloads/*.nix | wc -l) workload files in bench/workloads/"
