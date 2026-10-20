/* Exercises builtins_misc.c's ASNIX_BUILTINS_MISC table through the REAL
 * evaluator (asnix_eval over hand-built IR), not by calling the native C
 * functions directly -- this is the only way to actually validate
 * make_curried_native's Closure chain (substring/concatStringsSep), since
 * its whole point is to be driven by OP_LAMBDA/OP_CALL/OP_VAR_REF/OP_RET,
 * the same as any real curried user lambda. Every expected value below
 * was independently checked against a real `nix-instantiate --eval` (see
 * the case-by-case comments and builtins_misc.c's own per-builtin
 * comments for the exact commands run). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "builtins_misc.h"
#include "value_util.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static Value lookup(const char *name) {
    for (size_t i = 0; i < ASNIX_BUILTINS_MISC_COUNT; i++) {
        if (strcmp(ASNIX_BUILTINS_MISC[i].name, name) == 0) return ASNIX_BUILTINS_MISC[i].build();
    }
    fprintf(stderr, "test_builtins_misc: no such builtin %s\n", name);
    exit(1);
}

/* Applies `fn` to `args[0..n-1]` one at a time via REAL OP_CALL dispatch
 * (asnix_eval over hand-built IR) -- works uniformly whether `fn` is a
 * plain arity-1 PrimOp (stringLength, isNull, ...) or the outermost
 * Closure of a make_curried_native chain (substring, concatStringsSep):
 * either way, each curry stage is applied with exactly 1 argument, same
 * as a real `f a b c` call site. */
static Value call_curried(Value fn, const Value *args, int64_t n) {
    Env *root = asnix_env_alloc(NULL, 1);
    root->values[0] = fn;

    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    for (int64_t i = 0; i < n; i++) {
        ib_push_const(b, (int64_t)args[i].raw);
        ib_call(b, 1);
    }
    ib_halt(b);
    IrProgram *prog = ib_finish(b);
    return asnix_eval(prog, root);
}

static Value str(const char *s) {
    return asnix_make_string_value(s, strlen(s));
}

static void check_string(Value v, const char *want, const char *msg) {
    CHECK(asnix_tag_of(v) == ASNIX_TAG_PTR, msg);
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return;
    const char *s = (const char *)(v.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    const char *bytes = s + ASNIX_STRING_OFF_BYTES;
    int ok = (size_t)len == strlen(want) && memcmp(bytes, want, (size_t)len) == 0;
    CHECK(ok, msg);
    if (!ok) fprintf(stderr, "  got:  \"%.*s\"\n  want: \"%s\"\n", (int)len, bytes, want);
}

static void check_bool(Value v, int want, const char *msg) {
    int ok = asnix_tag_of(v) == ASNIX_TAG_IMM &&
             asnix_imm_subtag(v) == (want ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
    CHECK(ok, msg);
}

static Value make_list(const Value *elems, int64_t n) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)n * 8);
    *(int64_t *)mem = n;
    if (n > 0) memcpy((char *)mem + ASNIX_LIST_OFF_ELEMS, elems, (size_t)n * sizeof(Value));
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

static Value make_empty_attrs(void) {
    void *mem = asnix_arena_alloc(8);
    *(int64_t *)mem = 0;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

/* nix-instantiate --eval -E 'builtins.substring 1 3 "hello"'        => "ell"
 * nix-instantiate --eval -E 'builtins.substring 1 100 "hello"'      => "ello"
 * nix-instantiate --eval -E 'builtins.substring 10 3 "hello"'       => ""
 * nix-instantiate --eval -E 'builtins.substring 5 3 "hello"'        => ""
 * nix-instantiate --eval -E 'builtins.substring 1 (-1) "hello"'     => "ello" */
static void test_substring(void) {
    Value fn = lookup("substring");
    Value a1[] = { asnix_make_int(1), asnix_make_int(3), str("hello") };
    check_string(call_curried(fn, a1, 3), "ell", "substring 1 3 \"hello\" == \"ell\"");

    Value a2[] = { asnix_make_int(1), asnix_make_int(100), str("hello") };
    check_string(call_curried(fn, a2, 3), "ello", "substring 1 100 \"hello\" == \"ello\" (len clamped)");

    Value a3[] = { asnix_make_int(10), asnix_make_int(3), str("hello") };
    check_string(call_curried(fn, a3, 3), "", "substring 10 3 \"hello\" == \"\" (start past end)");

    Value a4[] = { asnix_make_int(5), asnix_make_int(3), str("hello") };
    check_string(call_curried(fn, a4, 3), "", "substring 5 3 \"hello\" == \"\" (start == length)");

    Value a5[] = { asnix_make_int(1), asnix_make_int(-1), str("hello") };
    check_string(call_curried(fn, a5, 3), "ello", "substring 1 (-1) \"hello\" == \"ello\" (negative len == rest)");

    /* Partial application must itself be a usable function value (real
     * Nix: `isFunction (builtins.substring 1)` == true), proving the
     * intermediate Closure stages are real, independently callable
     * values -- not just an artifact of calling all 3 args in one go. */
    Env *root = asnix_env_alloc(NULL, 1);
    root->values[0] = fn;
    IrBuilder *b = ib_new();
    ib_var_ref(b, 0, 0);
    ib_push_int(b, 1);
    ib_call(b, 1);
    ib_push_int(b, 3);
    ib_call(b, 1);
    ib_halt(b);
    Value partial = asnix_eval(ib_finish(b), root);
    CHECK(asnix_tag_of(partial) == ASNIX_TAG_CLOSURE, "substring 1 3 (partial) is callable (a Closure)");
    Value a6[] = { str("hello") };
    check_string(call_curried(partial, a6, 1), "ell", "the partially-applied closure, called with \"hello\", == \"ell\"");
}

/* nix-instantiate --eval -E 'builtins.stringLength "hello"' => 5
 * nix-instantiate --eval -E 'builtins.stringLength ""'      => 0 */
static void test_stringLength(void) {
    Value fn = lookup("stringLength");
    Value a1[] = { str("hello") };
    Value v1 = call_curried(fn, a1, 1);
    CHECK(asnix_tag_of(v1) == ASNIX_TAG_INT && asnix_int_value(v1) == 5, "stringLength \"hello\" == 5");

    Value a2[] = { str("") };
    Value v2 = call_curried(fn, a2, 1);
    CHECK(asnix_tag_of(v2) == ASNIX_TAG_INT && asnix_int_value(v2) == 0, "stringLength \"\" == 0");
}

/* nix-instantiate --eval -E 'builtins.concatStringsSep ", " ["a" "b" "c"]' => "a, b, c"
 * nix-instantiate --eval -E 'builtins.concatStringsSep ", " []'            => "" */
static void test_concatStringsSep(void) {
    Value fn = lookup("concatStringsSep");
    Value elems[] = { str("a"), str("b"), str("c") };
    Value a1[] = { str(", "), make_list(elems, 3) };
    check_string(call_curried(fn, a1, 2), "a, b, c", "concatStringsSep \", \" [\"a\" \"b\" \"c\"] == \"a, b, c\"");

    Value a2[] = { str(", "), make_list(NULL, 0) };
    check_string(call_curried(fn, a2, 2), "", "concatStringsSep \", \" [] == \"\"");
}

/* nix-instantiate --eval -E 'builtins.isNull null' => true; 'builtins.isNull 1' => false */
static void test_isNull(void) {
    Value fn = lookup("isNull");
    Value a1[] = { ASNIX_NULL };
    check_bool(call_curried(fn, a1, 1), 1, "isNull null == true");
    Value a2[] = { asnix_make_int(1) };
    check_bool(call_curried(fn, a2, 1), 0, "isNull 1 == false");
}

/* nix-instantiate --eval -E 'builtins.isString "a"' => true; 'builtins.isString 1' => false */
static void test_isString(void) {
    Value fn = lookup("isString");
    Value a1[] = { str("a") };
    check_bool(call_curried(fn, a1, 1), 1, "isString \"a\" == true");
    Value a2[] = { asnix_make_int(1) };
    check_bool(call_curried(fn, a2, 1), 0, "isString 1 == false");
}

/* nix-instantiate --eval -E 'builtins.isList [1 2 3]' => true */
static void test_isList(void) {
    Value fn = lookup("isList");
    Value elems[] = { asnix_make_int(1), asnix_make_int(2), asnix_make_int(3) };
    Value a1[] = { make_list(elems, 3) };
    check_bool(call_curried(fn, a1, 1), 1, "isList [1 2 3] == true");
    Value a2[] = { asnix_make_int(1) };
    check_bool(call_curried(fn, a2, 1), 0, "isList 1 == false");
}

/* nix-instantiate --eval -E 'builtins.isAttrs { a = 1; }' => true */
static void test_isAttrs(void) {
    Value fn = lookup("isAttrs");
    Value a1[] = { make_empty_attrs() };
    check_bool(call_curried(fn, a1, 1), 1, "isAttrs {} == true");
    Value a2[] = { asnix_make_int(1) };
    check_bool(call_curried(fn, a2, 1), 0, "isAttrs 1 == false");
}

/* nix-instantiate --eval -E 'builtins.isFunction (a: a)' => true
 * nix-instantiate --eval -E 'builtins.isFunction builtins.length' => true
 * nix-instantiate --eval -E 'builtins.isFunction (builtins.substring 1)' => true */
static void test_isFunction(void) {
    Value fn = lookup("isFunction");
    Value a1[] = { lookup("isNull") }; /* a PrimOp value */
    check_bool(call_curried(fn, a1, 1), 1, "isFunction <primop> == true");
    Value a2[] = { lookup("substring") }; /* a Closure value (make_curried_native) */
    check_bool(call_curried(fn, a2, 1), 1, "isFunction <closure> == true");
    Value a3[] = { asnix_make_int(1) };
    check_bool(call_curried(fn, a3, 1), 0, "isFunction 1 == false");
}

/* nix-instantiate --eval -E 'builtins.isInt 1' => true */
static void test_isInt(void) {
    Value fn = lookup("isInt");
    Value a1[] = { asnix_make_int(1) };
    check_bool(call_curried(fn, a1, 1), 1, "isInt 1 == true");
    Value a2[] = { str("a") };
    check_bool(call_curried(fn, a2, 1), 0, "isInt \"a\" == false");
}

/* nix-instantiate --eval -E 'builtins.isBool true' => true */
static void test_isBool(void) {
    Value fn = lookup("isBool");
    Value a1[] = { ASNIX_TRUE };
    check_bool(call_curried(fn, a1, 1), 1, "isBool true == true");
    Value a2[] = { asnix_make_int(1) };
    check_bool(call_curried(fn, a2, 1), 0, "isBool 1 == false");
}

int main(void) {
    asnix_arena_init(0);
    test_substring();
    test_stringLength();
    test_concatStringsSep();
    test_isNull();
    test_isString();
    test_isList();
    test_isAttrs();
    test_isFunction();
    test_isInt();
    test_isBool();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_builtins_misc: all checks passed\n");
    return 0;
}
