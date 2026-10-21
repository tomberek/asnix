#include "value_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static Value coerce_to_string_for_interp(Value v) {
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
        if (asnix_value_is_path(v)) return asnix_coerce_to_string(v);
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

Value asnix_concat_strings(const Value *parts, int64_t count) {
    if (count <= 0) return asnix_make_string_value("", 0);

    Value *coerced = malloc((size_t)count * sizeof(Value));
    int64_t total_len = 0;
    for (int64_t i = 0; i < count; i++) {
        coerced[i] = coerce_to_string_for_interp(parts[i]);
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

Value asnix_add_values(Value lhs, Value rhs) {
    int lt = asnix_tag_of(lhs), rt = asnix_tag_of(rhs);
    if (lt == ASNIX_TAG_INT && rt == ASNIX_TAG_INT) {
        return asnix_make_int(asnix_int_value(lhs) + asnix_int_value(rhs));
    }
    if (lt == ASNIX_TAG_INT || rt == ASNIX_TAG_INT) {
        fprintf(stderr, "asnix: '+': cannot add an integer and a non-integer\n");
        exit(1);
    }
    Value parts[2] = { lhs, rhs };
    Value result = asnix_concat_strings(parts, 2);
    if (asnix_value_is_path(lhs)) {
        int64_t len;
        const char *bytes = asnix_ptr_bytes(result, &len);
        return asnix_make_path_value(bytes, (size_t)len);
    }
    return result;
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

char *asnix_string_value_to_cstr(Value string_val) {
    const char *s = (const char *)(string_val.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    char *out = asnix_arena_alloc((size_t)len + 1);
    memcpy(out, s + ASNIX_STRING_OFF_BYTES, (size_t)len);
    out[len] = '\0';
    return out;
}
