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

/* `expr.${nameExpr}`: pops the name Value then the select target (both
 * already pushed by the compiler), looks the name up at runtime -- see
 * OP_SELECT_DYNAMIC's own comment (ir.h) and op_select_dynamic.S. */
size_t ib_select_dynamic(IrBuilder *b);

/* M6: pushes a thunk wrapping body_prog (compiled like a lambda body,
 * ending in OP_RET) captured over the *current* Env at the point this op
 * runs -- same pool-travels-with-it rationale as ib_lambda. */
size_t ib_mk_thunk(IrBuilder *b, IrProgram *body_prog);

/* Recursion support (letrec / rec attrsets): allocate a new Env frame with
 * `nslots` slots and install it as the current ENV (OP_FRAME_ALLOC), store
 * the value on top of VALSTACK into slot `slot` of the current ENV
 * (OP_FRAME_SET), and restore ENV to its parent (OP_FRAME_EXIT) -- see
 * ir.h for the full rationale. */
size_t ib_frame_alloc(IrBuilder *b, int64_t nslots);
size_t ib_frame_set(IrBuilder *b, int64_t slot);
size_t ib_frame_exit(IrBuilder *b);

/* "operators" milestone: comparisons (INT-only for ordering, see
 * op_cmp.S), booleans, attrset update, assert. `&&`/`||` have no
 * dedicated op -- see ir.h's OP_EQ block comment. */
size_t ib_eq(IrBuilder *b);
size_t ib_neq(IrBuilder *b);
size_t ib_lt(IrBuilder *b);
size_t ib_gt(IrBuilder *b);
size_t ib_le(IrBuilder *b);
size_t ib_ge(IrBuilder *b);
size_t ib_not(IrBuilder *b);
size_t ib_update(IrBuilder *b);
size_t ib_concat(IrBuilder *b);
size_t ib_div(IrBuilder *b);
/* Emits OP_ASSERT_FAIL over a {msg, len} pool pair; `msg` must outlive the
 * resulting IrProgram (compiler.c arena-allocates it, which is never
 * freed, exactly like string literals -- see AST_STRING). */
size_t ib_assert_fail(IrBuilder *b, const char *msg, size_t len);

/* Pops `count` values (reverse push order) and pushes one concatenated
 * String -- see OP_STR_CONCAT in ir.h. */
size_t ib_str_concat(IrBuilder *b, int64_t count);

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

/* Lambda-patterns: pops a value, pushes TRUE/FALSE -- see op_has_attr.S. */
size_t ib_has_attr(IrBuilder *b, uint32_t sym);

/* Lambda-patterns: eager pre-destructuring validation of the call argument
 * (see op_check_formals.S for the exact semantics and pool layout).
 * `required_syms_sorted` must have `required_count` entries, sorted
 * ascending. `all_syms_sorted` is read (and must have `total_count`
 * sorted-ascending entries) only when `total_count >= 0`; pass
 * `total_count = -1` and `all_syms_sorted = NULL` for a pattern with `...`
 * (skips the "no extra fields" check entirely). Per-slot binding of the
 * destructured fields reuses ib_frame_alloc()/ib_frame_set()/
 * ib_frame_exit() above -- recursive `let`'s own alloc-before-thunks
 * mechanism, not a second duplicate op pair. */
size_t ib_check_formals(IrBuilder *b, int64_t required_count, const uint32_t *required_syms_sorted,
                         int64_t total_count, const uint32_t *all_syms_sorted);

/* `expr.attr or default`: emits with a placeholder delta (operand `a`,
 * patched via ib_patch_delta, same convention as ib_if/ib_jmp/ib_with_try)
 * and the fixed symbol id `sym` (operand `b`) -- see OP_SELECT_OR in ir.h. */
size_t ib_select_or(IrBuilder *b, uint32_t sym);

/* Finalizes the builder: runs ir_link() over the accumulated ops in place
 * and returns an IrProgram ready for asnix_eval(). */
IrProgram *ib_finish(IrBuilder *b);

#endif /* ASNIX_TEST_IRBUILD_H */
