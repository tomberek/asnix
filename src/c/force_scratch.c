/* Per-depth scratch-buffer cache backing src/asm/force.S's
 * asnix_force_for_print -- see include/asnix/force_scratch.h's header
 * comment for why this exists and why indexing by depth (rather than a
 * single cached slot) is both correct and necessary in practice. */
#include "asnix/force_scratch.h"

#include <stdlib.h>

#include "asnix/arena.h"

typedef struct {
    void *valstack;
    void *retstack;
} ForceScratchSlot;

static ForceScratchSlot *g_slots = NULL;
static size_t g_cap = 0;

/* Same doubling-growth convention as every other dynamic array in this
 * codebase (e.g. eval_file.c's g_import_cache) -- grows to at least
 * `depth + 1` slots, zeroing the newly added ones so a not-yet-cached
 * slot's pointers read back as NULL. */
static void ensure_cap(size_t depth) {
    if (depth < g_cap) return;
    size_t new_cap = g_cap ? g_cap * 2 : 8;
    while (new_cap <= depth) new_cap *= 2;
    g_slots = realloc(g_slots, new_cap * sizeof(ForceScratchSlot));
    for (size_t i = g_cap; i < new_cap; i++) {
        g_slots[i].valstack = NULL;
        g_slots[i].retstack = NULL;
    }
    g_cap = new_cap;
}

void *asnix_force_scratch_valstack(int64_t depth) {
    ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_slots[depth];
    if (!slot->valstack) slot->valstack = asnix_arena_alloc(ASNIX_FORCE_VALSTACK_BYTES);
    return slot->valstack;
}

void *asnix_force_scratch_retstack(int64_t depth) {
    ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_slots[depth];
    if (!slot->retstack) slot->retstack = asnix_arena_alloc(ASNIX_FORCE_RETSTACK_BYTES);
    return slot->retstack;
}
