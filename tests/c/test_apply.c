#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
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

static Value make_add_one_closure(void) {
    IrBuilder *body = ib_new();
    ib_var_ref(body, 0, 0);
    ib_push_int(body, 1);
    ib_add(body);
    ib_ret(body);
    IrProgram *body_prog = ib_finish(body);

    IrBuilder *b = ib_new();
    ib_lambda(b, 1, body_prog, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    return asnix_eval(prog, root_env());
}

static Value make_sub_closure(void) {
    IrBuilder *body = ib_new();
    ib_var_ref(body, 0, 0);
    ib_var_ref(body, 0, 1);
    ib_sub(body);
    ib_ret(body);
    IrProgram *body_prog = ib_finish(body);

    IrBuilder *b = ib_new();
    ib_lambda(b, 2, body_prog, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    return asnix_eval(prog, root_env());
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

static void fixture_apply_closure_unary(void) {
    Value fn = make_add_one_closure();
    CHECK(asnix_tag_of(fn) == ASNIX_TAG_CLOSURE, "make_add_one_closure produced a Closure value");

    Value args[1] = { asnix_make_int(41) };
    Value v = asnix_apply(fn, args, 1);

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "applying a unary closure yields an INT");
    CHECK(asnix_int_value(v) == 42, "asnix_apply runs the closure body (41+1==42)");
}

static void fixture_apply_closure_binary(void) {
    Value fn = make_sub_closure();
    CHECK(asnix_tag_of(fn) == ASNIX_TAG_CLOSURE, "make_sub_closure produced a Closure value");

    Value args[2] = { asnix_make_int(10), asnix_make_int(3) };
    Value v = asnix_apply(fn, args, 2);

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "applying a binary closure yields an INT");
    CHECK(asnix_int_value(v) == 7, "asnix_apply preserves argument order (10-3==7)");
}

static void fixture_apply_closure_twice(void) {
    Value fn = make_add_one_closure();
    Value args1[1] = { asnix_make_int(0) };
    Value args2[1] = { asnix_make_int(100) };

    Value v1 = asnix_apply(fn, args1, 1);
    Value v2 = asnix_apply(fn, args2, 1);

    CHECK(asnix_int_value(v1) == 1, "first sequential asnix_apply call is correct");
    CHECK(asnix_int_value(v2) == 101, "second sequential asnix_apply call is correct (no leaked state)");
}

static void fixture_apply_primop(void) {
    Value fn = make_primop((void *)test_add_two, 2);
    Value args[2] = { asnix_make_int(3), asnix_make_int(4) };
    Value v = asnix_apply(fn, args, 2);

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "applying a primop yields an INT");
    CHECK(asnix_int_value(v) == 7, "asnix_apply dispatches straight to the native fn (3+4==7)");
}

static void fixture_apply_nested(void) {
    IrBuilder *body = ib_new();
    ib_var_ref(body, 0, 0);
    ib_ret(body);
    IrProgram *body_prog = ib_finish(body);

    IrBuilder *b = ib_new();
    ib_lambda(b, 1, body_prog, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value identity = asnix_eval(prog, root_env());

    Value inner_args[1] = { asnix_make_int(7) };
    Value inner = asnix_apply(identity, inner_args, 1);
    Value outer_args[1] = { inner };
    Value outer = asnix_apply(identity, outer_args, 1);

    CHECK(asnix_int_value(outer) == 7, "nested/sequential asnix_apply calls both see correct results");
}

static void test_apply_closure_arity_mismatch_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        Value fn = make_add_one_closure();
        Value args[2] = { asnix_make_int(1), asnix_make_int(2) };
        asnix_apply(fn, args, 2);
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "applying a closure with the wrong argc aborts with exit code 1, not a crash");
}

static void test_apply_primop_arity_mismatch_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        Value fn = make_primop((void *)test_add_two, 2);
        Value args[1] = { asnix_make_int(1) };
        asnix_apply(fn, args, 1);
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "applying a primop with the wrong argc aborts with exit code 1, not a crash");
}

static void test_apply_type_error_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        Value not_a_fn = asnix_make_int(123);
        Value args[1] = { asnix_make_int(1) };
        asnix_apply(not_a_fn, args, 1);
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "applying a non-function Value aborts with exit code 1, not a crash");
}

int main(void) {
    fixture_apply_closure_unary();
    fixture_apply_closure_binary();
    fixture_apply_closure_twice();
    fixture_apply_primop();
    fixture_apply_nested();
    test_apply_closure_arity_mismatch_aborts();
    test_apply_primop_arity_mismatch_aborts();
    test_apply_type_error_aborts();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_apply: all checks passed\n");
    return 0;
}

