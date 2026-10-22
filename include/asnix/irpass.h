#ifndef ASNIX_IRPASS_H
#define ASNIX_IRPASS_H

#include <stddef.h>
#include "asnix/ir.h"

/*
 * Bytecode-to-bytecode optimization-pass framework.
 *
 * Operates on an IrBuilder's accumulated, NOT-YET-LINKED ops array --
 * `handler` still holds a small op-id at this point, exactly what
 * ib_emit() just wrote (irbuild.c). ib_finish() runs every currently
 * registered pass (asnix_irpass_register) to a fixpoint over
 * b->ops[0..b->len) immediately before its own ir_link() call -- so EVERY
 * IrProgram this project ever builds (the top-level file, every lambda
 * body, every thunk body: each is its own IrBuilder, ib_finish()-ed
 * independently) gets the same treatment for free, with no change needed
 * at any of compiler.c's many ib_new()/ib_finish() call sites.
 *
 * With ZERO passes registered (the default -- asnix's own normal
 * production run never registers any), ib_finish()'s behavior is
 * byte-for-byte identical to before this framework existed. Registering a
 * pass is an explicit, opt-in action a test/benchmark harness (or
 * ASNIX_IR_OPT=1, main.c) takes before compiling, not a change to the
 * shipped interpreter's default behavior.
 */

/* Returns 1 if it rewrote/removed anything, 0 if it found nothing to do.
 * May shrink `*len` via asnix_irpass_remove below; may NOT grow it (no
 * growable-array support here -- every pass this framework ships only
 * removes, matching the "make the bytecode smaller/cheaper" framing of
 * the passes actually worth writing at this level; a pass that needs to
 * INSERT ops should instead rewrite via the IrBuilder API before
 * ib_finish, which is outside this framework's scope). */
typedef int (*AsnixIrPass)(IrOp *ops, size_t *len);

/* Registers `pass` to run (to a fixpoint, alongside every other registered
 * pass, in registration order) inside every subsequent ib_finish() call.
 * Not thread-safe -- same convention as every other process-global table
 * in this codebase (symtab, the import cache, ...); asnix is
 * single-threaded throughout. */
void asnix_irpass_register(AsnixIrPass pass);

/* Testing/benchmarking convenience: drops every registered pass,
 * restoring ib_finish() to its no-passes default. Lets a harness compile
 * the SAME source once with the pipeline on and once with it off, in the
 * same process, for a paired before/after comparison. */
void asnix_irpass_clear(void);

/* Runs every currently-registered pass over ops[0..*len) to a fixpoint
 * (each pass re-run until a full round makes no further change, capped at
 * ASNIX_IRPASS_MAX_ITERS so a pass that can't reach a fixpoint can't hang
 * compilation). This is what ib_finish() calls internally; exposed
 * directly too so a standalone tool can apply the same pipeline without
 * going through the full builder API. Returns 1 if anything changed
 * across the whole run. */
int asnix_irpass_run(IrOp *ops, size_t *len);

/* The one shrink primitive a pass should use to remove ops: deletes
 * `count` ops starting at `start` from ops[0..*len), decrementing *len,
 * and fixes up EVERY jump-delta-bearing op (OP_IF/OP_JMP/OP_WITH_TRY/
 * OP_SELECT_OR/OP_SELECT_DYNAMIC_OR -- see ir.h's own per-op comments;
 * exhaustive, nothing else in OP_COUNT's enum carries a self-relative
 * index delta) in the WHOLE array, including ones before `start` whose
 * target lands after `start+count`, so every jump still lands on the same
 * logical op it did before the removal. Fatal (exit 1) if any op's target
 * would land INSIDE the removed range -- a pass must only ever remove a
 * self-contained sequence with no live jump into its interior; this is
 * checked unconditionally, not just under a debug build, since a pass bug
 * here would otherwise silently miscompile. */
void asnix_irpass_remove(IrOp *ops, size_t *len, size_t start, size_t count);

#define ASNIX_IRPASS_MAX_ITERS 64

/* Registers this framework's one shipped example/proof-of-concept pass:
 * folds `OP_PUSH_INT a; OP_PUSH_INT b; OP_<binop>` into a single
 * OP_PUSH_INT/OP_PUSH_TRUE/OP_PUSH_FALSE for every pure int-int binary op
 * this IR has (ADD/SUB/MUL/DIV and the 6 comparisons) -- see
 * irpass_examples.c. Exists mainly to exercise asnix_irpass_remove's
 * splice-and-refixup machinery end to end with a real, if modest, win. */
void asnix_irpass_register_cfold(void);

#endif /* ASNIX_IRPASS_H */
