#ifndef ASNIX_BUILTINS_SHARED_H
#define ASNIX_BUILTINS_SHARED_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"
#include "builtins_list.h" /* shared AsnixPrimFn/AsnixBuiltinDef typedefs */

/*
 * Shared helpers for this pass's new builtins modules (builtins_closure.c,
 * builtins_list2.c, builtins_attrs2.c, builtins_compare.c -- see
 * docs/architecture.md's "builtins expansion, round 2" section).
 *
 * M17's three sibling modules (builtins_list.c/builtins_attrs.c/
 * builtins_misc.c) each deliberately *duplicated* small helpers
 * (make_primop_value, list/attrs raw accessors, ...) instead of sharing
 * them, specifically because three different agents were writing those
 * three files in parallel and sharing a helper file would have meant
 * textually colliding on it (see builtins_misc.c's header comment,
 * "deliberately re-declared here rather than shared"). That reason does
 * not apply this time: one agent is writing all four new modules in this
 * pass, so there is no parallel-collision risk to avoid, and four-way
 * duplication of a ~30-line curry helper (make_curried_native,
 * builtins_misc.c) plus a dozen tiny List/Attrs accessors would just be
 * real, unmotivated duplication with no upside. One precedent for sharing
 * already exists in the established code (builtins_attrs.h includes
 * builtins_list.h for the AsnixPrimFn/AsnixBuiltinDef shape rather than
 * redefining it) -- this header is that same judgment call applied one
 * step further, not a new pattern.
 */

Value asnix_bshared_make_primop(void *fn, int64_t arity);

/* Builds a curried Closure chain of length `arity` wrapping the real,
 * ordinary arity-`arity` native PrimOp `native_fn` -- see
 * builtins_misc.c's make_curried_native for the full rationale (ordinary
 * Nix-level application always desugars to nested arity-1 calls --
 * parser.c's parse_app -- so a true arity-2+ builtin can't be registered
 * as a single multi-arg PrimOp; this builds the same "x: y: ... body"-
 * shaped Closure chain a real curried user lambda would compile to). This
 * is a fresh port of that file's logic (which is `static`), used by every
 * arity>=2 builtin added in this pass. */
Value asnix_bshared_make_curried_native(void *native_fn, int64_t arity);

/* `fn a b` via two successive single-argument asnix_apply calls -- same
 * shape as builtins_list.c's local (file-static) apply2. */
Value asnix_bshared_apply2(Value fn, Value a, Value b);

void asnix_bshared_fatal(const char *who, const char *msg);
void asnix_bshared_require_tag(Value v, int tag, const char *who, const char *what);
#define ASNIX_REQUIRE_ATTRS(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_ATTRS, (who), "an attrset")
#define ASNIX_REQUIRE_STRING(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_PTR, (who), "a string")
#define ASNIX_REQUIRE_LIST(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_LIST, (who), "a list")
#define ASNIX_REQUIRE_INT(v, who) asnix_bshared_require_tag((v), ASNIX_TAG_INT, (who), "an integer")

int64_t asnix_bshared_list_count(Value v);
const Value *asnix_bshared_list_elems(Value v);
Value asnix_bshared_make_list(const Value *elems, int64_t count);

int64_t asnix_bshared_attrs_count(Value v);
uint32_t asnix_bshared_attrs_sym(Value v, int64_t i);
Value asnix_bshared_attrs_value(Value v, int64_t i);
int64_t asnix_bshared_attrs_find(Value attrs, uint32_t sym);

/* `syms`/`vals` need NOT be pre-sorted (sorted in place, ascending by sym
 * id, before building) -- unlike builtins_attrs.c's make_attrs_from_sorted
 * (which only ever inherits sortedness from an existing Attrs object),
 * several of this pass's builtins (partition's {right;wrong;}, listToAttrs)
 * build a brand new small attrset from freshly interned symbols whose
 * relative sym-id order depends on global intern history, not any order
 * this code controls -- sorting here, once, is simpler than making every
 * caller get that right itself. Plain insertion sort: these are always
 * small (see sorted_name_order's identical rationale in builtins_attrs.c). */
Value asnix_bshared_make_attrs(uint32_t *syms, Value *vals, int64_t count);

/* Fatal if `v` isn't ASNIX_TAG_PTR (string OR path -- same dual-kind
 * dispatch as value_util.c's asnix_ptr_bytes; callers that specifically
 * need a *string*, not a path, should ASNIX_REQUIRE_STRING first, which
 * this does NOT itself distinguish). */
void asnix_bshared_string_bytes(Value v, const char **bytes_out, int64_t *len_out, const char *who);

/* Real structural equality, matching real Nix's `==`/`!=`: ints (numeric),
 * IMM (raw subtag -- true/false/null), strings/paths (byte content), lists
 * (same length, elementwise recursive equality), attrsets (same set of
 * attr names, recursive equality of each matching value) -- forcing any
 * thunk found along the way (list/attrset elements are lazy and not
 * pre-forced the way a bare variable reference is; see op_var_ref.S/
 * op_select.S) via the already-existing `asnix_force_for_print` C-callable
 * force entry point (`asnix/ir.h`), safe to call re-entrantly here for the
 * same reason it's already safe to call from a builtin or from inside
 * another nested force. Closures/primops (and thunks that are still
 * in-progress, though that can't happen reached from here) fall back to
 * pointer identity -- real Nix never structurally compares functions
 * either (confirmed: two syntactically-identical-but-distinct lambdas are
 * `!=`, see tests/conformance/equal-function-*.nix). This is the single
 * implementation both the `==`/`!=` operators (op_cmp.S, which calls this
 * directly) and dedup-style builtins (genericClosure, elem) use -- no
 * separate, narrower "scope cut" version exists anymore (see Known Bugs
 * entry below: the previous pointer-identity fallback for lists/attrsets
 * was a real, high-impact bug, not an acceptable scope cut, since `==` on
 * aggregates is pervasive in real nixpkgs code). */
int asnix_bshared_value_eq(Value a, Value b);

/* Ordering used by lessThan/sort: INT<INT (numeric) or string<string
 * (lexicographic byte compare, shorter-is-less on a shared prefix) only --
 * deliberately narrower than real Nix's `lessThan` (which also orders
 * paths, and lists lexicographically -- see ~/nix/src/libexpr/primops.cc's
 * CompareValues), matching the existing `<`/`>` operator's own INT-only
 * scope cut (op_cmp.S) extended just far enough to make `sort`/`lessThan`
 * useful on the string lists this subset's actual callers have (nixpkgs-
 * style name/version sorting). Fatal on any other type, or a type
 * mismatch, matching real Nix's error class (not necessarily its exact
 * wording). */
int asnix_bshared_value_lt(Value a, Value b, const char *who);

/* Runtime implementation of `expr.${nameExpr}` (dynamic attribute select,
 * OP_SELECT_DYNAMIC -- see ir.h and op_select_dynamic.S). Fatal on a
 * non-attrset target, a non-string name, or a missing key (same error
 * class as plain OP_SELECT -- op_select.S), unlike OP_SELECT_OR's
 * graceful fallthrough (`.${expr} or default` isn't supported, see
 * parser.c). Forces the result if it's an unforced thunk, matching plain
 * OP_SELECT's laziness. */
Value asnix_bshared_select_dynamic(Value target, Value name_val);

/* `expr.${nameExpr} or default` (M28) -- see the full comment in
 * builtins_shared.c. Returns 1 and writes the forced value through
 * `*out` on a hit, 0 (not a non-attrset/missing-key fatal, unlike the
 * plain form above) on a miss -- a non-string name is still fatal. */
int asnix_bshared_select_dynamic_or(Value target, Value name_val, Value *out);

/* M23: builds a genuinely LAZY Thunk that, when first forced, computes
 * `f arg` (asnix_bshared_make_lazy_call1) or the curried `(f arg1) arg2`
 * (asnix_bshared_make_lazy_call2) -- NOT an eager asnix_apply call made
 * right now. Fixes a real, previously-undiscovered strictness bug:
 * `map`/`mapAttrs` were calling their function argument immediately,
 * once per element, while building their result, instead of deferring
 * each call into its own thunk the way real Nix does (confirmed:
 * `builtins.length (map (x: throw "boom") [1 2 3])` => 3 on real Nix,
 * aborted here; numtide/nix-conformance's eval-okay-intersectAttrs.nix
 * relies on exactly this laziness via `builtins.mapAttrs throw alphabet`
 * as a "canary" that must NOT fire just from being built). Implemented
 * by reusing this project's own existing `make_curried2`/
 * `build_curry2_body` trick (builtins_attrs.c) one level further: a
 * small, singleton (built once, cached) IR body that reads its captured
 * values back out of a fresh per-call Env via plain OP_VAR_REF/OP_CALL,
 * wrapped in an OP_MK_THUNK-shaped heap Thunk object built directly here
 * in C (same four words op_mk_thunk.S itself writes) rather than
 * compiled bytecode -- no new IR op, no asm changes. */
Value asnix_bshared_make_lazy_call1(Value f, Value arg);
Value asnix_bshared_make_lazy_call2(Value f, Value arg1, Value arg2);

#endif /* ASNIX_BUILTINS_SHARED_H */
