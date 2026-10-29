#include <stdint.h>

#include "asnix/ir.h"

extern void asnix_op_push_int(void);
extern void asnix_op_push_true(void);
extern void asnix_op_push_false(void);
extern void asnix_op_push_null(void);
extern void asnix_op_var_ref(void);
extern void asnix_op_var_ref_cell(void);
extern void asnix_op_var_ref_raw(void);
extern void asnix_op_add(void);
extern void asnix_op_sub(void);
extern void asnix_op_mul(void);
extern void asnix_op_if(void);
extern void asnix_op_jmp(void);
extern void asnix_op_let(void);
extern void asnix_op_lambda(void);
extern void asnix_op_call(void);
extern void asnix_op_ret(void);
extern void asnix_op_halt(void);
extern void asnix_op_push_const(void);
extern void asnix_op_mk_list(void);
extern void asnix_op_mk_attrs(void);
extern void asnix_op_select(void);
extern void asnix_op_mk_thunk(void);
extern void asnix_op_frame_alloc(void);
extern void asnix_op_frame_set(void);
extern void asnix_op_frame_get(void);
extern void asnix_op_frame_exit(void);
extern void asnix_op_eq(void);
extern void asnix_op_neq(void);
extern void asnix_op_lt(void);
extern void asnix_op_gt(void);
extern void asnix_op_le(void);
extern void asnix_op_ge(void);
extern void asnix_op_not(void);
extern void asnix_op_update(void);
extern void asnix_op_assert_fail(void);
extern void asnix_op_str_concat(void);
extern void asnix_op_with(void);
extern void asnix_op_with_try(void);
extern void asnix_op_with_miss(void);
extern void asnix_op_with_exit(void);
extern void asnix_op_has_attr(void);
extern void asnix_op_check_formals(void);
extern void asnix_op_select_or(void);
extern void asnix_op_select_dynamic(void);
extern void asnix_op_concat(void);
extern void asnix_op_div(void);
extern void asnix_op_insert_dynamic_attr(void);
extern void asnix_op_select_dynamic_or(void);
extern void asnix_op_resolve_search_path(void);
extern void asnix_op_has_attr_dynamic(void);
extern void asnix_op_rec_overrides_merge(void);
extern void asnix_op_path_concat(void);
extern void asnix_op_eq_if(void);
extern void asnix_op_neq_if(void);
extern void asnix_op_lt_if(void);
extern void asnix_op_ge_if(void);
extern void asnix_op_has_attr_if(void);
extern void asnix_op_lazy_thunk_body(void);

void *asnix_handler_table[OP_COUNT] = {
    [OP_PUSH_INT] = (void *)asnix_op_push_int,
    [OP_PUSH_TRUE] = (void *)asnix_op_push_true,
    [OP_PUSH_FALSE] = (void *)asnix_op_push_false,
    [OP_PUSH_NULL] = (void *)asnix_op_push_null,
    [OP_VAR_REF] = (void *)asnix_op_var_ref,
    [OP_VAR_REF_CELL] = (void *)asnix_op_var_ref_cell,
    [OP_VAR_REF_RAW] = (void *)asnix_op_var_ref_raw,
    [OP_ADD] = (void *)asnix_op_add,
    [OP_SUB] = (void *)asnix_op_sub,
    [OP_MUL] = (void *)asnix_op_mul,
    [OP_IF] = (void *)asnix_op_if,
    [OP_JMP] = (void *)asnix_op_jmp,
    [OP_LET] = (void *)asnix_op_let,
    [OP_LAMBDA] = (void *)asnix_op_lambda,
    [OP_CALL] = (void *)asnix_op_call,
    [OP_RET] = (void *)asnix_op_ret,
    [OP_HALT] = (void *)asnix_op_halt,
    [OP_PUSH_CONST] = (void *)asnix_op_push_const,
    [OP_MK_LIST] = (void *)asnix_op_mk_list,
    [OP_MK_ATTRS] = (void *)asnix_op_mk_attrs,
    [OP_SELECT] = (void *)asnix_op_select,
    [OP_MK_THUNK] = (void *)asnix_op_mk_thunk,
    [OP_FRAME_ALLOC] = (void *)asnix_op_frame_alloc,
    [OP_FRAME_SET] = (void *)asnix_op_frame_set,
    [OP_FRAME_GET] = (void *)asnix_op_frame_get,
    [OP_FRAME_EXIT] = (void *)asnix_op_frame_exit,
    [OP_EQ] = (void *)asnix_op_eq,
    [OP_NEQ] = (void *)asnix_op_neq,
    [OP_LT] = (void *)asnix_op_lt,
    [OP_GT] = (void *)asnix_op_gt,
    [OP_LE] = (void *)asnix_op_le,
    [OP_GE] = (void *)asnix_op_ge,
    [OP_NOT] = (void *)asnix_op_not,
    [OP_UPDATE] = (void *)asnix_op_update,
    [OP_ASSERT_FAIL] = (void *)asnix_op_assert_fail,
    [OP_STR_CONCAT] = (void *)asnix_op_str_concat,
    [OP_WITH] = (void *)asnix_op_with,
    [OP_WITH_TRY] = (void *)asnix_op_with_try,
    [OP_WITH_MISS] = (void *)asnix_op_with_miss,
    [OP_WITH_EXIT] = (void *)asnix_op_with_exit,
    [OP_HAS_ATTR] = (void *)asnix_op_has_attr,
    [OP_CHECK_FORMALS] = (void *)asnix_op_check_formals,
    [OP_SELECT_OR] = (void *)asnix_op_select_or,
    [OP_SELECT_DYNAMIC] = (void *)asnix_op_select_dynamic,
    [OP_CONCAT] = (void *)asnix_op_concat,
    [OP_DIV] = (void *)asnix_op_div,
    [OP_INSERT_DYNAMIC_ATTR] = (void *)asnix_op_insert_dynamic_attr,
    [OP_SELECT_DYNAMIC_OR] = (void *)asnix_op_select_dynamic_or,
    [OP_RESOLVE_SEARCH_PATH] = (void *)asnix_op_resolve_search_path,
    [OP_HAS_ATTR_DYNAMIC] = (void *)asnix_op_has_attr_dynamic,
    [OP_REC_OVERRIDES_MERGE] = (void *)asnix_op_rec_overrides_merge,
    [OP_PATH_CONCAT] = (void *)asnix_op_path_concat,
    [OP_EQ_IF] = (void *)asnix_op_eq_if,
    [OP_NEQ_IF] = (void *)asnix_op_neq_if,
    [OP_LT_IF] = (void *)asnix_op_lt_if,
    [OP_GE_IF] = (void *)asnix_op_ge_if,
    [OP_HAS_ATTR_IF] = (void *)asnix_op_has_attr_if,
    [OP_LAZY_THUNK_BODY] = (void *)asnix_op_lazy_thunk_body,
};

void ir_link(IrOp *ops, size_t n) {
    for (size_t i = 0; i < n; i++) {
        intptr_t op_id = (intptr_t)ops[i].handler;
        ops[i].handler = asnix_handler_table[op_id];
    }
}

