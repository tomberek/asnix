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

/* Ported straight from builtins_misc.c's make_curried_native -- see that
 * file's long header comment for the full rationale (equivalent to
 * compiling the fictitious Nix source `a1: a2: ... aN: __native a1 a2 ... aN`
 * via the same IrBuilder API the real compiler uses for ordinary curried
 * lambdas). */
Value asnix_bshared_make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = asnix_bshared_make_primop(native_fn, arity);
    /* Never a lambda-pattern -- `builtins.functionArgs` on any of these
     * closures is `{}`, same as real Nix's `functionArgs builtins.add`
     * (M37). */
    Value empty_formals = asnix_bshared_make_attrs(NULL, NULL, 0);

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
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_BODY) = prog->ops;
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_POOL) = prog->pool;
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
        case ASNIX_TAG_PTR: return asnix_value_is_path(v) ? "a path" : "a string";
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

int64_t asnix_bshared_attrs_count(Value v) {
    return *(const int64_t *)((const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_COUNT);
}

uint32_t asnix_bshared_attrs_sym(Value v, int64_t i) {
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
}

Value asnix_bshared_attrs_value(Value v, int64_t i) {
    const char *entry =
        (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
}

int64_t asnix_bshared_attrs_find(Value attrs, uint32_t sym) {
    int64_t lo = 0, hi = asnix_bshared_attrs_count(attrs);
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t s = asnix_bshared_attrs_sym(attrs, mid);
        if (s == sym) return mid;
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    return -1;
}

Value asnix_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count) {
    for (int64_t i = 1; i < count; i++) {
        int64_t j = i;
        while (j > 0 && syms[j - 1] > syms[j]) {
            uint32_t ts = syms[j - 1]; syms[j - 1] = syms[j]; syms[j] = ts;
            Value tv = vals[j - 1]; vals[j - 1] = vals[j]; vals[j] = tv;
            j--;
        }
    }
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = count;
    for (int64_t i = 0; i < count; i++) {
        char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        *(int64_t *)(entry + ASNIX_ATTR_OFF_SYM) = (int64_t)syms[i]; /* zero-extends */
        *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[i];
    }
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

int asnix_bshared_value_eq(Value a, Value b) {
    a = asnix_force_for_print(a);
    b = asnix_force_for_print(b);
    int ta = asnix_tag_of(a), tb = asnix_tag_of(b);
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
            int64_t na = asnix_bshared_attrs_count(a), nb = asnix_bshared_attrs_count(b);
            if (na != nb) return 0;
            for (int64_t i = 0; i < na; i++) {
                if (asnix_bshared_attrs_sym(a, i) != asnix_bshared_attrs_sym(b, i)) return 0;
                if (!asnix_bshared_value_eq(asnix_bshared_attrs_value(a, i), asnix_bshared_attrs_value(b, i)))
                    return 0;
            }
            return 1;
        }
        default: return a.raw == b.raw; /* pointer identity: closures/primops/thunks */
    }
}

Value asnix_bshared_select_dynamic(Value target, Value name_val) {
    if (asnix_tag_of(target) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: cannot select an attribute (dynamic) on a non-attrset value (got %s)\n",
                asnix_bshared_type_name(target));
        exit(1);
    }
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val)) {
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
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val)) {
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
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val)) {
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
    if (ta == ASNIX_TAG_PTR && tb == ASNIX_TAG_PTR && !asnix_value_is_path(a) && !asnix_value_is_path(b)) {
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
            Value ea_i = asnix_force_for_print(ea[i]);
            Value eb_i = asnix_force_for_print(eb[i]);
            if (asnix_bshared_value_eq(ea_i, eb_i)) continue;
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
    *(void **)((char *)mem + ASNIX_THUNK_OFF_BODY) = prog->ops;
    *(void **)((char *)mem + ASNIX_THUNK_OFF_POOL) = prog->pool;
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
