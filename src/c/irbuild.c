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

size_t ib_lambda(IrBuilder *b, int64_t arity, IrProgram *body_prog) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)body_prog->ops);
    ib_pool_add(b, (int64_t)(intptr_t)body_prog->pool); /* always pool_idx+1, see op_lambda.S */
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

size_t ib_frame_exit(IrBuilder *b) {
    return ib_emit(b, OP_FRAME_EXIT, 0, 0);
}

IrProgram *ib_finish(IrBuilder *b) {
    ir_link(b->ops, b->len);
    IrProgram *prog = calloc(1, sizeof(IrProgram));
    prog->ops = b->ops;
    prog->pool = b->pool;
    return prog;
}
