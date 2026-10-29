#include "asnix/force_scratch.h"

#include <stdlib.h>

#include "asnix/arena.h"

typedef struct {
    void *valstack;
    void *retstack;
} ForceScratchSlot;

static ForceScratchSlot *g_slots = NULL;
static size_t g_cap = 0;

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

static ForceScratchSlot *g_apply_slots = NULL;
static size_t g_apply_cap = 0;

static void apply_ensure_cap(size_t depth) {
    if (depth < g_apply_cap) return;
    size_t new_cap = g_apply_cap ? g_apply_cap * 2 : 8;
    while (new_cap <= depth) new_cap *= 2;
    g_apply_slots = realloc(g_apply_slots, new_cap * sizeof(ForceScratchSlot));
    for (size_t i = g_apply_cap; i < new_cap; i++) {
        g_apply_slots[i].valstack = NULL;
        g_apply_slots[i].retstack = NULL;
    }
    g_apply_cap = new_cap;
}

void *asnix_apply_scratch_valstack(int64_t depth) {
    apply_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_apply_slots[depth];
    if (!slot->valstack) slot->valstack = asnix_arena_alloc(ASNIX_FORCE_VALSTACK_BYTES);
    return slot->valstack;
}

void *asnix_apply_scratch_retstack(int64_t depth) {
    apply_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_apply_slots[depth];
    if (!slot->retstack) slot->retstack = asnix_arena_alloc(ASNIX_FORCE_RETSTACK_BYTES);
    return slot->retstack;
}

static ForceScratchSlot *g_eval_slots = NULL;
static size_t g_eval_cap = 0;

static void eval_ensure_cap(size_t depth) {
    if (depth < g_eval_cap) return;
    size_t new_cap = g_eval_cap ? g_eval_cap * 2 : 8;
    while (new_cap <= depth) new_cap *= 2;
    g_eval_slots = realloc(g_eval_slots, new_cap * sizeof(ForceScratchSlot));
    for (size_t i = g_eval_cap; i < new_cap; i++) {
        g_eval_slots[i].valstack = NULL;
        g_eval_slots[i].retstack = NULL;
    }
    g_eval_cap = new_cap;
}

void *asnix_eval_scratch_valstack(int64_t depth) {
    eval_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_eval_slots[depth];
    if (!slot->valstack) slot->valstack = asnix_arena_alloc(ASNIX_EVAL_VALSTACK_BYTES);
    return slot->valstack;
}

void *asnix_eval_scratch_retstack(int64_t depth) {
    eval_ensure_cap((size_t)depth);
    ForceScratchSlot *slot = &g_eval_slots[depth];
    if (!slot->retstack) slot->retstack = asnix_arena_alloc(ASNIX_EVAL_RETSTACK_BYTES);
    return slot->retstack;
}

