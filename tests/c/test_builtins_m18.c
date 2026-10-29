#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "builtins_attrs2.h"
#include "builtins_closure.h"
#include "builtins_compare.h"
#include "builtins_list2.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

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

static Value lookup(const AsnixBuiltinMiscDef *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(table[i].name, name) == 0) return table[i].build();
    }
    fprintf(stderr, "test_builtins_m18: no such builtin %s\n", name);
    exit(1);
}

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

static Value str(const char *s) { return asnix_make_string_value(s, strlen(s)); }

static Value make_int_list(const int64_t *ints, int64_t n) {
    IrBuilder *b = ib_new();
    for (int64_t i = 0; i < n; i++) ib_push_int(b, ints[i]);
    ib_mk_list(b, n);
    ib_halt(b);
    return asnix_eval(ib_finish(b), root_env());
}

static int64_t list_count_of(Value v) {
    CHECK(asnix_tag_of(v) == ASNIX_TAG_LIST, "result is a list");
    return asnix_bshared_list_count(v);
}
static int64_t list_elem_int(Value v, int64_t i) { return asnix_int_value(asnix_bshared_list_elems(v)[i]); }

static void check_bool(Value v, int want, const char *msg) {
    int ok = asnix_tag_of(v) == ASNIX_TAG_IMM && asnix_imm_subtag(v) == (want ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
    CHECK(ok, msg);
}

static void check_int(Value v, int64_t want, const char *msg) {
    int ok = asnix_tag_of(v) == ASNIX_TAG_INT && asnix_int_value(v) == want;
    CHECK(ok, msg);
}

static void check_string(Value v, const char *want, const char *msg) {
    CHECK(asnix_tag_of(v) == ASNIX_TAG_PTR, msg);
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return;
    int64_t len;
    const char *bytes = asnix_ptr_bytes(v, &len);
    int ok = (size_t)len == strlen(want) && memcmp(bytes, want, (size_t)len) == 0;
    CHECK(ok, msg);
    if (!ok) fprintf(stderr, "  got:  \"%.*s\"\n  want: \"%s\"\n", (int)len, bytes, want);
}

static Value make_sub_op(void) {
    IrBuilder *inner = ib_new();
    ib_var_ref(inner, 1, 0);
    ib_var_ref(inner, 0, 0);
    ib_sub(inner);
    ib_ret(inner);
    IrProgram *inner_prog = ib_finish(inner);

    IrBuilder *outer = ib_new();
    ib_lambda(outer, 1, inner_prog, 0);
    ib_ret(outer);
    IrProgram *outer_prog = ib_finish(outer);

    IrBuilder *top = ib_new();
    ib_lambda(top, 1, outer_prog, 0);
    ib_halt(top);
    return asnix_eval(ib_finish(top), root_env());
}

static void fixture_elem(void) {
    Value elem = lookup(ASNIX_LIST2_BUILTINS, ASNIX_LIST2_BUILTINS_COUNT, "elem");
    int64_t ints[3] = { 1, 2, 3 };
    Value list = make_int_list(ints, 3);
    Value a1[2] = { asnix_make_int(2), list };
    check_bool(call_curried(elem, a1, 2), 1, "elem 2 [1 2 3] == true");
    Value a2[2] = { asnix_make_int(5), list };
    check_bool(call_curried(elem, a2, 2), 0, "elem 5 [1 2 3] == false");
}

static void fixture_sort(void) {
    Value sort = lookup(ASNIX_LIST2_BUILTINS, ASNIX_LIST2_BUILTINS_COUNT, "sort");
    Value lessThan = lookup(ASNIX_COMPARE_BUILTINS, ASNIX_COMPARE_BUILTINS_COUNT, "lessThan");
    int64_t ints[3] = { 3, 1, 2 };
    Value list = make_int_list(ints, 3);
    Value args[2] = { lessThan, list };
    Value v = call_curried(sort, args, 2);
    CHECK(list_count_of(v) == 3, "sort result has 3 elements");
    CHECK(list_elem_int(v, 0) == 1, "sort elem 0 == 1");
    CHECK(list_elem_int(v, 1) == 2, "sort elem 1 == 2");
    CHECK(list_elem_int(v, 2) == 3, "sort elem 2 == 3");
}

static void fixture_foldl_foldr(void) {
    Value foldl = lookup(ASNIX_LIST2_BUILTINS, ASNIX_LIST2_BUILTINS_COUNT, "foldl");
    Value foldr = lookup(ASNIX_LIST2_BUILTINS, ASNIX_LIST2_BUILTINS_COUNT, "foldr");
    int64_t ints[3] = { 1, 2, 3 };
    Value list = make_int_list(ints, 3);

    Value largs[3] = { make_sub_op(), asnix_make_int(0), list };
    check_int(call_curried(foldl, largs, 3), -6, "foldl (a: b: a - b) 0 [1 2 3] == -6");

    Value rargs[3] = { make_sub_op(), asnix_make_int(0), list };
    check_int(call_curried(foldr, rargs, 3), 2, "foldr (a: b: a - b) 0 [1 2 3] == 2");
}

static Value gt2_pred(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_int_value(args[0]) > 2 ? ASNIX_TRUE : ASNIX_FALSE;
}

static void fixture_partition(void) {
    Value partition = lookup(ASNIX_LIST2_BUILTINS, ASNIX_LIST2_BUILTINS_COUNT, "partition");
    int64_t ints[4] = { 1, 2, 3, 4 };
    Value list = make_int_list(ints, 4);
    Value pred = asnix_bshared_make_primop((void *)gt2_pred, 1);
    Value result = call_curried(partition, (Value[]){ pred, list }, 2);

    uint32_t sym_right = asnix_intern("right", 5);
    uint32_t sym_wrong = asnix_intern("wrong", 5);
    int64_t ri = asnix_bshared_attrs_find(result, sym_right);
    int64_t wi = asnix_bshared_attrs_find(result, sym_wrong);
    CHECK(ri >= 0 && wi >= 0, "partition result has 'right'/'wrong'");
    Value right = asnix_bshared_attrs_value(result, ri);
    Value wrong = asnix_bshared_attrs_value(result, wi);
    CHECK(list_count_of(right) == 2 && list_elem_int(right, 0) == 3 && list_elem_int(right, 1) == 4,
          "partition right == [3 4]");
    CHECK(list_count_of(wrong) == 2 && list_elem_int(wrong, 0) == 1 && list_elem_int(wrong, 1) == 2,
          "partition wrong == [1 2]");
}

static Value make_key_item(uint32_t sym_key, int64_t key) {
    uint32_t syms[1] = { sym_key };
    Value vals[1] = { asnix_make_int(key) };
    return asnix_bshared_make_attrs(syms, vals, 1);
}

static Value test_operator(const Value *args, int64_t argc) {
    (void)argc;
    Value item = args[0];
    uint32_t sym_key = asnix_intern("key", 3);
    int64_t idx = asnix_bshared_attrs_find(item, sym_key);
    int64_t key = asnix_int_value(asnix_bshared_attrs_value(item, idx));
    if (key == 1) {
        Value out[2] = { make_key_item(sym_key, 2), make_key_item(sym_key, 3) };
        return asnix_bshared_make_list(out, 2);
    } else if (key == 2) {
        Value out[1] = { make_key_item(sym_key, 4) };
        return asnix_bshared_make_list(out, 1);
    } else if (key == 3) {
        Value out[1] = { make_key_item(sym_key, 5) };
        return asnix_bshared_make_list(out, 1);
    }
    return asnix_bshared_make_list(NULL, 0);
}

static void fixture_genericClosure(void) {
    Value genericClosure = lookup(ASNIX_CLOSURE_BUILTINS, ASNIX_CLOSURE_BUILTINS_COUNT, "genericClosure");
    uint32_t sym_key = asnix_intern("key", 3);

    Value start_item = make_key_item(sym_key, 1);
    Value start_set = asnix_bshared_make_list(&start_item, 1);
    Value operator_fn = asnix_bshared_make_primop((void *)test_operator, 1);

    uint32_t syms[2] = { asnix_intern("startSet", 8), asnix_intern("operator", 8) };
    Value vals[2] = { start_set, operator_fn };
    Value input = asnix_bshared_make_attrs(syms, vals, 2);

    Value result = call_curried(genericClosure, &input, 1);
    CHECK(list_count_of(result) == 5, "genericClosure BFS result has 5 elements");
    const Value *elems = asnix_bshared_list_elems(result);
    for (int64_t i = 0; i < 5 && i < list_count_of(result); i++) {
        int64_t idx = asnix_bshared_attrs_find(elems[i], sym_key);
        CHECK(idx >= 0, "genericClosure result element has a 'key'");
        int64_t key = asnix_int_value(asnix_bshared_attrs_value(elems[i], idx));
        char msg[64];
        snprintf(msg, sizeof msg, "genericClosure result[%lld].key == %lld", (long long)i, (long long)(i + 1));
        CHECK(key == i + 1, msg);
    }
}

static Value test_operator_dup(const Value *args, int64_t argc) {
    (void)argc, (void)args;
    uint32_t sym_key = asnix_intern("key", 3);
    Value out[2] = { make_key_item(sym_key, 1), make_key_item(sym_key, 3) };
    return asnix_bshared_make_list(out, 2);
}

static void fixture_genericClosure_dedup(void) {
    Value genericClosure = lookup(ASNIX_CLOSURE_BUILTINS, ASNIX_CLOSURE_BUILTINS_COUNT, "genericClosure");
    uint32_t sym_key = asnix_intern("key", 3);

    Value items[2] = { make_key_item(sym_key, 1), make_key_item(sym_key, 2) };
    Value start_set = asnix_bshared_make_list(items, 2);
    Value operator_fn = asnix_bshared_make_primop((void *)test_operator_dup, 1);

    uint32_t syms[2] = { asnix_intern("startSet", 8), asnix_intern("operator", 8) };
    Value vals[2] = { start_set, operator_fn };
    Value input = asnix_bshared_make_attrs(syms, vals, 2);

    Value result = call_curried(genericClosure, &input, 1);
    CHECK(list_count_of(result) == 3, "genericClosure dedup result has exactly 3 elements");
}

static void fixture_versions(void) {
    Value splitVersion = lookup(ASNIX_COMPARE_BUILTINS, ASNIX_COMPARE_BUILTINS_COUNT, "splitVersion");
    Value v = call_curried(splitVersion, (Value[]){ str("1pre") }, 1);
    CHECK(list_count_of(v) == 2, "splitVersion \"1pre\" has 2 components");
    check_string(asnix_bshared_list_elems(v)[0], "1", "splitVersion \"1pre\"[0] == \"1\"");
    check_string(asnix_bshared_list_elems(v)[1], "pre", "splitVersion \"1pre\"[1] == \"pre\"");

    Value compareVersions = lookup(ASNIX_COMPARE_BUILTINS, ASNIX_COMPARE_BUILTINS_COUNT, "compareVersions");
    check_int(call_curried(compareVersions, (Value[]){ str("1.9"), str("1.10") }, 2), -1,
              "compareVersions \"1.9\" \"1.10\" == -1");
    check_int(call_curried(compareVersions, (Value[]){ str("1pre"), str("1") }, 2), -1,
              "compareVersions \"1pre\" \"1\" == -1");
    check_int(call_curried(compareVersions, (Value[]){ str("1.0"), str("1") }, 2), 1,
              "compareVersions \"1.0\" \"1\" == 1");
}

static void fixture_replaceStrings(void) {
    Value replaceStrings = lookup(ASNIX_COMPARE_BUILTINS, ASNIX_COMPARE_BUILTINS_COUNT, "replaceStrings");
    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)str("").raw);
    ib_mk_list(b, 1);
    ib_halt(b);
    Value from_list = asnix_eval(ib_finish(b), root_env());

    IrBuilder *b2 = ib_new();
    ib_push_const(b2, (int64_t)str("X").raw);
    ib_mk_list(b2, 1);
    ib_halt(b2);
    Value to_list = asnix_eval(ib_finish(b2), root_env());

    Value v = call_curried(replaceStrings, (Value[]){ from_list, to_list, str("ab") }, 3);
    check_string(v, "XaXbX", "replaceStrings [\"\"] [\"X\"] \"ab\" == \"XaXbX\"");
}

static void fixture_listToAttrs(void) {
    Value listToAttrs = lookup(ASNIX_ATTRS2_BUILTINS, ASNIX_ATTRS2_BUILTINS_COUNT, "listToAttrs");
    uint32_t sym_name = asnix_intern("name", 4);
    uint32_t sym_value = asnix_intern("value", 5);

    uint32_t s1[2] = { sym_name, sym_value };
    Value v1[2] = { str("foo"), asnix_make_int(123) };
    Value e1 = asnix_bshared_make_attrs(s1, v1, 2);

    uint32_t s2[2] = { sym_name, sym_value };
    Value v2[2] = { str("bar"), asnix_make_int(456) };
    Value e2 = asnix_bshared_make_attrs(s2, v2, 2);

    uint32_t s3[2] = { sym_name, sym_value };
    Value v3[2] = { str("bar"), asnix_make_int(420) };
    Value e3 = asnix_bshared_make_attrs(s3, v3, 2);

    Value elems[3] = { e1, e2, e3 };
    Value list = asnix_bshared_make_list(elems, 3);

    Value result = call_curried(listToAttrs, &list, 1);
    CHECK(asnix_bshared_attrs_count(result) == 2, "listToAttrs result has 2 attrs (deduped)");
    uint32_t sym_bar = asnix_intern("bar", 3);
    int64_t idx = asnix_bshared_attrs_find(result, sym_bar);
    CHECK(idx >= 0, "listToAttrs result has 'bar'");
    check_int(asnix_bshared_attrs_value(result, idx), 456, "listToAttrs 'bar' == 456 (first occurrence wins)");
}

static void fixture_attrByPath(void) {
    Value attrByPath = lookup(ASNIX_ATTRS2_BUILTINS, ASNIX_ATTRS2_BUILTINS_COUNT, "attrByPath");
    uint32_t sym_a = asnix_intern("a", 1);
    uint32_t sym_b = asnix_intern("b", 1);

    uint32_t sb[1] = { sym_b };
    Value vb[1] = { asnix_make_int(42) };
    Value inner = asnix_bshared_make_attrs(sb, vb, 1);
    uint32_t sa[1] = { sym_a };
    Value va[1] = { inner };
    Value outer = asnix_bshared_make_attrs(sa, va, 1);

    IrBuilder *pb = ib_new();
    ib_push_const(pb, (int64_t)str("a").raw);
    ib_push_const(pb, (int64_t)str("b").raw);
    ib_mk_list(pb, 2);
    ib_halt(pb);
    Value path = asnix_eval(ib_finish(pb), root_env());

    check_int(call_curried(attrByPath, (Value[]){ path, asnix_make_int(99), outer }, 3), 42,
              "attrByPath [\"a\" \"b\"] 99 {a={b=42;};} == 42");

    uint32_t sa2[1] = { sym_a };
    Value va2[1] = { asnix_bshared_make_attrs(NULL, NULL, 0) };
    Value outer2 = asnix_bshared_make_attrs(sa2, va2, 1);
    check_int(call_curried(attrByPath, (Value[]){ path, asnix_make_int(99), outer2 }, 3), 99,
              "attrByPath [\"a\" \"b\"] 99 {a={};} == 99 (default)");
}

int main(void) {
    asnix_arena_init(0);
    fixture_elem();
    fixture_sort();
    fixture_foldl_foldr();
    fixture_partition();
    fixture_genericClosure();
    fixture_genericClosure_dedup();
    fixture_versions();
    fixture_replaceStrings();
    fixture_listToAttrs();
    fixture_attrByPath();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_builtins_m18: all checks passed\n");
    return 0;
}

