#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asnix/arena.h"
#include "asnix/env.h"
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

static void fixture_thunk_not_evaluated_until_forced(void) {
    IrBuilder *body = ib_new();
    ib_push_int(body, 999);
    ib_ret(body);
    IrProgram *body_prog = ib_finish(body);

    IrBuilder *b = ib_new();
    ib_mk_thunk(b, body_prog);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());

    CHECK(asnix_tag_of(v) == ASNIX_TAG_THUNK, "unforced thunk keeps its THUNK tag at HALT");
    void *cell = (void *)(v.raw & ~0x7ULL);
    int64_t state = *(int64_t *)((char *)cell + ASNIX_THUNK_OFF_STATE) & ASNIX_TAG_MASK;
    CHECK(state == ASNIX_THUNK_STATE_UNFORCED, "constructing a thunk never runs its body");
}

static void fixture_force_let(void) {
    IrBuilder *body = ib_new();
    ib_push_int(body, 1);
    ib_push_int(body, 2);
    ib_add(body);
    ib_ret(body);
    IrProgram *body_prog = ib_finish(body);

    IrBuilder *b = ib_new();
    ib_mk_thunk(b, body_prog);
    ib_let(b, 1);
    ib_var_ref(b, 0, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "forcing a let-bound thunk yields an INT");
    CHECK(asnix_int_value(v) == 3, "forcing a let-bound thunk runs its body (1+2==3)");
}

static void fixture_memoization(void) {
    IrBuilder *body = ib_new();
    ib_push_int(body, 42);
    ib_ret(body);
    IrProgram *body_prog = ib_finish(body);

    void *thunk_mem = asnix_arena_alloc(ASNIX_THUNK_SIZE);
    *(IrProgram **)((char *)thunk_mem + ASNIX_THUNK_OFF_STATE) = body_prog;
    *(void **)((char *)thunk_mem + ASNIX_THUNK_OFF_ENV) = root_env();
    CHECK((*(int64_t *)((char *)thunk_mem + ASNIX_THUNK_OFF_STATE) & ASNIX_TAG_MASK) == ASNIX_THUNK_STATE_UNFORCED,
          "memoization: starts UNFORCED");

    Env *frame = asnix_env_alloc(root_env(), 1);
    Value thunk_val = { .raw = (uint64_t)(uintptr_t)thunk_mem | ASNIX_TAG_THUNK };
    frame->values[0] = thunk_val;

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value result = asnix_eval(prog, frame);

    CHECK(asnix_tag_of(result) == ASNIX_TAG_INT && asnix_int_value(result) == 42,
          "memoization: forced value is correct");
    CHECK(*(int64_t *)((char *)thunk_mem + ASNIX_THUNK_OFF_STATE) == ASNIX_THUNK_STATE_FORCED,
          "memoization: state transitions UNFORCED -> FORCED");
    Value cached = *(Value *)((char *)thunk_mem + ASNIX_THUNK_OFF_ENV);
    CHECK(asnix_values_equal(cached, result), "memoization: cached raw value matches the returned result");
}

static void fixture_blackhole_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);

        IrBuilder *body = ib_new();
        ib_var_ref(body, 0, 0);
        ib_ret(body);
        IrProgram *body_prog = ib_finish(body);

        Env *frame = asnix_env_alloc(root_env(), 1);
        void *thunk_mem = asnix_arena_alloc(ASNIX_THUNK_SIZE);
        *(IrProgram **)((char *)thunk_mem + ASNIX_THUNK_OFF_STATE) = body_prog;
        *(void **)((char *)thunk_mem + ASNIX_THUNK_OFF_ENV) = frame;
        Value thunk_val = { .raw = (uint64_t)(uintptr_t)thunk_mem | ASNIX_TAG_THUNK };
        frame->values[0] = thunk_val;

        IrBuilder *b = ib_new();
        ib_var_ref(b, 0, 0);
        ib_halt(b);
        IrProgram *prog = ib_finish(b);
        asnix_eval(prog, frame);
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1, "forcing a self-referential thunk aborts (blackhole)");
}

int main(void) {
    asnix_arena_init(0);
    fixture_thunk_not_evaluated_until_forced();
    fixture_force_let();
    fixture_memoization();
    fixture_blackhole_aborts();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_m6_fixtures: all checks passed\n");
    return 0;
}

