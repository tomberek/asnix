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
                case ASNIX_IMM_TRUE: return asnix_make_string_value("true", 4);
                case ASNIX_IMM_FALSE: return asnix_make_string_value("false", 5);
                case ASNIX_IMM_NULL: return asnix_make_string_value("", 0); /* matches real Nix */
                default: break;
            }
            break;
        case ASNIX_TAG_PTR: {
            if (!asnix_value_is_path(v)) return v; /* already a string */
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            return asnix_make_string_value(bytes, (size_t)len); /* path -> string, see value_util.h */
        }
        default: break;
    }
    fprintf(stderr, "asnix: cannot coerce this value to a string\n");
    exit(1);
}

char *asnix_string_value_to_cstr(Value string_val) {
    const char *s = (const char *)(string_val.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    char *out = asnix_arena_alloc((size_t)len + 1);
    memcpy(out, s + ASNIX_STRING_OFF_BYTES, (size_t)len);
    out[len] = '\0';
    return out;
}
