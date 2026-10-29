#include "asnix/irpass.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int dce_op_is_jump(int op_id) {
    return op_id == OP_IF || op_id == OP_JMP || op_id == OP_WITH_TRY || op_id == OP_SELECT_OR ||
           op_id == OP_SELECT_DYNAMIC_OR || op_id == OP_EQ_IF || op_id == OP_NEQ_IF || op_id == OP_LT_IF ||
           op_id == OP_GE_IF || op_id == OP_HAS_ATTR_IF;
}

static void dce_mark_reachable(const IrOp *ops, size_t n, unsigned char *reachable, size_t *stack) {
    size_t sp = 0;
    if (n == 0) return;
    reachable[0] = 1;
    stack[sp++] = 0;
    while (sp > 0) {
        size_t i = stack[--sp];
        int id = (int)(intptr_t)ops[i].handler;
        int is_terminator = (id == OP_RET || id == OP_HALT);
        int is_unconditional_jump = (id == OP_JMP);
        if (!is_terminator && !is_unconditional_jump) {
            size_t nxt = i + 1;
            if (nxt < n && !reachable[nxt]) {
                reachable[nxt] = 1;
                stack[sp++] = nxt;
            }
        }
        if (dce_op_is_jump(id)) {
            int64_t target = (int64_t)i + ops[i].a / ASNIX_IROP_SIZE;
            if (target >= 0 && (size_t)target < n && !reachable[(size_t)target]) {
                reachable[(size_t)target] = 1;
                stack[sp++] = (size_t)target;
            }
        }
    }
}

static int dead_code_eliminate(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap;
    size_t n = *len;
    if (n == 0) return 0;

    unsigned char *reachable = calloc(n, 1);
    size_t *stack = malloc(n * sizeof(size_t));
    if (!reachable || !stack) {
        fprintf(stderr, "dce: out of memory\n");
        exit(1);
    }
    dce_mark_reachable(ops, n, reachable, stack);
    free(stack);

    int removed_any = 0;
    size_t removed_so_far = 0;
    for (size_t orig_i = 0; orig_i < n; orig_i++) {
        if (reachable[orig_i]) continue;
        asnix_irpass_remove(ops, len, orig_i - removed_so_far, 1);
        removed_so_far++;
        removed_any = 1;
    }
    free(reachable);
    return removed_any;
}

void asnix_irpass_register_dce(void) {
    asnix_irpass_register("dce", dead_code_eliminate);
}

