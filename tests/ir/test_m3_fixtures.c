#include <stdio.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "asnix/irbuild.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static Env *root_env(void) {
    Env *e = asnix_arena_alloc(8);
    e->up = NULL;
    return e;
}

static void check_int_result(Value v, int64_t expected, const char *what) {
    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, what);
    CHECK(asnix_int_value(v) == expected, what);
}

/* 2 + 3*4 = 14 */
static void fixture_arith(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 2);
    ib_push_int(b, 3);
    ib_push_int(b, 4);
    ib_mul(b);
    ib_add(b);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());
    check_int_result(v, 14, "fixture_arith: 2 + 3*4 == 14");
}

/* if true then 1 else 2 == 1; if false then 1 else 2 == 2 */
static void fixture_if(int cond, int64_t expected) {
    IrBuilder *b = ib_new();
    if (cond)
        ib_push_true(b);
    else
        ib_push_false(b);
    size_t if_site = ib_if(b);
    ib_push_int(b, 1);
    size_t jmp_site = ib_jmp(b);
    size_t else_idx = ib_len(b);
    ib_patch_delta(b, if_site, else_idx);
    ib_push_int(b, 2);
    size_t end_idx = ib_len(b);
    ib_patch_delta(b, jmp_site, end_idx);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());
    check_int_result(v, expected, cond ? "fixture_if(true) == 1" : "fixture_if(false) == 2");
}

/* let x=10; y=20; in x+y == 30 -- validates frame alloc + depth-0 lookup */
static void fixture_let(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 10);
    ib_push_int(b, 20);
    ib_let(b, 2);
    ib_var_ref(b, 0, 0);
    ib_var_ref(b, 0, 1);
    ib_add(b);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());
    check_int_result(v, 30, "fixture_let: let x=10;y=20;in x+y == 30");
}

/* let x=1; in let y=2; in x+y == 3 -- validates the depth-1 up-chain hop */
static void fixture_nested_let(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 1);
    ib_let(b, 1);
    ib_push_int(b, 2);
    ib_let(b, 1);
    ib_var_ref(b, 1, 0); /* x, one frame up */
    ib_var_ref(b, 0, 0); /* y, current frame */
    ib_add(b);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());
    check_int_result(v, 3, "fixture_nested_let: let x=1;in let y=2;in x+y == 3");
}

/* let f = \a b -> a+b; in f 3 4 == 7 -- validates closure capture + call/ret */
static void fixture_lambda_call(void) {
    IrBuilder *body = ib_new();
    ib_var_ref(body, 0, 0);
    ib_var_ref(body, 0, 1);
    ib_add(body);
    ib_ret(body);
    IrProgram *body_prog = ib_finish(body);

    IrBuilder *b = ib_new();
    ib_lambda(b, 2, body_prog);
    ib_let(b, 1);
    ib_var_ref(b, 0, 0); /* push f */
    ib_push_int(b, 3);
    ib_push_int(b, 4);
    ib_call(b, 2);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());
    check_int_result(v, 7, "fixture_lambda_call: let f=\\a b->a+b;in f 3 4 == 7");
}

int main(void) {
    asnix_arena_init(0);
    fixture_arith();
    fixture_if(1, 1);
    fixture_if(0, 2);
    fixture_let();
    fixture_nested_let();
    fixture_lambda_call();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_m3_fixtures: all checks passed\n");
    return 0;
}
