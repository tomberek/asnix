#include "value_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"

Value asnix_make_string_value(const char *s, size_t len) {
    void *mem = asnix_arena_alloc(8 + len);
    *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + ASNIX_STRING_OFF_BYTES, s, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
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
        case ASNIX_TAG_PTR:
            return v; /* already a string */
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
    if (asnix_tag_of(v) == ASNIX_TAG_PTR) return v;
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

char *asnix_string_value_to_cstr(Value string_val) {
    const char *s = (const char *)(string_val.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    char *out = asnix_arena_alloc((size_t)len + 1);
    memcpy(out, s + ASNIX_STRING_OFF_BYTES, (size_t)len);
    out[len] = '\0';
    return out;
}
