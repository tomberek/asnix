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

static void fixture_valstack_large_list(int64_t count) {
    IrBuilder *b = ib_new();
    for (int64_t i = 0; i < count; i++) {
        ib_push_int(b, i);
    }
    ib_mk_list(b, count);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());

    CHECK(asnix_tag_of(v) == ASNIX_TAG_LIST, "a large list literal still produces a LIST value");
    const char *base = (const char *)(v.raw & ~0x7ULL);
    int64_t got_count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
    CHECK(got_count == count, "the large list's element count survives intact");
    const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
    CHECK(asnix_int_value(elems[0]) == 0, "first element of the large list is correct");
    CHECK(asnix_int_value(elems[count - 1]) == count - 1, "last element of the large list is correct");
}

static void fixture_retsp_deep_chain(int64_t depth) {
    IrBuilder *b = ib_new();
    for (int64_t i = 0; i < depth; i++) {
        IrBuilder *body = ib_new();
        if (i == 0) {
            ib_push_int(body, 0);
        } else {
            ib_var_ref(body, 0, 0);
            ib_push_int(body, 1);
            ib_add(body);
        }
        ib_ret(body);
        IrProgram *body_prog = ib_finish(body);

        ib_mk_thunk(b, body_prog);
        ib_let(b, 1);
    }
    ib_var_ref(b, 0, 0);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "forcing the end of a deep dependent-thunk chain yields an INT");
    CHECK(asnix_int_value(v) == depth - 1, "the deep chain computes the right value (no corruption)");
}

static void test_valstack_overflow_aborts_cleanly(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        IrBuilder *b = ib_new();
        for (int64_t i = 0; i < 2000000; i++) {
            ib_push_int(b, i);
        }
        ib_halt(b);
        IrProgram *prog = ib_finish(b);
        asnix_eval(prog, root_env());
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "exceeding the VALSTACK cap aborts the process with exit code 1, not a crash");
}

static void test_retsp_overflow_aborts_cleanly(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        const int64_t depth = 70000;
        IrBuilder *b = ib_new();
        for (int64_t i = 0; i < depth; i++) {
            IrBuilder *body = ib_new();
            if (i == 0) {
                ib_push_int(body, 0);
            } else {
                ib_var_ref(body, 0, 0);
                ib_push_int(body, 1);
                ib_add(body);
            }
            ib_ret(body);
            IrProgram *body_prog = ib_finish(body);
            ib_mk_thunk(b, body_prog);
            ib_let(b, 1);
        }
        ib_var_ref(b, 0, 0);
        ib_halt(b);
        IrProgram *prog = ib_finish(b);
        asnix_eval(prog, root_env());
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "exceeding the RETSP cap aborts the process with exit code 1, not a crash");
}

static Value build_manual_thunk_chain(int64_t depth) {
    Env *frame = root_env();
    for (int64_t i = 0; i < depth; i++) {
        IrBuilder *body = ib_new();
        if (i == 0) {
            ib_push_int(body, 0);
        } else {
            ib_var_ref(body, 0, 0);
            ib_push_int(body, 1);
            ib_add(body);
        }
        ib_ret(body);
        IrProgram *body_prog = ib_finish(body);

        void *thunk_mem = asnix_arena_alloc(ASNIX_THUNK_SIZE);
        *(IrProgram **)((char *)thunk_mem + ASNIX_THUNK_OFF_STATE) = body_prog;
        *(void **)((char *)thunk_mem + ASNIX_THUNK_OFF_ENV) = frame;

        Env *next = asnix_env_alloc(frame, 1);
        next->values[0].raw = (uint64_t)(uintptr_t)thunk_mem | ASNIX_TAG_THUNK;
        frame = next;
    }
    return frame->values[0];
}

static void fixture_force_deep_chain(int64_t depth) {
    Value chain = build_manual_thunk_chain(depth);
    Value v = asnix_force_for_print(chain);
    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "force.S forcing a deep standalone chain yields an INT");
    CHECK(asnix_int_value(v) == depth - 1, "force.S's deep chain computes the right value (no corruption)");
}

static void test_force_retsp_overflow_aborts_cleanly(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        Value chain = build_manual_thunk_chain(5000);
        asnix_force_for_print(chain);
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "exceeding force.S's own RETSP cap aborts the process with exit code 1, not a crash");
}

int main(void) {
    fixture_valstack_large_list(20000);
    fixture_retsp_deep_chain(2000);
    fixture_force_deep_chain(500);
    test_valstack_overflow_aborts_cleanly();
    test_retsp_overflow_aborts_cleanly();
    test_force_retsp_overflow_aborts_cleanly();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_stacklimits: all checks passed\n");
    return 0;
}

