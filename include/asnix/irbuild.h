#ifndef ASNIX_TEST_IRBUILD_H
#define ASNIX_TEST_IRBUILD_H

#include <stddef.h>
#include <stdint.h>
#include "asnix/ir.h"

/* IR-stream builder shared by the M3 test fixtures (tests/ir) and the real
 * M4 compiler (src/c/compiler.c) -- "append an IrOp, track patch sites" is
 * the same API either way. */

typedef struct {
    IrOp *ops;
    size_t len, cap;
    int64_t *pool;
    size_t pool_len, pool_cap;
} IrBuilder;

IrBuilder *ib_new(void);
size_t ib_len(IrBuilder *b);

size_t ib_push_int(IrBuilder *b, int64_t v);
size_t ib_push_true(IrBuilder *b);
size_t ib_push_false(IrBuilder *b);
size_t ib_push_null(IrBuilder *b);
size_t ib_var_ref(IrBuilder *b, int64_t depth, int64_t slot);
size_t ib_add(IrBuilder *b);
size_t ib_sub(IrBuilder *b);
size_t ib_mul(IrBuilder *b);
size_t ib_if(IrBuilder *b);   /* emits with a placeholder delta; patch via ib_patch_delta */
size_t ib_jmp(IrBuilder *b);  /* ditto */
void ib_patch_delta(IrBuilder *b, size_t site, size_t target_idx);
size_t ib_let(IrBuilder *b, int64_t nslots);
/* body_prog must come from this same translation's ib_finish() -- its
 * `pool` travels with the resulting closure (see ASNIX_CLOSURE_OFF_POOL in
 * ir.h) so a lambda body containing its own nested OP_LAMBDA resolves
 * against the right constant pool at call time, not the caller's. */
size_t ib_lambda(IrBuilder *b, int64_t arity, IrProgram *body_prog);
size_t ib_call(IrBuilder *b, int64_t argc);
size_t ib_ret(IrBuilder *b);
size_t ib_halt(IrBuilder *b);

/* M5: pushes a fully-formed Value.raw (e.g. a string literal) stashed in
 * the constant pool. */
size_t ib_push_const(IrBuilder *b, int64_t raw_value);
size_t ib_mk_list(IrBuilder *b, int64_t count);
/* sorted_syms must already be sorted ascending -- the compiler sorts
 * bindings by symbol id and pushes their values in that same order before
 * emitting this, so OP_MK_ATTRS's reverse-pop fill lines each value up
 * with the right symbol (see op_mk_attrs.S). */
size_t ib_mk_attrs(IrBuilder *b, int64_t count, const uint32_t *sorted_syms);
size_t ib_select(IrBuilder *b, uint32_t sym);

/* M6: pushes a thunk wrapping body_prog (compiled like a lambda body,
 * ending in OP_RET) captured over the *current* Env at the point this op
 * runs -- same pool-travels-with-it rationale as ib_lambda. */
size_t ib_mk_thunk(IrBuilder *b, IrProgram *body_prog);

/* `with`: pops the with-expr thunk (pushed by a preceding ib_mk_thunk, same
 * convention as ib_let) into a new with-tagged 1-slot Env frame -- see
 * ASNIX_ENV_WITH_TAG in env.h and op_with.S. */
size_t ib_with(IrBuilder *b);
/* Emits with a placeholder delta (operand `a`, patched via ib_patch_delta,
 * same convention as ib_if/ib_jmp) and the fixed symbol id `sym` (operand
 * `b`): pops the attrs value a preceding ib_var_ref forced/pushed,
 * searches for `sym`, and either falls through with the match on top of
 * VALSTACK or jumps by the (not-yet-patched) delta to try the next
 * enclosing `with`. */
size_t ib_with_try(IrBuilder *b, uint32_t sym);
/* Every enclosing `with` was tried and none had the symbol: aborts with
 * "undefined variable", same error class as the compile-time check. */
size_t ib_with_miss(IrBuilder *b);

/* Finalizes the builder: runs ir_link() over the accumulated ops in place
 * and returns an IrProgram ready for asnix_eval(). */
IrProgram *ib_finish(IrBuilder *b);

#endif /* ASNIX_TEST_IRBUILD_H */
