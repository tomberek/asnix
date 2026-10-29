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
    int64_t pool_storage[12] = { 1, 0, 10, 20, 0, 30, 0, 40, 50, 2, 0, 0 };
    int64_t *pool = pool_storage;
    size_t pool_len = 12, pool_cap = 12;
    ops[0] = mk(OP_PUSH_INT, 0, 0);
    ops[1] = mk(OP_JMP, (9 - 1) * ASNIX_IROP_SIZE, 0);
    ops[2] = mk(OP_PUSH_INT, 0, 2);
    ops[3] = mk(OP_PUSH_INT, 0, 3);
    ops[4] = mk(OP_IF, (7 - 4) * ASNIX_IROP_SIZE, 0);
    ops[5] = mk(OP_PUSH_INT, 0, 5);
    ops[6] = mk(OP_JMP, (8 - 6) * ASNIX_IROP_SIZE, 0);
    ops[7] = mk(OP_PUSH_INT, 0, 7);
    ops[8] = mk(OP_PUSH_INT, 0, 8);
    ops[9] = mk(OP_PUSH_INT, 0, 9);
    ops[10] = mk(OP_ADD, 0, 0);
    ops[11] = mk(OP_HALT, 0, 0);

    asnix_irpass_clear();
    asnix_irpass_register_dce();
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
    CHECK(pool[ops[0].b] == 1, "fixture_dead_region_with_internal_jumps: op0 (PUSH_INT 1) untouched");
    int64_t jmp_target = 1 + ops[1].a / ASNIX_IROP_SIZE;
    CHECK(jmp_target == 2, "fixture_dead_region_with_internal_jumps: op1 (JMP) retargeted to new index 2");
    CHECK(pool[ops[2].b] == 2, "fixture_dead_region_with_internal_jumps: surviving PUSH_INT 2 untouched");

    ir_link(ops, len);
    IrProgram prog;
    prog.ops = ops;
    prog.pool = pool;
    Value v = asnix_eval(&prog, root_env());
    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "fixture_dead_region_with_internal_jumps: result is an int");
    CHECK(asnix_int_value(v) == 3, "fixture_dead_region_with_internal_jumps: 1 + 2 == 3 after the rewrite");

    asnix_irpass_clear();
}

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

