#include "attrs_update.h"

#include <stdint.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"

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
