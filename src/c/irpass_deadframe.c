#include "asnix/irpass.h"

#include <stdint.h>

static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static int is_targeted_by_another_jump(IrOp *ops, size_t len, size_t idx) {
    for (size_t j = 0; j < len; j++) {
        if (j == idx) continue;
        int id = (int)(intptr_t)ops[j].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)j + ops[j].a / (int64_t)ASNIX_IROP_SIZE;
        if (target == (int64_t)idx) return 1;
    }
    return 0;
}

static int op_is_var_ref_family(int id) {
    return id == OP_VAR_REF || id == OP_VAR_REF_CELL || id == OP_VAR_REF_RAW;
}

static int op_touches_innermost_frame_slots(int id) {
    return id == OP_FRAME_SET || id == OP_FRAME_GET || id == OP_CHECK_FORMALS || id == OP_REC_OVERRIDES_MERGE;
}

static size_t find_matching_frame_exit(IrOp *ops, size_t len, size_t alloc_idx) {
    int depth = 1;
    for (size_t i = alloc_idx + 1; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (id == OP_FRAME_ALLOC) {
            depth++;
        } else if (id == OP_FRAME_EXIT) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return (size_t)-1;
}

static int range_is_safe_to_elide(IrOp *ops, size_t i, size_t j) {
    int local_nest = 0;
    for (size_t k = i + 1; k < j; k++) {
        int id = (int)(intptr_t)ops[k].handler;
        if (id == OP_FRAME_ALLOC || id == OP_WITH) {
            local_nest++;
            continue;
        }
        if (id == OP_FRAME_EXIT || id == OP_WITH_EXIT) {
            local_nest--;
            continue;
        }
        if (op_touches_innermost_frame_slots(id)) {
            if (local_nest == 0) return 0;
            continue;
        }
        if (op_is_var_ref_family(id)) {
            if (ops[k].a == (int64_t)local_nest) return 0;
            continue;
        }
    }
    return 1;
}

static void apply_depth_fixup(IrOp *ops, size_t i, size_t j) {
    int local_nest = 0;
    for (size_t k = i + 1; k < j; k++) {
        int id = (int)(intptr_t)ops[k].handler;
        if (id == OP_FRAME_ALLOC || id == OP_WITH) {
            local_nest++;
            continue;
        }
        if (id == OP_FRAME_EXIT || id == OP_WITH_EXIT) {
            local_nest--;
            continue;
        }
        if (op_is_var_ref_family(id) && ops[k].a > (int64_t)local_nest) {
            ops[k].a -= 1;
        }
    }
}

static int eliminate_dead_zero_frame(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    size_t n = *len;
    for (size_t i = 0; i + 1 < n; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_FRAME_ALLOC) continue;
        if (ops[i].a != 0) continue;

        size_t j = find_matching_frame_exit(ops, n, i);
        if (j == (size_t)-1) continue;
        if (!range_is_safe_to_elide(ops, i, j)) continue;
        if (is_targeted_by_another_jump(ops, n, i)) continue;
        if (is_targeted_by_another_jump(ops, n, j)) continue;

        apply_depth_fixup(ops, i, j);
        asnix_irpass_remove(ops, len, j, 1);
        asnix_irpass_remove(ops, len, i, 1);
        return 1;
    }
    return 0;
}

void asnix_irpass_register_deadframe(void) {
    asnix_irpass_register("deadframe", eliminate_dead_zero_frame);
}

