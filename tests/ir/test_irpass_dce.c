/* Synthetic, hand-assembled regression test for the "dce" bytecode pass
 * (src/c/irpass_dce.c) -- the real corpus (tests/e2e, tests/conformance,
 * numtide/nix-conformance's lang/) never gives this pass anything to do
 * (confirmed directly: today's compiler never emits a genuinely dead
 * op -- every ib_ret()/ib_halt() call is the LAST emit into its own
 * IrBuilder before that very builder's own ib_finish(), and every
 * jump this compiler emits points strictly forward, see irpass_dce.c's
 * own header comment), so THIS is the only place the actual removal
 * logic -- specifically the "a dead region can itself contain an
 * internal jump" case -- gets exercised at all.
 *
 * Hand-builds (via raw IrOp[], not the ordinary IrBuilder API -- the
 * builder's own normal usage patterns never produce dead code, by the
 * same argument above, so there is no way to reach this shape through
 * ib_if/ib_jmp/ib_patch_delta) an ops array with:
 *   - a LIVE unconditional OP_JMP whose target sits AFTER a dead region
 *     (exercises the ordinary "fix up a live jump's delta after
 *     shrinking" path -- already covered by cfold's own use of
 *     asnix_irpass_remove, re-confirmed here too), and
 *   - a dead region that itself contains an OP_IF and an OP_JMP whose
 *     OWN targets land elsewhere WITHIN that same dead region. Removing
 *     a whole maximal contiguous dead range in ONE asnix_irpass_remove
 *     call is fatal in exactly this shape (confirmed with a standalone
 *     throwaway test directly against the bare primitive while
 *     designing this pass) -- which is the entire reason this pass
 *     removes dead ops ascending, one at a time, instead of the
 *     "maximal ranges, descending" shape this pass's own spec first
 *     suggested (see irpass_dce.c's header comment for the inductive
 *     argument for why ascending order is safe).
 *
 * Layout (index: op -- meaning):
 *   0: PUSH_INT 1      -- live
 *   1: JMP -> 9        -- live; unconditional, no fallthrough, skips
 *                          the whole dead region below
 *   2: PUSH_INT 10     -- dead
 *   3: PUSH_INT 20     -- dead
 *   4: IF -> 7         -- dead; its OWN target (7) is ALSO inside
 *                          [2,9) -- the troublesome internal case
 *   5: PUSH_INT 30     -- dead ("then" arm)
 *   6: JMP -> 8        -- dead; target (8) ALSO inside [2,9)
 *   7: PUSH_INT 40     -- dead ("else" arm, op4's target)
 *   8: PUSH_INT 50     -- dead (merge point, op6's target)
 *   9: PUSH_INT 2      -- live (op1's target)
 *  10: ADD             -- live
 *  11: HALT            -- live, terminator
 *
 * Reachable by hand: {0,1,9,10,11}. Dead: {2,3,4,5,6,7,8} (7 ops),
 * forming exactly the self-contained-but-internally-jumpy region
 * above. Expected after the pass: len==5, ops == [PUSH_INT 1,
 * JMP(->2), PUSH_INT 2, ADD, HALT] (op1's delta retargeted from
 * absolute index 9 down to 2) -- checked both as a static shape AND by
 * actually ir_link()-ing and asnix_eval()-ing the result, confirming
 * 1+2 still evaluates to 3 after the rewrite, not just that the pass
 * didn't crash. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/irpass.h"
#include "asnix/value.h"
#include "irpass_dce.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static IrOp mk(int op, int64_t a, int64_t b) {
    IrOp o;
    o.handler = (void *)(intptr_t)op;
    o.a = a;
    o.b = b;
    return o;
}

static Env *root_env(void) {
    return asnix_env_alloc(NULL, 0);
}

static void fixture_dead_region_with_internal_jumps(void) {
    IrOp ops[12];
    size_t len = 12;
    ops[0] = mk(OP_PUSH_INT, 1, 0);
    ops[1] = mk(OP_JMP, (9 - 1) * ASNIX_IROP_SIZE, 0);
    ops[2] = mk(OP_PUSH_INT, 10, 0);
    ops[3] = mk(OP_PUSH_INT, 20, 0);
    ops[4] = mk(OP_IF, (7 - 4) * ASNIX_IROP_SIZE, 0);
    ops[5] = mk(OP_PUSH_INT, 30, 0);
    ops[6] = mk(OP_JMP, (8 - 6) * ASNIX_IROP_SIZE, 0);
    ops[7] = mk(OP_PUSH_INT, 40, 0);
    ops[8] = mk(OP_PUSH_INT, 50, 0);
    ops[9] = mk(OP_PUSH_INT, 2, 0);
    ops[10] = mk(OP_ADD, 0, 0);
    ops[11] = mk(OP_HALT, 0, 0);

    asnix_irpass_clear();
    asnix_irpass_register_dce();

    int64_t *pool = NULL;
    size_t pool_len = 0, pool_cap = 0;
    int changed = asnix_irpass_run(ops, &len, &pool, &pool_len, &pool_cap);
    CHECK(changed, "fixture_dead_region_with_internal_jumps: dce should report a change");
    CHECK(len == 5, "fixture_dead_region_with_internal_jumps: len==5 after removing the 7 dead ops");
    if (len != 5) {
        asnix_irpass_clear();
        return;
    }

    int ids[5];
    for (int i = 0; i < 5; i++) ids[i] = (int)(intptr_t)ops[i].handler;
    int expect_ids[5] = { OP_PUSH_INT, OP_JMP, OP_PUSH_INT, OP_ADD, OP_HALT };
    int shape_ok = 1;
    for (int i = 0; i < 5; i++) {
        if (ids[i] != expect_ids[i]) shape_ok = 0;
    }
    CHECK(shape_ok, "fixture_dead_region_with_internal_jumps: surviving op shape matches expectation");
    CHECK(ops[0].a == 1, "fixture_dead_region_with_internal_jumps: op0 (PUSH_INT 1) untouched");
    int64_t jmp_target = 1 + ops[1].a / ASNIX_IROP_SIZE;
    CHECK(jmp_target == 2, "fixture_dead_region_with_internal_jumps: op1 (JMP) retargeted to new index 2");
    CHECK(ops[2].a == 2, "fixture_dead_region_with_internal_jumps: surviving PUSH_INT 2 untouched");

    /* Link + actually run the rewritten bytecode -- confirms the
     * rewrite is not just shaped right but still evaluates correctly
     * (1 + 2 == 3), exercising the exact same ir_link/asnix_eval path
     * the real interpreter uses. */
    ir_link(ops, len);
    IrProgram prog;
    prog.ops = ops;
    prog.pool = NULL;
    Value v = asnix_eval(&prog, root_env());
    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "fixture_dead_region_with_internal_jumps: result is an int");
    CHECK(asnix_int_value(v) == 3, "fixture_dead_region_with_internal_jumps: 1 + 2 == 3 after the rewrite");

    asnix_irpass_clear();
}

/* No dead code at all -- the pass must report "no change" and leave the
 * array byte-for-byte identical (the single most important invariant:
 * a program with nothing dead must come out exactly as it went in). */
static void fixture_no_dead_code_is_a_no_op(void) {
    IrOp ops[3];
    IrOp original[3];
    size_t len = 3;
    ops[0] = mk(OP_PUSH_INT, 5, 0);
    ops[1] = mk(OP_PUSH_INT, 6, 0);
    ops[2] = mk(OP_HALT, 0, 0);
    memcpy(original, ops, sizeof(ops));

    asnix_irpass_clear();
    asnix_irpass_register_dce();
    int64_t *pool = NULL;
    size_t pool_len = 0, pool_cap = 0;
    int changed = asnix_irpass_run(ops, &len, &pool, &pool_len, &pool_cap);
    CHECK(!changed, "fixture_no_dead_code_is_a_no_op: dce should report no change");
    CHECK(len == 3, "fixture_no_dead_code_is_a_no_op: len unchanged");
    CHECK(memcmp(ops, original, sizeof(ops)) == 0, "fixture_no_dead_code_is_a_no_op: ops array byte-identical");
    asnix_irpass_clear();
}

int main(void) {
    asnix_arena_init(0);
    fixture_dead_region_with_internal_jumps();
    fixture_no_dead_code_is_a_no_op();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_irpass_dce: all checks passed\n");
    return 0;
}
