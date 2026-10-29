#include "asnix/irbuild.h"

#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/irpass.h"

IrBuilder *ib_new(void) {
    IrBuilder *b = calloc(1, sizeof(IrBuilder));
    return b;
}

size_t ib_len(IrBuilder *b) {
    return b->len;
}

#ifdef ASNIX_IRDUMP_LINE_TRACKING
static int g_line_tracking_enabled = 0;
static uint32_t g_current_src_pos = 0;

void ib_enable_line_tracking(void) {
    g_line_tracking_enabled = 1;
}

void ib_set_current_src_pos(uint32_t pos) {
    g_current_src_pos = pos;
}

#define ASNIX_IB_MAX_LINE_PROGRAMS 65536
static const IrProgram *g_line_programs[ASNIX_IB_MAX_LINE_PROGRAMS];
static const uint32_t *g_line_program_lines[ASNIX_IB_MAX_LINE_PROGRAMS];
static size_t g_nline_programs = 0;

const uint32_t *asnix_ib_lookup_program_lines(const IrProgram *prog) {
    for (size_t i = 0; i < g_nline_programs; i++) {
        if (g_line_programs[i] == prog) return g_line_program_lines[i];
    }
    return NULL;
}
#endif

static size_t ib_emit(IrBuilder *b, int op_id, int64_t a, int64_t bval) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 24;
        b->ops = realloc(b->ops, b->cap * sizeof(IrOp));
    }
    b->ops[b->len].handler = (void *)(intptr_t)op_id;
    b->ops[b->len].a = a;
    b->ops[b->len].b = bval;
#ifdef ASNIX_IRDUMP_LINE_TRACKING
    if (g_line_tracking_enabled) {
        if (b->len == b->lines_cap) {
            b->lines_cap = b->lines_cap ? b->lines_cap * 2 : 24;
            b->lines = realloc(b->lines, b->lines_cap * sizeof(uint32_t));
        }
        b->lines[b->len] = g_current_src_pos;
    }
#endif
    return b->len++;
}

static size_t ib_pool_add(IrBuilder *b, int64_t v);

size_t ib_push_int(IrBuilder *b, int64_t v) {
    size_t pool_idx = ib_pool_add(b, v);
    return ib_emit(b, OP_PUSH_INT, 0, (int64_t)pool_idx);
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
size_t ib_var_ref_cell(IrBuilder *b, int64_t depth, int64_t slot) {
    return ib_emit(b, OP_VAR_REF_CELL, depth, slot);
}
size_t ib_var_ref_raw(IrBuilder *b, int64_t depth, int64_t slot) {
    return ib_emit(b, OP_VAR_REF_RAW, depth, slot);
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
        b->pool_cap = b->pool_cap ? b->pool_cap * 2 : 24;
        b->pool = realloc(b->pool, b->pool_cap * sizeof(int64_t));
    }
    b->pool[b->pool_len] = v;
    return b->pool_len++;
}

size_t ib_lambda(IrBuilder *b, int64_t arity, IrProgram *body_prog, int64_t formals_raw) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)body_prog);
    ib_pool_add(b, formals_raw);
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
    if (count == 0) pool_idx = ib_pool_add(b, 0);
    return ib_emit(b, OP_MK_ATTRS, count, (int64_t)pool_idx);
}

size_t ib_select(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_SELECT, (int64_t)sym, 0);
}

size_t ib_select_dynamic(IrBuilder *b) {
    return ib_emit(b, OP_SELECT_DYNAMIC, 0, 0);
}

size_t ib_mk_thunk(IrBuilder *b, IrProgram *body_prog) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)body_prog);
    return ib_emit(b, OP_MK_THUNK, 0, (int64_t)pool_idx);
}

size_t ib_mk_lazy_thunk_stub(IrBuilder *b, void *desc) {
    size_t pool_idx = ib_pool_add(b, (int64_t)(intptr_t)desc);
    return ib_emit(b, OP_LAZY_THUNK_BODY, 0, (int64_t)pool_idx);
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
    if (n == 0) pool_idx = slot_idx;
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
    ib_pool_add(b, (int64_t)len);
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
    size_t pool_idx = ib_pool_add(b, total_count);
    for (int64_t i = 0; i < required_count; i++) ib_pool_add(b, (int64_t)required_syms_sorted[i]);
    if (total_count >= 0) {
        for (int64_t i = 0; i < total_count; i++) ib_pool_add(b, (int64_t)all_syms_sorted[i]);
    }
    return ib_emit(b, OP_CHECK_FORMALS, required_count, (int64_t)pool_idx);
}

size_t ib_select_or(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_SELECT_OR, 0, (int64_t)sym);
}

size_t ib_eq_if(IrBuilder *b) {
    return ib_emit(b, OP_EQ_IF, 0, 0);
}
size_t ib_neq_if(IrBuilder *b) {
    return ib_emit(b, OP_NEQ_IF, 0, 0);
}
size_t ib_lt_if(IrBuilder *b) {
    return ib_emit(b, OP_LT_IF, 0, 0);
}
size_t ib_ge_if(IrBuilder *b) {
    return ib_emit(b, OP_GE_IF, 0, 0);
}
size_t ib_has_attr_if(IrBuilder *b, uint32_t sym) {
    return ib_emit(b, OP_HAS_ATTR_IF, 0, (int64_t)sym);
}

IrProgram *ib_finish(IrBuilder *b) {
    asnix_irpass_begin_program();
#ifdef ASNIX_IRDUMP_LINE_TRACKING
    asnix_irpass_set_line_array(g_line_tracking_enabled ? b->lines : NULL);
#endif
    asnix_irpass_run(b->ops, &b->len, &b->pool, &b->pool_len, &b->pool_cap);
#ifdef ASNIX_IRDUMP_LINE_TRACKING
    asnix_irpass_set_line_array(NULL);
#endif
    ir_link(b->ops, b->len);
    if (b->len > 0 && b->len < b->cap) {
        size_t bytes = b->len * sizeof(IrOp);
        IrOp *shrunk = asnix_arena_alloc(bytes);
        memcpy(shrunk, b->ops, bytes);
        free(b->ops);
        b->ops = shrunk;
    }
    if (b->pool_len > 0 && b->pool_len < b->pool_cap) {
        size_t bytes = b->pool_len * sizeof(int64_t);
        int64_t *shrunk = asnix_arena_alloc(bytes);
        memcpy(shrunk, b->pool, bytes);
        free(b->pool);
        b->pool = shrunk;
    }
    IrProgram *prog = calloc(1, sizeof(IrProgram));
    prog->ops = b->ops;
    prog->pool = b->pool;
    asnix_irpass_mark_program(prog);
#ifdef ASNIX_IRDUMP_LINE_TRACKING
    if (g_line_tracking_enabled && b->lines && g_nline_programs < ASNIX_IB_MAX_LINE_PROGRAMS) {
        g_line_programs[g_nline_programs] = prog;
        g_line_program_lines[g_nline_programs] = b->lines;
        g_nline_programs++;
    }
#endif
    free(b);
    return prog;
}

