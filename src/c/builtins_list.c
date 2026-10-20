/*
 * builtins-list stage: map, filter, foldl', elemAt, concatLists, tail,
 * head. See builtins_list.h for why these live in their own file/table
 * instead of editing builtins.c's BUILTINS[] directly.
 *
 * Currying (map/filter/foldl'/elemAt are arity >1 in real Nix): exactly
 * the pattern docs/architecture.md's M8 notes spell out for
 * `builtins.hashString` -- parser.c's parse_app always desugars `f x y`
 * into nested arity-1 AST_APP nodes, so OP_CALL only ever sees argc=1
 * calls for builtin application. hashString's trick (return one of N
 * *statically known* arity-1 PrimOps) doesn't generalize here though:
 * map/filter/foldl' need to carry an arbitrary *runtime* Value (the
 * mapped function, the predicate, the accumulator) from one curried call
 * into the next, not just pick between a handful of hardcoded function
 * pointers. The PrimOp heap object is normally just {fn; arity} (16
 * bytes, heap.h) with no spare field and no self/closure pointer handed
 * to `fn` -- op_call.S's primop path calls it with nothing but (args,
 * argc).
 *
 * The fix: allocate *larger* PrimOp objects for these curried stages --
 * {fn; arity; capture0; [capture1]} -- nothing anywhere checks that a
 * PrimOp heap object is exactly ASNIX_PRIMOP_SIZE bytes, only that
 * offsets 0/8 hold fn/arity (see make_primop1/make_primop2 below). The
 * captured Value(s) are recovered inside the stage's own `fn` via
 * `self_captures()`, which exploits a real invariant of op_call.S's
 * primop call path: the callee's own tagged Value sits in the VALSTACK
 * slot immediately before `args[0]` (op_call.S computes `args = callee_
 * slot + 8` and never overwrites that slot before calling), so
 * `args[-1]` is always the exact PrimOp Value that was called -- letting
 * `fn` recover "self" and read its own trailing capture fields with no
 * assembly changes. This invariant holds for every *ordinary* Nix-level
 * application (`f x` always desugars to an OP_CALL, regardless of how
 * `f` was obtained), which is the only way these stage-2/3 PrimOps are
 * ever invoked -- this module itself only ever uses asnix_apply (see
 * apply_shim.h) to call the *user-supplied* function/predicate/operator,
 * never its own curried stages, so the asnix_apply primop path (which
 * does NOT preserve this invariant -- it tail-calls `fn` with the
 * caller's own args pointer, nothing before it) never needs to recover a
 * self pointer this way.
 *
 * genericClosure: NOT implemented here -- cut as out of scope for this
 * pass. Real Nix's builtins.genericClosure needs attrset field access
 * (`key`/`operator`), value-equality-based dedup across arbitrary key
 * Values (not just ints/strings), and a breadth-first worklist that
 * preserves a specific visitation/output order -- enough surface area
 * (and enough ways to subtly mismatch real Nix's exact semantics without
 * much more verification time than the other 7 builtins combined needed)
 * that it's a reasonable rabbit hole to defer rather than rush. The other
 * 7 are complete and verified below.
 */
#include "builtins_list.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

/* Plain, non-capturing PrimOp (ASNIX_PRIMOP_SIZE bytes, heap.h). */
static Value make_primop0(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

/* PrimOp with one trailing captured Value (see header comment above). */
static Value make_primop1(void *fn, int64_t arity, Value cap0) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE + sizeof(Value));
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    *(Value *)((char *)mem + ASNIX_PRIMOP_SIZE) = cap0;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

/* PrimOp with two trailing captured Values (foldl''s stage3: op + acc). */
static Value make_primop2(void *fn, int64_t arity, Value cap0, Value cap1) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE + 2 * sizeof(Value));
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    *(Value *)((char *)mem + ASNIX_PRIMOP_SIZE) = cap0;
    *(Value *)((char *)mem + ASNIX_PRIMOP_SIZE + sizeof(Value)) = cap1;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

/* Recovers this call's own PrimOp heap pointer from the args pointer (see
 * header comment above for the op_call.S invariant this relies on), and
 * returns a pointer to its first trailing captured Value. */
static Value *self_captures(const Value *args) {
    Value self = args[-1];
    void *obj = (void *)(uintptr_t)(self.raw & ~(uint64_t)ASNIX_TAG_MASK);
    return (Value *)((char *)obj + ASNIX_PRIMOP_SIZE);
}

static void fatal(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void check_list(Value v, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_LIST) {
        fprintf(stderr, "asnix: %s: expected a list\n", who);
        exit(1);
    }
}

static int64_t list_count(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    return *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
}

static const Value *list_elems(Value v) {
    const char *base = (const char *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    return (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
}

static Value make_list(int64_t count, const Value *src) {
    void *mem = asnix_arena_alloc(8 + (uint64_t)count * sizeof(Value));
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = count;
    if (count > 0) {
        memcpy((char *)mem + ASNIX_LIST_OFF_ELEMS, src, (size_t)count * sizeof(Value));
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

/* Applies a curried-by-construction 2-argument Nix function (e.g. `a: b:
 * ...`, which compiles to two nested arity-1 closures, exactly like
 * every other multi-argument Nix lambda) to two arguments via two
 * successive single-argument asnix_apply calls -- `fn a b` is `(fn a) b`,
 * same as any other curried application in this codebase. */
static Value apply2(Value fn, Value a, Value b) {
    Value step1 = asnix_apply(fn, &a, 1);
    return asnix_apply(step1, &b, 1);
}

/* --------------------------------------------------------------------- */
/* map f list                                                            */
/* --------------------------------------------------------------------- */

static Value prim_map_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value f = self_captures(args)[0];
    Value list = args[0];
    check_list(list, "map");
    int64_t count = list_count(list);
    const Value *elems = list_elems(list);

    Value *out = count > 0 ? malloc((size_t)count * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < count; i++) {
        Value elem = elems[i];
        out[i] = asnix_apply(f, &elem, 1);
    }
    Value result = make_list(count, out);
    free(out);
    return result;
}

static Value prim_map(const Value *args, int64_t argc) {
    (void)argc;
    return make_primop1((void *)prim_map_stage2, 1, args[0]);
}

Value asnix_make_map_primop(void) {
    return make_primop0((void *)prim_map, 1);
}

/* --------------------------------------------------------------------- */
/* filter pred list                                                      */
/* --------------------------------------------------------------------- */

static Value prim_filter_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value pred = self_captures(args)[0];
    Value list = args[0];
    check_list(list, "filter");
    int64_t count = list_count(list);
    const Value *elems = list_elems(list);

    Value *kept = count > 0 ? malloc((size_t)count * sizeof(Value)) : NULL;
    int64_t n_kept = 0;
    for (int64_t i = 0; i < count; i++) {
        Value elem = elems[i];
        Value keep = asnix_apply(pred, &elem, 1);
        if (asnix_tag_of(keep) != ASNIX_TAG_IMM ||
            (asnix_imm_subtag(keep) != ASNIX_IMM_TRUE && asnix_imm_subtag(keep) != ASNIX_IMM_FALSE)) {
            free(kept);
            fatal("asnix: filter: predicate did not return a boolean");
        }
        if (asnix_imm_subtag(keep) == ASNIX_IMM_TRUE) {
            kept[n_kept++] = elem;
        }
    }
    Value result = make_list(n_kept, kept);
    free(kept);
    return result;
}

static Value prim_filter(const Value *args, int64_t argc) {
    (void)argc;
    return make_primop1((void *)prim_filter_stage2, 1, args[0]);
}

/* --------------------------------------------------------------------- */
/* foldl' op acc list                                                    */
/* --------------------------------------------------------------------- */

static Value prim_foldl_prime_stage3(const Value *args, int64_t argc) {
    (void)argc;
    Value *caps = self_captures(args);
    Value op = caps[0];
    Value acc = caps[1];
    Value list = args[0];
    check_list(list, "foldl'");
    int64_t count = list_count(list);
    const Value *elems = list_elems(list);

    for (int64_t i = 0; i < count; i++) {
        acc = apply2(op, acc, elems[i]);
    }
    return acc;
}

static Value prim_foldl_prime_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value op = self_captures(args)[0];
    Value acc = args[0];
    return make_primop2((void *)prim_foldl_prime_stage3, 1, op, acc);
}

static Value prim_foldl_prime(const Value *args, int64_t argc) {
    (void)argc;
    return make_primop1((void *)prim_foldl_prime_stage2, 1, args[0]);
}

/* --------------------------------------------------------------------- */
/* elemAt list n                                                         */
/* --------------------------------------------------------------------- */

static Value prim_elemAt_stage2(const Value *args, int64_t argc) {
    (void)argc;
    Value list = self_captures(args)[0];
    Value idx_v = args[0];
    if (asnix_tag_of(idx_v) != ASNIX_TAG_INT) {
        fatal("asnix: elemAt: index is not an integer");
    }
    int64_t idx = asnix_int_value(idx_v);
    int64_t count = list_count(list);
    if (idx < 0 || idx >= count) {
        fprintf(stderr, "asnix: elemAt: index %lld out of bounds (list has %lld elements)\n",
                (long long)idx, (long long)count);
        exit(1);
    }
    return list_elems(list)[idx];
}

static Value prim_elemAt(const Value *args, int64_t argc) {
    (void)argc;
    check_list(args[0], "elemAt");
    return make_primop1((void *)prim_elemAt_stage2, 1, args[0]);
}

/* --------------------------------------------------------------------- */
/* concatLists list-of-lists                                             */
/* --------------------------------------------------------------------- */

static Value prim_concatLists(const Value *args, int64_t argc) {
    (void)argc;
    Value outer = args[0];
    check_list(outer, "concatLists");
    int64_t ocount = list_count(outer);
    const Value *oelems = list_elems(outer);

    int64_t total = 0;
    for (int64_t i = 0; i < ocount; i++) {
        if (asnix_tag_of(oelems[i]) != ASNIX_TAG_LIST) {
            fprintf(stderr, "asnix: concatLists: element %lld is not a list\n", (long long)i);
            exit(1);
        }
        total += list_count(oelems[i]);
    }

    void *mem = asnix_arena_alloc(8 + (uint64_t)total * sizeof(Value));
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = total;
    Value *out = (Value *)((char *)mem + ASNIX_LIST_OFF_ELEMS);
    int64_t pos = 0;
    for (int64_t i = 0; i < ocount; i++) {
        int64_t icount = list_count(oelems[i]);
        if (icount > 0) {
            memcpy(out + pos, list_elems(oelems[i]), (size_t)icount * sizeof(Value));
        }
        pos += icount;
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

/* --------------------------------------------------------------------- */
/* tail list / head list                                                 */
/* --------------------------------------------------------------------- */

static Value prim_tail(const Value *args, int64_t argc) {
    (void)argc;
    Value list = args[0];
    check_list(list, "tail");
    int64_t count = list_count(list);
    if (count == 0) {
        fatal("asnix: tail: list is empty");
    }
    return make_list(count - 1, list_elems(list) + 1);
}

static Value prim_head(const Value *args, int64_t argc) {
    (void)argc;
    Value list = args[0];
    check_list(list, "head");
    if (list_count(list) == 0) {
        fatal("asnix: head: list is empty");
    }
    return list_elems(list)[0];
}

/* --------------------------------------------------------------------- */

const AsnixBuiltinDef asnix_builtins_list[] = {
    { "map", prim_map, 1 },
    { "filter", prim_filter, 1 },
    { "foldl'", prim_foldl_prime, 1 },
    { "elemAt", prim_elemAt, 1 },
    { "concatLists", prim_concatLists, 1 },
    { "tail", prim_tail, 1 },
    { "head", prim_head, 1 },
};

const size_t asnix_builtins_list_count = sizeof(asnix_builtins_list) / sizeof(asnix_builtins_list[0]);
