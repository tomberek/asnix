#include "asnix/irpass.h"

#include <stdint.h>

static int op_is_jump(int id) {
    return id == OP_IF || id == OP_JMP || id == OP_WITH_TRY || id == OP_SELECT_OR ||
           id == OP_SELECT_DYNAMIC_OR || id == OP_EQ_IF || id == OP_NEQ_IF || id == OP_LT_IF ||
           id == OP_GE_IF || id == OP_HAS_ATTR_IF;
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

static int remove_noop_jmp(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    for (size_t i = 0; i < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_JMP) continue;
        if (ops[i].a != (int64_t)ASNIX_IROP_SIZE) continue;
        if (is_targeted_by_another_jump(ops, *len, i)) continue;
        asnix_irpass_remove(ops, len, i, 1);
        return 1;
    }
    return 0;
}

void asnix_irpass_register_jmpnop(void) {
    asnix_irpass_register("jmpnop", remove_noop_jmp);
}

