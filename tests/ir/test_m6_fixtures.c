/* M6 validation, same strategy as M3/M5: exercise thunk creation, forcing,
 * memoization, and blackhole detection via hand-fed IR (and, for
 * memoization/blackhole, hand-built Thunk cells) before the compiler
 * depends on any of it. */
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
    Env *e = asnix_arena_alloc(8);
    e->up = NULL;
    return e;
}

/* Constructing a thunk and halting immediately (never forcing it) proves
 * the defining property of laziness: building a thunk does not run its
 * body. */
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
    /* M103: OFF_STATE now doubles as the prog pointer while UNFORCED --
     * mask to the low tag bits rather than comparing the raw word. */
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
    ib_var_ref(b, 0, 0); /* forces */
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
    /* M103: OFF_STATE doubles as the prog pointer while UNFORCED (tag
     * bits 0 == UNFORCED, free since body_prog is >=8-byte-aligned) --
     * one store, not two; see ir.h's comment. */
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

/* A thunk whose body forces itself (constructed directly, bypassing the
 * compiler -- M6's compiler only emits non-recursive bindings; see
 * docs/architecture.md). This tests the asm-level blackhole mechanism
 * itself: re-entering an IN_PROGRESS thunk must abort cleanly, not loop or
 * corrupt state. */
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
        /* M103: OFF_STATE doubles as the prog pointer while UNFORCED --
         * see fixture_memoization's matching comment above. */
        *(IrProgram **)((char *)thunk_mem + ASNIX_THUNK_OFF_STATE) = body_prog;
        *(void **)((char *)thunk_mem + ASNIX_THUNK_OFF_ENV) = frame; /* captures its own frame */
        Value thunk_val = { .raw = (uint64_t)(uintptr_t)thunk_mem | ASNIX_TAG_THUNK };
        frame->values[0] = thunk_val; /* ...at the very slot that references it */

        IrBuilder *b = ib_new();
        ib_var_ref(b, 0, 0);
        ib_halt(b);
        IrProgram *prog = ib_finish(b);
        asnix_eval(prog, frame);
        _exit(42); /* reaching here means blackholing failed to trigger */
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
