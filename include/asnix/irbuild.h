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

/* Lambda-patterns: pops a value, pushes TRUE/FALSE -- see op_has_attr.S. */
size_t ib_has_attr(IrBuilder *b, uint32_t sym);

/* Lambda-patterns: eager pre-destructuring validation of the call argument
 * (see op_check_formals.S for the exact semantics and pool layout).
 * `required_syms_sorted` must have `required_count` entries, sorted
 * ascending. `all_syms_sorted` is read (and must have `total_count`
 * sorted-ascending entries) only when `total_count >= 0`; pass
 * `total_count = -1` and `all_syms_sorted = NULL` for a pattern with `...`
 * (skips the "no extra fields" check entirely). */
size_t ib_check_formals(IrBuilder *b, int64_t required_count, const uint32_t *required_syms_sorted,
                         int64_t total_count, const uint32_t *all_syms_sorted);

/* Lambda-patterns: allocates a new Env frame (up = current ENV) with
 * `nslots` still-unbound slots and makes it the current ENV -- does not
 * touch VALSTACK. See op_frame.S / ir.h's OP_FRAME_NEW. */
size_t ib_frame_new(IrBuilder *b, int64_t nslots);

/* Lambda-patterns: pops one value off VALSTACK into the *current* ENV
 * frame's values[slot] -- no allocation, unlike ib_let(). Meant to be
 * paired with ib_frame_new() and one ib_mk_thunk() per slot in between, so
 * each field's thunk captures the (already-current) new frame as its own
 * scope before any slot is actually filled in -- see op_frame.S. */
size_t ib_frame_bind_slot(IrBuilder *b, int64_t slot);

/* Finalizes the builder: runs ir_link() over the accumulated ops in place
 * and returns an IrProgram ready for asnix_eval(). */
IrProgram *ib_finish(IrBuilder *b);

#endif /* ASNIX_TEST_IRBUILD_H */
