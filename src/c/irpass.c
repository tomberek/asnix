#include "asnix/irpass.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ASNIX_IRPASS_MAX_PASSES 32
static const char *g_names[ASNIX_IRPASS_MAX_PASSES];
static AsnixIrPass g_passes[ASNIX_IRPASS_MAX_PASSES];
static uint64_t g_fire_counts[ASNIX_IRPASS_MAX_PASSES];
static size_t g_npasses = 0;

/* Which pass is currently executing, for asnix_irpass_remove's firing-log
 * attribution below -- set by asnix_irpass_run just before calling each
 * g_passes[p], valid only during that call. */
static size_t g_current_pass = (size_t)-1;

void asnix_irpass_register(const char *name, AsnixIrPass pass) {
    if (g_npasses >= ASNIX_IRPASS_MAX_PASSES) {
        fprintf(stderr, "asnix_irpass_register: too many passes registered\n");
        exit(1);
    }
    g_names[g_npasses] = name;
    g_passes[g_npasses] = pass;
    g_fire_counts[g_npasses] = 0;
    g_npasses++;
}

void asnix_irpass_clear(void) {
    g_npasses = 0;
}

size_t asnix_irpass_num_passes(void) {
    return g_npasses;
}

const char *asnix_irpass_name(size_t index) {
    return g_names[index];
}

uint64_t asnix_irpass_fire_count(size_t index) {
    return g_fire_counts[index];
}

/* --- Firing-location log: WHERE each pass fired, not just how many times.
 *
 * Recorded by asnix_irpass_remove (the one shrink primitive every
 * existing pass uses) at the index the kept/folded op ends up at --
 * start-1 by convention, since every pass so far follows "rewrite ops[i]
 * in place, then asnix_irpass_remove(ops, len, i+1, count)" (see
 * irpass_cfold.c). A pass that removes a range with no "kept predecessor"
 * (e.g. a pure dead-code deletion) still gets a location logged at
 * start-1 (clamped to 0) -- less precise for that shape, but still a
 * useful "around here" marker, which is this feature's stated bar.
 *
 * Entries are kept valid against the CURRENT ops array at all times: every
 * later asnix_irpass_remove call also shifts any already-logged op_index
 * >= the newly-removed range, by the same amount it shifts jump targets
 * below -- so by the time a compile finishes, every logged index is still
 * correct for the FINAL ops array irdump actually prints. Not disambiguated
 * across separate IrPrograms (root vs. a nested lambda/thunk body each get
 * their own ib_finish() call, but share this one global log/index space) --
 * best-effort for nested cases, exact for a single flat program. */
#define ASNIX_IRPASS_MAX_FIRINGS 4096
typedef struct {
    size_t pass_index;
    size_t op_index;
} AsnixIrFiring;
static AsnixIrFiring g_firings[ASNIX_IRPASS_MAX_FIRINGS];
static size_t g_nfirings = 0;

void asnix_irpass_reset_stats(void) {
    for (size_t i = 0; i < g_npasses; i++) g_fire_counts[i] = 0;
    g_nfirings = 0;
}

void asnix_irpass_print_stats(FILE *out) {
    if (g_npasses == 0) return;
    int any = 0;
    for (size_t i = 0; i < g_npasses; i++) {
        if (g_fire_counts[i] == 0) continue;
        fprintf(out, "%s: %lu firings\n", g_names[i], (unsigned long)g_fire_counts[i]);
        any = 1;
    }
    if (!any) fprintf(out, "(no passes fired)\n");
}

size_t asnix_irpass_num_firings(void) {
    return g_nfirings;
}

const char *asnix_irpass_firing_pass_name(size_t i) {
    return g_names[g_firings[i].pass_index];
}

size_t asnix_irpass_firing_pass_index(size_t i) {
    return g_firings[i].pass_index;
}

size_t asnix_irpass_firing_op_index(size_t i) {
    return g_firings[i].op_index;
}

/* Which op-ids carry a byte-delta-to-target in field `a` -- see ir.h's own
 * per-op comments. Exhaustive: nothing else in OP_COUNT's enum uses a
 * self-relative index delta. */
static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR;
}

void asnix_irpass_remove(IrOp *ops, size_t *len, size_t start, size_t count) {
    size_t old_len = *len;
    if (count == 0) return;
    if (start + count > old_len) {
        fprintf(stderr, "asnix_irpass_remove: range [%zu,%zu) out of bounds for len=%zu\n", start, start + count,
                old_len);
        exit(1);
    }
    for (size_t i = 0; i < old_len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target >= (int64_t)start && target < (int64_t)(start + count)) {
            fprintf(stderr,
                    "asnix_irpass_remove: op %zu jumps into the removed range [%zu,%zu) -- "
                    "a pass may only remove self-contained, unreferenced sequences\n",
                    i, start, start + count);
            exit(1);
        }
    }
    memmove(&ops[start], &ops[start + count], (old_len - start - count) * sizeof(IrOp));
    size_t new_len = old_len - count;
    /* Re-point every jump whose own position or target shifted. Op i's new
     * position maps back to old position (i < start ? i : i + count) --
     * exactly the inverse of the memmove above -- so that's how to recover
     * each jump's OLD absolute target for remapping. */
    for (size_t i = 0; i < new_len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        size_t old_i = (i < start) ? i : i + count;
        int64_t old_target = (int64_t)old_i + ops[i].a / ASNIX_IROP_SIZE;
        int64_t new_target = (old_target < (int64_t)start) ? old_target : old_target - (int64_t)count;
        ops[i].a = (new_target - (int64_t)i) * ASNIX_IROP_SIZE;
    }
    /* Keep every already-logged firing location valid against the new,
     * shorter array -- same shift rule as jump targets just above (an
     * index at/after the removed range moves back by `count`; one before
     * it is untouched). */
    for (size_t i = 0; i < g_nfirings; i++) {
        size_t idx = g_firings[i].op_index;
        if (idx >= start + count) g_firings[i].op_index = idx - count;
        else if (idx >= start) g_firings[i].op_index = start > 0 ? start - 1 : 0; /* was inside the removed range itself */
    }
    /* Log this firing: by convention (irpass_cfold.c and friends) the
     * kept/folded result lives at start-1; clamp to 0 for a pass that
     * removes starting at index 0 with no predecessor. */
    if (g_current_pass != (size_t)-1 && g_nfirings < ASNIX_IRPASS_MAX_FIRINGS) {
        g_firings[g_nfirings].pass_index = g_current_pass;
        g_firings[g_nfirings].op_index = start > 0 ? start - 1 : 0;
        g_nfirings++;
    }
    *len = new_len;
}

int asnix_irpass_run(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    int any_changed_ever = 0;
    for (int iter = 0; iter < ASNIX_IRPASS_MAX_ITERS; iter++) {
        int changed = 0;
        for (size_t p = 0; p < g_npasses; p++) {
            g_current_pass = p;
            int fired = g_passes[p](ops, len, pool, pool_len, pool_cap);
            g_current_pass = (size_t)-1;
            if (fired) {
                g_fire_counts[p]++;
                changed = 1;
            }
        }
        if (!changed) break;
        any_changed_ever = 1;
    }
    return any_changed_ever;
}

size_t asnix_irpass_pool_append(int64_t **pool, size_t *pool_len, size_t *pool_cap, int64_t v) {
    if (*pool_len == *pool_cap) {
        *pool_cap = *pool_cap ? *pool_cap * 2 : 8;
        *pool = realloc(*pool, *pool_cap * sizeof(int64_t));
    }
    (*pool)[*pool_len] = v;
    return (*pool_len)++;
}
