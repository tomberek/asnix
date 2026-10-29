#include "builtins_shared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/stats.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "attrs_update.h"
#include "symtab.h"
#include "value_util.h"

Value asnix_bshared_make_primop(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    asnix_stats_primop++;
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

Value asnix_bshared_make_primop_noforce(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    asnix_stats_primop++;
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_NOFORCE) = 1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

Value asnix_bshared_native_formals_marker(void) {
    static Value marker = { .raw = 0 };
    if (marker.raw == 0) marker = asnix_bshared_make_attrs(NULL, NULL, 0);
    return marker;
}

Value asnix_bshared_make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = asnix_bshared_make_primop(native_fn, arity);
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
    asnix_stats_closure++;
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog;
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

Value asnix_bshared_make_curried_native_noforce(void *native_fn, int64_t arity) {
    asnix_stats_closure++;
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
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog;
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

_Noreturn void asnix_bshared_select_missing_fatal(uint32_t sym) {
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    fprintf(stderr, "asnix: attribute '%.*s' missing, aborting\n", (int)len, name);
    exit(1);
}

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
        case ASNIX_TAG_THUNK: return "an unforced thunk";
        default: return "a value of unknown type";
    }
}

_Noreturn void asnix_bshared_select_type_fatal(Value v) {
    fprintf(stderr, "asnix: '.' selection target is not an attrset (got %s), aborting\n",
            asnix_bshared_type_name(v));
    exit(1);
}

_Noreturn void asnix_bshared_call_type_fatal(Value v) {
    fprintf(stderr, "asnix: call target is not a function (got %s), aborting\n",
            asnix_bshared_type_name(v));
    exit(1);
}

Value asnix_bshared_call_functor(Value attrs_val, Value arg) {
    uint32_t sym = asnix_intern("__functor", 9);
    int64_t idx = asnix_bshared_attrs_find(attrs_val, sym);
    if (idx < 0) asnix_bshared_call_type_fatal(attrs_val);
    Value functor_fn = asnix_force_for_print(asnix_bshared_attrs_value(attrs_val, idx));
    Value step1 = asnix_apply(functor_fn, &attrs_val, 1);
    return asnix_apply(step1, &arg, 1);
}

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
    asnix_stats_list++;
    asnix_stats_list_elements += (uint64_t)count;
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = count;
    if (count > 0) memcpy((char *)mem + ASNIX_LIST_OFF_ELEMS, elems, (size_t)count * sizeof(Value));
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

Value asnix_bshared_attrs_resolve(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
    if (count >= 0) return v;

    Value cached = *(const Value *)(base + ASNIX_ATTRS_UNION_OFF_FLATCACHE);
    if (cached.raw != 0) return cached;

    int64_t nleaves = -count;
    const Value *leaves = (const Value *)(base + ASNIX_ATTRS_UNION_OFF_LEAVES);

    if (nleaves >= 3) {
        int64_t leaf0_count = *(const int64_t *)((leaves[0].raw & ~(uint64_t)0x7ULL) + ASNIX_ATTRS_OFF_COUNT);
        int64_t rest_count = 0;
        for (int64_t i = 1; i < nleaves; i++) {
            rest_count += *(const int64_t *)((leaves[i].raw & ~(uint64_t)0x7ULL) + ASNIX_ATTRS_OFF_COUNT);
        }
        if (leaf0_count > rest_count) {
            Value rest_work[ASNIX_ATTRS_UNION_MAX_LEAVES];
            for (int64_t i = 1; i < nleaves; i++) rest_work[i - 1] = leaves[i];
            int64_t rn = nleaves - 1;
            while (rn > 1) {
                int64_t out_n = 0;
                int64_t i = 0;
                for (; i + 1 < rn; i += 2) rest_work[out_n++] = asnix_attrs_eager_merge(rest_work[i], rest_work[i + 1]);
                if (i < rn) rest_work[out_n++] = rest_work[i];
                rn = out_n;
            }
            Value acc = asnix_attrs_eager_merge(leaves[0], rest_work[0]);
            *(Value *)((char *)base + ASNIX_ATTRS_UNION_OFF_FLATCACHE) = acc;
            return acc;
        }
    }

    Value work[ASNIX_ATTRS_UNION_MAX_LEAVES];
    for (int64_t i = 0; i < nleaves; i++) work[i] = leaves[i];
    int64_t n = nleaves;
    while (n > 1) {
        int64_t out_n = 0;
        int64_t i = 0;
        for (; i + 1 < n; i += 2) {
            work[out_n++] = asnix_attrs_eager_merge(work[i], work[i + 1]);
        }
        if (i < n) work[out_n++] = work[i];
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

static int64_t attrs_find_flat_noresolve(Value flat, uint32_t sym) {
    const char *base = (const char *)(uintptr_t)(flat.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t hi = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
    const char *entries = base + ASNIX_ATTRS_OFF_ENTRIES;
    if (hi <= 8) {
        const char *entry = entries;
        for (int64_t i = 0; i < hi; i++, entry += ASNIX_ATTR_SIZE) {
            if (*(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) == sym) return i;
        }
        return -1;
    }
    int64_t lo = 0;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        const char *entry = entries + mid * ASNIX_ATTR_SIZE;
        uint32_t s = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
        if (s == sym) return mid;
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    return -1;
}

int64_t asnix_bshared_attrs_find(Value attrs, uint32_t sym) {
    attrs = asnix_bshared_attrs_resolve(attrs);
    return attrs_find_flat_noresolve(attrs, sym);
}

int asnix_bshared_attrs_probe(Value attrs, uint32_t sym, Value *out_value) {
    const char *base = (const char *)(uintptr_t)(attrs.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
    Value probe_target = attrs;
    if (count < 0) {
        Value cached = *(const Value *)(base + ASNIX_ATTRS_UNION_OFF_FLATCACHE);
        if (cached.raw != 0) {
            probe_target = cached;
        } else {
            int64_t nleaves = -count;
            const Value *leaves = (const Value *)(base + ASNIX_ATTRS_UNION_OFF_LEAVES);
            for (int64_t i = nleaves - 1; i >= 0; i--) {
                int64_t idx = attrs_find_flat_noresolve(leaves[i], sym);
                if (idx < 0) continue;
                const char *lbase = (const char *)(uintptr_t)(leaves[i].raw & ~(uint64_t)ASNIX_TAG_MASK);
                const char *entry = lbase + ASNIX_ATTRS_OFF_ENTRIES + idx * ASNIX_ATTR_SIZE;
                *out_value = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
                return 1;
            }
            return 0;
        }
    }
    int64_t idx = attrs_find_flat_noresolve(probe_target, sym);
    if (idx < 0) return 0;
    const char *pbase = (const char *)(uintptr_t)(probe_target.raw & ~(uint64_t)ASNIX_TAG_MASK);
    const char *entry = pbase + ASNIX_ATTRS_OFF_ENTRIES + idx * ASNIX_ATTR_SIZE;
    *out_value = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
    return 1;
}

typedef struct {
    uint32_t sym;
    int64_t idx;
} MakeAttrsEntry;

#define ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX 32

static MakeAttrsEntry *merge_sort_make_attrs_entries(MakeAttrsEntry *a, MakeAttrsEntry *tmp, int64_t n) {
    MakeAttrsEntry *src = a, *dst = tmp;
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t i = 0; i < n; i += 2 * width) {
            int64_t mid = i + width < n ? i + width : n;
            int64_t end = i + 2 * width < n ? i + 2 * width : n;
            int64_t l = i, r = mid, k = i;
            while (l < mid && r < end) {
                if (src[l].sym <= src[r].sym) dst[k++] = src[l++];
                else dst[k++] = src[r++];
            }
            while (l < mid) dst[k++] = src[l++];
            while (r < end) dst[k++] = src[r++];
        }
        MakeAttrsEntry *t = src; src = dst; dst = t;
    }
    return src;
}

Value asnix_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * ASNIX_ATTR_SIZE);
    asnix_stats_attrs++;
    asnix_stats_attrs_entries += (uint64_t)count;
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = count;

    if (count <= ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX) {
        int64_t order_stack[ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX];
        for (int64_t i = 0; i < count; i++) order_stack[i] = i;
        for (int64_t i = 1; i < count; i++) {
            int64_t j = i;
            while (j > 0 && syms[order_stack[j - 1]] > syms[order_stack[j]]) {
                int64_t tmp = order_stack[j - 1];
                order_stack[j - 1] = order_stack[j];
                order_stack[j] = tmp;
                j--;
            }
        }
        for (int64_t i = 0; i < count; i++) {
            char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            int64_t src = order_stack[i];
            *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = syms[src];
            *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[src];
        }
    } else {
        MakeAttrsEntry *entries = asnix_arena_alloc((uint64_t)count * sizeof(MakeAttrsEntry));
        MakeAttrsEntry *scratch = asnix_arena_alloc((uint64_t)count * sizeof(MakeAttrsEntry));
        for (int64_t i = 0; i < count; i++) {
            entries[i].sym = syms[i];
            entries[i].idx = i;
        }
        MakeAttrsEntry *sorted = merge_sort_make_attrs_entries(entries, scratch, count);
        for (int64_t i = 0; i < count; i++) {
            char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            int64_t src = sorted[i].idx;
            *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = sorted[i].sym;
            *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[src];
        }
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

typedef struct {
    uint32_t sym;
    int64_t idx;
} OverrideSymIdx;

static int cmp_override_sym_idx(const void *a, const void *b) {
    uint32_t x = ((const OverrideSymIdx *)a)->sym, y = ((const OverrideSymIdx *)b)->sym;
    return x < y ? -1 : (x > y ? 1 : 0);
}

#define ASNIX_REC_OVERRIDES_PRODUCT_MAX 2048

Value asnix_bshared_rec_overrides_merge(Env *env, int64_t n, const int64_t *meta) {
    int64_t overrides_slot = meta[n];
    Value overrides_val = asnix_force_for_print(env->values[overrides_slot]);
    if (asnix_tag_of(overrides_val) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: '__overrides' must be a set (got %s)\n",
                asnix_bshared_type_name(overrides_val));
        exit(1);
    }

    int64_t ocount = asnix_bshared_attrs_count(overrides_val);
    int64_t cap = n + ocount;
    uint32_t *syms = malloc((size_t)cap * sizeof(uint32_t));
    Value *vals = malloc((size_t)cap * sizeof(Value));
    for (int64_t i = 0; i < n; i++) {
        syms[i] = (uint32_t)meta[i];
        vals[i] = env->values[i];
    }
    int use_sorted = n * ocount > ASNIX_REC_OVERRIDES_PRODUCT_MAX;
    OverrideSymIdx *sym_order = NULL;
    if (use_sorted) {
        sym_order = malloc((size_t)n * sizeof(OverrideSymIdx));
        for (int64_t i = 0; i < n; i++) {
            sym_order[i].sym = syms[i];
            sym_order[i].idx = i;
        }
        if (n <= ASNIX_MAKE_ATTRS_SORT_INSERTION_MAX) {
            for (int64_t i = 1; i < n; i++) {
                OverrideSymIdx key = sym_order[i];
                int64_t j = i - 1;
                while (j >= 0 && sym_order[j].sym > key.sym) {
                    sym_order[j + 1] = sym_order[j];
                    j--;
                }
                sym_order[j + 1] = key;
            }
        } else {
            qsort(sym_order, (size_t)n, sizeof(OverrideSymIdx), cmp_override_sym_idx);
        }
    }
    int64_t total = n;
    for (int64_t oi = 0; oi < ocount; oi++) {
        uint32_t osym = asnix_bshared_attrs_sym(overrides_val, oi);
        Value oval = asnix_bshared_attrs_value(overrides_val, oi);
        int64_t match = -1;
        if (use_sorted) {
            int64_t lo = 0, hi = n - 1;
            while (lo <= hi) {
                int64_t mid = lo + (hi - lo) / 2;
                if (sym_order[mid].sym == osym) { match = sym_order[mid].idx; break; }
                else if (sym_order[mid].sym < osym) lo = mid + 1;
                else hi = mid - 1;
            }
        } else {
            for (int64_t i = 0; i < n; i++) {
                if (syms[i] == osym) { match = i; break; }
            }
        }
        if (match >= 0) {
            vals[match] = oval;
            env->values[match] = oval;
        } else {
            syms[total] = osym;
            vals[total] = oval;
            total++;
        }
    }
    Value out = asnix_bshared_make_attrs(syms, vals, total);
    free(syms);
    free(vals);
    free(sym_order);
    return out;
}

void asnix_bshared_string_bytes(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: %s: expected a string argument\n", who);
        exit(1);
    }
    *bytes_out = asnix_ptr_bytes(v, len_out);
}

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
        default: return 0;
    }
}

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
    if (ta == ASNIX_TAG_LIST && tb == ASNIX_TAG_LIST) {
        int64_t na = asnix_bshared_list_count(a), nb = asnix_bshared_list_count(b);
        const Value *ea = asnix_bshared_list_elems(a), *eb = asnix_bshared_list_elems(b);
        int64_t n = na < nb ? na : nb;
        for (int64_t i = 0; i < n; i++) {
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

