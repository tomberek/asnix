/* M5 validation, same strategy as M3: exercise the new asm ops via
 * hand-fed IR before any parser grammar depends on them, so a bug is
 * isolated to the asm side, not tangled up with parser/compiler changes. */
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/irbuild.h"
#include "asnix/value.h"
#include "symtab.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static Env *root_env(void) {
    /* M118: Env.up is a dedicated-arena-relative compressed offset --
     * must come from the real allocator, not a hand-rolled general-
     * arena alloc. */
    return asnix_env_alloc(NULL, 0);
}

static void *untag(Value v) {
    return (void *)(v.raw & ~0x7ULL);
}

static void fixture_mk_list(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 10);
    ib_push_int(b, 20);
    ib_push_int(b, 30);
    ib_mk_list(b, 3);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());

    CHECK(asnix_tag_of(v) == ASNIX_TAG_LIST, "mk_list: result is tagged LIST");
    int64_t *list = untag(v);
    int64_t count = *(int64_t *)((char *)list + ASNIX_LIST_OFF_COUNT);
    CHECK(count == 3, "mk_list: count == 3");
    Value *elems = (Value *)((char *)list + ASNIX_LIST_OFF_ELEMS);
    CHECK(asnix_int_value(elems[0]) == 10, "mk_list: elems[0] == 10 (push order preserved)");
    CHECK(asnix_int_value(elems[1]) == 20, "mk_list: elems[1] == 20");
    CHECK(asnix_int_value(elems[2]) == 30, "mk_list: elems[2] == 30");
}

static void fixture_mk_attrs_and_select(void) {
    uint32_t sym_a = asnix_intern("a", 1);
    uint32_t sym_b = asnix_intern("b", 1);
    uint32_t sorted[2];
    uint32_t vals[2]; /* keep push order matching the sorted symbol order */
    if (sym_a < sym_b) {
        sorted[0] = sym_a;
        sorted[1] = sym_b;
    } else {
        sorted[0] = sym_b;
        sorted[1] = sym_a;
    }
    (void)vals;

    IrBuilder *b = ib_new();
    /* Push values in the SAME order as `sorted` (ascending symbol id),
     * matching what the real compiler will do for `{a=1; b=2;}`. */
    ib_push_int(b, sorted[0] == sym_a ? 1 : 2);
    ib_push_int(b, sorted[0] == sym_a ? 2 : 1);
    ib_mk_attrs(b, 2, sorted);
    ib_select(b, sym_a);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, root_env());

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "mk_attrs+select: selecting 'a' gives an INT");
    CHECK(asnix_int_value(v) == 1, "mk_attrs+select: attrs.a == 1");
}

static void fixture_select_missing_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        uint32_t sym_a = asnix_intern("only_key_in_this_fixture", 24);
        uint32_t sorted[1] = { sym_a };
        IrBuilder *b = ib_new();
        ib_push_int(b, 1);
        ib_mk_attrs(b, 1, sorted);
        ib_select(b, asnix_intern("definitely_not_present", 22));
        ib_halt(b);
        IrProgram *prog = ib_finish(b);
        asnix_eval(prog, root_env());
        _exit(42); /* reaching here means the missing-attr abort didn't fire */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1, "selecting a missing attribute aborts with exit code 1");
}

int main(void) {
    asnix_arena_init(0);
    fixture_mk_list();
    fixture_mk_attrs_and_select();
    fixture_select_missing_aborts();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_m5_fixtures: all checks passed\n");
    return 0;
}
