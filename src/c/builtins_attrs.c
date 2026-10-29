#include "builtins_attrs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

static void require_tag(Value v, int tag, const char *who, const char *what) {
    if (asnix_tag_of(v) != tag) {
        fprintf(stderr, "asnix: %s: argument is not %s\n", who, what);
        exit(1);
    }
}
#define require_attrs(v, who)  require_tag((v), ASNIX_TAG_ATTRS, (who), "an attrset")
#define require_string(v, who) require_tag((v), ASNIX_TAG_PTR, (who), "a string")
#define require_list(v, who)   require_tag((v), ASNIX_TAG_LIST, (who), "a list")

static int64_t attrs_count(Value v) {
    v = asnix_bshared_attrs_resolve(v);
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_COUNT);
}
static uint32_t attrs_entry_sym(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry = (const char *)(v.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
}
static Value attrs_entry_value(Value v, int64_t i) {
    v = asnix_bshared_attrs_resolve(v);
    const char *entry = (const char *)(v.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
    return *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
}
static int64_t list_count(Value v) {
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + ASNIX_LIST_OFF_COUNT);
}
static Value list_elem(Value v, int64_t i) {
    return *(const Value *)((const char *)(v.raw & ~0x7ULL) + ASNIX_LIST_OFF_ELEMS + i * 8);
}
static Value make_list(const Value *elems, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * 8);
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = count;
    if (count > 0) memcpy((char *)mem + ASNIX_LIST_OFF_ELEMS, elems, (size_t)count * 8);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}
static Value make_attrs_from_sorted(const uint32_t *syms, const Value *vals, int64_t count) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = count;
    for (int64_t i = 0; i < count; i++) {
        char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = syms[i];
        *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = vals[i];
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

static int64_t string_len(Value v) {
    return *(const int64_t *)((const char *)(v.raw & ~0x7ULL) + ASNIX_STRING_OFF_LEN);
}
static const char *string_bytes(Value v) {
    return (const char *)(v.raw & ~0x7ULL) + ASNIX_STRING_OFF_BYTES;
}

static int name_cmp(const char *an, size_t al, const char *bn, size_t bl) {
    size_t n = al < bl ? al : bl;
    int c = memcmp(an, bn, n);
    if (c != 0) return c;
    return (al > bl) - (al < bl);
}

typedef struct {
    int64_t idx;
    const char *name;
    size_t len;
} NameOrderEntry;

static int name_order_entry_cmp(const void *ap, const void *bp) {
    const NameOrderEntry *a = (const NameOrderEntry *)ap;
    const NameOrderEntry *b = (const NameOrderEntry *)bp;
    return name_cmp(a->name, a->len, b->name, b->len);
}

static void sorted_name_order(Value attrs, int64_t n, int64_t *order) {
    NameOrderEntry *entries = malloc((size_t)n * sizeof(NameOrderEntry));
    for (int64_t i = 0; i < n; i++) {
        size_t len;
        const char *name = asnix_sym_name(attrs_entry_sym(attrs, i), &len);
        entries[i] = (NameOrderEntry){ .idx = i, .name = name, .len = len };
    }
    qsort(entries, (size_t)n, sizeof(NameOrderEntry), name_order_entry_cmp);
    for (int64_t i = 0; i < n; i++) order[i] = entries[i].idx;
    free(entries);
}

static int64_t attrs_find(Value attrs, uint32_t sym) {
    int64_t lo = 0, hi = attrs_count(attrs);
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t s = attrs_entry_sym(attrs, mid);
        if (s == sym) return mid;
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    return -1;
}

static Value make_curried2(Value captured, IrProgram *inner_prog) {
    Env *env = asnix_env_alloc(NULL, 1);
    env->values[0] = captured;
    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = env;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = inner_prog;
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = asnix_bshared_native_formals_marker().raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

static IrProgram *build_curry2_body(Value primop2) {
    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)primop2.raw);
    ib_var_ref(b, 1, 0);
    ib_var_ref(b, 0, 0);
    ib_call(b, 2);
    ib_ret(b);
    return ib_finish(b);
}

static Value prim_attrNames(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "attrNames");
    Value attrs = args[0];
    int64_t n = attrs_count(attrs);
    int64_t *order = asnix_arena_alloc((uint64_t)n * sizeof(int64_t));
    sorted_name_order(attrs, n, order);
    Value *out = asnix_arena_alloc((uint64_t)n * sizeof(Value));
    for (int64_t i = 0; i < n; i++) {
        size_t len;
        const char *name = asnix_sym_name(attrs_entry_sym(attrs, order[i]), &len);
        out[i] = asnix_make_string_value(name, len);
    }
    return make_list(out, n);
}

static Value prim_attrValues(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "attrValues");
    Value attrs = args[0];
    int64_t n = attrs_count(attrs);
    int64_t *order = asnix_arena_alloc((uint64_t)n * sizeof(int64_t));
    sorted_name_order(attrs, n, order);
    Value *out = asnix_arena_alloc((uint64_t)n * sizeof(Value));
    for (int64_t i = 0; i < n; i++) out[i] = attrs_entry_value(attrs, order[i]);
    return make_list(out, n);
}

static Value prim_hasAttr_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_string(args[0], "hasAttr");
    require_attrs(args[1], "hasAttr");
    uint32_t sym = asnix_intern(string_bytes(args[0]), (size_t)string_len(args[0]));
    return attrs_find(args[1], sym) >= 0 ? ASNIX_TRUE : ASNIX_FALSE;
}

static Value prim_hasAttr(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_hasAttr_apply2, 2));
    return make_curried2(args[0], inner);
}

static Value prim_getAttr_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_string(args[0], "getAttr");
    require_attrs(args[1], "getAttr");
    uint32_t sym = asnix_intern(string_bytes(args[0]), (size_t)string_len(args[0]));
    int64_t idx = attrs_find(args[1], sym);
    if (idx < 0) {
        fprintf(stderr, "asnix: getAttr: attribute '%.*s' missing\n",
                (int)string_len(args[0]), string_bytes(args[0]));
        exit(1);
    }
    return attrs_entry_value(args[1], idx);
}

static Value prim_getAttr(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_getAttr_apply2, 2));
    return make_curried2(args[0], inner);
}

#define ASNIX_REMOVE_ATTRS_SORT_INSERTION_MAX 32

#define ASNIX_REMOVE_ATTRS_PRODUCT_MAX 2048

static int cmp_u32(const void *a, const void *b) {
    uint32_t ua = *(const uint32_t *)a, ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

static int remove_syms_contains(const uint32_t *sorted, int64_t nnames, uint32_t sym) {
    int64_t lo = 0, hi = nnames - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sorted[mid] == sym) return 1;
        if (sorted[mid] < sym) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

static Value prim_removeAttrs_apply2(const Value *args, int64_t argc) {
    (void)argc;
    require_attrs(args[0], "removeAttrs");
    require_list(args[1], "removeAttrs");
    Value attrs = args[0];
    Value namelist = args[1];
    int64_t nnames = list_count(namelist);
    uint32_t *remove_syms = asnix_arena_alloc((uint64_t)nnames * sizeof(uint32_t));
    for (int64_t i = 0; i < nnames; i++) {
        Value nv = asnix_force_for_print(list_elem(namelist, i));
        require_string(nv, "removeAttrs");
        remove_syms[i] = asnix_intern(string_bytes(nv), (size_t)string_len(nv));
    }
    int64_t n = attrs_count(attrs);
    int use_sorted = n * nnames > ASNIX_REMOVE_ATTRS_PRODUCT_MAX;
    if (use_sorted) {
        if (nnames <= ASNIX_REMOVE_ATTRS_SORT_INSERTION_MAX) {
            for (int64_t i = 1; i < nnames; i++) {
                uint32_t e = remove_syms[i];
                int64_t j = i;
                while (j > 0 && remove_syms[j - 1] > e) {
                    remove_syms[j] = remove_syms[j - 1];
                    j--;
                }
                remove_syms[j] = e;
            }
        } else {
            qsort(remove_syms, (size_t)nnames, sizeof(uint32_t), cmp_u32);
        }
    }
    uint32_t *out_syms = asnix_arena_alloc((uint64_t)n * sizeof(uint32_t));
    Value *out_vals = asnix_arena_alloc((uint64_t)n * sizeof(Value));
    int64_t out_n = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = attrs_entry_sym(attrs, i);
        int removed;
        if (use_sorted) {
            removed = remove_syms_contains(remove_syms, nnames, sym);
        } else {
            removed = 0;
            for (int64_t j = 0; j < nnames; j++) {
                if (remove_syms[j] == sym) { removed = 1; break; }
            }
        }
        if (!removed) {
            out_syms[out_n] = sym;
            out_vals[out_n] = attrs_entry_value(attrs, i);
            out_n++;
        }
    }
    return make_attrs_from_sorted(out_syms, out_vals, out_n);
}

static Value prim_removeAttrs(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_removeAttrs_apply2, 2));
    return make_curried2(args[0], inner);
}

Value asnix_make_removeAttrs_primop(void) {
    return make_primop_value((void *)prim_removeAttrs, 1);
}

static Value prim_mapAttrs_apply2(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0];
    Value attrs = args[1];
    require_attrs(attrs, "mapAttrs");
    Value flat = asnix_bshared_attrs_resolve(attrs);
    const char *flat_base = (const char *)(flat.raw & ~0x7ULL);
    int64_t n = *(const int64_t *)(flat_base + ASNIX_ATTRS_OFF_COUNT);
    const char *src_entries = flat_base + ASNIX_ATTRS_OFF_ENTRIES;
    if (n == 0) return make_attrs_from_sorted(NULL, NULL, 0);

    uint32_t *syms = asnix_arena_alloc((uint64_t)n * sizeof(uint32_t));
    Value *vals = asnix_arena_alloc((uint64_t)n * sizeof(Value));

    if (n > ASNIX_THUNKMAP_MAX_IDX + 1) {
        for (int64_t i = 0; i < n; i++) {
            const char *entry = src_entries + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            Value orig = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            size_t len;
            const char *name = asnix_sym_name(sym, &len);
            Value name_val = asnix_make_string_value(name, len);
            syms[i] = sym;
            vals[i] = asnix_make_lazyapp2(f, name_val, orig);
        }
        return make_attrs_from_sorted(syms, vals, n);
    }

    void *desc = asnix_thunkmap_make_desc(f, src_entries);
    for (int64_t i = 0; i < n; i++) {
        syms[i] = *(const uint32_t *)(src_entries + i * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM);
        vals[i] = asnix_thunkmap_pack(desc, i);
    }
    Value out = make_attrs_from_sorted(syms, vals, n);
    asnix_thunkmap_set_out(desc, out);
    return out;
}

static Value prim_mapAttrs(const Value *args, int64_t argc) {
    (void)argc;
    static IrProgram *inner = NULL;
    if (!inner) inner = build_curry2_body(make_primop_value((void *)prim_mapAttrs_apply2, 2));
    return make_curried2(args[0], inner);
}

const AsnixBuiltinDef ASNIX_ATTRS_BUILTINS[] = {
    { "attrNames", prim_attrNames, 1 },
    { "attrValues", prim_attrValues, 1 },
    { "hasAttr", prim_hasAttr, 1 },
    { "getAttr", prim_getAttr, 1 },
    { "mapAttrs", prim_mapAttrs, 1 },
    { "removeAttrs", prim_removeAttrs, 1 },
};

const size_t ASNIX_ATTRS_BUILTINS_COUNT = sizeof(ASNIX_ATTRS_BUILTINS) / sizeof(ASNIX_ATTRS_BUILTINS[0]);

