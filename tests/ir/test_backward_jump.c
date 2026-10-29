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
    int64_t pool[2] = { 1, 2 };
    IrOp ops[6];
    ops[0] = mk(OP_PUSH_INT, 0, 0);
    ops[1] = mk(OP_PUSH_INT, 0, 1);
    ops[2] = mk(OP_JMP, (5 - 2) * ASNIX_IROP_SIZE, 0);
    ops[3] = mk(OP_ADD, 0, 0);
    ops[4] = mk(OP_HALT, 0, 0);
    ops[5] = mk(OP_JMP, (3 - 5) * ASNIX_IROP_SIZE, 0);

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

