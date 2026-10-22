#include "asnix/irpass.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASNIX_IRPASS_MAX_PASSES 16
static AsnixIrPass g_passes[ASNIX_IRPASS_MAX_PASSES];
static size_t g_npasses = 0;

void asnix_irpass_register(AsnixIrPass pass) {
    if (g_npasses >= ASNIX_IRPASS_MAX_PASSES) {
        fprintf(stderr, "asnix_irpass_register: too many passes registered\n");
        exit(1);
    }
    g_passes[g_npasses++] = pass;
}

void asnix_irpass_clear(void) {
    g_npasses = 0;
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
    *len = new_len;
}

int asnix_irpass_run(IrOp *ops, size_t *len) {
    int any_changed_ever = 0;
    for (int iter = 0; iter < ASNIX_IRPASS_MAX_ITERS; iter++) {
        int changed = 0;
        for (size_t p = 0; p < g_npasses; p++) {
            if (g_passes[p](ops, len)) changed = 1;
        }
        if (!changed) break;
        any_changed_ever = 1;
    }
    return any_changed_ever;
}
