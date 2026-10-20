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
};

void ir_link(IrOp *ops, size_t n) {
    for (size_t i = 0; i < n; i++) {
        intptr_t op_id = (intptr_t)ops[i].handler;
        ops[i].handler = asnix_handler_table[op_id];
    }
}
