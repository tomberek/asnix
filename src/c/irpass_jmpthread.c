#include "asnix/irpass.h"

#include <stdint.h>

static int op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static int thread_jumps(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    size_t n = *len;
    size_t hop_limit = n + 1;
    for (size_t i = 0; i < n; i++) {
        int id = (int)(intptr_t)ops[i].handler;
        if (!op_is_jump(id)) continue;

        int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
        if (target < 0 || (size_t)target >= n) continue;
        if ((int)(intptr_t)ops[(size_t)target].handler != OP_JMP) continue;

        int64_t cur = target;
        size_t hops = 0;
        while ((int)(intptr_t)ops[(size_t)cur].handler == OP_JMP && hops < hop_limit) {
            int64_t next = cur + ops[(size_t)cur].a / ASNIX_IROP_SIZE;
            if (next == cur) break;
            if (next < 0 || (size_t)next >= n) break;
            cur = next;
            hops++;
        }
        if (hops >= hop_limit) continue;
        if (cur == target) continue;

        ops[i].a = (cur - (int64_t)i) * ASNIX_IROP_SIZE;
        return 1;
    }
    return 0;
}

void asnix_irpass_register_jmpthread(void) {
    asnix_irpass_register("jmpthread", thread_jumps);
}

