#include "builtins_shared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "attrs_update.h" /* M88: asnix_attrs_eager_merge, for asnix_bshared_attrs_resolve */
#include "symtab.h"
#include "value_util.h"

Value asnix_bshared_make_primop(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    /* ASNIX_PRIMOP_OFF_NOFORCE left at its fresh-arena zero (force args,
     * the default for every primop except tryEval -- see that field's
     * own comment, heap.h). */
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

Value asnix_bshared_make_primop_noforce(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_NOFORCE) = 1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

/* M80: the ONE canonical marker Value used by every native-builtin-
 * currying helper in this codebase -- see builtins_shared.h's own long
 * comment on the declaration for the full rationale. Lazily built on
 * first use (happens very early, during the one-time builtin-table
 * registration pass that runs before any Nix source is even parsed, let
 * alone compiled) and memoized in a function-local static, the same
 * "compute once, cache in a static" shape this codebase already uses
 * elsewhere (e.g. builtins_attrs.c's prim_hasAttr/prim_getAttr `static
 * IrProgram *inner`). */
Value asnix_bshared_native_formals_marker(void) {
    static Value marker = { .raw = 0 };
    if (marker.raw == 0) marker = asnix_bshared_make_attrs(NULL, NULL, 0);
    return marker;
}

/* Ported straight from builtins_misc.c's make_curried_native -- see that
 * file's long header comment for the full rationale (equivalent to
 * compiling the fictitious Nix source `a1: a2: ... aN: __native a1 a2 ... aN`
 * via the same IrBuilder API the real compiler uses for ordinary curried
 * lambdas). */
Value asnix_bshared_make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = asnix_bshared_make_primop(native_fn, arity);
    /* Never a lambda-pattern -- `builtins.functionArgs` on any of these
     * closures is `{}`, same as real Nix's `functionArgs builtins.add`
     * (M37). M80: uses the shared canonical marker (see above) instead of
     * a fresh `asnix_bshared_make_attrs` call so the printer can
     * recognize these as native-builtin curry stages (still a completely
     * valid, correct `{ }` for `functionArgs` itself). */
    Value empty_formals = asnix_bshared_native_formals_marker();

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog; /* M100: one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

/* M92: see builtins_shared.h's own long comment -- identical to
 * asnix_bshared_make_curried_native just above, except the reassembly
 * loop uses ib_var_ref_raw (no forcing at all) instead of ib_var_ref,
 * and the leaf primop is built NOFORCE (asnix_bshared_make_primop_
 * noforce) instead of force-by-default. */
Value asnix_bshared_make_curried_native_noforce(void *native_fn, int64_t arity) {
    Value native_primop = asnix_bshared_make_primop_noforce(native_fn, arity);
    Value empty_formals = asnix_bshared_native_formals_marker();

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref_raw(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog; /* M100: one pointer, not {ops,pool} */
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

Value asnix_bshared_apply2(Value fn, Value a, Value b) {
    Value step1 = asnix_apply(fn, &a, 1);
    return asnix_apply(step1, &b, 1);
}

void asnix_bshared_fatal(const char *who, const char *msg) {
    fprintf(stderr, "asnix: %s: %s\n", who, msg);
    exit(1);
}

/* OP_SELECT's (op_select.S) static-attribute-missing fatal path -- names
 * the attribute, matching real Nix's own "attribute 'x' missing" wording
 * (confirmed via `nix-instantiate --eval '{ a = 1; }.b'`). Previously a
 * single generic "attribute missing, aborting" line with no indication of
 * *which* attribute or selection site -- found to cost real debugging time
 * (every "attribute missing" abort chasing real nixpkgs needed a one-off
 * gdb session reading the symbol id out of the live IP register to find
 * out). Only ever called immediately before `exit(1)`, so the extra
 * `asnix_sym_name` call/format here costs nothing on the hot (successful
 * select) path -- op_select.S's own `.Lselect_found`/`.Lselect_push`
 * never reach this function at all. */
_Noreturn void asnix_bshared_select_missing_fatal(uint32_t sym) {
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    fprintf(stderr, "asnix: attribute '%.*s' missing, aborting\n", (int)len, name);
    exit(1);
}

/* M50: the "cheap pass" extension of M36's idea -- name the actual VALUE
 * (its type/kind) at every other fatal path that previously said nothing
 * more specific than "is not an attrset"/"is not a function". Reuses the
 * exact same tag -> name mapping builtins.c's `prim_typeOf` already
 * established (int/bool/null/string/path/list/set/lambda), just phrased
 * as a full "a/an X" clause instead of typeOf's bare noun, since every
 * call site below wants to say "...(got X), aborting". Deliberately does
 * NOT force `v` first (unlike typeOf, which relies on OP_CALL having
 * already forced every primop argument, M25): every call site below reads
 * a value already sitting on VALSTACK or passed in from an asm op's own
 * C glue, which by this evaluator's standing invariant (op_call.S's own
 * M23 comment: every value reaching VALSTACK is already forced) is never
 * an unforced Thunk in practice -- the ASNIX_TAG_THUNK case is dead code,
 * kept only so a future violation of that invariant prints something sane
 * instead of misreading raw thunk bits. Declared non-static (in
 * builtins_shared.h) because attrs_update.c's asnix_attrs_insert_dynamic
 * needs it too, not just this file's own dynamic-select fatals below. */
const char *asnix_bshared_type_name(Value v) {
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: return "an integer";
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE:
                case ASNIX_IMM_FALSE: return "a boolean";
                case ASNIX_IMM_NULL: return "null";
                default: return "an immediate value";
            }
        case ASNIX_TAG_PTR:
            return asnix_value_is_path(v) ? "a path" : asnix_value_is_float(v) ? "a float" : "a string";
        case ASNIX_TAG_LIST: return "a list";
        case ASNIX_TAG_ATTRS: return "an attrset";
        case ASNIX_TAG_CLOSURE:
        case ASNIX_TAG_PRIMOP: return "a function";
        case ASNIX_TAG_THUNK: return "an unforced thunk"; /* see comment above: dead in practice */
        default: return "a value of unknown type";
    }
}

/* OP_SELECT's (op_select.S) type-error fatal path -- names the actual
 * type of the non-attrset select target, same motivation as
 * asnix_bshared_select_missing_fatal above (M36) applied to the sibling
 * fatal one op up: "asnix: '.' selection target is not an attrset,
 * aborting" gave no indication of what the value actually WAS, forcing
 * the same kind of one-off gdb session M36's own commit message
 * complains about. `v` arrives still tagged (the raw Value popped off
 * VALSTACK, before op_select.S's own `andq $-8` untagging) -- exactly
 * what asnix_bshared_type_name above wants. */
_Noreturn void asnix_bshared_select_type_fatal(Value v) {
    fprintf(stderr, "asnix: '.' selection target is not an attrset (got %s), aborting\n",
            asnix_bshared_type_name(v));
    exit(1);
}

/* OP_CALL's (op_call.S) "call target is not a function" fatal path --
 * same idea, one op over: names the actual type of whatever was called.
 * `v` arrives still tagged (read out of the callee's VALSTACK slot
 * before op_call.S's own closure/primop dispatch, which is exactly the
 * point this path is reached from -- neither tag matched). */
_Noreturn void asnix_bshared_call_type_fatal(Value v) {
    fprintf(stderr, "asnix: call target is not a function (got %s), aborting\n",
            asnix_bshared_type_name(v));
    exit(1);
}

/* `__functor` protocol (M42): calling an attrset `s` with argument `arg`
 * (`s arg`) is sugar for `s.__functor s arg` when `s` has a `__functor`
 * attribute -- real Nix's exact mechanism, confirmed in
 * `~/nix/src/libexpr/eval.cc`'s `callFunction`: `callFunction(functor,
 * {self_copy, args[0]}, ...)`, i.e. `__functor` is called with the
 * attrset itself as its first argument, then the real argument second
 * (a curried 2-arg function in Nix terms, which this project already
 * represents as two nested arity-1 Closures, so this is just two
 * ordinary `asnix_apply` calls chained). Reached from both `OP_CALL`
 * (op_call.S) and `asnix_apply` (apply.S) ONLY after their own Closure/
 * PrimOp dispatch has already failed to match -- so this never costs
 * anything on the common (plain function call) path. Does not special-
 * case a `__functor` that itself returns another attrset-with-
 * `__functor`; `asnix_apply`'s own ATTRS branch (which this function's
 * second `asnix_apply` call re-enters) already handles that
 * recursively for free, matching real Nix's own `while` loop over
 * `args` in `callFunction`, which re-checks `vCur`'s type after every
 * step. Fatal (same "not a function" message as a plain attrset) if
 * `attrs_val` has no `__functor` attribute at all -- confirmed against
 * real Nix: `{ a = 1; } 5` and `{ __functor = ...; }`-less attrsets
 * both get the identical "attempt to call something which is not a
 * function" error. */
Value asnix_bshared_call_functor(Value attrs_val, Value arg) {
    uint32_t sym = asnix_intern("__functor", 9);
    int64_t idx = asnix_bshared_attrs_find(attrs_val, sym);
    if (idx < 0) asnix_bshared_call_type_fatal(attrs_val);
    Value functor_fn = asnix_force_for_print(asnix_bshared_attrs_value(attrs_val, idx));
    Value step1 = asnix_apply(functor_fn, &attrs_val, 1);
    return asnix_apply(step1, &arg, 1);
}

/* OP_CHECK_FORMALS's (op_check_formals.S) two lambda-pattern-mismatch
 * fatal paths -- same idea as the attribute-missing case above (M36),
 * applied to `{a, b, ...}: ...`'s own argument-validation errors: neither
 * "lambda called without a required argument" nor "lambda called with an
 * unexpected argument" said *which* field, which is exactly the
 * information a human needs to fix the call site. `sym` arrives already
 * resolved out of the IR constant pool (missing case, op_check_formals.S's
 * %r15) or out of the given attrset's own entries (extra case, %r11) --
 * both already-interned symbol ids, same asnix_sym_name lookup M36 used. */
_Noreturn void asnix_bshared_cf_missing_fatal(uint32_t sym) {
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    fprintf(stderr, "asnix: lambda called without a required argument '%.*s', aborting\n", (int)len, name);
    exit(1);
}

_Noreturn void asnix_bshared_cf_extra_fatal(uint32_t sym) {
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    fprintf(stderr, "asnix: lambda called with an unexpected argument '%.*s', aborting\n", (int)len, name);
    exit(1);
}

void asnix_bshared_require_tag(Value v, int tag, const char *who, const char *what) {
    if (asnix_tag_of(v) != tag) {
        fprintf(stderr, "asnix: %s: argument is not %s\n", who, what);
        exit(1);
    }
}

int64_t asnix_bshared_list_count(Value v) {
    return *(const int64_t *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_LIST_OFF_COUNT);
}

const Value *asnix_bshared_list_elems(Value v) {
    return (const Value *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_LIST_OFF_ELEMS);
}

Value asnix_bshared_make_list(const Value *elems, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * sizeof(Value));
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = count;
    if (count > 0) memcpy((char *)mem + ASNIX_LIST_OFF_ELEMS, elems, (size_t)count * sizeof(Value));
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

/* M88: see builtins_shared.h's comment and heap.h's UnionHdr layout. */
Value asnix_bshared_attrs_resolve(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
    if (count >= 0) return v; /* already flat -- the common case */

    Value cached = *(const Value *)(base + ASNIX_ATTRS_UNION_OFF_FLATCACHE);
    if (cached.raw != 0) return cached; /* materialized by an earlier call */

    int64_t nleaves = -count;
    const Value *leaves = (const Value *)(base + ASNIX_ATTRS_UNION_OFF_LEAVES);
    /* Leaves are guaranteed flat by construction (asnix_attrs_update,
     * attrs_update.c, always resolves a would-be leaf before storing it).
     *
     * IMPORTANT: this is a bottom-up PAIRWISE (binary-tree) merge, NOT a
     * left-to-right fold (`acc = merge(acc, leaf)` repeated) -- a fold
     * would re-copy the ALREADY-MERGED, ever-growing accumulator on every
     * one of the (up to ASNIX_ATTRS_UNION_MAX_LEAVES-1) merge steps,
     * costing O(total_size * nleaves) instead of O(total_size *
     * log2(nleaves)). That was an actual bug found measuring this
     * milestone (see docs/architecture.md's M88 section): with the fold,
     * repeated materialization cost as much total copying as the eager
     * `//` path it was meant to avoid, measured via peak RSS on a
     * synthetic many-small-leaves benchmark (nearly identical memory to
     * the eager baseline). The pairwise merge below halves the work list
     * each round (same shape as mergesort's merge phase), preserving
     * `//`'s right-biased collision semantics: each pairwise
     * asnix_attrs_eager_merge(work[i], work[i+1]) keeps `work[i+1]` (the
     * more-recently-layered, higher-priority side, since `leaves` is
     * stored lowest-priority-first) as the winning operand, and pairing
     * adjacent elements bottom-up preserves that same relative priority
     * order all the way up the tree. */
    Value work[ASNIX_ATTRS_UNION_MAX_LEAVES];
    for (int64_t i = 0; i < nleaves; i++) work[i] = leaves[i];
    int64_t n = nleaves;
    while (n > 1) {
        int64_t out_n = 0;
        int64_t i = 0;
        for (; i + 1 < n; i += 2) {
            work[out_n++] = asnix_attrs_eager_merge(work[i], work[i + 1]);
        }
        if (i < n) work[out_n++] = work[i]; /* odd one out, carried up unmerged */
        n = out_n;
    }
    Value acc = work[0];
    *(Value *)((char *)base + ASNIX_ATTRS_UNION_OFF_FLATCACHE) = acc;
    return acc;
}

int64_t asnix_bshared_attrs_count(Value v) {
    v = asnix_bshared_attrs_resolve(v);
    return *(const int64_t *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_COUNT);
}

uint32_t asnix_bshared_attrs_sym(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
}

Value asnix_bshared_attrs_value(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
}

int64_t asnix_bshared_attrs_find(Value attrs, uint32_t sym) {
    attrs = asnix_bshared_attrs_resolve(attrs);
    int64_t lo = 0, hi = asnix_bshared_attrs_count(attrs);
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t s = asnix_bshared_attrs_sym(attrs, mid);
        if (s == sym) return mid;
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    return -1;
}

typedef struct {
    uint32_t sym;
    int64_t idx;
} MakeAttrsEntry;

/* Tie-break by original index, not just `sym` -- matches the insertion
 * sort this replaced (which stopped swapping once `syms[j-1] <=
 * syms[j]`, so equal-sym runs kept their original relative order). Some
 * callers rely on `asnix_bshared_make_attrs` NOT deduping duplicate
 * syms at all (e.g. a caller that already dedupes before calling this
 * passes a genuinely unique set, but nothing here enforces that), so
 * preserving the old stable-sort behavior exactly, rather than letting
 * qsort's unspecified tie order change which of two equal-sym entries
 * ends up first, avoids silently changing behavior for any caller that
 * does pass duplicates. */
static int cmp_make_attrs_entry(const void *a, const void *b) {
    const MakeAttrsEntry *ea = a, *eb = b;
    if (ea->sym != eb->sym) return ea->sym < eb->sym ? -1 : 1;
    if (ea->idx != eb->idx) return ea->idx < eb->idx ? -1 : 1;
    return 0;
}

/* M89 perf: was an insertion sort (O(n^2) worst case) -- measured on a
 * full nixpkgs evaluation (`./asnix d.nix`, ffmpeg.outPath): 52,539
 * calls, up to 5,234 entries in a single call, ~699,000 total swaps.
 * Switched to qsort over (sym, original-index) pairs, same "resolve
 * once into a sortable key, sort that" approach M88 already used for
 * `sorted_name_order` -- see docs/architecture.md's M88/M89 notes. */
Value asnix_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count) {
    MakeAttrsEntry *entries = count > 0 ? malloc((size_t)count * sizeof(MakeAttrsEntry)) : NULL;
    for (int64_t i = 0; i < count; i++) {
        entries[i].sym = syms[i];
        entries[i].idx = i;
    }
    if (count > 1) qsort(entries, (size_t)count, sizeof(MakeAttrsEntry), cmp_make_attrs_entry);

    void *mem = asnix_arena_alloc(8 + (uint64_t)count * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = count;
    for (int64_t i = 0; i < count; i++) {
        char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        int64_t src = entries[i].idx;
        /* M103: must be a 4-byte write -- entry's value bytes now start right after sym (no pad),
         * so an 8-byte write here would clobber them. */
        *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = entries[i].sym;
        *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[src];
    }
    free(entries);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

Value asnix_bshared_rec_overrides_merge(Env *env, int64_t n, const int64_t *meta) {
    int64_t overrides_slot = meta[n];
    Value overrides_val = asnix_force_for_print(env->values[overrides_slot]);
    if (asnix_tag_of(overrides_val) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: '__overrides' must be a set (got %s)\n",
                asnix_bshared_type_name(overrides_val));
        exit(1);
    }

    /* Working (sym, value) pairs: the n static bindings first (by Env
     * slot order, matched ones replaced below), then any override-only
     * extras appended -- asnix_bshared_make_attrs sorts the whole lot by
     * sym at the end, so this function never needs to track order itself. */
    int64_t ocount = asnix_bshared_attrs_count(overrides_val);
    int64_t cap = n + ocount;
    uint32_t *syms = malloc((size_t)cap * sizeof(uint32_t));
    Value *vals = malloc((size_t)cap * sizeof(Value));
    for (int64_t i = 0; i < n; i++) {
        syms[i] = (uint32_t)meta[i];
        vals[i] = env->values[i];
    }
    int64_t total = n;
    for (int64_t oi = 0; oi < ocount; oi++) {
        uint32_t osym = asnix_bshared_attrs_sym(overrides_val, oi);
        Value oval = asnix_bshared_attrs_value(overrides_val, oi);
        int64_t match = -1;
        for (int64_t i = 0; i < n; i++) {
            if (syms[i] == osym) { match = i; break; }
        }
        if (match >= 0) {
            vals[match] = oval;
            env->values[match] = oval; /* propagate into sibling self-references */
        } else {
            syms[total] = osym;
            vals[total] = oval;
            total++;
        }
    }
    Value out = asnix_bshared_make_attrs(syms, vals, total);
    free(syms);
    free(vals);
    return out;
}

void asnix_bshared_string_bytes(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: %s: expected a string argument\n", who);
        exit(1);
    }
    *bytes_out = asnix_ptr_bytes(v, len_out);
}

/* M67: real Nix's `eqValues` (eval.cc) special-cases attrsets that denote
 * a derivation -- `isDerivation(v)` checks for a `type` attribute whose
 * forced value is exactly the string "derivation" -- comparing them by
 * their `outPath` attribute's value (itself a recursive `eqValues` call,
 * not a raw byte-compare) rather than the generic attribute-by-attribute
 * structural walk below. Verified directly against real `nix-instantiate
 * --eval`: two separately-constructed `derivation {...}` calls with
 * identical args are `==` (same outPath) even though they're two
 * different heap attrsets; `derivation {...} // { dummy = 1; }` is still
 * `==` to the original (outPath is unaffected by an unrelated `//`
 * merge); two derivations differing only in `builder` are not `==`
 * (different outPath). Needed because the generic structural walk below
 * would otherwise (a) give the wrong answer whenever a `//` merge changes
 * the attribute count/shape without changing outPath, and (b) risk
 * forcing attributes real Nix never would (e.g. a `throw`-valued
 * attribute merged on afterward) just to decide structural equality. */
static int value_is_derivation(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_ATTRS) return 0;
    uint32_t sym_type = asnix_intern("type", 4);
    int64_t idx = asnix_bshared_attrs_find(v, sym_type);
    if (idx < 0) return 0;
    Value tv = asnix_force_for_print(asnix_bshared_attrs_value(v, idx));
    if (asnix_tag_of(tv) != ASNIX_TAG_PTR || asnix_value_is_path(tv) || asnix_value_is_float(tv)) return 0;
    int64_t len;
    const char *bytes = asnix_ptr_bytes(tv, &len);
    return len == 10 && memcmp(bytes, "derivation", 10) == 0;
}

/* M92: real Nix's `eqValues` (eval.cc ~line 2919) starts with `if (&v1 ==
 * &v2) return true` -- comparing the ADDRESS of the Value slot/cell
 * itself, BEFORE forcing either side, not the heap payload either one
 * eventually forces to. asnix has no single uniform "every Value lives in
 * an addressable cell" model the way real Nix does (see heap.h's
 * ASNIX_CELL_MARKER comment) -- the closest equivalents are (a) two Cells
 * (possibly different Cell objects, from two independent OP_VAR_REF_CELL
 * executions) whose `target` fields point at the exact same Env slot, and
 * (b) two Values that are the literal same raw THUNK -- genuinely shared
 * memoization storage, e.g. the same already-forced attrset's entry read
 * twice (`builtins.attrValues x == builtins.attrValues x`). Must run on
 * the RAW, pre-force operands -- forcing loses exactly the identity
 * information being checked (confirmed via numtide/nix-conformance's
 * eval-okay-eq-slot-elem.nix/-identity.nix).
 *
 * Deliberately does NOT do a blanket `a.raw == b.raw` on any already-
 * concrete tag (INT/IMM/PTR/LIST/ATTRS/CLOSURE/PRIMOP): `==`'s own two
 * direct operands (asnix_bshared_value_eq_op) are ALWAYS already fully
 * forced by the ordinary (always-forcing) OP_VAR_REF/OP_SELECT/etc.
 * chokepoint that produced them -- a bare variable used directly as an
 * operand is never Cell-ified (compiler.c's try_cell_ref only fires at
 * `ib_mk_thunk` call sites, never for an ordinary compile_expr(AST_VAR)
 * read) -- so two closures that happen to be bit-identical CONTENT
 * (e.g. `let f = x: x; in f == f`, both reads forced down to the same
 * underlying closure object) are NOT "the same slot" in real Nix's
 * sense (ExprOpEq::eval always copies into a fresh local Value first --
 * see builtins_shared.h's own M80 comment) and must still compare
 * `false` (eval-okay-equal-function-direct-identical.nix) -- exactly
 * the case a blanket raw-equality check would have wrongly short-
 * circuited to `true`. List/Attrs pointer-equality is still handled,
 * correctly, by asnix_bshared_value_eq's own LIST/ATTRS cases below
 * (content equality there IS genuine equality, unlike for a function). */
static int asnix_bshared_slot_identical(Value a, Value b) {
    if (asnix_value_is_cell(a) && asnix_value_is_cell(b)) return asnix_cell_target(a) == asnix_cell_target(b);
    if (asnix_tag_of(a) == ASNIX_TAG_THUNK && asnix_tag_of(b) == ASNIX_TAG_THUNK) return a.raw == b.raw;
    return 0;
}

int asnix_bshared_value_eq(Value a, Value b) {
    if (asnix_bshared_slot_identical(a, b)) return 1;
    a = asnix_force_for_print(a);
    b = asnix_force_for_print(b);
    int ta = asnix_tag_of(a), tb = asnix_tag_of(b);
    /* M51: a Float on either side -- checked FIRST, before the generic
     * PTR-vs-PTR string/path byte-compare below (a Float's bytes are a
     * marker+double, not length-prefixed text, so that branch would
     * misread them) and before the plain `ta != tb` type-mismatch
     * rejection (int-vs-float must still compare equal when numerically
     * equal, matching real Nix's own `==`/eqValues -- confirmed against
     * `nix-instantiate --eval`: `1 == 1.0` is `true`). Not numeric on
     * one side and not float-tagged on the other (e.g. a Float vs a
     * string) simply isn't equal, same as any other cross-type `==`. */
    if (asnix_value_is_float(a) || asnix_value_is_float(b)) {
        double da, db;
        if (asnix_value_is_float(a)) da = asnix_float_value(a);
        else if (ta == ASNIX_TAG_INT) da = (double)asnix_int_value(a);
        else return 0;
        if (asnix_value_is_float(b)) db = asnix_float_value(b);
        else if (tb == ASNIX_TAG_INT) db = (double)asnix_int_value(b);
        else return 0;
        return da == db;
    }
    if (ta == ASNIX_TAG_PTR && tb == ASNIX_TAG_PTR) {
        int64_t la, lb;
        const char *ba = asnix_ptr_bytes(a, &la);
        const char *bb = asnix_ptr_bytes(b, &lb);
        return la == lb && (la == 0 || memcmp(ba, bb, (size_t)la) == 0);
    }
    if (ta != tb) return 0;
    switch (ta) {
        case ASNIX_TAG_INT: return asnix_int_value(a) == asnix_int_value(b);
        case ASNIX_TAG_IMM: return asnix_imm_subtag(a) == asnix_imm_subtag(b);
        case ASNIX_TAG_LIST: {
            if (a.raw == b.raw) return 1;
            int64_t na = asnix_bshared_list_count(a), nb = asnix_bshared_list_count(b);
            if (na != nb) return 0;
            const Value *ea = asnix_bshared_list_elems(a), *eb = asnix_bshared_list_elems(b);
            for (int64_t i = 0; i < na; i++) {
                if (!asnix_bshared_value_eq(ea[i], eb[i])) return 0;
            }
            return 1;
        }
        case ASNIX_TAG_ATTRS: {
            if (a.raw == b.raw) return 1;
            if (value_is_derivation(a) && value_is_derivation(b)) {
                uint32_t sym_outpath = asnix_intern("outPath", 7);
                int64_t ia = asnix_bshared_attrs_find(a, sym_outpath);
                int64_t ib = asnix_bshared_attrs_find(b, sym_outpath);
                if (ia >= 0 && ib >= 0) {
                    return asnix_bshared_value_eq(asnix_bshared_attrs_value(a, ia), asnix_bshared_attrs_value(b, ib));
                }
            }
            int64_t na = asnix_bshared_attrs_count(a), nb = asnix_bshared_attrs_count(b);
            if (na != nb) return 0;
            for (int64_t i = 0; i < na; i++) {
                if (asnix_bshared_attrs_sym(a, i) != asnix_bshared_attrs_sym(b, i)) return 0;
                if (!asnix_bshared_value_eq(asnix_bshared_attrs_value(a, i), asnix_bshared_attrs_value(b, i)))
                    return 0;
            }
            return 1;
        }
        /* M92: closures/primops (the only tags that can still reach here --
         * INT/IMM/PTR/LIST/ATTRS are all handled above, and THUNK is
         * impossible: force_for_print already fully resolved both sides)
         * are NEVER equal at this point, matching real Nix's `case
         * nFunction: return false` (eval.cc), which runs unconditionally
         * once the `&v1==&v2` slot-identity shortcut hasn't already
         * fired -- and it HAS already been checked, above, on the raw
         * pre-force operands (asnix_bshared_slot_identical). Previously
         * this fell back to `a.raw == b.raw` (forced-object pointer
         * identity), which happened to coincide with real Nix's answer
         * whenever the SAME Thunk/Cell happened to be read from two
         * places (there was no other way for asnix to express "same
         * slot") but gave the WRONG answer (true) whenever two
         * INDEPENDENTLY-computed closures just happened to evaluate to
         * the literal same heap object -- e.g. `[(id id)] == [(id id)]`:
         * each `(id id)` is a fresh, unaliased Thunk that both happen to
         * evaluate to the very same `id` closure; real Nix says `false`
         * (confirmed via numtide/nix-conformance's
         * eval-okay-eq-slot-elem.nix/-identity.nix, which this fixes).
         * `asnix_bshared_slot_identical` above is what keeps the cases
         * that DO need `true` (eval-okay-eq-slot-attrs.nix's `g = x: x;`
         * field, reached twice through the literal same shared Thunk; a
         * bare-variable aliased pair via a Cell) working. */
        default: return 0;
    }
}

/* See builtins_shared.h's long M80 comment. M92: also checks the Cell
 * slot-identity shortcut (asnix_bshared_slot_identical) on the RAW,
 * PRE-FORCE `a`/`b` -- before EITHER of this function's own two
 * `force_for_print` calls, which would otherwise discard exactly the
 * aliasing information being checked. In practice `==`'s own two direct
 * operands are compiled via the ordinary (always-forcing) OP_VAR_REF/
 * OP_SELECT/etc. chokepoints, never OP_VAR_REF_CELL (that's only ever
 * emitted at the ten `ib_mk_thunk` call sites, see compiler.c's
 * `try_cell_ref`) -- so a raw Cell basically never reaches here directly
 * -- but checking costs one cheap tag test and keeps this function
 * exactly as correct as `asnix_bshared_value_eq`'s own recursive check,
 * with no special-case asymmetry between the two. */
int asnix_bshared_value_eq_op(Value a, Value b) {
    if (asnix_bshared_slot_identical(a, b)) return 1;
    Value fa = asnix_force_for_print(a);
    Value fb = asnix_force_for_print(b);
    int ta = asnix_tag_of(fa), tb = asnix_tag_of(fb);
    if (ta == tb && (ta == ASNIX_TAG_CLOSURE || ta == ASNIX_TAG_PRIMOP)) return 0;
    return asnix_bshared_value_eq(fa, fb);
}

Value asnix_bshared_select_dynamic(Value target, Value name_val) {
    if (asnix_tag_of(target) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: cannot select an attribute (dynamic) on a non-attrset value (got %s)\n",
                asnix_bshared_type_name(target));
        exit(1);
    }
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '.${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    uint32_t sym = asnix_intern(bytes, (size_t)len);
    int64_t idx = asnix_bshared_attrs_find(target, sym);
    if (idx < 0) {
        fprintf(stderr, "asnix: attribute '%.*s' missing, aborting\n", (int)len, bytes);
        exit(1);
    }
    return asnix_force_for_print(asnix_bshared_attrs_value(target, idx));
}

/* `expr.${nameExpr} or default` (M28) -- OP_SELECT_DYNAMIC_OR's C half.
 * Unlike asnix_bshared_select_dynamic above (fatal on a non-attrset
 * target or a missing key), a non-attrset target here falls through
 * gracefully to the default too (same "not even an attrset simply means
 * not found" rule OP_SELECT_OR/OP_HAS_ATTR already established for `or`/
 * `?`) -- only the name itself being a non-string is still a hard fatal
 * (that's a genuine misuse, not a "not found"). Returns 1 and writes the
 * forced value through `*out` on a hit, 0 (leaving `*out` untouched) on
 * a miss -- the caller (op_select_dynamic_or.S) decides whether to push
 * the value or jump to the default's code, same split OP_SELECT_OR's own
 * inline asm makes for the static case. */
int asnix_bshared_select_dynamic_or(Value target, Value name_val, Value *out) {
    if (asnix_tag_of(target) != ASNIX_TAG_ATTRS) return 0;
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '.${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    uint32_t sym = asnix_intern(bytes, (size_t)len);
    int64_t idx = asnix_bshared_attrs_find(target, sym);
    if (idx < 0) return 0;
    *out = asnix_force_for_print(asnix_bshared_attrs_value(target, idx));
    return 1;
}

int asnix_bshared_has_attr_dynamic(Value target, Value name_val) {
    if (asnix_tag_of(target) != ASNIX_TAG_ATTRS) return 0;
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    int64_t len;
    const char *bytes = asnix_ptr_bytes(name_val, &len);
    uint32_t sym = asnix_intern(bytes, (size_t)len);
    return asnix_bshared_attrs_find(target, sym) >= 0;
}

int asnix_bshared_value_lt(Value a, Value b, const char *who) {
    a = asnix_force_for_print(a);
    b = asnix_force_for_print(b);
    int ta = asnix_tag_of(a), tb = asnix_tag_of(b);
    if (ta == ASNIX_TAG_INT && tb == ASNIX_TAG_INT) {
        return asnix_int_value(a) < asnix_int_value(b);
    }
    if (ta == ASNIX_TAG_PTR && tb == ASNIX_TAG_PTR && !asnix_value_is_path(a) && !asnix_value_is_path(b) &&
        !asnix_value_is_float(a) && !asnix_value_is_float(b)) {
        int64_t la, lb;
        const char *ba = asnix_ptr_bytes(a, &la);
        const char *bb = asnix_ptr_bytes(b, &lb);
        int64_t n = la < lb ? la : lb;
        int c = n > 0 ? memcmp(ba, bb, (size_t)n) : 0;
        if (c != 0) return c < 0;
        return la < lb;
    }
    /* Lists order lexicographically, recursively comparing elements
     * pairwise (shorter-is-less on a common prefix) -- verified against
     * real `nix-instantiate --eval`: `lessThan [1 2] [1 3]` == true,
     * `lessThan [1] [1 2]` == true, `lessThan [1 2] [1]` == false,
     * `lessThan [] []` == false, equal-length-equal-elements lists are
     * neither less (`lessThan [1 2] [1 2]` == false). A type mismatch
     * between two elements at the first differing index is the same
     * fatal "cannot compare" error as a top-level mismatch (confirmed:
     * `lessThan [1] ["a"]` throws "cannot compare an integer with a
     * string" in real Nix too). */
    if (ta == ASNIX_TAG_LIST && tb == ASNIX_TAG_LIST) {
        int64_t na = asnix_bshared_list_count(a), nb = asnix_bshared_list_count(b);
        const Value *ea = asnix_bshared_list_elems(a), *eb = asnix_bshared_list_elems(b);
        int64_t n = na < nb ? na : nb;
        for (int64_t i = 0; i < n; i++) {
            /* M92: asnix_bshared_value_eq on the RAW (pre-force) elements
             * -- NOT force_for_print'd first, same reasoning as `elem`'s
             * own fix (builtins_list2.c): forcing here would strip
             * exactly the slot/Cell identity asnix_bshared_value_eq's
             * own shortcut needs to tell "two bare-var-aliased elements
             * (e.g. both lists' shared `id`)" apart from "two
             * independently-equal-content-but-unrelated closures" --
             * the former must short-circuit "equal, compare the next
             * element" without ever reaching the fatal "cannot compare
             * these two values" fallback below, the latter correctly
             * falls all the way through to it. Found via
             * numtide/nix-conformance's eval-okay-eq-slot-sort.nix:
             * `lessThan [id 2] [id 1]`, its shared `id` element, used to
             * rely on asnix_bshared_value_eq's old default "pointer-
             * identity" fallback for closures/primops (removed this
             * same milestone, see that function's own comment) --
             * forcing first here would have made `id == id` wrongly
             * fall all the way to "cannot compare". */
            if (asnix_bshared_value_eq(ea[i], eb[i])) continue;
            Value ea_i = asnix_force_for_print(ea[i]);
            Value eb_i = asnix_force_for_print(eb[i]);
            return asnix_bshared_value_lt(ea_i, eb_i, who);
        }
        return na < nb;
    }
    fprintf(stderr, "asnix: %s: cannot compare these two values (only int-vs-int, string-vs-string, and list-vs-list are supported)\n",
            who);
    exit(1);
}

/* See builtins_shared.h's long comment on these two. `env_slots` is a
 * fresh, 2-or-3-slot Env private to this one thunk (never shared, so no
 * aliasing risk even though the IR body is a shared singleton). */
static Value make_thunk_from(IrProgram *prog, Env *env) {
    void *mem = asnix_arena_alloc(ASNIX_THUNK_SIZE);
    *(int64_t *)((char *)mem + ASNIX_THUNK_OFF_STATE) = ASNIX_THUNK_STATE_UNFORCED;
    *(void **)((char *)mem + ASNIX_THUNK_OFF_ENV) = env;
    *(IrProgram **)((char *)mem + ASNIX_THUNK_OFF_PROG) = prog; /* M100: one pointer, not {ops,pool} */
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_THUNK };
}

Value asnix_bshared_make_lazy_call1(Value f, Value arg) {
    static IrProgram *prog = NULL;
    if (!prog) {
        IrBuilder *b = ib_new();
        ib_var_ref(b, 0, 0); /* f */
        ib_var_ref(b, 0, 1); /* arg */
        ib_call(b, 1);
        ib_ret(b);
        prog = ib_finish(b);
    }
    Env *env = asnix_env_alloc(NULL, 2);
    env->values[0] = f;
    env->values[1] = arg;
    return make_thunk_from(prog, env);
}

Value asnix_bshared_make_lazy_call2(Value f, Value arg1, Value arg2) {
    static IrProgram *prog = NULL;
    if (!prog) {
        IrBuilder *b = ib_new();
        ib_var_ref(b, 0, 0); /* f */
        ib_var_ref(b, 0, 1); /* arg1 */
        ib_call(b, 1);       /* -> step1 = f arg1 */
        ib_var_ref(b, 0, 2); /* arg2 (step1 stays underneath on VALSTACK) */
        ib_call(b, 1);       /* -> step1 arg2 */
        ib_ret(b);
        prog = ib_finish(b);
    }
    Env *env = asnix_env_alloc(NULL, 3);
    env->values[0] = f;
    env->values[1] = arg1;
    env->values[2] = arg2;
    return make_thunk_from(prog, env);
}
