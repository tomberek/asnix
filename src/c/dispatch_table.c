#include <stdint.h>

#include "asnix/ir.h"

/* Each op handler is a GAS jmp-target, not a conventional callable function
 * -- the `extern void ...(void)` declarations below exist purely so we can
 * take their address with ordinary C syntax. Designated initializers (keyed
 * by the enum, not position) mean reordering the enum in ir.h can never
 * silently desynchronize this table from the handlers it names. */

extern void asnix_op_push_int(void);
extern void asnix_op_push_true(void);
extern void asnix_op_push_false(void);
extern void asnix_op_push_null(void);
extern void asnix_op_var_ref(void);
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
extern void asnix_op_has_attr(void);
extern void asnix_op_check_formals(void);
extern void asnix_op_select_or(void);
extern void asnix_op_select_dynamic(void);
extern void asnix_op_concat(void);
extern void asnix_op_div(void);

void *asnix_handler_table[OP_COUNT] = {
    [OP_PUSH_INT] = (void *)asnix_op_push_int,
    [OP_PUSH_TRUE] = (void *)asnix_op_push_true,
    [OP_PUSH_FALSE] = (void *)asnix_op_push_false,
    [OP_PUSH_NULL] = (void *)asnix_op_push_null,
    [OP_VAR_REF] = (void *)asnix_op_var_ref,
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
    [OP_HAS_ATTR] = (void *)asnix_op_has_attr,
    [OP_CHECK_FORMALS] = (void *)asnix_op_check_formals,
    [OP_SELECT_OR] = (void *)asnix_op_select_or,
    [OP_SELECT_DYNAMIC] = (void *)asnix_op_select_dynamic,
    [OP_CONCAT] = (void *)asnix_op_concat,
    [OP_DIV] = (void *)asnix_op_div,
};

void ir_link(IrOp *ops, size_t n) {
    for (size_t i = 0; i < n; i++) {
        intptr_t op_id = (intptr_t)ops[i].handler;
        ops[i].handler = asnix_handler_table[op_id];
    }
}
