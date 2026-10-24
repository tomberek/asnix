/* M169 regression test: a genuinely BACKWARD IR jump (a negative
 * self-relative byte delta in `a`).
 *
 * This project's own compiler NEVER emits one (Nix has no loop
 * construct, and every forward-only jump this compiler does emit is
 * already exercised by the ordinary e2e/conformance corpus) -- confirmed
 * directly: grepping compiler.c's every ib_patch_delta call site shows
 * `target_idx` is always a LATER point in the same IrBuilder than
 * `site`, and tests/ir/test_irpass_dce.c's own header comment says so
 * explicitly for the optimizer passes too. So nothing in the existing
 * test suite -- which only ever feeds this project real compiled Nix
 * source, or a hand-built fixture that (like test_irpass_dce.c's own)
 * happens to only use forward deltas -- would ever catch a sign-
 * extension bug specific to a NEGATIVE delta.
 *
 * That matters a great deal for M169 specifically: IrOp.a/.b narrowed
 * from int64_t to int32_t, which means every asm handler's read of
 * those fields had to change from a plain 8-byte `movq` to a 4-byte
 * `movslq` (SIGN-extending into the 64-bit register the rest of the
 * handler expects) -- using `movl` (zero-extend) instead would be
 * silently correct for every non-negative field (which is every field
 * except a jump's own byte delta) and silently WRONG only for a genuine
 * backward jump: a negative int32 zero-extended into 64 bits becomes a
 * huge positive number, so `next_ip = this_op_addr + delta` would jump
 * to a wildly invalid address instead of backward -- a crash, not a
 * quiet miscalculation, but one this project's whole existing suite
 * would never exercise at all without a test like this one.
 *
 * Hand-builds (bypassing IrBuilder/ib_jmp/ib_patch_delta entirely, same
 * "raw IrOp[] array" style as test_irpass_dce.c, and for the identical
 * reason: there is no way to reach this shape through the ordinary
 * compiler/builder API):
 *   0: PUSH_INT 1   -- live
 *   1: PUSH_INT 2   -- live
 *   2: JMP -> 5      -- live; forward, skips the backward-landing pad
 *   3: ADD           -- live; BACKWARD-jump landing pad (op5's target),
 *                       never reached by falling through from op2
 *   4: HALT          -- live; op3 (ADD) falls straight through into this
 *   5: JMP -> 3       -- live; BACKWARD (delta = (3-5)*16 = -32, negative)
 *
 * Execution order: 0, 1, 2 (jumps fwd to 5), 5 (jumps BACKWARD to 3), 3
 * (ADD: pops 2 and 1, pushes 3), 4 (HALT). No cycle: op3 falls through to
 * op4 (HALT), never back to op1/op2, so this terminates in one pass
 * through the backward edge, not an infinite loop. If the backward
 * delta were ever misread as a huge positive offset, op5's `jmp *%rax`
 * would land on garbage -- almost certainly a segfault, not a wrong
 * answer, well before this test's own CHECK()s ever ran. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/value.h"

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
    o.a = (int32_t)a;
    o.b = (int32_t)b;
    return o;
}

static Env *root_env(void) {
    return asnix_env_alloc(NULL, 0);
}

static void fixture_backward_jump_adds_correctly(void) {
    int64_t pool[2] = { 1, 2 }; /* op0's literal at index 0, op1's at index 1 */
    IrOp ops[6];
    ops[0] = mk(OP_PUSH_INT, 0, 0); /* b = pool index 0 -> literal 1 */
    ops[1] = mk(OP_PUSH_INT, 0, 1); /* b = pool index 1 -> literal 2 */
    ops[2] = mk(OP_JMP, (5 - 2) * ASNIX_IROP_SIZE, 0);  /* forward: +48 */
    ops[3] = mk(OP_ADD, 0, 0);
    ops[4] = mk(OP_HALT, 0, 0);
    ops[5] = mk(OP_JMP, (3 - 5) * ASNIX_IROP_SIZE, 0); /* BACKWARD: -32 */

    /* Sanity on the test's own construction, independent of asnix_eval:
     * the delta really is negative, so this genuinely exercises the
     * sign-extension path, not an accidental forward jump. */
    CHECK(ops[5].a == -32, "op5's own backward delta is stored as a genuine negative int32");

    ir_link(ops, 6);
    IrProgram prog;
    prog.ops = ops;
    prog.pool = pool;
    Value v = asnix_eval(&prog, root_env());
    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "backward-jump program's result is an int");
    CHECK(asnix_int_value(v) == 3, "backward-jump program evaluates 1 + 2 == 3 after landing on the ADD");
}

int main(void) {
    asnix_arena_init(0);
    fixture_backward_jump_adds_correctly();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_backward_jump: all checks passed\n");
    return 0;
}
