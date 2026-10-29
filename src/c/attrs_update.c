#include "attrs_update.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/stats.h"
#include "builtins_shared.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

#define ASNIX_UNION_RHS_MAX 16
#define ASNIX_UNION_LHS_MIN 16

Value asnix_attrs_eager_merge(Value lhs, Value rhs) {
    const char *abase = (const char *)(lhs.raw & ~0x7ULL);
    const char *bbase = (const char *)(rhs.raw & ~0x7ULL);
    int64_t acount = *(const int64_t *)(abase + ASNIX_ATTRS_OFF_COUNT);
    int64_t bcount = *(const int64_t *)(bbase + ASNIX_ATTRS_OFF_COUNT);
    const char *aentries = abase + ASNIX_ATTRS_OFF_ENTRIES;
    const char *bentries = bbase + ASNIX_ATTRS_OFF_ENTRIES;

    void *mem = asnix_arena_alloc(8 + (uint64_t)(acount + bcount) * ASNIX_ATTR_SIZE);
    char *out_entries = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES;

    const char *pa = aentries;
    const char *pb = bentries;
    const char *aend = aentries + acount * ASNIX_ATTR_SIZE;
    const char *bend = bentries + bcount * ASNIX_ATTR_SIZE;
    char *pout = out_entries;
    while (pa < aend && pb < bend) {
        uint32_t asym = *(const uint32_t *)(pa + ASNIX_ATTR_OFF_SYM);
        uint32_t bsym = *(const uint32_t *)(pb + ASNIX_ATTR_OFF_SYM);
        if (asym < bsym) {
            memcpy(pout, pa, ASNIX_ATTR_SIZE);
            pa += ASNIX_ATTR_SIZE;
        } else if (bsym < asym) {
            memcpy(pout, pb, ASNIX_ATTR_SIZE);
            pb += ASNIX_ATTR_SIZE;
        } else {
            memcpy(pout, pb, ASNIX_ATTR_SIZE);
            pa += ASNIX_ATTR_SIZE;
            pb += ASNIX_ATTR_SIZE;
        }
        pout += ASNIX_ATTR_SIZE;
    }
    while (pa < aend) {
        memcpy(pout, pa, ASNIX_ATTR_SIZE);
        pa += ASNIX_ATTR_SIZE;
        pout += ASNIX_ATTR_SIZE;
    }
    while (pb < bend) {
        memcpy(pout, pb, ASNIX_ATTR_SIZE);
        pb += ASNIX_ATTR_SIZE;
        pout += ASNIX_ATTR_SIZE;
    }

    int64_t k = (pout - out_entries) / ASNIX_ATTR_SIZE;
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = k;
    asnix_stats_attrs++;
    asnix_stats_attrs_entries += (uint64_t)k;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

static Value make_union_header(const Value *base_leaves, int64_t n_base_leaves, Value new_leaf) {
    int64_t nleaves = n_base_leaves + 1;
    void *mem = asnix_arena_alloc(ASNIX_ATTRS_UNION_OFF_LEAVES + (uint64_t)nleaves * sizeof(Value));
    *(int64_t *)((char *)mem + ASNIX_ATTRS_UNION_OFF_NLEAVES) = -nleaves;
    *(Value *)((char *)mem + ASNIX_ATTRS_UNION_OFF_FLATCACHE) = (Value){ .raw = 0 };
    Value *out_leaves = (Value *)((char *)mem + ASNIX_ATTRS_UNION_OFF_LEAVES);
    if (n_base_leaves > 0) memcpy(out_leaves, base_leaves, (size_t)n_base_leaves * sizeof(Value));
    out_leaves[n_base_leaves] = new_leaf;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

Value asnix_attrs_update(Value lhs, Value rhs) {
    Value rhs_flat = asnix_bshared_attrs_resolve(rhs);
    int64_t rhs_count = *(const int64_t *)((rhs_flat.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_COUNT);

    const char *lhs_base = (const char *)(lhs.raw & ~0x7ULL);
    int64_t lhs_raw = *(const int64_t *)(lhs_base + ASNIX_ATTRS_OFF_COUNT);
    int lhs_is_union = lhs_raw < 0;
    int64_t lhs_leaves = lhs_is_union ? -lhs_raw : 1;

    if (rhs_count <= ASNIX_UNION_RHS_MAX && lhs_leaves < ASNIX_ATTRS_UNION_MAX_LEAVES
        && (lhs_is_union || lhs_raw >= ASNIX_UNION_LHS_MIN)) {
        if (lhs_is_union) {
            const Value *existing_leaves = (const Value *)(lhs_base + ASNIX_ATTRS_UNION_OFF_LEAVES);
            return make_union_header(existing_leaves, lhs_leaves, rhs_flat);
        }
        return make_union_header(&lhs, 1, rhs_flat);
    }

    Value lhs_flat = asnix_bshared_attrs_resolve(lhs);
    return asnix_attrs_eager_merge(lhs_flat, rhs_flat);
}

Value asnix_attrs_insert_dynamic(Value attrs, Value name_val, Value value) {
    attrs = asnix_bshared_attrs_resolve(attrs);
    if (asnix_tag_of(name_val) == ASNIX_TAG_IMM && asnix_imm_subtag(name_val) == ASNIX_IMM_NULL) {
        return attrs;
    }
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val) || asnix_value_is_float(name_val)) {
        fprintf(stderr, "asnix: a dynamic '${...}' attribute name must be a string (got %s)\n",
                asnix_bshared_type_name(name_val));
        exit(1);
    }
    const AsnixStringContext *name_ctx = asnix_string_context_of(name_val);
    if (!asnix_ctx_is_empty(name_ctx)) {
        fprintf(stderr, "asnix: the string used as a dynamic '${...}' attribute name is not allowed to refer to a "
                        "store path\n");
        exit(1);
    }
    int64_t name_len;
    const char *name_bytes = asnix_ptr_bytes(name_val, &name_len);
    uint32_t sym = asnix_intern(name_bytes, (size_t)name_len);

    const char *abase = (const char *)(attrs.raw & ~0x7ULL);
    int64_t acount = *(const int64_t *)(abase + ASNIX_ATTRS_OFF_COUNT);
    const char *aentries = abase + ASNIX_ATTRS_OFF_ENTRIES;

    int64_t lo = 0, hi = acount;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t s = *(const uint32_t *)(aentries + mid * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM);
        if (s == sym) {
            fprintf(stderr, "asnix: dynamic attribute '%.*s' already defined, aborting\n", (int)name_len,
                    name_bytes);
            exit(1);
        }
        if (s < sym) lo = mid + 1; else hi = mid;
    }
    int64_t pos = lo;

    void *mem = asnix_arena_alloc(8 + (uint64_t)(acount + 1) * ASNIX_ATTR_SIZE);
    char *out_entries = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES;
    if (pos > 0) memcpy(out_entries, aentries, (size_t)pos * ASNIX_ATTR_SIZE);
    *(uint32_t *)(out_entries + pos * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM) = sym;
    *(Value *)(out_entries + pos * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE) = value;
    if (pos < acount) {
        memcpy(out_entries + (pos + 1) * ASNIX_ATTR_SIZE, aentries + pos * ASNIX_ATTR_SIZE,
               (size_t)(acount - pos) * ASNIX_ATTR_SIZE);
    }
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = acount + 1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

