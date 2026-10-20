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
 * Closure carries one -- see above). `force_target`: non-zero only when
 * this frame was pushed to force a thunk (OP_VAR_REF/OP_SELECT, M6) rather
 * than to make an ordinary OP_CALL -- OP_RET uses it to know which Thunk
 * cell to memoize the result into before resuming (zero means "ordinary
 * call, no memoization"). */
#define ASNIX_RETFRAME_OFF_IP           0
#define ASNIX_RETFRAME_OFF_ENV          8
#define ASNIX_RETFRAME_OFF_POOL         16
#define ASNIX_RETFRAME_OFF_FORCE_TARGET 24
#define ASNIX_RETFRAME_SIZE             32

/* Thunk heap object (M6): a deferred, memoized computation -- the
 * mechanism behind Nix's laziness. `state` distinguishes:
 *   UNFORCED:     env/body/pool are valid (the deferred computation)
 *   IN_PROGRESS:  currently being forced -- re-entering (e.g. `let x = x;
 *                 in x`) is a cycle, caught and aborted (blackholing)
 *   FORCED:       `env`'s slot has been overwritten with the cached
 *                 Value.raw instead; body/pool are no longer meaningful
 * Forcing overwrites the cell in place (single-threaded, so no CAS claim
 * protocol is needed the way a parallel evaluator like ~/gnix/iets
 * requires) -- every tagged pointer to this same cell sees the memoized
 * value after the first force, exactly once computed. */
#define ASNIX_THUNK_OFF_STATE 0
#define ASNIX_THUNK_OFF_ENV   8  /* UNFORCED: Env*; FORCED: cached Value.raw */
#define ASNIX_THUNK_OFF_BODY  16
#define ASNIX_THUNK_OFF_POOL  24
#define ASNIX_THUNK_SIZE      32

#define ASNIX_THUNK_STATE_UNFORCED    0
#define ASNIX_THUNK_STATE_IN_PROGRESS 1
#define ASNIX_THUNK_STATE_FORCED      2

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
    OP_PUSH_CONST, /* M5: b = pool index of a fully-formed Value.raw (string literals) */
    OP_MK_LIST,    /* M5: a = count; pops `count` values, builds a List */
    OP_MK_ATTRS,   /* M5: a = count, b = pool index of `count` sorted symbol ids */
    OP_SELECT,     /* M5: a = symbol id; pops an Attrs, pushes the matching value or aborts */
    OP_MK_THUNK,   /* M6: b = pool index of {body IrOp*, body pool} -- see OP_LAMBDA's note */
    OP_FRAME_ALLOC, /* a = nslots; allocates a new Env{up=current ENV, nslots} and installs
                      * it as the current ENV (no popping) -- the recursive-scope building
                      * block: thunks constructed afterward (OP_MK_THUNK) naturally capture
                      * THIS frame, enabling self/sibling reference. Paired with
                      * OP_FRAME_EXIT once all consumers of the frame are done. */
    OP_FRAME_SET,   /* a = slot index; pops one value off VALSTACK and stores it into
                      * slot `a` of the CURRENT Env (set up by OP_FRAME_ALLOC). */
    OP_FRAME_EXIT,  /* no operands; ENV = ENV->up -- undoes exactly one OP_FRAME_ALLOC,
                      * restoring the enclosing scope so a `let`/`rec {}` used as a
                      * non-tail subexpression doesn't leak its frame into whatever
                      * depth-relative OP_VAR_REF comes after it in the same IR stream. */
    /* "operators" milestone: comparisons/booleans/update/assert. `&&`/
     * `||` are deliberately NOT ops here -- they compile to the existing
     * branch-shaped OP_IF/OP_JMP (see compiler.c's AST_AND/AST_OR cases)
     * so the untaken side is provably never evaluated (short-circuit). */
    OP_EQ,          /* pops rhs,lhs; pushes TRUE/FALSE -- see op_cmp.S for the scope cut
                      * (INT numeric equality, IMM subtag equality, pointer identity for
                      * everything else -- no deep structural equality on lists/attrsets) */
    OP_NEQ,         /* ditto, negated */
    OP_LT,          /* pops rhs,lhs (both must be TAG_INT, else fatal); pushes TRUE/FALSE */
    OP_GT,
    OP_LE,
    OP_GE,
    OP_NOT,         /* in-place: top of stack must be TAG_IMM true/false, else fatal */
    OP_UPDATE,      /* pops rhs,lhs (both must be TAG_ATTRS, else fatal); pushes the
                      * right-biased sorted-array merge (asnix_attrs_update, src/c/attrs_update.c) */
    OP_ASSERT_FAIL, /* b = pool index of {msg ptr, msg len}; unconditionally asnix_fatal()s --
                      * compiler.c's AST_ASSERT only reaches this on the cond-false branch */
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

/* M6: forces `v` if it's a thunk (returning the cached value unchanged
 * otherwise), usable from C code with no active asnix_eval invocation --
 * see src/asm/force.S for exactly what this does and doesn't cover. */
Value asnix_force_for_print(Value v);

#endif /* __ASSEMBLER__ */

#endif /* ASNIX_IR_H */
