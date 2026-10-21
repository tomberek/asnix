#define _DEFAULT_SOURCE /* lstat() -- same feature-test macro builtins_m23.c uses */

#include "value_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h" /* asnix_force_for_print, asnix_apply */
#include "builtins_shared.h"
#include "symtab.h"

Value asnix_make_string_value(const char *s, size_t len) {
    void *mem = asnix_arena_alloc(8 + len);
    *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + ASNIX_STRING_OFF_BYTES, s, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

Value asnix_make_path_value(const char *abs_path, size_t len) {
    void *mem = asnix_arena_alloc(16 + len);
    *(int64_t *)((char *)mem + ASNIX_PATH_OFF_MARKER) = ASNIX_PATH_MARKER;
    *(int64_t *)((char *)mem + ASNIX_PATH_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + ASNIX_PATH_OFF_BYTES, abs_path, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

int asnix_value_is_path(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & ~0x7ULL);
    return *(const int64_t *)(base + ASNIX_PATH_OFF_MARKER) == ASNIX_PATH_MARKER;
}

Value asnix_make_float_value(double d) {
    void *mem = asnix_arena_alloc(16);
    *(int64_t *)((char *)mem + ASNIX_FLOAT_OFF_MARKER) = ASNIX_FLOAT_MARKER;
    *(double *)((char *)mem + ASNIX_FLOAT_OFF_VALUE) = d;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

int asnix_value_is_float(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & ~0x7ULL);
    return *(const int64_t *)(base + ASNIX_FLOAT_OFF_MARKER) == ASNIX_FLOAT_MARKER;
}

double asnix_float_value(Value v) {
    const char *base = (const char *)(v.raw & ~0x7ULL);
    return *(const double *)(base + ASNIX_FLOAT_OFF_VALUE);
}

/* Shared by asnix_add_values/asnix_sub_values/asnix_mul_values/
 * asnix_div_values/the four asnix_cmp_*_values functions below: real
 * Nix promotes an int operand to float whenever the OTHER operand is
 * already a float (verified against `nix-instantiate --eval` for every
 * one of the four arithmetic operators, and for the four ordering
 * comparisons' CompareValues -- see
 * value_util.h's comments on each). Returns 1 and writes `*out` if `v`
 * is numeric (int or float), 0 (leaving `*out` untouched) otherwise --
 * callers combine this with their own fatal message, since each call
 * site's wording differs slightly (matching existing convention). */
static int as_double(Value v, double *out) {
    if (asnix_value_is_float(v)) {
        *out = asnix_float_value(v);
        return 1;
    }
    if (asnix_tag_of(v) == ASNIX_TAG_INT) {
        *out = (double)asnix_int_value(v);
        return 1;
    }
    return 0;
}

const char *asnix_ptr_bytes(Value v, int64_t *out_len) {
    const char *base = (const char *)(v.raw & ~0x7ULL);
    if (asnix_value_is_path(v)) {
        *out_len = *(const int64_t *)(base + ASNIX_PATH_OFF_LEN);
        return base + ASNIX_PATH_OFF_BYTES;
    }
    *out_len = *(const int64_t *)(base + ASNIX_STRING_OFF_LEN);
    return base + ASNIX_STRING_OFF_BYTES;
}

Value asnix_coerce_to_string(Value v) {
    char buf[32];
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: {
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)asnix_int_value(v));
            return asnix_make_string_value(buf, (size_t)n);
        }
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                /* Matches real Nix exactly (verified with `nix-instantiate --eval
                 * -E 'builtins.toString true/false/null'` -- *not* "true"/"false":
                 * real Nix's `toString` on a bool yields "1" for true and "" for
                 * false, same empty string as null. A prior version of this
                 * function returned "true"/"false" here, which is wrong -- see
                 * docs/architecture.md's "Known bugs" list. */
                case ASNIX_IMM_TRUE: return asnix_make_string_value("1", 1);
                case ASNIX_IMM_FALSE: return asnix_make_string_value("", 0);
                case ASNIX_IMM_NULL: return asnix_make_string_value("", 0);
                default: break;
            }
            break;
        case ASNIX_TAG_PTR: {
            /* M51: a Float formats via the SAME rule real Nix's own
             * `coerceToString` uses for `toString`/coerceMore (verified
             * against `nix-instantiate --eval`: `toString 1.5` ==
             * "1.500000", `toString (0.0 - 1.0/0.0 ... )`-style nan/inf
             * cases print "nan"/"-nan"/"inf"/"-inf") -- `std::to_string`
             * on a double is glibc's `%f`: always 6 digits after the
             * decimal point for a finite value, and for non-finite
             * values `%f`'s nan/inf spelling (with sign) falls out for
             * free, no special-casing needed. NOT the same formatting
             * the top-level value printer uses (main.c's
             * print_value_inner, "%.6g"-style) -- two genuinely
             * different real-Nix formatting rules for two different call
             * sites, confirmed by directly comparing `nix-instantiate
             * --eval '1.5'` ("1.5") against `nix-instantiate --eval
             * 'toString 1.5'` ("1.500000"). */
            if (asnix_value_is_float(v)) {
                char buf[512]; /* worst case: %f of the largest finite double is ~317 bytes */
                int n = snprintf(buf, sizeof(buf), "%f", asnix_float_value(v));
                return asnix_make_string_value(buf, (size_t)n);
            }
            if (!asnix_value_is_path(v)) return v; /* already a string */
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            return asnix_make_string_value(bytes, (size_t)len); /* path -> string, see value_util.h */
        }
        case ASNIX_TAG_ATTRS: {
            /* An attrset with a `__toString` or `outPath` attribute
             * coerces to a string via that attribute (M23) -- real Nix's
             * own `coerceToString` checks exactly these two, in this
             * order, before giving up (e.g. a derivation attrset's
             * `outPath` is exactly how `"${someDerivation}"` and
             * `toString someDerivation` work) -- found needed by
             * numtide/nix-conformance's eval-okay-pathexists.nix
             * (`builtins.toPath { __toString = x: ...; }` and
             * `{ outPath = ...; }`), verified against `nix-instantiate
             * --eval`. `__toString` is called with the attrset itself as
             * its one argument, matching real Nix; its result (and a
             * plain `outPath`'s value) is coerced recursively, since
             * either could itself be a path or another such object.
             * Scope cut: this is purely a *value*-coercion upgrade, not
             * the separate, larger "string context" feature (dependency
             * tracking through string ops) this project deliberately
             * doesn't implement -- a derivation's `outPath` still works
             * here as a plain string, just without any attached context. */
            uint32_t sym_tostring = asnix_intern("__toString", 10);
            int64_t idx = asnix_bshared_attrs_find(v, sym_tostring);
            if (idx >= 0) {
                Value fn = asnix_force_for_print(asnix_bshared_attrs_value(v, idx));
                Value result = asnix_apply(fn, &v, 1);
                return asnix_coerce_to_string(asnix_force_for_print(result));
            }
            uint32_t sym_outpath = asnix_intern("outPath", 7);
            idx = asnix_bshared_attrs_find(v, sym_outpath);
            if (idx >= 0) {
                return asnix_coerce_to_string(asnix_force_for_print(asnix_bshared_attrs_value(v, idx)));
            }
            break;
        }
        case ASNIX_TAG_LIST: {
            /* A list coerces to a string by recursively coercing each
             * element and joining with a single space (M47) -- real
             * Nix's own `coerceToString`'s `coerceMore` branch does the
             * same (`~/nix/src/libexpr/eval.cc`), found needed by
             * `<nix/fetchurl.nix>`'s own `urls = [ url ];` derivation
             * attribute (every derivation attribute value is coerced to
             * a string for the environment, see `derivation.c`).
             * Verified against real `nix-instantiate --eval`:
             * `builtins.toString [1 2 3]` == `"1 2 3"`, `builtins.toString
             * [ "a" "b" ]` == `"a b"`, `builtins.toString []` == `""`.
             * Does not replicate cppnix's own "skip the separator before
             * an empty nested list" quirk (that function's own comment
             * calls it "not quite correct") -- not hit by anything this
             * project has evaluated so far. */
            int64_t count = asnix_bshared_list_count(v);
            const Value *elems = asnix_bshared_list_elems(v);
            char *result = NULL;
            size_t result_len = 0;
            for (int64_t i = 0; i < count; i++) {
                Value piece = asnix_coerce_to_string(asnix_force_for_print(elems[i]));
                int64_t piece_len;
                const char *piece_bytes = asnix_ptr_bytes(piece, &piece_len);
                int has_sep = (i + 1 < count);
                result = realloc(result, result_len + (size_t)piece_len + (has_sep ? 1 : 0));
                memcpy(result + result_len, piece_bytes, (size_t)piece_len);
                result_len += (size_t)piece_len;
                if (has_sep) result[result_len++] = ' ';
            }
            Value out = asnix_make_string_value(result ? result : "", result_len);
            free(result);
            return out;
        }
        default: break;
    }
    fprintf(stderr, "asnix: cannot coerce this value to a string\n");
    exit(1);
}

/* String *interpolation* coercion is strictly narrower than `toString`'s
 * (asnix_coerce_to_string, above) -- verified directly against real Nix:
 * `nix-instantiate --eval -E '"${7}"'` and `'"${true}"'` and `'"${null}"'`
 * all fail with "cannot coerce ... to a string", even though
 * `builtins.toString 7`/`true`/`null` succeed. (Real Nix's own
 * EvalState::coerceToString has a `coerceMore` flag; `toString` passes
 * true, plain "${...}" interpolation passes false -- only strings, paths,
 * and sets with a `__toString`/`outPath` attr are accepted without it;
 * none of the latter exist in this project yet.) This is *not* the
 * function docs/architecture.md's Known Bugs entry is about -- that one
 * was about `asnix_coerce_to_string` returning the wrong *string*
 * ("true"/"false" instead of "1"/""); this is a separate, stricter
 * function for a different call site, needed because the task's own
 * verification step (checked against real nix-instantiate before
 * trusting it) caught that bare interpolation of a non-string is
 * supposed to be a hard error, not silently stringified. */
/* M67: `check_path_exists` -- true for genuine `"${...}"` string
 * interpolation (OP_STR_CONCAT, via `asnix_concat_strings` below), false
 * for the UNRELATED internal reuse of this same coercion logic by the
 * `+` operator's path-concat case (`asnix_add_values`, via
 * `concat_strings_impl` below). Real Nix's own path-existence
 * requirement is specific to interpolation (string coercion that would
 * trigger a copy-to-store) -- `+` on a Path is ordinary text
 * concatenation with no such requirement (confirmed against real
 * `nix-instantiate --eval`: `toString (/foo/bar + "/x")` succeeds even
 * when `/foo/bar` doesn't exist, while `"${./bar}"` on the same
 * nonexistent path fails) -- found as a regression this fix introduced
 * (`tests/e2e/m57g_path_canon_concat.nix`, path `+` string, broke when
 * the existence check below was first added unconditionally). */
static Value coerce_to_string_for_interp(Value v, int check_path_exists) {
    if (asnix_tag_of(v) == ASNIX_TAG_PTR) {
        /* A Path is a second, differently-shaped ASNIX_TAG_PTR kind (see
         * value_util.h) -- returning it unchanged, as if every TAG_PTR were
         * already a plain String, reads its marker field as a bogus string
         * length and corrupts memory (found interpolating a path literal:
         * `"${./foo}"` segfaulted). Real Nix accepts a path here without
         * `builtins.toString` (unlike ints/bools/null -- see this
         * function's own comment above), so reject isn't right either;
         * coerce it the same way `asnix_coerce_to_string` does. Real Nix
         * additionally copies the path to the store and substitutes the
         * resulting store path here -- not implemented (no daemon
         * AddToStore yet, see docs/architecture.md's import/path-literals
         * scope notes) -- so this produces the plain path text instead,
         * a known, documented behavioral gap, not a crash. */
        if (asnix_value_is_path(v)) {
            /* M67: real Nix additionally requires the path *exist* on
             * disk at this specific call site -- string interpolation of
             * a bare path triggers "copy to store" coercion (unlike
             * `builtins.toString`, which just formats the text and never
             * checks existence: confirmed `toString ./nonexistent`
             * succeeds against real `nix-instantiate --eval`, while
             * `"${./nonexistent}"` fails with `error: path '...' does
             * not exist`). No actual store-copy is implemented here (see
             * this function's own comment just above), but the
             * existence check that gates it is cheap and independently
             * observable, so add just that much: `eval-fail-nonexist-
             * path.nix` (`"${./fnord}/xyzzy"`) and `eval-fail-bad-
             * string-interpolation-2.nix` (`"${./fnord}"`) both expect a
             * fatal error here, verified against real Nix. `lstat`, not
             * `stat`, so a dangling symlink still counts as "exists" --
             * matches `builtins.pathExists`'s own existing convention in
             * builtins_m23.c. Gated on `check_path_exists` -- see this
             * function's own header comment for why `+`'s reuse of this
             * same coercion needs to skip it. */
            if (check_path_exists) {
                int64_t len;
                const char *bytes = asnix_ptr_bytes(v, &len);
                char *nul_terminated = malloc((size_t)len + 1);
                memcpy(nul_terminated, bytes, (size_t)len);
                nul_terminated[len] = '\0';
                struct stat st;
                int missing = (lstat(nul_terminated, &st) != 0);
                if (missing) {
                    fprintf(stderr, "asnix: path '%s' does not exist\n", nul_terminated);
                }
                free(nul_terminated);
                if (missing) exit(1);
            }
            return asnix_coerce_to_string(v);
        }
        /* M51: a bare Float is a THIRD such kind -- same memory-corruption
         * risk as Path if returned unchanged (its marker+double bytes
         * would be misread as a string length+content). Unlike Path, a
         * Float is REJECTED here, same as an int (verified against
         * `nix-instantiate --eval`: `"${1.5}"` fails "cannot coerce a
         * float to a string", while `"${toString 1.5}"` succeeds --
         * `coerceToString`'s `coerceMore` branch, which handles nFloat,
         * is exactly the branch `toString` takes and bare interpolation
         * does not, same split this function's own header comment
         * already describes for int/bool/null). Falls through to the
         * generic rejection below, which already knows how to name a
         * Float via asnix_tag_of(v)==ASNIX_TAG_PTR's... no -- that
         * switch is INT-only for "an integer"; handled explicitly right
         * here instead since the switch below has no float case of its
         * own. */
        if (asnix_value_is_float(v)) {
            fprintf(stderr, "asnix: cannot coerce a float to a string in a string interpolation "
                             "(use builtins.toString explicitly)\n");
            exit(1);
        }
        return v;
    }
    if (asnix_tag_of(v) == ASNIX_TAG_ATTRS) {
        /* An attrset with `__toString`/`outPath` IS accepted here even
         * though int/bool/null/list/plain-attrset aren't (M23) --
         * real Nix's own `coerceMore=false` path still special-cases
         * exactly this (verified: `"${derivation {...}}"` interpolates
         * fine via its `outPath`, while `"${{}}""` -- a plain attrset
         * with neither attribute -- still fails the same as any other
         * disallowed type). Delegate to asnix_coerce_to_string, which
         * already implements the __toString/outPath lookup -- if
         * neither is present it fatals with its own (slightly
         * differently worded, but equally clear) "cannot coerce"
         * message, which is fine since that's the "a plain set" case
         * real Nix also rejects here regardless of wording. */
        uint32_t sym_tostring = asnix_intern("__toString", 10);
        uint32_t sym_outpath = asnix_intern("outPath", 7);
        if (asnix_bshared_attrs_find(v, sym_tostring) >= 0 || asnix_bshared_attrs_find(v, sym_outpath) >= 0) {
            return asnix_coerce_to_string(v);
        }
    }
    const char *kind = "this value";
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: kind = "an integer"; break;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE:
                case ASNIX_IMM_FALSE: kind = "a boolean"; break;
                case ASNIX_IMM_NULL: kind = "null"; break;
                default: break;
            }
            break;
        case ASNIX_TAG_LIST: kind = "a list"; break;
        case ASNIX_TAG_ATTRS: kind = "a set"; break;
        case ASNIX_TAG_CLOSURE:
        case ASNIX_TAG_PRIMOP: kind = "a function"; break;
        default: break;
    }
    fprintf(stderr, "asnix: cannot coerce %s to a string in a string interpolation "
                     "(use builtins.toString explicitly)\n", kind);
    exit(1);
}

/* Shared by `asnix_concat_strings` (real `"${...}"` interpolation,
 * `check_path_exists=1`) and `asnix_add_values`'s path-concat case (`+`,
 * `check_path_exists=0`) -- see `coerce_to_string_for_interp`'s own
 * header comment for why the two need different existence-checking
 * behavior despite sharing every other coercion rule. */
static Value concat_strings_impl(const Value *parts, int64_t count, int check_path_exists) {
    if (count <= 0) return asnix_make_string_value("", 0);

    Value *coerced = malloc((size_t)count * sizeof(Value));
    int64_t total_len = 0;
    for (int64_t i = 0; i < count; i++) {
        coerced[i] = coerce_to_string_for_interp(parts[i], check_path_exists);
        const char *s = (const char *)(coerced[i].raw & ~0x7ULL);
        total_len += *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    }

    void *mem = asnix_arena_alloc(8 + (size_t)total_len);
    *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = total_len;
    char *out = (char *)mem + ASNIX_STRING_OFF_BYTES;
    for (int64_t i = 0; i < count; i++) {
        const char *s = (const char *)(coerced[i].raw & ~0x7ULL);
        int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
        memcpy(out, s + ASNIX_STRING_OFF_BYTES, (size_t)len);
        out += len;
    }
    free(coerced);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

/* Shared by asnix_add_values (`path + string`) and asnix_concat_path
 * (M64, `~`/path-literal interpolation): re-canonicalizes a plain String
 * Value's bytes (which must already start with '/' -- true for both
 * callers, since each only reaches this once it's established the
 * concatenation's first operand was already an absolute Path) and
 * rewraps it as a Path. */
static Value canon_wrap_as_path(Value string_result) {
    int64_t len;
    const char *bytes = asnix_ptr_bytes(string_result, &len);
    char *nul_terminated = malloc((size_t)len + 1);
    memcpy(nul_terminated, bytes, (size_t)len);
    nul_terminated[len] = '\0';
    char *canon = asnix_canon_path(nul_terminated);
    free(nul_terminated);
    Value out = asnix_make_path_value(canon, strlen(canon));
    free(canon);
    return out;
}

/* M64: the runtime half of path-literal interpolation (`./${foo}`,
 * `~/${foo}`, `/${foo}`, ...) -- see OP_PATH_CONCAT's comment in ir.h for
 * why unconditionally canon-wrapping is safe here (compiler.c's AST_PATH
 * case only ever emits this op when `parts[0]` is already a Path
 * constant). Calls `concat_strings_impl` directly with
 * `check_path_exists=0` (NOT the public `asnix_concat_strings` wrapper,
 * which since M67 defaults to `check_path_exists=1`) -- a path LITERAL's
 * interpolated parts are plain text being assembled into a new path, not
 * a Path VALUE being embedded in a string, so the M67 existence check
 * (which exists specifically for the latter, `"${somePath}"`) must not
 * apply here; same distinction `asnix_add_values`'s own `path + string`
 * case (right below) already makes for the identical reason. Re-
 * canonicalizes the WHOLE result after concatenation, matching real
 * Nix's `ExprConcatStrings::eval` exactly (it re-canonicalizes
 * unconditionally too, even though the first chunk's own text was
 * deliberately left un-canonicalized when it was built -- see
 * resolve_path_chunk in compiler.c). */
Value asnix_concat_path(const Value *parts, int64_t count) {
    Value result = concat_strings_impl(parts, count, 0);
    return canon_wrap_as_path(result);
}

Value asnix_concat_strings(const Value *parts, int64_t count) {
    return concat_strings_impl(parts, count, 1);
}

Value asnix_add_values(Value lhs, Value rhs) {
    int lt = asnix_tag_of(lhs), rt = asnix_tag_of(rhs);
    if (lt == ASNIX_TAG_INT && rt == ASNIX_TAG_INT) {
        return asnix_make_int(asnix_int_value(lhs) + asnix_int_value(rhs));
    }
    /* M51: int/float or float/float -- promote and add as doubles, same
     * rule as real Nix's `+` (ExprConcatStrings::eval, ~/nix's eval.cc:
     * "Upgrade the type from int to float" once either operand is a
     * float). Checked BEFORE the plain-int-vs-non-int fatal below and
     * the string/path concat fallback after it, since a float is
     * neither. */
    if (asnix_value_is_float(lhs) || asnix_value_is_float(rhs)) {
        double a, b;
        if (!as_double(lhs, &a)) {
            fprintf(stderr, "asnix: '+': cannot add a non-numeric value to a float\n");
            exit(1);
        }
        if (!as_double(rhs, &b)) {
            fprintf(stderr, "asnix: '+': cannot add a non-numeric value to a float\n");
            exit(1);
        }
        return asnix_make_float_value(a + b);
    }
    if (lt == ASNIX_TAG_INT || rt == ASNIX_TAG_INT) {
        fprintf(stderr, "asnix: '+': cannot add an integer and a non-integer\n");
        exit(1);
    }
    Value parts[2] = { lhs, rhs };
    Value result = concat_strings_impl(parts, 2, 0); /* M67: `+` never requires a Path to exist */
    if (asnix_value_is_path(lhs)) {
        /* M57g: canonicalize `.`/`..`/doubled-slash segments the result
         * may now have (e.g. `/foo/bar + "/../xyzzy/./foo.txt"` ==
         * `/foo/xyzzy/foo.txt`), same as a path *literal* already gets
         * at compile time (compiler.c's AST_PATH case) -- a runtime
         * `path + string` concatenation previously just kept the raw,
         * uncollapsed text. `result`'s bytes always start with '/'
         * here since `lhs` is already an absolute Path. M64: shares
         * canon_wrap_as_path with asnix_concat_path now, same logic. */
        return canon_wrap_as_path(result);
    }
    return result;
}

/* M51: `-`/`*`'s slow-path fallback -- see value_util.h's comment. Only
 * ever called when at least one operand isn't a plain int (the all-int
 * case is already handled by op_arith.S's own fast path before this is
 * reached), so getting here with two non-floats is already a type
 * error. */
Value asnix_sub_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return asnix_make_float_value(a - b);
    fprintf(stderr, "asnix: '-': operands must both be numbers (int or float), aborting\n");
    exit(1);
}

Value asnix_mul_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return asnix_make_float_value(a * b);
    fprintf(stderr, "asnix: '*': operands must both be numbers (int or float), aborting\n");
    exit(1);
}

Value asnix_div_values(Value lhs, Value rhs) {
    double a, b;
    if (!as_double(lhs, &a) || !as_double(rhs, &b)) {
        fprintf(stderr, "asnix: '/': operands must both be numbers (int or float), aborting\n");
        exit(1);
    }
    /* Matches real Nix exactly: the float-coerced divisor is checked
     * against zero unconditionally, even when both operands started out
     * as ints and would otherwise divide evenly as floats (e.g. there is
     * no IEEE-754 "divide by zero gives infinity" case here at all --
     * see value_util.h's comment). The pure-int/int zero check already
     * lives in op_arith.S's own fast path; this is the one case that
     * reaches here with a zero divisor: an int/float or float/float pair. */
    if (b == 0.0) {
        fprintf(stderr, "asnix: division by zero, aborting\n");
        exit(1);
    }
    return asnix_make_float_value(a / b);
}

/* True iff `v` is a genuine String -- ASNIX_TAG_PTR but neither a Path
 * nor a Float, the two other kinds that share the tag (see heap.h).
 * Shared by the four ordering fallbacks below for their string<string
 * case (same scope real Nix's `<`/`>`/`<=`/`>=` support beyond numbers:
 * `~/nix/src/libexpr/primops.cc`'s `CompareValues` also orders paths and
 * lists, but this project's existing `asnix_bshared_value_lt` -- used by
 * `sort`/`lessThan` -- already deliberately narrows that to "INT<INT or
 * string<string only", and this mirrors that same already-accepted
 * scope cut for the operators rather than inventing a wider one). */
static int is_plain_string(Value v) {
    return asnix_tag_of(v) == ASNIX_TAG_PTR && !asnix_value_is_path(v) && !asnix_value_is_float(v);
}

/* Lexicographic byte comparison, shorter-is-less on a shared prefix --
 * same rule as asnix_bshared_value_lt's existing string<string case
 * (builtins_shared.c), reimplemented here rather than shared across the
 * two files for the same reason value_util.c and builtins_shared.c
 * already don't share this kind of tiny helper elsewhere (no import
 * cycle: builtins_shared.c depends on value_util.h, not the reverse). */
static int str_lt(Value a, Value b) {
    int64_t la, lb;
    const char *ba = asnix_ptr_bytes(a, &la);
    const char *bb = asnix_ptr_bytes(b, &lb);
    int64_t n = la < lb ? la : lb;
    int c = n > 0 ? memcmp(ba, bb, (size_t)n) : 0;
    if (c != 0) return c < 0;
    return la < lb;
}

Value asnix_cmp_lt_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a < b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return str_lt(lhs, rhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_cmp_gt_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a > b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return str_lt(rhs, lhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_cmp_le_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a <= b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return !str_lt(rhs, lhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_cmp_ge_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a >= b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return !str_lt(lhs, rhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_concat_lists(Value lhs, Value rhs) {
    if (asnix_tag_of(lhs) != ASNIX_TAG_LIST) {
        fprintf(stderr, "asnix: expected a list but found a non-list value (lhs of '++')\n");
        exit(1);
    }
    if (asnix_tag_of(rhs) != ASNIX_TAG_LIST) {
        fprintf(stderr, "asnix: expected a list but found a non-list value (rhs of '++')\n");
        exit(1);
    }
    const char *lbase = (const char *)(uintptr_t)(lhs.raw & ~(uint64_t)ASNIX_TAG_MASK);
    const char *rbase = (const char *)(uintptr_t)(rhs.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t lcount = *(const int64_t *)(lbase + ASNIX_LIST_OFF_COUNT);
    int64_t rcount = *(const int64_t *)(rbase + ASNIX_LIST_OFF_COUNT);
    int64_t total = lcount + rcount;

    void *mem = asnix_arena_alloc(8 + (uint64_t)total * sizeof(Value));
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = total;
    Value *out = (Value *)((char *)mem + ASNIX_LIST_OFF_ELEMS);
    if (lcount > 0) {
        memcpy(out, lbase + ASNIX_LIST_OFF_ELEMS, (size_t)lcount * sizeof(Value));
    }
    if (rcount > 0) {
        memcpy(out + lcount, rbase + ASNIX_LIST_OFF_ELEMS, (size_t)rcount * sizeof(Value));
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

/* Moved here from compiler.c (M57g) so asnix_add_values can reuse it for
 * `path + string` concatenation -- see this function's own long comment
 * in value_util.h for why, and compiler.c's AST_PATH case for the
 * original, still-current, compile-time-literal use. */
char *asnix_canon_path(const char *in) {
    size_t len = strlen(in);
    char *out = malloc(len + 2);
    char **comps = malloc((len + 1) * sizeof(char *));
    size_t ncomps = 0;

    size_t i = 0;
    while (i < len) {
        while (i < len && in[i] == '/') i++;
        size_t start = i;
        while (i < len && in[i] != '/') i++;
        size_t clen = i - start;
        if (clen == 0) continue;
        if (clen == 1 && in[start] == '.') continue; /* "." -- drop */
        if (clen == 2 && in[start] == '.' && in[start + 1] == '.') {
            if (ncomps > 0) ncomps--; /* ".." -- pop (no-op at root, like real Nix) */
            continue;
        }
        char *c = malloc(clen + 1);
        memcpy(c, in + start, clen);
        c[clen] = '\0';
        comps[ncomps++] = c;
    }

    size_t pos = 0;
    for (size_t k = 0; k < ncomps; k++) {
        out[pos++] = '/';
        size_t clen = strlen(comps[k]);
        memcpy(out + pos, comps[k], clen);
        pos += clen;
        free(comps[k]);
    }
    free(comps);
    if (pos == 0) out[pos++] = '/';
    out[pos] = '\0';
    return out;
}

char *asnix_string_value_to_cstr(Value string_val) {
    const char *s = (const char *)(string_val.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    char *out = asnix_arena_alloc((size_t)len + 1);
    memcpy(out, s + ASNIX_STRING_OFF_BYTES, (size_t)len);
    out[len] = '\0';
    return out;
}
