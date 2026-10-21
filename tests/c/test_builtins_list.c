/*
 * Validates builtins_list.c's map/filter/foldl'/elemAt/concatLists/tail/
 * head against hand-built fixtures, run through the *real* dispatch loop
 * (asnix_eval + OP_CALL, via the same IrBuilder API the compiler uses) --
 * not by calling the static prim_* functions directly -- because the
 * curried stages' self-capture trick (see builtins_list.c's header
 * comment) relies on an invariant of op_call.S's primop call path
 * (args[-1] == the callee's own tagged Value) that only holds for
 * ordinary Nix-level application, not for a C test calling a raw function
 * pointer with a hand-rolled args array. Going through asnix_eval exactly
 * matches how real compiled Nix source invokes these.
 *
 * Every numeric fixture here is also cross-checked against real
 * `nix-instantiate --eval` in the report for this stage (not just
 * transcribed from memory) -- see the e2e fixtures under tests/e2e/
 * m_list_*.nix for the on-disk equivalents.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h" /* asnix_force_for_print */
#include "asnix/irbuild.h"
#include "asnix/value.h"
#include "builtins_list.h"

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

static Value lookup_builtin(const char *name) {
    for (size_t i = 0; i < asnix_builtins_list_count; i++) {
        if (strcmp(asnix_builtins_list[i].name, name) == 0) {
            void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
            *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = (void *)asnix_builtins_list[i].fn;
            *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = asnix_builtins_list[i].arity;
            return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
        }
    }
    fprintf(stderr, "lookup_builtin: no such builtin %s\n", name);
    exit(1);
}

static Value make_primop(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

static Value test_add_one(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_int(asnix_int_value(args[0]) + 1);
}

static Value test_is_even(const Value *args, int64_t argc) {
    (void)argc;
    return (asnix_int_value(args[0]) % 2 == 0) ? ASNIX_TRUE : ASNIX_FALSE;
}

/* `a: b: a + b`, two nested arity-1 closures, exactly how every curried
 * user lambda compiles -- used as foldl''s operator. */
static Value make_add_closure(void) {
    IrBuilder *inner = ib_new();
    ib_var_ref(inner, 1, 0); /* a, one frame up */
    ib_var_ref(inner, 0, 0); /* b, this frame */
    ib_add(inner);
    ib_ret(inner);
    IrProgram *inner_prog = ib_finish(inner);

    IrBuilder *outer = ib_new();
    ib_lambda(outer, 1, inner_prog);
    ib_ret(outer);
    IrProgram *outer_prog = ib_finish(outer);

    IrBuilder *top = ib_new();
    ib_lambda(top, 1, outer_prog);
    ib_halt(top);
    IrProgram *prog = ib_finish(top);
    return asnix_eval(prog, root_env());
}

static Value make_int_list3(int64_t a, int64_t b, int64_t c) {
    IrBuilder *b1 = ib_new();
    ib_push_int(b1, a);
    ib_push_int(b1, b);
    ib_push_int(b1, c);
    ib_mk_list(b1, 3);
    ib_halt(b1);
    IrProgram *prog = ib_finish(b1);
    return asnix_eval(prog, root_env());
}

static int64_t list_count_of(Value v) {
    CHECK(asnix_tag_of(v) == ASNIX_TAG_LIST, "result is a list");
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    return *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
}
static int64_t list_elem_int(Value v, int64_t i) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
    /* `map`'s result elements are genuinely lazy Thunks (M23 fix -- see
     * builtins_list.c's prim_map_stage2 comment), so this must force
     * before reading the raw int, unlike before that fix when `map`
     * itself had already computed every element eagerly. */
    return asnix_int_value(asnix_force_for_print(elems[i]));
}

/* map (x: x+1) [1 2 3] == [2 3 4] -- verified against
 * `nix-instantiate --eval -E 'map (x: x + 1) [1 2 3]'` => [ 2 3 4 ]. */
static void fixture_map(void) {
    Env *frame = asnix_env_alloc(root_env(), 2);
    frame->values[0] = lookup_builtin("map");
    frame->values[1] = make_primop((void *)test_add_one, 1);

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0); /* map */
    ib_var_ref(b, 0, 1); /* add_one */
    ib_call(b, 1);       /* map add_one -> stage2 */
    ib_push_int(b, 1);
    ib_push_int(b, 2);
    ib_push_int(b, 3);
    ib_mk_list(b, 3);
    ib_call(b, 1); /* stage2 [1,2,3] */
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, frame);

    CHECK(list_count_of(v) == 3, "map result has 3 elements");
    CHECK(list_elem_int(v, 0) == 2, "map elem 0 == 2");
    CHECK(list_elem_int(v, 1) == 3, "map elem 1 == 3");
    CHECK(list_elem_int(v, 2) == 4, "map elem 2 == 4");
}

/* filter (x: x mod 2 == 0 ...emulated in C) [1 2 3 4] == [2 4] --
 * verified against `nix-instantiate --eval -E
 * 'builtins.filter (x: x / 2 * 2 == x) [1 2 3 4]'` => [ 2 4 ] (no `mod`
 * builtin exists in this project yet, so the test's native predicate
 * directly mirrors "is even"). */
static void fixture_filter(void) {
    Env *frame = asnix_env_alloc(root_env(), 2);
    frame->values[0] = lookup_builtin("filter");
    frame->values[1] = make_primop((void *)test_is_even, 1);

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_var_ref(b, 0, 1);
    ib_call(b, 1);
    ib_push_int(b, 1);
    ib_push_int(b, 2);
    ib_push_int(b, 3);
    ib_push_int(b, 4);
    ib_mk_list(b, 4);
    ib_call(b, 1);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, frame);

    CHECK(list_count_of(v) == 2, "filter result has 2 elements");
    CHECK(list_elem_int(v, 0) == 2, "filter elem 0 == 2");
    CHECK(list_elem_int(v, 1) == 4, "filter elem 1 == 4");
}

/* builtins.foldl' (a: b: a + b) 0 [1 2 3] == 6 -- verified against
 * `nix-instantiate --eval -E "builtins.foldl' (a: b: a + b) 0 [1 2 3]"`
 * => 6. */
static void fixture_foldl_prime(void) {
    Env *frame = asnix_env_alloc(root_env(), 2);
    frame->values[0] = lookup_builtin("foldl'");
    frame->values[1] = make_add_closure();

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_var_ref(b, 0, 1);
    ib_call(b, 1);
    ib_push_int(b, 0);
    ib_call(b, 1);
    ib_push_int(b, 1);
    ib_push_int(b, 2);
    ib_push_int(b, 3);
    ib_mk_list(b, 3);
    ib_call(b, 1);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, frame);

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "foldl' result is an INT");
    CHECK(asnix_int_value(v) == 6, "foldl' (a: b: a+b) 0 [1 2 3] == 6");
}

/* elemAt [10 20 30] 1 == 20 -- verified against `nix-instantiate --eval
 * -E 'builtins.elemAt [10 20 30] 1'` => 20. */
static void fixture_elemAt(void) {
    Env *frame = asnix_env_alloc(root_env(), 1);
    frame->values[0] = lookup_builtin("elemAt");

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_push_int(b, 10);
    ib_push_int(b, 20);
    ib_push_int(b, 30);
    ib_mk_list(b, 3);
    ib_call(b, 1);
    ib_push_int(b, 1);
    ib_call(b, 1);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, frame);

    CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "elemAt result is an INT");
    CHECK(asnix_int_value(v) == 20, "elemAt [10 20 30] 1 == 20");
}

/* concatLists [[1 2] [3] [] [4]] == [1 2 3 4] -- verified against
 * `nix-instantiate --eval -E 'builtins.concatLists [[1 2] [3] [] [4]]'`
 * => [ 1 2 3 4 ]. */
static void fixture_concatLists(void) {
    Env *frame = asnix_env_alloc(root_env(), 1);
    frame->values[0] = lookup_builtin("concatLists");

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_push_int(b, 1);
    ib_push_int(b, 2);
    ib_mk_list(b, 2);
    ib_push_int(b, 3);
    ib_mk_list(b, 1);
    ib_mk_list(b, 0);
    ib_push_int(b, 4);
    ib_mk_list(b, 1);
    ib_mk_list(b, 4); /* [[1,2],[3],[],[4]] */
    ib_call(b, 1);
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    Value v = asnix_eval(prog, frame);

    CHECK(list_count_of(v) == 4, "concatLists result has 4 elements");
    CHECK(list_elem_int(v, 0) == 1, "concatLists elem 0 == 1");
    CHECK(list_elem_int(v, 1) == 2, "concatLists elem 1 == 2");
    CHECK(list_elem_int(v, 2) == 3, "concatLists elem 2 == 3");
    CHECK(list_elem_int(v, 3) == 4, "concatLists elem 3 == 4");
}

/* head [5 6 7] == 5; tail [5 6 7] == [6 7] -- verified against
 * `nix-instantiate --eval -E 'builtins.head [5 6 7]'` => 5 and
 * `nix-instantiate --eval -E 'builtins.tail [5 6 7]'` => [ 6 7 ]. */
static void fixture_head_tail(void) {
    Value list = make_int_list3(5, 6, 7);

    Env *hframe = asnix_env_alloc(root_env(), 2);
    hframe->values[0] = lookup_builtin("head");
    hframe->values[1] = list;
    IrBuilder *hb = ib_new();
    ib_var_ref(hb, 0, 0);
    ib_var_ref(hb, 0, 1);
    ib_call(hb, 1);
    ib_halt(hb);
    Value hv = asnix_eval(ib_finish(hb), hframe);
    CHECK(asnix_tag_of(hv) == ASNIX_TAG_INT, "head result is an INT");
    CHECK(asnix_int_value(hv) == 5, "head [5 6 7] == 5");

    Env *tframe = asnix_env_alloc(root_env(), 2);
    tframe->values[0] = lookup_builtin("tail");
    tframe->values[1] = list;
    IrBuilder *tb = ib_new();
    ib_var_ref(tb, 0, 0);
    ib_var_ref(tb, 0, 1);
    ib_call(tb, 1);
    ib_halt(tb);
    Value tv = asnix_eval(ib_finish(tb), tframe);
    CHECK(list_count_of(tv) == 2, "tail [5 6 7] has 2 elements");
    CHECK(list_elem_int(tv, 0) == 6, "tail [5 6 7] elem 0 == 6");
    CHECK(list_elem_int(tv, 1) == 7, "tail [5 6 7] elem 1 == 7");
}

int main(void) {
    asnix_arena_init(0);
    fixture_map();
    fixture_filter();
    fixture_foldl_prime();
    fixture_elemAt();
    fixture_concatLists();
    fixture_head_tail();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_builtins_list: all checks passed\n");
    return 0;
}
