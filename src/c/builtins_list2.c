#include "builtins_list2.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "symtab.h"

static Value check_bool(Value v, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_IMM ||
        (asnix_imm_subtag(v) != ASNIX_IMM_TRUE && asnix_imm_subtag(v) != ASNIX_IMM_FALSE)) {
        asnix_bshared_fatal(who, "function did not return a boolean");
    }
    return v;
}

Value asnix_prim_elem2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value x = args[0];
    Value list = asnix_force_for_print(args[1]);
    ASNIX_REQUIRE_LIST(list, "elem");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        if (asnix_bshared_value_eq(x, elems[i])) return ASNIX_TRUE;
    }
    return ASNIX_FALSE;
}
static Value build_elem(void) { return asnix_bshared_make_curried_native_noforce((void *)asnix_prim_elem2_impl, 2); }

Value asnix_prim_any2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "any");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(asnix_apply(pred, &elems[i], 1), "any");
        if (asnix_imm_subtag(r) == ASNIX_IMM_TRUE) return ASNIX_TRUE;
    }
    return ASNIX_FALSE;
}
static Value build_any(void) { return asnix_bshared_make_curried_native((void *)asnix_prim_any2_impl, 2); }

Value asnix_prim_all2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "all");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(asnix_apply(pred, &elems[i], 1), "all");
        if (asnix_imm_subtag(r) == ASNIX_IMM_FALSE) return ASNIX_FALSE;
    }
    return ASNIX_TRUE;
}
static Value build_all(void) { return asnix_bshared_make_curried_native((void *)asnix_prim_all2_impl, 2); }

static Value concatMap2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "concatMap");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    Value *subs = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        subs[i] = asnix_apply(f, &elems[i], 1);
        ASNIX_REQUIRE_LIST(subs[i], "concatMap");
        total += asnix_bshared_list_count(subs[i]);
    }
    Value *out = total > 0 ? malloc((size_t)total * sizeof(Value)) : NULL;
    int64_t pos = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t c = asnix_bshared_list_count(subs[i]);
        if (c > 0) memcpy(out + pos, asnix_bshared_list_elems(subs[i]), (size_t)c * sizeof(Value));
        pos += c;
    }
    Value result = asnix_bshared_make_list(out, total);
    free(subs);
    free(out);
    return result;
}
static Value build_concatMap(void) { return asnix_bshared_make_curried_native((void *)concatMap2_impl, 2); }

static Value genList2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], n_v = args[1];
    ASNIX_REQUIRE_INT(n_v, "genList");
    int64_t n = asnix_int_value(n_v);
    if (n < 0) asnix_bshared_fatal("genList", "length must be non-negative");
    Value *out = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < n; i++) {
        Value iv = asnix_make_int(i);
        out[i] = asnix_apply(f, &iv, 1);
    }
    Value result = asnix_bshared_make_list(out, n);
    free(out);
    return result;
}
static Value build_genList(void) { return asnix_bshared_make_curried_native((void *)genList2_impl, 2); }

static Value sort2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value cmp = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "sort");
    int64_t n = asnix_bshared_list_count(list);
    if (n == 0) return list;
    Value *arr = malloc((size_t)n * sizeof(Value));
    memcpy(arr, asnix_bshared_list_elems(list), (size_t)n * sizeof(Value));
    for (int64_t i = 1; i < n; i++) {
        Value key = arr[i];
        int64_t j = i - 1;
        while (j >= 0) {
            Value lt = check_bool(asnix_bshared_apply2(cmp, key, arr[j]), "sort");
            if (asnix_imm_subtag(lt) != ASNIX_IMM_TRUE) break;
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    Value result = asnix_bshared_make_list(arr, n);
    free(arr);
    return result;
}
static Value build_sort(void) { return asnix_bshared_make_curried_native((void *)sort2_impl, 2); }

static Value partition2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "partition");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    Value *right = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    Value *wrong = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t nr = 0, nw = 0;
    for (int64_t i = 0; i < n; i++) {
        Value r = check_bool(asnix_apply(pred, &elems[i], 1), "partition");
        if (asnix_imm_subtag(r) == ASNIX_IMM_TRUE) right[nr++] = elems[i];
        else wrong[nw++] = elems[i];
    }
    uint32_t syms[2];
    Value vals[2];
    syms[0] = asnix_intern("right", 5);
    vals[0] = asnix_bshared_make_list(right, nr);
    syms[1] = asnix_intern("wrong", 5);
    vals[1] = asnix_bshared_make_list(wrong, nw);
    Value result = asnix_bshared_make_attrs(syms, vals, 2);
    free(right);
    free(wrong);
    return result;
}
static Value build_partition(void) { return asnix_bshared_make_curried_native((void *)partition2_impl, 2); }

static Value foldl3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value op = args[0], acc = args[1], list = args[2];
    ASNIX_REQUIRE_LIST(list, "foldl");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = 0; i < n; i++) acc = asnix_bshared_apply2(op, acc, elems[i]);
    return acc;
}
static Value build_foldl(void) { return asnix_bshared_make_curried_native((void *)foldl3_impl, 3); }

static Value foldr3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value op = args[0], acc = args[1], list = args[2];
    ASNIX_REQUIRE_LIST(list, "foldr");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    for (int64_t i = n - 1; i >= 0; i--) acc = asnix_bshared_apply2(op, elems[i], acc);
    return acc;
}
static Value build_foldr(void) { return asnix_bshared_make_curried_native((void *)foldr3_impl, 3); }

static Value catAttrs2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value name_v = args[0], list = args[1];
    ASNIX_REQUIRE_STRING(name_v, "catAttrs");
    ASNIX_REQUIRE_LIST(list, "catAttrs");
    int64_t nlen;
    const char *nbytes;
    asnix_bshared_string_bytes(name_v, &nbytes, &nlen, "catAttrs");
    uint32_t sym = asnix_intern(nbytes, (size_t)nlen);

    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    Value *out = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t nout = 0;
    for (int64_t i = 0; i < n; i++) {
        Value elem = asnix_force_for_print(elems[i]);
        ASNIX_REQUIRE_ATTRS(elem, "catAttrs");
        int64_t idx = asnix_bshared_attrs_find(elem, sym);
        if (idx >= 0) out[nout++] = asnix_bshared_attrs_value(elem, idx);
    }
    Value result = asnix_bshared_make_list(out, nout);
    free(out);
    return result;
}
static Value build_catAttrs(void) { return asnix_bshared_make_curried_native((void *)catAttrs2_impl, 2); }

const AsnixBuiltinMiscDef ASNIX_LIST2_BUILTINS[] = {
    { "elem", build_elem },
    { "any", build_any },
    { "all", build_all },
    { "concatMap", build_concatMap },
    { "genList", build_genList },
    { "sort", build_sort },
    { "partition", build_partition },
    { "foldl", build_foldl },
    { "foldr", build_foldr },
    { "catAttrs", build_catAttrs },
};

const size_t ASNIX_LIST2_BUILTINS_COUNT = sizeof(ASNIX_LIST2_BUILTINS) / sizeof(ASNIX_LIST2_BUILTINS[0]);

