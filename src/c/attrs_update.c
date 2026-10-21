#include "attrs_update.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "symtab.h"
#include "value_util.h"

Value asnix_attrs_update(Value lhs, Value rhs) {
    const char *abase = (const char *)(lhs.raw & ~0x7ULL);
    const char *bbase = (const char *)(rhs.raw & ~0x7ULL);
    int64_t acount = *(const int64_t *)(abase + ASNIX_ATTRS_OFF_COUNT);
    int64_t bcount = *(const int64_t *)(bbase + ASNIX_ATTRS_OFF_COUNT);
    const char *aentries = abase + ASNIX_ATTRS_OFF_ENTRIES;
    const char *bentries = bbase + ASNIX_ATTRS_OFF_ENTRIES;

    /* Upper bound on the merged count (shrinks below acount+bcount only if
     * there are duplicate keys) -- allocate once for the worst case and
     * just record however many entries actually land, same "don't bother
     * reclaiming the slack" tradeoff the never-free arena already makes
     * everywhere else. */
    void *mem = asnix_arena_alloc(8 + (uint64_t)(acount + bcount) * ASNIX_ATTR_SIZE);
    char *out_entries = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES;

    int64_t i = 0, j = 0, k = 0;
    while (i < acount && j < bcount) {
        uint32_t asym = *(const uint32_t *)(aentries + i * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM);
        uint32_t bsym = *(const uint32_t *)(bentries + j * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM);
        if (asym < bsym) {
            memcpy(out_entries + k * ASNIX_ATTR_SIZE, aentries + i * ASNIX_ATTR_SIZE, ASNIX_ATTR_SIZE);
            i++;
        } else if (bsym < asym) {
            memcpy(out_entries + k * ASNIX_ATTR_SIZE, bentries + j * ASNIX_ATTR_SIZE, ASNIX_ATTR_SIZE);
            j++;
        } else {
            /* duplicate key: rhs (b) wins, matching `//`'s right-biased semantics */
            memcpy(out_entries + k * ASNIX_ATTR_SIZE, bentries + j * ASNIX_ATTR_SIZE, ASNIX_ATTR_SIZE);
            i++;
            j++;
        }
        k++;
    }
    while (i < acount) {
        memcpy(out_entries + k * ASNIX_ATTR_SIZE, aentries + i * ASNIX_ATTR_SIZE, ASNIX_ATTR_SIZE);
        i++;
        k++;
    }
    while (j < bcount) {
        memcpy(out_entries + k * ASNIX_ATTR_SIZE, bentries + j * ASNIX_ATTR_SIZE, ASNIX_ATTR_SIZE);
        j++;
        k++;
    }

    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = k;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

Value asnix_attrs_insert_dynamic(Value attrs, Value name_val, Value value) {
    /* `${null} = value;` is silently omitted, not an error -- a real,
     * deliberate nixpkgs idiom for a conditionally-present key
     * (`${if cond then "key" else null} = value;`), confirmed against
     * both real `nix-instantiate --eval` (`{ ${null} = 1; }` == `{}`,
     * while `{ ${1} = 1; }` IS a hard error) and cppnix's own source
     * (`~/nix/src/libexpr/eval.cc`'s attrset-construction loop: `if
     * (nameVal.type() == nNull) continue;`, checked *before* the
     * string-force/type-check below). Found via d.nix. */
    if (asnix_tag_of(name_val) == ASNIX_TAG_IMM && asnix_imm_subtag(name_val) == ASNIX_IMM_NULL) {
        return attrs;
    }
    if (asnix_tag_of(name_val) != ASNIX_TAG_PTR || asnix_value_is_path(name_val)) {
        fprintf(stderr, "asnix: a dynamic '${...}' attribute name must be a string\n");
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
    int64_t pos = lo; /* insertion point keeping sorted-by-sym order */

    void *mem = asnix_arena_alloc(8 + (uint64_t)(acount + 1) * ASNIX_ATTR_SIZE);
    char *out_entries = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES;
    if (pos > 0) memcpy(out_entries, aentries, (size_t)pos * ASNIX_ATTR_SIZE);
    *(int64_t *)(out_entries + pos * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_SYM) = (int64_t)sym; /* zero-extends,
        matching builtins_shared.c's asnix_bshared_make_attrs -- the sym slot is a full 8 bytes */
    *(Value *)(out_entries + pos * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE) = value;
    if (pos < acount) {
        memcpy(out_entries + (pos + 1) * ASNIX_ATTR_SIZE, aentries + pos * ASNIX_ATTR_SIZE,
               (size_t)(acount - pos) * ASNIX_ATTR_SIZE);
    }
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = acount + 1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}
