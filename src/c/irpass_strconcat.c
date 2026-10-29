#include "asnix/irpass.h"

#include <stdint.h>
#include <stdlib.h>
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

static int fold_adjacent_const_strings(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap) {
    for (size_t i = 0; i + 2 < *len; i++) {
        if ((int)(intptr_t)ops[i].handler != OP_PUSH_CONST) continue;
        if ((int)(intptr_t)ops[i + 1].handler != OP_PUSH_CONST) continue;
        if ((int)(intptr_t)ops[i + 2].handler != OP_STR_CONCAT) continue;
        if (ops[i + 2].a != 2) continue;
        if (range_has_incoming_jump(ops, *len, i + 1, 2)) continue;

        int64_t idx1 = ops[i].b, idx2 = ops[i + 1].b;
        if (idx1 < 0 || (size_t)idx1 >= *pool_len) continue;
        if (idx2 < 0 || (size_t)idx2 >= *pool_len) continue;

        Value v1 = { .raw = (uint64_t)(*pool)[idx1] };
        Value v2 = { .raw = (uint64_t)(*pool)[idx2] };
        if (asnix_tag_of(v1) != ASNIX_TAG_PTR || asnix_value_is_path(v1) || asnix_value_is_float(v1) ||
            asnix_value_is_cell(v1) || asnix_value_is_lazyapp(v1))
            continue;
        if (asnix_tag_of(v2) != ASNIX_TAG_PTR || asnix_value_is_path(v2) || asnix_value_is_float(v2) ||
            asnix_value_is_cell(v2) || asnix_value_is_lazyapp(v2))
            continue;

        int64_t len1, len2;
        const char *b1 = asnix_ptr_bytes(v1, &len1);
        const char *b2 = asnix_ptr_bytes(v2, &len2);

        char *buf = malloc((size_t)(len1 + len2));
        if (buf == NULL) continue;
        memcpy(buf, b1, (size_t)len1);
        memcpy(buf + len1, b2, (size_t)len2);
        Value folded = asnix_make_string_value(buf, (size_t)(len1 + len2));
        free(buf);

        size_t new_idx = asnix_irpass_pool_append(pool, pool_len, pool_cap, (int64_t)folded.raw);
        ops[i].handler = (void *)(intptr_t)OP_PUSH_CONST;
        ops[i].a = 0;
        ops[i].b = (int64_t)new_idx;
        asnix_irpass_remove(ops, len, i + 1, 2);
        return 1;
    }
    return 0;
}

void asnix_irpass_register_strconcat(void) {
    asnix_irpass_register("strconcat", fold_adjacent_const_strings);
}

