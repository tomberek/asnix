#ifndef ASNIX_IR_H
#define ASNIX_IR_H

/*
 * Flat, fixed-width IR: a 24-byte, 8-byte-aligned record per instruction.
 * `handler` starts out holding a small op-id (as emitted by the builder /
 * M4 compiler) and is rewritten in place to a real handler address by
 * ir_link() -- see docs/ir-format.md. Fixed-width records mean "advance to
 * the next instruction" is a pure pointer increment, never a decode.
 */

#define ASNIX_IROP_SIZE        24
#define ASNIX_IROP_OFF_HANDLER 0
#define ASNIX_IROP_OFF_A       8
#define ASNIX_IROP_OFF_B       16

#define ASNIX_IRPROG_OFF_OPS  0
#define ASNIX_IRPROG_OFF_POOL 8

/* Closure heap object: {up; body; arity; pool}. In this minimal model a
 * lambda's frame has exactly `arity` slots (one per parameter, no extra
 * pre-sized local slots) -- any nested `let` inside the body introduces its
 * own separate frame via OP_LET, so a distinct nslots field isn't needed.
 *
 * `pool` is each closure's OWN constant pool (every IrBuilder/IrProgram has
 * one, e.g. for ITS OWN nested lambda literals) -- it must travel with the
 * closure and get reinstalled in %r13 on call / restored on return,
 * otherwise a lambda body containing another `OP_LAMBDA` would resolve that
 * inner lambda's body pointer against the *caller's* pool instead of its
 * own (the bug M4's curried lambdas first exposed: M3's single-level
 * fixture never had a lambda body containing another OP_LAMBDA). */
#define ASNIX_CLOSURE_OFF_UP    0
#define ASNIX_CLOSURE_OFF_BODY  8
#define ASNIX_CLOSURE_OFF_ARITY 16
#define ASNIX_CLOSURE_OFF_POOL  24
#define ASNIX_CLOSURE_SIZE      32

/* Call-frame record on the explicit RETSP stack (kept off the native
 * machine stack, so the jmp-chain dispatch loop never grows it): where to
 * resume (return_ip), which Env to resume in (return_env), and which
 * constant pool was active in the caller (return_pool, for the same reason
 * Closure carries one -- see above). */
#define ASNIX_RETFRAME_OFF_IP   0
#define ASNIX_RETFRAME_OFF_ENV  8
#define ASNIX_RETFRAME_OFF_POOL 16
#define ASNIX_RETFRAME_SIZE     24

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>
#include "asnix/env.h"
#include "asnix/value.h"

typedef struct {
    void *handler;
    int64_t a;
    int64_t b;
} IrOp;

enum {
    OP_PUSH_INT,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_NULL,
    OP_VAR_REF,
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
    OP_COUNT
};

typedef struct {
    IrOp *ops;
    int64_t *pool;
} IrProgram;

extern void *asnix_handler_table[OP_COUNT];

/* Rewrites every ops[i].handler in place from a small op-id (as emitted by
 * the builder) to the real handler address from asnix_handler_table. Must
 * be called exactly once per ops array before it's passed to asnix_eval. */
void ir_link(IrOp *ops, size_t n);

/* The one public M3 entry point: executes `prog` (already ir_link()ed)
 * starting at prog->ops[0] with `root_env` as the outermost lexical scope,
 * and returns the value left on the operand stack by OP_HALT. */
Value asnix_eval(IrProgram *prog, Env *root_env);

#endif /* __ASSEMBLER__ */

#endif /* ASNIX_IR_H */
