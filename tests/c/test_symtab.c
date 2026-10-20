#include <stdio.h>
#include <string.h>

#include "symtab.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

int main(void) {
    uint32_t a = asnix_intern("foo", 3);
    uint32_t b = asnix_intern("bar", 3);
    uint32_t a2 = asnix_intern("foo", 3);

    CHECK(a == a2, "interning the same name twice returns the same id");
    CHECK(a != b, "distinct names get distinct ids");

    size_t len;
    const char *name = asnix_sym_name(a, &len);
    CHECK(len == 3 && memcmp(name, "foo", 3) == 0, "reverse lookup recovers the name");

    /* Enough distinct names to force at least one rehash (initial table
     * capacity is 16, 75% load factor). */
    uint32_t ids[200];
    char buf[32];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "sym_%d", i);
        ids[i] = asnix_intern(buf, strlen(buf));
    }
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "sym_%d", i);
        uint32_t again = asnix_intern(buf, strlen(buf));
        CHECK(again == ids[i], "id stable across rehashing");
    }
    for (int i = 0; i < 200; i++)
        for (int j = i + 1; j < 200; j++)
            if (ids[i] == ids[j]) {
                fprintf(stderr, "FAIL: duplicate id %u for sym_%d and sym_%d\n", ids[i], i, j);
                failures++;
            }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_symtab: all checks passed\n");
    return 0;
}
