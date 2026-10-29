#include "asnix/irpass.h"

#include <stdint.h>
#include <string.h>

#include "value_util.h"

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

static int is_genuine_string(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return 0;
    if (asnix_value_is_path(v)) return 0;
    if (asnix_value_is_float(v)) return 0;
    if (asnix_value_is_cell(v)) return 0;
    if (asnix_value_is_lazyapp(v)) return 0;
    return 1;
}

static int fold_const_string_eq(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    (void)pool_cap;
    for (size_t i = 0; i + 2 < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_PUSH_CONST) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_PUSH_CONST) continue;
        int op_id = (int)(intptr_t)ops[i + 2].handler;
        if (op_id != OP_EQ && op_id != OP_NEQ) continue;
        if (range_has_incoming_jump(ops, *len, i + 1, 2)) continue;

        int64_t idx1 = ops[i].b, idx2 = ops[i + 1].b;
        if (idx1 < 0 || (size_t)idx1 >= *pool_len) continue;
        if (idx2 < 0 || (size_t)idx2 >= *pool_len) continue;

        Value v1 = { .raw = (uint64_t)(*pool)[idx1] };
        Value v2 = { .raw = (uint64_t)(*pool)[idx2] };
        if (!is_genuine_string(v1) || !is_genuine_string(v2)) continue;

        int64_t len1, len2;
        const char *b1 = asnix_ptr_bytes(v1, &len1);
        const char *b2 = asnix_ptr_bytes(v2, &len2);
        int equal = (len1 == len2) && (len1 == 0 || memcmp(b1, b2, (size_t)len1) == 0);
        int result = (op_id == OP_EQ) ? equal : !equal;

        ops[i].handler = (void *)(intptr_t)(result ? OP_PUSH_TRUE : OP_PUSH_FALSE);
        ops[i].a = 0;
        ops[i].b = 0;
        asnix_irpass_remove(ops, len, i + 1, 2);
        return 1;
    }
    return 0;
}

void asnix_irpass_register_streq(void) {
    asnix_irpass_register("streq", fold_const_string_eq);
}

