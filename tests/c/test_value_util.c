#include <stdio.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/value.h"
#include "value_util.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

int main(void) {
    asnix_arena_init(0);

    Value s = asnix_make_string_value("hello", 5);
    Value p = asnix_make_path_value("/abs/foo.nix", 12);

    CHECK(asnix_tag_of(s) == ASNIX_TAG_PTR, "string value is ASNIX_TAG_PTR");
    CHECK(asnix_tag_of(p) == ASNIX_TAG_PTR, "path value is also ASNIX_TAG_PTR (no free tag bit -- see heap.h)");
    CHECK(!asnix_value_is_path(s), "a String is not a Path");
    CHECK(asnix_value_is_path(p), "a Path is recognized as a Path");

    int64_t slen, plen;
    const char *sbytes = asnix_ptr_bytes(s, &slen);
    const char *pbytes = asnix_ptr_bytes(p, &plen);
    CHECK(slen == 5 && memcmp(sbytes, "hello", 5) == 0, "asnix_ptr_bytes reads a String's bytes correctly");
    CHECK(plen == 12 && memcmp(pbytes, "/abs/foo.nix", 12) == 0, "asnix_ptr_bytes reads a Path's bytes correctly");

    /* toString on a path: real Nix changes its *type* from "path" to
     * "string" (verified against `nix-instantiate --eval`) -- same bytes,
     * no longer a Path afterward. */
    Value coerced = asnix_coerce_to_string(p);
    CHECK(!asnix_value_is_path(coerced), "toString(path) produces a plain String, not a Path");
    int64_t clen;
    const char *cbytes = asnix_ptr_bytes(coerced, &clen);
    CHECK(clen == 12 && memcmp(cbytes, "/abs/foo.nix", 12) == 0, "toString(path) keeps the same text");

    /* toString on an already-a-string value is a no-op passthrough
     * (same Value, not a copy) -- unaffected by the path addition. */
    Value coerced_s = asnix_coerce_to_string(s);
    CHECK(coerced_s.raw == s.raw, "toString(string) still returns the same value unchanged");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_value_util: all checks passed\n");
    return 0;
}
