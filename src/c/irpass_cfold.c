#include "asnix/irpass.h"

#include <stdint.h>

static int fold_constant_int_binop(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool_len;
    (void)pool_cap;
    for (size_t i = 0; i + 2 < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_PUSH_INT) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_PUSH_INT) continue;
        int op_id = (int)(intptr_t)ops[i + 2].handler;
        int64_t a = (*pool)[ops[i].b], b = (*pool)[ops[i + 1].b];
        int64_t result = 0;
        int is_bool = 0, ok = 1;
        switch (op_id) {
            case OP_ADD: if (__builtin_add_overflow(a, b, &result)) ok = 0; break;
            case OP_SUB: if (__builtin_sub_overflow(a, b, &result)) ok = 0; break;
            case OP_MUL: if (__builtin_mul_overflow(a, b, &result)) ok = 0; break;
            case OP_DIV:
                if (b == 0) { ok = 0; break; }
                if (a == INT64_MIN && b == -1) { ok = 0; break; }
                result = a / b;
                break;
            case OP_EQ: result = (a == b); is_bool = 1; break;
            case OP_NEQ: result = (a != b); is_bool = 1; break;
            case OP_LT: result = (a < b); is_bool = 1; break;
            case OP_GT: result = (a > b); is_bool = 1; break;
            case OP_LE: result = (a <= b); is_bool = 1; break;
            case OP_GE: result = (a >= b); is_bool = 1; break;
            default: ok = 0; break;
        }
        if (ok && !is_bool && (result < ASNIX_INT_MIN || result > ASNIX_INT_MAX)) ok = 0;
        if (!ok) continue;
        if (is_bool) {
            ops[i].handler = (void *)(intptr_t)(result ? OP_PUSH_TRUE : OP_PUSH_FALSE);
            ops[i].a = 0;
            ops[i].b = 0;
        } else {
            ops[i].handler = (void *)(intptr_t)OP_PUSH_INT;
            (*pool)[ops[i].b] = result;
            ops[i].a = 0;
        }
        asnix_irpass_remove(ops, len, i + 1, 2);
        return 1;
    }
    return 0;
}

void asnix_irpass_register_cfold(void) {
    asnix_irpass_register("cfold", fold_constant_int_binop);
}

