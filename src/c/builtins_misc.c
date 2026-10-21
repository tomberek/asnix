#include "builtins_misc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "base32.h"
#include "builtins_shared.h"
#include "hash_shim.h"
#include "value_util.h"

/* ---------------------------------------------------------------------
 * Small shared helpers (deliberately re-declared here rather than shared
 * with builtins.c's copies -- this file exists specifically so it has no
 * textual overlap with builtins.c for sibling agents working in
 * parallel; a later integration pass can dedupe if it wants to).
 * --------------------------------------------------------------------- */

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

static void fatal(const char *msg) {
    fprintf(stderr, "asnix: %s\n", msg);
    exit(1);
}

/* Reads a string Value's {len, bytes} without forcing/coercion -- same
 * strict style as builtins.c's prim_length (require the exact tag,
 * fatal otherwise), not real Nix's more permissive coerceToString. */
static void as_string(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: %s: expected a string argument\n", who);
        exit(1);
    }
    const char *s = (const char *)(v.raw & ~0x7ULL);
    *len_out = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    *bytes_out = s + ASNIX_STRING_OFF_BYTES;
}

/*
 * ---------------------------------------------------------------------
 * Generalizing hashString's curry trick to arbitrary-domain arguments
 * ---------------------------------------------------------------------
 * builtins.c's hashString comment explains why a *finite enum* argument
 * (the algorithm name -- only 4 possible values) can be curried across
 * two separate arity-1 PrimOp calls just by returning one of a few
 * statically-known dedicated PrimOp function pointers: parse_app always
 * desugars `f a b c` into nested arity-1 App nodes, so OP_CALL only ever
 * sees argc=1 calls into a primop, and a primop's native fn signature
 * `Value (*)(const Value *args, int64_t argc)` has NO self/closure
 * pointer -- nothing lets one call's native fn pass data of its own
 * choosing into the *next* call's native fn.
 *
 * `substring start len str` (arity 3) and `concatStringsSep sep list`
 * (arity 2) need to carry `start`/`len`/`sep` across curry stages too,
 * but those are arbitrary ints/strings, not a 4-way enum -- there's no
 * finite set of dedicated function pointers to pick from this time.
 *
 * The fix: don't try to carry data between two *native* calls at all --
 * use the exact mechanism real curried *user* lambdas already use for
 * this (`a: b: c: body`), which is already fully general: OP_LAMBDA
 * captures "whatever Env is live right now" as the new Closure's `up`,
 * and OP_VAR_REF walks that chain back by De Bruijn depth. `substring`'s
 * top-level builtins-attrs entry is therefore not a PrimOp at all, but a
 * hand-built Closure chain -- built once, here, via the same IrBuilder
 * API the real compiler (src/c/compiler.c) uses for ordinary `a: b: ...`
 * syntax -- equivalent to compiling this fictitious Nix source:
 *
 *   start: len: str: __substring3 start len str
 *
 * where `__substring3` is a real, ordinary arity-3 PrimOp (never exposed
 * to user code directly -- nothing stops a 3-argument PrimOp existing,
 * OP_CALL's arity check works for any N; it's only the *parser* that
 * never generates an argc>1 call site for user-written application).
 * `make_curried_native` below builds exactly that chain for any arity
 * N>=2. No mutable global, no stashing extra data after the Closure/
 * PrimOp struct and recovering a "self" pointer some other way, no asm
 * changes -- just the same Closure/Env machinery every other curried
 * function in this evaluator already relies on, so it's automatically
 * safe under re-entrant native-to-Nix calls (e.g. a future `map
 * (builtins.substring 1 3) list`) the way a bespoke "self via adjacent-
 * stack-slot" or "global data stack" trick would not be.
 */
static Value make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = make_primop_value(native_fn, arity);
    /* Never a lambda-pattern -- `builtins.functionArgs` on any of these
     * closures is `{}`, same as real Nix's `functionArgs builtins.add`
     * (M37). */
    Value empty_formals = asnix_bshared_make_attrs(NULL, NULL, 0);

    /* Innermost program: runs once the *last* curried argument has been
     * supplied, in the Env chain built up by `arity` nested single-slot
     * closure calls. Pushes the real native primop, then each of its
     * `arity` arguments in order by walking back up that chain (the
     * most-recently-bound param is depth 0, the first-bound one is
     * depth arity-1), then calls it for real. */
    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    /* Wrap `arity - 1` more times: each outer layer's entire body is
     * just "construct and return the next-inner layer's closure",
     * exactly what compile_expr's AST_LAMBDA case emits for each `x:`
     * in a real curried `a: b: c: ...` user lambda. */
    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    /* The outermost closure itself -- captures no Env (up = NULL, same
     * as any other root-scope value) since nothing has been bound yet. */
    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_BODY) = prog->ops;
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_POOL) = prog->pool;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

/* ---------------------------------------------------------------------
 * builtins.substring start len str (arity 3)
 * ---------------------------------------------------------------------
 * Verified against real `nix-instantiate --eval`:
 *   substring 1 3 "hello"    == "ell"    (ordinary slice)
 *   substring 1 100 "hello"  == "ello"   (len beyond end: clamp)
 *   substring 10 3 "hello"   == ""       (start beyond end: clamp to "")
 *   substring 5 3 "hello"    == ""       (start == length: clamp to "")
 *   substring 1 (-1) "hello" == "ello"   (negative len == "rest of string")
 *   substring (-1) 3 "hello" -> error "negative start position in 'substring'"
 */
static Value substring3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value start_v = args[0], len_v = args[1], str_v = args[2];
    if (asnix_tag_of(start_v) != ASNIX_TAG_INT || asnix_tag_of(len_v) != ASNIX_TAG_INT)
        fatal("substring: start and len must be integers");
    int64_t start = asnix_int_value(start_v);
    int64_t len = asnix_int_value(len_v);
    if (start < 0) fatal("negative start position in 'substring'");

    const char *bytes;
    int64_t slen;
    as_string(str_v, &bytes, &slen, "substring");

    int64_t start_n = start > slen ? slen : start;
    int64_t actual_len = len < 0 ? slen - start_n : (start_n + len > slen ? slen - start_n : len);
    return asnix_make_string_value(bytes + start_n, (size_t)actual_len);
}

static Value build_substring(void) {
    return make_curried_native((void *)substring3_impl, 3);
}

/* ---------------------------------------------------------------------
 * builtins.stringLength s (arity 1)
 * Verified: stringLength "hello" == 5, stringLength "" == 0.
 * --------------------------------------------------------------------- */
static Value prim_stringLength(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    as_string(args[0], &bytes, &len, "stringLength");
    return asnix_make_int(len);
}

static Value build_stringLength(void) {
    return make_primop_value((void *)prim_stringLength, 1);
}

/* ---------------------------------------------------------------------
 * builtins.concatStringsSep sep list (arity 2). Note: real Nix has
 * `concatStringsSep` as a genuine builtin but *not* `concatStrings`
 * (that one's `lib.concatStrings = concatStringsSep ""`, nixpkgs-level,
 * not builtins-level) -- confirmed via `builtins ? concatStrings` ==
 * false and `builtins ? concatStringsSep` == true on a real
 * nix-instantiate, so only concatStringsSep is implemented here.
 * Verified: concatStringsSep ", " ["a" "b" "c"] == "a, b, c";
 * concatStringsSep ", " [] == "".
 * --------------------------------------------------------------------- */
static Value concatStringsSep2_impl(const Value *args, int64_t argc) {
    (void)argc;
    const char *sep_bytes;
    int64_t sep_len;
    as_string(args[0], &sep_bytes, &sep_len, "concatStringsSep");

    Value list_v = args[1];
    if (asnix_tag_of(list_v) != ASNIX_TAG_LIST) fatal("concatStringsSep: second argument must be a list");
    const char *list_base = (const char *)(list_v.raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(list_base + ASNIX_LIST_OFF_COUNT);
    const Value *elems = (const Value *)(list_base + ASNIX_LIST_OFF_ELEMS);

    int64_t total = 0;
    for (int64_t i = 0; i < count; i++) {
        const char *b;
        int64_t l;
        /* Lazy list elements (M23's `map`, etc.) -- force before reading
         * bytes, same class of fix M23's own concatLists needed. */
        as_string(asnix_force_for_print(elems[i]), &b, &l, "concatStringsSep");
        total += l;
        if (i + 1 < count) total += sep_len;
    }

    char *out = asnix_arena_alloc((uint64_t)total);
    char *cur = out;
    for (int64_t i = 0; i < count; i++) {
        const char *b;
        int64_t l;
        as_string(asnix_force_for_print(elems[i]), &b, &l, "concatStringsSep");
        memcpy(cur, b, (size_t)l);
        cur += l;
        if (i + 1 < count) {
            memcpy(cur, sep_bytes, (size_t)sep_len);
            cur += sep_len;
        }
    }
    return asnix_make_string_value(out, (size_t)total);
}

static Value build_concatStringsSep(void) {
    return make_curried_native((void *)concatStringsSep2_impl, 2);
}

/* ---------------------------------------------------------------------
 * builtins.addErrorContext msg value (M39, `__addErrorContext` in real
 * Nix -- builtins-only, confirmed in ~/nix/src/libexpr/primops.cc). Real
 * Nix's only observable effect is annotating `value`'s error trace with
 * `msg` *if and only if* forcing `value` throws -- confirmed `msg` isn't
 * forced any more eagerly than `value` itself by ordinary call-by-need
 * (`let y = addErrorContext "ctx" (abort "x"); in 1` == 1, same as any
 * other unused lazy binding; `addErrorContext "ctx" (abort "x")`
 * forced directly DOES abort, exactly as if written bare -- no special
 * extra laziness beyond normal application). Since asnix has no error-
 * trace/position system at all (no position tracking anywhere in this
 * codebase), there is nothing to attach `msg` to -- implemented as a
 * pure pass-through, `msg: value: value`, which already reproduces
 * every observable success/failure case above via this project's
 * ordinary M25 call-by-need semantics (no NOFORCE needed: forcing only
 * happens when the *caller* forces the call's own result, same as real
 * Nix). */
static Value addErrorContext2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return args[1];
}
static Value build_addErrorContext(void) {
    return make_curried_native((void *)addErrorContext2_impl, 2);
}

/* ---------------------------------------------------------------------
 * Type predicates. All verified true/false the obvious way against real
 * nix-instantiate (isNull null/isNull 1, isString "a"/isString 1,
 * isList [1 2 3], isAttrs {a=1;}, isFunction (a:a)/isFunction
 * builtins.length, isInt 1/isInt 1.5, isBool true) -- 1.5 isn't
 * representable in this evaluator's INT-only numeric tower yet, so
 * isInt has no real "false on a float" case to check here, only "true
 * on an int". isFunction is true for both a user Closure and a native
 * PrimOp (including a partially-applied curried one, e.g.
 * `isFunction (builtins.substring 1)` == true on real Nix), matching
 * this file's own build_substring/build_concatStringsSep returning
 * ASNIX_TAG_CLOSURE values -- typeOf already treats CLOSURE and PRIMOP
 * identically ("lambda") in builtins.c, so this is consistent.
 * --------------------------------------------------------------------- */
static Value prim_isNull(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_IMM && asnix_imm_subtag(args[0]) == ASNIX_IMM_NULL
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isString(const Value *args, int64_t argc) {
    (void)argc;
    /* Paths and strings share ASNIX_TAG_PTR (see heap.h) -- isString must
     * exclude paths (found via d.nix: `builtins.isString ./.` wrongly
     * returned true here; real Nix returns false). */
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_PTR && !asnix_value_is_path(args[0])
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isPath(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_PTR && asnix_value_is_path(args[0])
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isList(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_LIST ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isAttrs(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_ATTRS ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isFunction(const Value *args, int64_t argc) {
    (void)argc;
    int tag = asnix_tag_of(args[0]);
    return asnix_make_imm(tag == ASNIX_TAG_CLOSURE || tag == ASNIX_TAG_PRIMOP ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}

/* `builtins.functionArgs` (M37, `__functionArgs` in real Nix -- builtins-
 * only, confirmed in ~/nix/src/libexpr/primops.cc): every Closure already
 * carries its pre-built result as ASNIX_CLOSURE_OFF_FORMALS (set once at
 * compile time, compiler.c's AST_LAMBDA case -- see ir.h's comment on
 * that field for why it's precomputed rather than derived here). A
 * PrimOp (or anything else -- real Nix's own version just requires a
 * function argument, but asnix hasn't implemented argument-type
 * checking that strictly here) has no such field at all; matching real
 * Nix's `functionArgs builtins.add` == `{}`. */
static Value prim_functionArgs(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) != ASNIX_TAG_CLOSURE) return asnix_bshared_make_attrs(NULL, NULL, 0);
    const char *base = (const char *)(args[0].raw & ~0x7ULL);
    return *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
}
static Value prim_isInt(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_INT ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isBool(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) != ASNIX_TAG_IMM) return asnix_make_imm(ASNIX_IMM_FALSE);
    int sub = asnix_imm_subtag(args[0]);
    return asnix_make_imm(sub == ASNIX_IMM_TRUE || sub == ASNIX_IMM_FALSE ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}

/* `baseNameOf`/`dirOf` (M23) -- two of real Nix's bare root-scope globals
 * (see builtins.h's asnix_make_import_primop comment for the full
 * curated list), found needed by numtide/nix-conformance's
 * eval-okay-baseNameOf.nix/eval-okay-builtins-dirOf.nix/eval-okay-path.nix
 * and others, all of which call these bare, not `builtins.X`. Ported
 * directly from real Nix's own `prim_baseNameOf`/`prim_dirOf`
 * (~/nix/src/libexpr/primops.cc), verified against `nix-instantiate
 * --eval` for every edge case below:
 *   baseNameOf "/foo/bar.nix"  -> "bar.nix"      (always a plain string,
 *   baseNameOf ./foo/bar.nix   -> "bar.nix"       regardless of whether
 *   baseNameOf "foo/"          -> "foo"           the input was a path)
 *   dirOf "/foo/bar.nix"       -> "/foo"          (type-preserving: a
 *   dirOf ./foo/bar.nix        -> /foo (a Path)    Path in, Path out;
 *   dirOf "foo"                -> "."              a String in,
 *   dirOf "/"                  -> "/"              String out)
 * Scope cut: real Nix accepts (and coerces) any string-coercible type
 * here (further erroring with real Nix's own wording on int/bool/null);
 * asnix only accepts an actual TAG_PTR (String or Path) argument, fatal
 * otherwise -- narrower, but every real caller found so far only ever
 * passes an actual path/string. */
static Value prim_baseNameOf(const Value *args, int64_t argc) {
    (void)argc;
    int64_t len;
    const char *bytes = asnix_ptr_bytes(args[0], &len);
    if (len > 0 && bytes[len - 1] == '/') len--; /* AT MOST ONE trailing slash stripped (an
                                                    * `if`, not a `while`) -- matching real
                                                    * Nix's legacyBaseNameOf exactly (NOT GNU
                                                    * basename's "any number"): verified
                                                    * `baseNameOf "a//"` => "" (strip one
                                                    * slash -> "a/", then the segment after
                                                    * ITS last '/' is empty), not "a" -- a
                                                    * prior version of this line used `while`,
                                                    * silently stripping every trailing slash
                                                    * instead of just one, found via
                                                    * numtide/nix-conformance's
                                                    * eval-okay-baseNameOf.nix. */
    int64_t start = len;
    while (start > 0 && bytes[start - 1] != '/') start--;
    return asnix_make_string_value(bytes + start, (size_t)(len - start));
}

static Value prim_dirOf(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    int64_t len;
    const char *bytes = asnix_ptr_bytes(v, &len);
    int is_path = asnix_value_is_path(v);
    /* asnix Paths are always already-resolved absolute paths with no
     * trailing slash (see compiler.c's AST_PATH/resolve_path_literal),
     * so "/" is the only case needing the real-Nix "isRoot, keep as-is"
     * special case -- everything else always contains a '/' to split on. */
    if (is_path) {
        if (len == 1 && bytes[0] == '/') return asnix_make_path_value("/", 1);
        int64_t pos = len;
        while (pos > 0 && bytes[pos - 1] != '/') pos--;
        pos--; /* back up onto the '/' itself */
        if (pos <= 0) return asnix_make_path_value("/", 1);
        return asnix_make_path_value(bytes, (size_t)pos);
    }
    int64_t pos = len;
    while (pos > 0 && bytes[pos - 1] != '/') pos--;
    if (pos == 0) return asnix_make_string_value(".", 1);
    pos--; /* back up onto the '/' itself */
    if (pos == 0) return asnix_make_string_value("/", 1);
    return asnix_make_string_value(bytes, (size_t)pos);
}

static Value build_baseNameOf(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
static Value build_dirOf(void) { return make_primop_value((void *)prim_dirOf, 1); }
Value asnix_make_baseNameOf_primop(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
Value asnix_make_dirOf_primop(void) { return make_primop_value((void *)prim_dirOf, 1); }

/* `builtins.placeholder` (M44) -- one of real Nix's bare root-scope
 * globals (confirmed in `~/nix/src/libexpr/primops.cc`: `.name =
 * "placeholder"`, no `__` prefix). Computes an output placeholder
 * string for a derivation output name, ported directly from
 * `~/nix/src/libstore/derivations.cc`'s `hashPlaceholder`: `"/" +
 * base32(sha256("nix-output:" + outputName))` (Nix's own base32
 * variant, no hash-type prefix) -- reuses this project's existing
 * `asnix_hash_sha256`/`asnix_base32_encode` (already relied on for
 * store-path hashing, M8) rather than adding any new hashing
 * machinery. Verified against real `nix-instantiate --eval`:
 * `builtins.placeholder "out"` ==
 * "/1rz4g4znpzjwh1xymhjpm42vipw92pr73vdgl6xs1hycac8kf2n9". */
static Value prim_placeholder(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "placeholder");
    int64_t name_len;
    const char *name_bytes;
    asnix_bshared_string_bytes(args[0], &name_bytes, &name_len, "placeholder");

    size_t preimage_len = 11 + (size_t)name_len; /* "nix-output:" */
    char *preimage = malloc(preimage_len);
    memcpy(preimage, "nix-output:", 11);
    memcpy(preimage + 11, name_bytes, (size_t)name_len);

    uint8_t digest[ASNIX_HASH_SHA256_LEN];
    int64_t rc = asnix_hash_sha256(preimage, preimage_len, digest);
    free(preimage);
    if (rc != 0) {
        fprintf(stderr, "asnix: placeholder: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    size_t b32_len = asnix_base32_encoded_length(ASNIX_HASH_SHA256_LEN);
    char *out = malloc(1 + b32_len);
    out[0] = '/';
    asnix_base32_encode(digest, ASNIX_HASH_SHA256_LEN, out + 1);
    Value result = asnix_make_string_value(out, 1 + b32_len);
    free(out);
    return result;
}
static Value build_placeholder(void) { return make_primop_value((void *)prim_placeholder, 1); }
Value asnix_make_placeholder_primop(void) { return make_primop_value((void *)prim_placeholder, 1); }

static Value build_isNull(void) { return make_primop_value((void *)prim_isNull, 1); }

/* `isNull` is one of real Nix's bare root-scope globals (verified against
 * `nix-instantiate --eval`, same research as M18's import/map/abort/
 * toString -- see docs/architecture.md). Wraps the exact same `prim_isNull`
 * `builtins.isNull` itself uses. */
Value asnix_make_isNull_primop(void) { return make_primop_value((void *)prim_isNull, 1); }
static Value build_isString(void) { return make_primop_value((void *)prim_isString, 1); }
static Value build_isPath(void) { return make_primop_value((void *)prim_isPath, 1); }
static Value build_functionArgs(void) { return make_primop_value((void *)prim_functionArgs, 1); }
static Value build_isList(void) { return make_primop_value((void *)prim_isList, 1); }
static Value build_isAttrs(void) { return make_primop_value((void *)prim_isAttrs, 1); }
static Value build_isFunction(void) { return make_primop_value((void *)prim_isFunction, 1); }
static Value build_isInt(void) { return make_primop_value((void *)prim_isInt, 1); }
static Value build_isBool(void) { return make_primop_value((void *)prim_isBool, 1); }

/* `builtins.nixVersion` -- a plain string constant, not a function (real
 * Nix's is a build-time-baked version string, not computed). This
 * `build()` slot doesn't require a PrimOp: it's called once, at
 * `builtins`-attrset construction time, and whatever Value it returns is
 * stored directly -- no wrapping needed for a non-callable constant.
 * Reports the real Nix compat version installed on this dev machine
 * (`nix-instantiate --version`), comfortably above any `>= "2.18"`-style
 * nixpkgs/lib version gate. */
static Value build_nixVersion(void) { return asnix_make_string_value("2.35.2", 6); }

/* `builtins.currentSystem` -- same "plain constant, no PrimOp needed"
 * shape as nixVersion above. Real Nix registers this internally as
 * `__currentSystem` (confirmed in ~/nix/src/libexpr/primops.cc), i.e.
 * NOT a bare global (M27's bare-global rule: only non-`__`-prefixed
 * names are exposed bare) -- `builtins.currentSystem` only, matching
 * `nix-instantiate --eval -E 'currentSystem'` => "undefined variable".
 * Hardcoded to this project's one supported platform (x86_64-linux,
 * see flake.nix's `systems` list) rather than computed from `uname` --
 * there's nothing else this process could ever be running on. Found
 * needed evaluating real nixpkgs (`d.nix`'s default.nix chain selects
 * `.currentSystem` while resolving the default platform). */
static Value build_currentSystem(void) { return asnix_make_string_value("x86_64-linux", 12); }

const AsnixBuiltinMiscDef ASNIX_BUILTINS_MISC[] = {
    { "substring", build_substring },
    { "stringLength", build_stringLength },
    { "concatStringsSep", build_concatStringsSep },
    { "isNull", build_isNull },
    { "isString", build_isString },
    { "isPath", build_isPath },
    { "functionArgs", build_functionArgs },
    { "addErrorContext", build_addErrorContext },
    { "isList", build_isList },
    { "isAttrs", build_isAttrs },
    { "isFunction", build_isFunction },
    { "isInt", build_isInt },
    { "isBool", build_isBool },
    { "nixVersion", build_nixVersion },
    { "currentSystem", build_currentSystem },
    { "baseNameOf", build_baseNameOf },
    { "dirOf", build_dirOf },
    { "placeholder", build_placeholder },
};

const size_t ASNIX_BUILTINS_MISC_COUNT = sizeof(ASNIX_BUILTINS_MISC) / sizeof(ASNIX_BUILTINS_MISC[0]);
