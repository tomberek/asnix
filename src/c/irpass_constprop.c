#include "asnix/irpass.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int constprop_op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static int constprop_is_const_push(int op_id) {
    return op_id == OP_PUSH_INT || op_id == OP_PUSH_TRUE || op_id == OP_PUSH_FALSE || op_id == OP_PUSH_NULL;
}

#define ASNIX_CONSTPROP_MAX_SLOTS 256

typedef struct {
    int64_t level;
    int32_t kind;
    int64_t value;
} ConstSlot;

static int constprop_run(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    size_t n = *len;
    if (n == 0) return 0;

    unsigned char *is_target = calloc(n, 1);
    if (!is_target) return 0;
    for (size_t i = 0; i < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!constprop_op_is_jump(id)) continue;
        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target >= 0 && (size_t)target < n) is_target[(size_t)target] = 1;
    }

    ConstSlot slots[ASNIX_CONSTPROP_MAX_SLOTS];
    for (size_t s = 0; s < ASNIX_CONSTPROP_MAX_SLOTS; s++) slots[s].level = -1;

    int fired = 0;
    int64_t level = 0;

    for (size_t i = 0; i < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;

        if (id == OP_FRAME_ALLOC || id == OP_WITH) {
            level++;
            continue;
        }
        if (id == OP_FRAME_EXIT || id == OP_WITH_EXIT) {
            if (level > 0) level--;
            continue;
        }
        if (id == OP_FRAME_SET) {
            int32_t s = ops[i].a;
            if (s >= 0 && s < ASNIX_CONSTPROP_MAX_SLOTS) {
                int prev_id = (i > 0) ? (int)(intptr_t)ops[i - 1].handler : -1;
                if (i > 0 && !is_target[i] && !is_target[i - 1] && constprop_is_const_push(prev_id)) {
                    slots[s].level = level;
                    slots[s].kind = prev_id;
                    slots[s].value = (prev_id == OP_PUSH_INT) ? (*pool)[ops[i - 1].b] : 0;
                } else {
                    slots[s].level = -1;
                }
            }
            continue;
        }
        if (id == OP_VAR_REF_CELL) {
            int32_t depth = ops[i].a, s = ops[i].b;
            if (depth == 0 && s >= 0 && s < ASNIX_CONSTPROP_MAX_SLOTS) slots[s].level = -1;
            continue;
        }
        if (id == OP_VAR_REF) {
            int32_t depth = ops[i].a, s = ops[i].b;
            if (depth == 0 && s >= 0 && s < ASNIX_CONSTPROP_MAX_SLOTS && slots[s].level == level && !is_target[i]) {
                int32_t kind = slots[s].kind;
                ops[i].handler = (void *)(intptr_t)kind;
                ops[i].a = 0;
                ops[i].b = (kind == OP_PUSH_INT)
                    ? (int32_t)asnix_irpass_pool_append(pool, pool_len, pool_cap, slots[s].value)
                    : 0;
                fired = 1;
            }
            continue;
        }
    }

    free(is_target);
    return fired;
}

void asnix_irpass_register_constprop(void) {
    asnix_irpass_register("constprop", constprop_run);
}

