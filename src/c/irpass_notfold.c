#include "asnix/irpass.h"

#include <stdint.h>

static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static int is_jump_target(IrOp *ops, size_t len, size_t index) {
    for (size_t i = 0; i < len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target == (int64_t)index) return 1;
    }
    return 0;
}

static int fold_bool_not(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    for (size_t i = 0; i + 1 < *len; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        int folded;
        switch (id) {
            case OP_PUSH_TRUE: folded = OP_PUSH_FALSE; break;
            case OP_PUSH_FALSE: folded = OP_PUSH_TRUE; break;
            default: continue;
        }
        if ((int)(intptr_t)ops[i + 1].handler != OP_NOT) continue;
        if (is_jump_target(ops, *len, i + 1)) continue;
        ops[i].handler = (void *)(intptr_t)folded;
        ops[i].a = 0;
        ops[i].b = 0;
        asnix_irpass_remove(ops, len, i + 1, 1);
        return 1;
    }
    return 0;
}

void asnix_irpass_register_notfold(void) {
    asnix_irpass_register("notfold", fold_bool_not);
}

