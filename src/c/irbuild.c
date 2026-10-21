#include "asnix/irbuild.h"

#include <stdlib.h>

IrBuilder *ib_new(void) {
    IrBuilder *b = calloc(1, sizeof(IrBuilder));
    return b;
}

size_t ib_len(IrBuilder *b) {
    return b->len;
}

static size_t ib_emit(IrBuilder *b, int op_id, int64_t a, int64_t bval) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 16;
        b->ops = realloc(b->ops, b->cap * sizeof(IrOp));
    }
    b->ops[b->len].handler = (void *)(intptr_t)op_id;
    b->ops[b->len].a = a;
    b->ops[b->len].b = bval;
    return b->len++;
}

size_t ib_push_int(IrBuilder *b, int64_t v) {
    return ib_emit(b, OP_PUSH_INT, v, 0);
}
size_t ib_push_true(IrBuilder *b) {
    return ib_emit(b, OP_PUSH_TRUE, 0, 0);
}
size_t ib_push_false(IrBuilder *b) {
    return ib_emit(b, OP_PUSH_FALSE, 0, 0);
}
size_t ib_push_null(IrBuilder *b) {
    return ib_emit(b, OP_PUSH_NULL, 0, 0);
}
size_t ib_var_ref(IrBuilder *b, int64_t depth, int64_t slot) {
    return ib_emit(b, OP_VAR_REF, depth, slot);
}
size_t ib_add(IrBuilder *b) {
    return ib_emit(b, OP_ADD, 0, 0);
}
size_t ib_sub(IrBuilder *b) {
    return ib_emit(b, OP_SUB, 0, 0);
}
size_t ib_mul(IrBuilder *b) {
    return ib_emit(b, OP_MUL, 0, 0);
}
size_t ib_if(IrBuilder *b) {
    return ib_emit(b, OP_IF, 0, 0);
}
size_t ib_jmp(IrBuilder *b) {
    return ib_emit(b, OP_JMP, 0, 0);
}

void ib_patch_delta(IrBuilder *b, size_t site, size_t target_idx) {
    b->ops[site].a = (int64_t)(target_idx - site) * ASNIX_IROP_SIZE;
}

size_t ib_let(IrBuilder *b, int64_t nslots) {
    return ib_emit(b, OP_LET, nslots, 0);
}

static size_t ib_pool_add(IrBuilder *b, int64_t v) {
    if (b->pool_len == b->pool_cap) {
        b->pool_cap = b->pool_cap ? b->pool_cap * 2 : 8;
        b->pool = realloc(b->pool, b->pool_cap * sizeof(int64_t));
    }
    b->pool[b->pool_len] = v;
    return b->pool_len++;
}

size_t ib_lambda(IrBuilder *b, int64_t arity, IrProgram *body_prog, int64_t formals_raw) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)body_prog->ops);
    ib_pool_add(b, (int64_t)(intptr_t)body_prog->pool); /* always pool_idx+1, see op_lambda.S */
    ib_pool_add(b, formals_raw); /* always pool_idx+2, see op_lambda.S */
    return ib_emit(b, OP_LAMBDA, arity, (int64_t)pool_idx);
}

size_t ib_call(IrBuilder *b, int64_t argc) {
    return ib_emit(b, OP_CALL, argc, 0);
}
size_t ib_ret(IrBuilder *b) {
    return ib_emit(b, OP_RET, 0, 0);
}
size_t ib_halt(IrBuilder *b) {
    return ib_emit(b, OP_HALT, 0, 0);
}

size_t ib_push_const(IrBuilder *b, int64_t raw_value) {
    size_t pool_idx = ib_pool_add(b, raw_value);
    return ib_emit(b, OP_PUSH_CONST, 0, (int64_t)pool_idx);
}

size_t ib_mk_list(IrBuilder *b, int64_t count) {
    return ib_emit(b, OP_MK_LIST, count, 0);
}

size_t ib_mk_attrs(IrBuilder *b, int64_t count, const uint32_t *sorted_syms) {
    size_t pool_idx = (size_t)-1;
    for (int64_t i = 0; i < count; i++) {
        size_t idx = ib_pool_add(b, (int64_t)sorted_syms[i]);
        if (i == 0) pool_idx = idx;
    }
    if (count == 0) pool_idx = ib_pool_add(b, 0); /* keep `b` a valid index even for {} */
    return ib_emit(b, OP_MK_ATTRS, count, (int64_t)pool_idx);
}

size_t ib_select(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_SELECT, (int64_t)sym, 0);
}

size_t ib_select_dynamic(IrBuilder *b) {
    return ib_emit(b, OP_SELECT_DYNAMIC, 0, 0);
}

size_t ib_mk_thunk(IrBuilder *b, IrProgram *body_prog) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)body_prog->ops);
    ib_pool_add(b, (int64_t)(intptr_t)body_prog->pool); /* always pool_idx+1, see op_mk_thunk.S */
    return ib_emit(b, OP_MK_THUNK, 0, (int64_t)pool_idx);
}

size_t ib_frame_alloc(IrBuilder *b, int64_t nslots) {
    return ib_emit(b, OP_FRAME_ALLOC, nslots, 0);
}

size_t ib_frame_set(IrBuilder *b, int64_t slot) {
    return ib_emit(b, OP_FRAME_SET, slot, 0);
}

size_t ib_frame_get(IrBuilder *b, int64_t slot) {
    return ib_emit(b, OP_FRAME_GET, slot, 0);
}

size_t ib_frame_exit(IrBuilder *b) {
    return ib_emit(b, OP_FRAME_EXIT, 0, 0);
}

size_t ib_rec_overrides_merge(IrBuilder *b, int64_t n, const uint32_t *syms, int64_t overrides_slot) {
    size_t pool_idx = (size_t)-1;
    for (int64_t i = 0; i < n; i++) {
        size_t idx = ib_pool_add(b, (int64_t)syms[i]);
        if (i == 0) pool_idx = idx;
    }
    size_t slot_idx = ib_pool_add(b, overrides_slot);
    if (n == 0) pool_idx = slot_idx; /* n == 0 can't actually happen (there's always at
                                       * least the __overrides binding itself), but keep
                                       * `b` valid regardless, same defensive habit
                                       * ib_mk_attrs's own count==0 case has. */
    return ib_emit(b, OP_REC_OVERRIDES_MERGE, n, (int64_t)pool_idx);
}

size_t ib_eq(IrBuilder *b) {
    return ib_emit(b, OP_EQ, 0, 0);
}
size_t ib_neq(IrBuilder *b) {
    return ib_emit(b, OP_NEQ, 0, 0);
}
size_t ib_lt(IrBuilder *b) {
    return ib_emit(b, OP_LT, 0, 0);
}
size_t ib_gt(IrBuilder *b) {
    return ib_emit(b, OP_GT, 0, 0);
}
size_t ib_le(IrBuilder *b) {
    return ib_emit(b, OP_LE, 0, 0);
}
size_t ib_ge(IrBuilder *b) {
    return ib_emit(b, OP_GE, 0, 0);
}
size_t ib_not(IrBuilder *b) {
    return ib_emit(b, OP_NOT, 0, 0);
}
size_t ib_update(IrBuilder *b) {
    return ib_emit(b, OP_UPDATE, 0, 0);
}
size_t ib_concat(IrBuilder *b) {
    return ib_emit(b, OP_CONCAT, 0, 0);
}
size_t ib_div(IrBuilder *b) {
    return ib_emit(b, OP_DIV, 0, 0);
}
size_t ib_insert_dynamic_attr(IrBuilder *b) {
    return ib_emit(b, OP_INSERT_DYNAMIC_ATTR, 0, 0);
}
size_t ib_select_dynamic_or(IrBuilder *b) {
    return ib_emit(b, OP_SELECT_DYNAMIC_OR, 0, 0);
}
size_t ib_resolve_search_path(IrBuilder *b) {
    return ib_emit(b, OP_RESOLVE_SEARCH_PATH, 0, 0);
}

size_t ib_assert_fail(IrBuilder *b, const char *msg, size_t len) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)msg);
    ib_pool_add(b, (int64_t)len); /* always pool_idx+1, see op_assert.S */
    return ib_emit(b, OP_ASSERT_FAIL, 0, (int64_t)pool_idx);
}

size_t ib_str_concat(IrBuilder *b, int64_t count) {
    return ib_emit(b, OP_STR_CONCAT, count, 0);
}

size_t ib_path_concat(IrBuilder *b, int64_t count) {
    return ib_emit(b, OP_PATH_CONCAT, count, 0);
}

size_t ib_with(IrBuilder *b) {
    return ib_emit(b, OP_WITH, 0, 0);
}

size_t ib_with_try(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_WITH_TRY, 0, (int64_t)sym);
}

size_t ib_with_miss(IrBuilder *b) {
    return ib_emit(b, OP_WITH_MISS, 0, 0);
}

size_t ib_with_exit(IrBuilder *b) {
    return ib_emit(b, OP_WITH_EXIT, 0, 0);
}

size_t ib_has_attr(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_HAS_ATTR, (int64_t)sym, 0);
}

size_t ib_has_attr_dynamic(IrBuilder *b) {
    return ib_emit(b, OP_HAS_ATTR_DYNAMIC, 0, 0);
}

size_t ib_check_formals(IrBuilder *b, int64_t required_count, const uint32_t *required_syms_sorted,
                         int64_t total_count, const uint32_t *all_syms_sorted) {
    /* Pool layout -- see op_check_formals.S's long comment. */
    size_t pool_idx = ib_pool_add(b, total_count); /* -1 means "has `...`, skip the extra-field check" */
    for (int64_t i = 0; i < required_count; i++) ib_pool_add(b, (int64_t)required_syms_sorted[i]);
    if (total_count >= 0) {
        for (int64_t i = 0; i < total_count; i++) ib_pool_add(b, (int64_t)all_syms_sorted[i]);
    }
    return ib_emit(b, OP_CHECK_FORMALS, required_count, (int64_t)pool_idx);
}

size_t ib_select_or(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_SELECT_OR, 0, (int64_t)sym);
}

IrProgram *ib_finish(IrBuilder *b) {
    ir_link(b->ops, b->len);
    IrProgram *prog = calloc(1, sizeof(IrProgram));
    prog->ops = b->ops;
    prog->pool = b->pool;
    return prog;
}
