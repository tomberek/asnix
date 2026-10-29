#ifndef ASNIX_IR_H
#define ASNIX_IR_H

#define ASNIX_IROP_SIZE        16
#define ASNIX_IROP_OFF_HANDLER 0
#define ASNIX_IROP_OFF_A       8
#define ASNIX_IROP_OFF_B       12

#define ASNIX_IRPROG_OFF_OPS  0
#define ASNIX_IRPROG_OFF_POOL 8

#define ASNIX_CLOSURE_OFF_UP    0
#define ASNIX_CLOSURE_OFF_PROG  8
#define ASNIX_CLOSURE_OFF_ARITY 16
#define ASNIX_CLOSURE_OFF_FORMALS 24
#define ASNIX_CLOSURE_SIZE      32

#define ASNIX_RETFRAME_OFF_IP           0
#define ASNIX_RETFRAME_OFF_ENV          8
#define ASNIX_RETFRAME_OFF_POOL         16
#define ASNIX_RETFRAME_OFF_FORCE_TARGET 24
#define ASNIX_RETFRAME_SIZE             32

#define ASNIX_THUNK_OFF_STATE 0
#define ASNIX_THUNK_OFF_ENV   8
#define ASNIX_THUNK_SIZE      16

#define ASNIX_THUNK_STATE_UNFORCED    0
#define ASNIX_THUNK_STATE_IN_PROGRESS 1
#define ASNIX_THUNK_STATE_FORCED      2
#define ASNIX_THUNK_STATE_FAILED      3

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>
#include "asnix/env.h"
#include "asnix/value.h"

typedef struct {
    void *handler;
    int32_t a;
    int32_t b;
} IrOp;

enum {
    OP_PUSH_INT,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_NULL,
    OP_VAR_REF,
    OP_VAR_REF_CELL,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_IF,
    OP_JMP,
    OP_LET,
    OP_LAMBDA,
    OP_CALL,
    OP_RET,
    OP_HALT,
    OP_PUSH_CONST,
    OP_MK_LIST,
    OP_MK_ATTRS,
    OP_SELECT,
    OP_MK_THUNK,
    OP_FRAME_ALLOC,
    OP_FRAME_SET,
    OP_FRAME_GET,
    OP_FRAME_EXIT,
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,
    OP_NOT,
    OP_UPDATE,
    OP_ASSERT_FAIL,
    OP_STR_CONCAT,
    OP_WITH,
    OP_WITH_TRY,
    OP_WITH_MISS,
    OP_WITH_EXIT,
    OP_HAS_ATTR,
    OP_CHECK_FORMALS,
    OP_SELECT_OR,
    OP_SELECT_DYNAMIC,
    OP_CONCAT,
    OP_DIV,
    OP_INSERT_DYNAMIC_ATTR,
    OP_SELECT_DYNAMIC_OR,
    OP_RESOLVE_SEARCH_PATH,
    OP_HAS_ATTR_DYNAMIC,
    OP_REC_OVERRIDES_MERGE,
    OP_VAR_REF_RAW,
    OP_PATH_CONCAT,
    OP_EQ_IF,
    OP_NEQ_IF,
    OP_LT_IF,
    OP_GE_IF,
    OP_HAS_ATTR_IF,
    OP_LAZY_THUNK_BODY,
    OP_COUNT
};

typedef struct {
    IrOp *ops;
    int64_t *pool;
} IrProgram;

extern void *asnix_handler_table[OP_COUNT];

void ir_link(IrOp *ops, size_t n);

Value asnix_eval(IrProgram *prog, Env *root_env);

Value asnix_force_for_print(Value v);

Value asnix_apply(Value fn, const Value *args, int64_t argc);

#endif

#endif

