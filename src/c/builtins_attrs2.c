#include "builtins_attrs2.h"

#include <stdint.h>
#include <stdlib.h>

#include "asnix/art_sym.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

static Value filterAttrs2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = args[0], set = args[1];
    ASNIX_REQUIRE_ATTRS(set, "filterAttrs");
    int64_t n = asnix_bshared_attrs_count(set);
    uint32_t *syms = n > 0 ? malloc((size_t)n * sizeof(uint32_t)) : NULL;
    Value *vals = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t out_n = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = asnix_bshared_attrs_sym(set, i);
        size_t len;
        const char *name = asnix_sym_name(sym, &len);
        Value name_val = asnix_make_string_value(name, len);
        Value orig = asnix_bshared_attrs_value(set, i);
        Value keep = asnix_bshared_apply2(pred, name_val, orig);
        if (asnix_tag_of(keep) != ASNIX_TAG_IMM ||
            (asnix_imm_subtag(keep) != ASNIX_IMM_TRUE && asnix_imm_subtag(keep) != ASNIX_IMM_FALSE)) {
            asnix_bshared_fatal("filterAttrs", "predicate did not return a boolean");
        }
        if (asnix_imm_subtag(keep) == ASNIX_IMM_TRUE) {
            syms[out_n] = sym;
            vals[out_n] = orig;
            out_n++;
        }
    }
    Value result = asnix_bshared_make_attrs(syms, vals, out_n);
    free(syms);
    free(vals);
    return result;
}
static Value build_filterAttrs(void) { return asnix_bshared_make_curried_native((void *)filterAttrs2_impl, 2); }

#define ASNIX_LIST_TO_ATTRS_ART_THRESHOLD 64

typedef struct { uint32_t sym; int64_t idx; Value val; } ListToAttrsEntry;

static Value prim_listToAttrs(const Value *args, int64_t argc) {
    (void)argc;
    Value list = args[0];
    ASNIX_REQUIRE_LIST(list, "listToAttrs");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);

    uint32_t sym_name = asnix_intern("name", 4);
    uint32_t sym_value = asnix_intern("value", 5);

    if (n > ASNIX_LIST_TO_ATTRS_ART_THRESHOLD) {
        AsnixArtSymSet set;
        asnix_art_sym_init(&set);
        for (int64_t i = 0; i < n; i++) {
            Value elem = asnix_force_for_print(elems[i]);
            ASNIX_REQUIRE_ATTRS(elem, "listToAttrs");
            int64_t name_idx = asnix_bshared_attrs_find(elem, sym_name);
            if (name_idx < 0) asnix_bshared_fatal("listToAttrs", "an element is missing the 'name' attribute");
            int64_t value_idx = asnix_bshared_attrs_find(elem, sym_value);
            if (value_idx < 0) asnix_bshared_fatal("listToAttrs", "an element is missing the 'value' attribute");

            Value name_v = asnix_force_for_print(asnix_bshared_attrs_value(elem, name_idx));
            ASNIX_REQUIRE_STRING(name_v, "listToAttrs");
            int64_t nlen;
            const char *nbytes;
            asnix_bshared_string_bytes(name_v, &nbytes, &nlen, "listToAttrs");
            uint32_t sym = asnix_intern(nbytes, (size_t)nlen);

            asnix_art_sym_insert(&set, sym, asnix_bshared_attrs_value(elem, value_idx));
        }
        uint32_t *syms = malloc((size_t)n * sizeof(uint32_t));
        Value *vals = malloc((size_t)n * sizeof(Value));
        int64_t out_n = asnix_art_sym_collect_sorted(&set, syms, vals);
        Value result = asnix_bshared_make_attrs(syms, vals, out_n);
        free(syms);
        free(vals);
        return result;
    }

    ListToAttrsEntry *entries = n > 0 ? malloc((size_t)n * sizeof(ListToAttrsEntry)) : NULL;

    for (int64_t i = 0; i < n; i++) {
        Value elem = asnix_force_for_print(elems[i]);
        ASNIX_REQUIRE_ATTRS(elem, "listToAttrs");
        int64_t name_idx = asnix_bshared_attrs_find(elem, sym_name);
        if (name_idx < 0) asnix_bshared_fatal("listToAttrs", "an element is missing the 'name' attribute");
        int64_t value_idx = asnix_bshared_attrs_find(elem, sym_value);
        if (value_idx < 0) asnix_bshared_fatal("listToAttrs", "an element is missing the 'value' attribute");

        Value name_v = asnix_force_for_print(asnix_bshared_attrs_value(elem, name_idx));
        ASNIX_REQUIRE_STRING(name_v, "listToAttrs");
        int64_t nlen;
        const char *nbytes;
        asnix_bshared_string_bytes(name_v, &nbytes, &nlen, "listToAttrs");
        uint32_t sym = asnix_intern(nbytes, (size_t)nlen);

        entries[i].sym = sym;
        entries[i].idx = i;
        entries[i].val = asnix_bshared_attrs_value(elem, value_idx);
    }

    for (int64_t i = 1; i < n; i++) {
        int64_t j = i;
        ListToAttrsEntry e = entries[i];
        while (j > 0 && (entries[j - 1].sym > e.sym ||
                         (entries[j - 1].sym == e.sym && entries[j - 1].idx > e.idx))) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = e;
    }

    uint32_t *syms = n > 0 ? malloc((size_t)n * sizeof(uint32_t)) : NULL;
    Value *vals = n > 0 ? malloc((size_t)n * sizeof(Value)) : NULL;
    int64_t out_n = 0;
    for (int64_t i = 0; i < n; i++) {
        if (out_n > 0 && syms[out_n - 1] == entries[i].sym) continue;
        syms[out_n] = entries[i].sym;
        vals[out_n] = entries[i].val;
        out_n++;
    }

    Value result = asnix_bshared_make_attrs(syms, vals, out_n);
    free(entries);
    free(syms);
    free(vals);
    return result;
}
static Value build_listToAttrs(void) { return asnix_bshared_make_primop((void *)prim_listToAttrs, 1); }

static Value attrByPath3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value path = args[0], deflt = args[1], set = args[2];
    ASNIX_REQUIRE_LIST(path, "attrByPath");
    int64_t n = asnix_bshared_list_count(path);
    const Value *comps = asnix_bshared_list_elems(path);

    Value cur = set;
    for (int64_t i = 0; i < n; i++) {
        if (asnix_tag_of(cur) != ASNIX_TAG_ATTRS) return deflt;
        Value comp = asnix_force_for_print(comps[i]);
        ASNIX_REQUIRE_STRING(comp, "attrByPath");
        int64_t clen;
        const char *cbytes;
        asnix_bshared_string_bytes(comp, &cbytes, &clen, "attrByPath");
        uint32_t sym = asnix_intern(cbytes, (size_t)clen);
        int64_t idx = asnix_bshared_attrs_find(cur, sym);
        if (idx < 0) return deflt;
        cur = asnix_force_for_print(asnix_bshared_attrs_value(cur, idx));
    }
    return cur;
}
static Value build_attrByPath(void) { return asnix_bshared_make_curried_native((void *)attrByPath3_impl, 3); }

const AsnixBuiltinMiscDef ASNIX_ATTRS2_BUILTINS[] = {
    { "filterAttrs", build_filterAttrs },
    { "listToAttrs", build_listToAttrs },
    { "attrByPath", build_attrByPath },
};

const size_t ASNIX_ATTRS2_BUILTINS_COUNT = sizeof(ASNIX_ATTRS2_BUILTINS) / sizeof(ASNIX_ATTRS2_BUILTINS[0]);

