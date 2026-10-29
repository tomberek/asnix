#include "asnix/irpass.h"

#include <stdint.h>

static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static int range_has_incoming_jump(IrOp *ops, size_t len, size_t start, size_t count) {
    for (size_t i = 0; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target >= (int64_t)start && target < (int64_t)(start + count)) return 1;
    }
    return 0;
}

static int dce_if(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    size_t n = *len;
    for (size_t i = 0; i + 1 < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (id != OP_PUSH_TRUE && id != OP_PUSH_FALSE) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_IF) continue;

        if (id == OP_PUSH_TRUE) {
            if (range_has_incoming_jump(ops, n, i, 2)) continue;
            asnix_irpass_remove(ops, len, i, 2);
            return 1;
        }

        int64_t else_start = (int64_t)(i + 1) + ops[i + 1].a / ASNIX_IROP_SIZE;
        if (else_start <= (int64_t)(i + 1)) continue;
        size_t count = (size_t)else_start - i;
        if (range_has_incoming_jump(ops, n, i, count)) continue;
        asnix_irpass_remove(ops, len, i, count);
        return 1;
    }
    return 0;
}

void asnix_irpass_register_dce_if(void) {
    asnix_irpass_register("dce_if", dce_if);
}

