#include <stdio.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/irbuild.h"
#include "asnix/value.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static Env *root_env(void) {
    return asnix_env_alloc(NULL, 0);
}

static Value test_add_one(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_int(asnix_int_value(args[0]) + 1);
}

static Value test_add_two(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_int(asnix_int_value(args[0]) + asnix_int_value(args[1]));
}

static Value make_primop(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    Value v = { .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
    return v;
}

static void fixture_call_primop_unary(void) {
    Value primop_val = make_primop((void *)test_add_one, 1);
    Env *frame = asnix_env_alloc(root_env(), 1);
    frame->values[0] = primop_val;

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_push_int(b, 41);
    ib_call(b, 1);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, frame);

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "primop call result is an INT");
    CHECK(asnix_int_value(v) == 42, "calling a unary primop runs the native fn (41+1==42)");
}

static void fixture_call_primop_binary(void) {
    Value primop_val = make_primop((void *)test_add_two, 2);
    Env *frame = asnix_env_alloc(root_env(), 1);
    frame->values[0] = primop_val;

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_push_int(b, 3);
    ib_push_int(b, 4);
    ib_call(b, 2);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, frame);

    CHECK(asnix_int_value(v) == 7, "calling a binary primop with 2 args (3+4==7)");
}

int main(void) {
    asnix_arena_init(0);
    fixture_call_primop_unary();
    fixture_call_primop_binary();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_m7_fixtures: all checks passed\n");
    return 0;
}

