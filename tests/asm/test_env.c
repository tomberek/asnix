#include <stdio.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/value.h"

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

    Env *root = asnix_env_alloc(NULL, 0);
    CHECK(root != NULL, "alloc of a 0-slot root frame succeeds");

    Env *mid = asnix_env_alloc(root, 2);
    mid->values[0] = asnix_make_int(10);
    mid->values[1] = asnix_make_int(20);
    /* M118: mid->up is now a 32-bit offset into asnix_env_arena_base's
     * region, not a raw pointer -- compress `root` the same way
     * asnix_env_alloc itself does before comparing. */
    CHECK(mid->up == (uint32_t)((uintptr_t)root - (uintptr_t)asnix_env_arena_base),
          "env_alloc wires up the parent pointer");

    Env *inner = asnix_env_alloc(mid, 1);
    inner->values[0] = asnix_make_int(99);

    CHECK(asnix_int_value(asnix_env_lookup(inner, 0, 0)) == 99, "depth-0 lookup reads the current frame");
    CHECK(asnix_int_value(asnix_env_lookup(inner, 1, 0)) == 10, "depth-1 lookup walks one `up` hop");
    CHECK(asnix_int_value(asnix_env_lookup(inner, 1, 1)) == 20, "depth-1 lookup indexes the right slot");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_env: all checks passed\n");
    return 0;
}
