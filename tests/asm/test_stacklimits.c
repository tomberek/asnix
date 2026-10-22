/*
 * Regression coverage for the VALSTACK/RETSP bounds-check fix (see
 * include/asnix/stackcheck.h, src/asm/stacklimits.S). Before this fix,
 * neither buffer had any bounds check at all: exceeding either silently
 * corrupted whatever the shared arena allocated next (sometimes a clean
 * segfault, sometimes wrong answers) instead of aborting loudly -- and
 * both old caps (4096 Values / 128 call frames) were small enough to hit
 * in ordinary, not-even-pathological programs.
 *
 * Builds hand-fed, flat (non-recursive) IR via IrBuilder rather than
 * going through the lexer/parser/compiler specifically so these cases
 * can reach depths/sizes well past the *old* caps without tripping over
 * the separate, unrelated native-C-stack-recursion limit in compiler.c's
 * AST_LET handling (see docs/architecture.md's "Known bugs" list) --
 * that limit is a real, independently-confirmed issue, but it kicks in
 * around ~31k levels of *syntactic* nesting and is not what this file is
 * about; building the IR directly with a flat loop sidesteps it so each
 * check here isolates the asm-level VALSTACK/RETSP behavior only.
 */
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
    Env *e = asnix_arena_alloc(8);
    e->up = NULL;
    return e;
}

/* Pushes `count` int literals (far beyond the old 4096-Value cap) and
 * folds them into one list -- proves VALSTACK now handles a single large
 * list literal correctly rather than corrupting past ~4096 elements. */
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

/* Builds a chain of `depth` nested, mutually dependent lets -- each
 * binding's thunk body forces the previous one (x_i = x_{i-1} + 1) --
 * exactly the shape that used to blow the old 128-call-frame RETSP cap
 * once depth passed ~127 simultaneous force-frames. Forcing the last
 * binding proves RETSP now handles that depth correctly. */
static void fixture_retsp_deep_chain(int64_t depth) {
    IrBuilder *b = ib_new();
    for (int64_t i = 0; i < depth; i++) {
        IrBuilder *body = ib_new();
        if (i == 0) {
            ib_push_int(body, 0);
        } else {
            ib_var_ref(body, 0, 0); /* x_{i-1}: top slot of the frame this thunk captured */
            ib_push_int(body, 1);
            ib_add(body);
        }
        ib_ret(body);
        IrProgram *body_prog = ib_finish(body);

        ib_mk_thunk(b, body_prog); /* captures the current (not-yet-extended) frame */
        ib_let(b, 1);              /* new frame, up = the one just captured above */
    }
    ib_var_ref(b, 0, 0); /* force x_{depth-1} */
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "forcing the end of a deep dependent-thunk chain yields an INT");
    CHECK(asnix_int_value(v) == depth - 1, "the deep chain computes the right value (no corruption)");
}

/* Exceeding the (new, much larger) VALSTACK cap must abort cleanly via
 * asnix_fatal (exit code 1, message on stderr) rather than segfaulting
 * or silently corrupting memory. */
static void test_valstack_overflow_aborts_cleanly(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr); /* keep the abort message out of test output */
        IrBuilder *b = ib_new();
        /* Comfortably past ASNIX_VALSTACK_BYTES/8 (1,048,576 Values) --
         * never popped, so this must trip the bounds check before HALT. */
        for (int64_t i = 0; i < 2000000; i++) {
            ib_push_int(b, i);
        }
        ib_halt(b);
        IrProgram *prog = ib_finish(b);
        asnix_eval(prog, root_env());
        _exit(42); /* reaching here means the overflow check failed to trigger */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "exceeding the VALSTACK cap aborts the process with exit code 1, not a crash");
}

/* Same proof for RETSP: a dependent-thunk chain deep enough to exceed
 * the new cap (65,536 call frames) must abort cleanly, not segfault. */
static void test_retsp_overflow_aborts_cleanly(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        const int64_t depth = 70000; /* > ASNIX_RETSTACK_BYTES/ASNIX_RETFRAME_SIZE (65,536) */
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
        ib_var_ref(b, 0, 0); /* forces the whole chain, pushing one RetFrame per level */
        ib_halt(b);
        IrProgram *prog = ib_finish(b);
        asnix_eval(prog, root_env());
        _exit(42); /* reaching here means the overflow check failed to trigger */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "exceeding the RETSP cap aborts the process with exit code 1, not a crash");
}

/* Builds the same kind of dependent-thunk chain as
 * fixture_retsp_deep_chain, but entirely by hand (manual Thunk/Env cells,
 * same pattern as tests/ir/test_m6_fixtures.c's fixture_memoization) and
 * returns the tagged Value for the deepest (innermost-dependent) thunk,
 * for forcing via asnix_force_for_print directly -- i.e. *without* any
 * asnix_eval invocation at all, so this exercises force.S's own
 * separate, smaller VALSTACK/RETSP buffers specifically (see force.S),
 * not dispatch.S's. */
static Value build_manual_thunk_chain(int64_t depth) {
    Env *frame = root_env();
    for (int64_t i = 0; i < depth; i++) {
        IrBuilder *body = ib_new();
        if (i == 0) {
            ib_push_int(body, 0);
        } else {
            ib_var_ref(body, 0, 0); /* previous thunk: top slot of `frame` */
            ib_push_int(body, 1);
            ib_add(body);
        }
        ib_ret(body);
        IrProgram *body_prog = ib_finish(body);

        void *thunk_mem = asnix_arena_alloc(ASNIX_THUNK_SIZE);
        *(int64_t *)((char *)thunk_mem + ASNIX_THUNK_OFF_STATE) = ASNIX_THUNK_STATE_UNFORCED;
        *(void **)((char *)thunk_mem + ASNIX_THUNK_OFF_ENV) = frame;
        *(IrProgram **)((char *)thunk_mem + ASNIX_THUNK_OFF_PROG) = body_prog; /* M100: one pointer, not {ops,pool} */

        Env *next = asnix_env_alloc(frame, 1);
        next->values[0].raw = (uint64_t)(uintptr_t)thunk_mem | ASNIX_TAG_THUNK;
        frame = next;
    }
    return frame->values[0];
}

/* force.S's own RETSP buffer, used only when asnix_force_for_print forces
 * a thunk from *outside* any active asnix_eval (e.g. main.c's printer,
 * or a builtin like derivation.c's that forces a value mid-eval) --
 * proves it now handles a chain well past the old 32-call-frame cap
 * (ASNIX_FORCE_RETSTACK_BYTES was 1024, / ASNIX_RETFRAME_SIZE=32). */
static void fixture_force_deep_chain(int64_t depth) {
    Value chain = build_manual_thunk_chain(depth);
    Value v = asnix_force_for_print(chain);
    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "force.S forcing a deep standalone chain yields an INT");
    CHECK(asnix_int_value(v) == depth - 1, "force.S's deep chain computes the right value (no corruption)");
}

/* Exceeding force.S's own (new, larger) RETSP cap must also abort
 * cleanly, not segfault -- same fix, same proof, for the separate
 * buffers force.S brings up itself. */
static void test_force_retsp_overflow_aborts_cleanly(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        /* Comfortably past ASNIX_FORCE_RETSTACK_BYTES/ASNIX_RETFRAME_SIZE
         * (2,048 call frames). */
        Value chain = build_manual_thunk_chain(5000);
        asnix_force_for_print(chain);
        _exit(42); /* reaching here means the overflow check failed to trigger */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "exceeding force.S's own RETSP cap aborts the process with exit code 1, not a crash");
}

int main(void) {
    fixture_valstack_large_list(20000);   /* ~5x the old 4096-Value cap */
    fixture_retsp_deep_chain(2000);       /* ~15x the old 128-call-frame cap */
    fixture_force_deep_chain(500);        /* ~15x force.S's old 32-call-frame cap */
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
