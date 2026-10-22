#include "asnix/irpass.h"

#include <stdint.h>

/* Proof-of-concept pass: folds a `OP_PUSH_INT a; OP_PUSH_INT b; OP_<binop>`
 * triple into a single OP_PUSH_INT/OP_PUSH_TRUE/OP_PUSH_FALSE, for every
 * pure int-int binary op this IR has whose result depends only on its two
 * operands (ADD/SUB/MUL/DIV and the 6 ordering/equality comparisons) --
 * compiler.c already constant-folds int-literal `+`/`-`/`*` at the AST
 * level in some cases, but NOT uniformly across every arithmetic/
 * comparison op or through intermediate binding boundaries the AST-level
 * fold doesn't see (e.g. a constant produced by inlining, or by this same
 * pass's own prior iteration landing two now-adjacent constants next to
 * each other) -- this is the bytecode-level backstop, and (since it works
 * on OP_PUSH_INT regardless of how it got there) also now covers any
 * actual source-level `1 + 2` the AST-level fold misses.
 *
 * OP_DIV is skipped whenever the divisor is 0, or the pair is
 * (INT64_MIN, -1) -- both must still reach the runtime op (abort with
 * "division by zero", and avoid signed-overflow UB respectively), not be
 * silently folded away. */
static int fold_constant_int_binop(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool;
    (void)pool_len;
    (void)pool_cap; /* strconcat-fold extension widened AsnixIrPass's signature for every pass
                      * uniformly (see irpass.h) -- this pass folds only int immediates (OP_PUSH_INT's
                      * own operand field, never a pool index), so it has no use for the pool itself. */
    for (size_t i = 0; i + 2 < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_PUSH_INT) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_PUSH_INT) continue;
        int op_id = (int)(intptr_t)ops[i + 2].handler;
        int64_t a = ops[i].a, b = ops[i + 1].a;
        int64_t result = 0;
        int is_bool = 0, ok = 1;
        switch (op_id) {
            case OP_ADD: result = a + b; break;
            case OP_SUB: result = a - b; break;
            case OP_MUL: result = a * b; break;
            case OP_DIV:
                if (b == 0) { ok = 0; break; }
                if (a == INT64_MIN && b == -1) { ok = 0; break; }
                result = a / b; /* C's / already truncates toward zero, matching OP_DIV's own semantics */
                break;
            case OP_EQ: result = (a == b); is_bool = 1; break;
            case OP_NEQ: result = (a != b); is_bool = 1; break;
            case OP_LT: result = (a < b); is_bool = 1; break;
            case OP_GT: result = (a > b); is_bool = 1; break;
            case OP_LE: result = (a <= b); is_bool = 1; break;
            case OP_GE: result = (a >= b); is_bool = 1; break;
            default: ok = 0; break;
        }
        if (!ok) continue;
        if (is_bool) {
            ops[i].handler = (void *)(intptr_t)(result ? OP_PUSH_TRUE : OP_PUSH_FALSE);
            ops[i].a = 0;
        } else {
            ops[i].handler = (void *)(intptr_t)OP_PUSH_INT;
            ops[i].a = result;
        }
        ops[i].b = 0;
        asnix_irpass_remove(ops, len, i + 1, 2);
        return 1; /* re-scan from the top next iteration -- simplest correct strategy */
    }
    return 0;
}

void asnix_irpass_register_cfold(void) {
    asnix_irpass_register("cfold", fold_constant_int_binop);
}
