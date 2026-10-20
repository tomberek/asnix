#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "eval_file.h"
#include "symtab.h"
#include "value_util.h"

static void print_value_inner(Value v) {
    v = asnix_force_for_print(v); /* list elements and attrset values are lazy (M6) */
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT:
            printf("%lld", (long long)asnix_int_value(v));
            return;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: printf("true"); return;
                case ASNIX_IMM_FALSE: printf("false"); return;
                case ASNIX_IMM_NULL: printf("null"); return;
                default: printf("<imm %d>", asnix_imm_subtag(v)); return;
            }
        case ASNIX_TAG_PTR: {
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            /* Paths print bare/unquoted (an absolute filesystem path, not
             * string syntax) -- matches `nix-instantiate --eval`. */
            if (asnix_value_is_path(v)) printf("%.*s", (int)len, bytes);
            else printf("\"%.*s\"", (int)len, bytes);
            return;
        }
        case ASNIX_TAG_LIST: {
            const char *base = (const char *)(v.raw & ~0x7ULL);
            int64_t count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
            printf("[ ");
            for (int64_t i = 0; i < count; i++) {
                print_value_inner(elems[i]);
                printf(" ");
            }
            printf("]");
            return;
        }
        case ASNIX_TAG_ATTRS: {
            /* Entries are stored sorted by symbol id (intern/first-seen
             * order, for OP_SELECT's binary search) -- real Nix prints
             * attrset keys sorted by *name*, which is a different order
             * in general (found by checking against
             * numtide/nix-conformance's eval-okay-attrs6: `{ b=1; a=2; }`
             * printed `{ b = 1; a = 2; }` here vs. Nix's `{ a = 2; b = 1;
             * }`). Re-sort by name for display only; the stored order is
             * untouched. */
            const char *base = (const char *)(v.raw & ~0x7ULL);
            int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
            const char *names[64];
            size_t name_lens[64];
            Value vals[64];
            int64_t n = count > 64 ? 64 : count; /* defensive cap; no attrset in current fixtures is anywhere near this */
            for (int64_t i = 0; i < n; i++) {
                const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
                uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
                names[i] = asnix_sym_name(sym, &name_lens[i]);
                vals[i] = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            }
            for (int64_t i = 1; i < n; i++) {
                int64_t j = i;
                while (j > 0) {
                    size_t minlen = name_lens[j - 1] < name_lens[j] ? name_lens[j - 1] : name_lens[j];
                    int cmp = memcmp(names[j - 1], names[j], minlen);
                    if (cmp == 0) cmp = (int)name_lens[j - 1] - (int)name_lens[j];
                    if (cmp <= 0) break;
                    const char *tn = names[j - 1];
                    names[j - 1] = names[j];
                    names[j] = tn;
                    size_t tl = name_lens[j - 1];
                    name_lens[j - 1] = name_lens[j];
                    name_lens[j] = tl;
                    Value tv = vals[j - 1];
                    vals[j - 1] = vals[j];
                    vals[j] = tv;
                    j--;
                }
            }
            printf("{ ");
            for (int64_t i = 0; i < n; i++) {
                printf("%.*s = ", (int)name_lens[i], names[i]);
                print_value_inner(vals[i]);
                printf("; ");
            }
            printf("}");
            return;
        }
        default:
            printf("<unprintable value, tag=%d>", asnix_tag_of(v));
            return;
    }
}

static void print_value(Value v) {
    print_value_inner(v);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.nix>\n", argv[0]);
        return 1;
    }

    asnix_arena_init(0);

    Value result = asnix_eval_file(argv[1]);
    print_value(result);
    return 0;
}
