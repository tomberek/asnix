#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "asnix/arena.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void test_basic_alloc(void) {
    asnix_arena_init(0);
    void *a = asnix_arena_alloc(16);
    void *b = asnix_arena_alloc(16);
    CHECK(a != NULL, "alloc returns non-null");
    CHECK(b != NULL, "alloc returns non-null");
    CHECK(a != b, "distinct allocations don't overlap pointers");
    CHECK((uintptr_t)b - (uintptr_t)a == 16, "sequential allocs are contiguous and exactly-sized");
    memset(a, 0xAB, 16);
    memset(b, 0xCD, 16);
    CHECK(((unsigned char *)a)[0] == 0xAB, "allocated memory is writable (a)");
    CHECK(((unsigned char *)b)[15] == 0xCD, "allocated memory is writable (b)");
}

static void test_alignment(void) {
    asnix_arena_init(0);
    for (int i = 0; i < 20; i++) {
        void *p = asnix_arena_alloc((uint64_t)i);
        CHECK(((uintptr_t)p & 0x7) == 0, "every allocation is 8-byte aligned");
    }
}

static void test_chunk_boundary_crossing(void) {
    asnix_arena_init(0);
    void *first = asnix_arena_alloc(ASNIX_ARENA_CHUNK_SIZE - 64);
    void *second = asnix_arena_alloc(256); /* must overflow into a fresh chunk */
    CHECK(first != NULL && second != NULL, "allocations across a chunk boundary both succeed");
    memset(first, 0x11, ASNIX_ARENA_CHUNK_SIZE - 64);
    memset(second, 0x22, 256);
    CHECK(((unsigned char *)first)[0] == 0x11, "pre-boundary allocation is writable");
    CHECK(((unsigned char *)second)[255] == 0x22, "post-boundary (new chunk) allocation is writable");
}

static void test_large_single_allocation(void) {
    asnix_arena_init(0);
    uint64_t big = (uint64_t)ASNIX_ARENA_CHUNK_SIZE * 3; /* larger than one default chunk */
    void *p = asnix_arena_alloc(big);
    CHECK(p != NULL, "a single allocation larger than the default chunk size succeeds");
    memset(p, 0x33, big);
    CHECK(((unsigned char *)p)[big - 1] == 0x33, "oversized allocation is fully writable");
}

static void test_memory_cap_abort(void) {
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr); /* keep the abort message out of test output */
        asnix_arena_init(64);
        asnix_arena_alloc(32);
        asnix_arena_alloc(64); /* 32+64=96 > cap(64) -- must abort(1), never return */
        _exit(42);              /* reaching here means the cap failed to trigger */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "exceeding ASNIX_MAX_MEMORY aborts the process with exit code 1");
}

int main(void) {
    test_basic_alloc();
    test_alignment();
    test_chunk_boundary_crossing();
    test_large_single_allocation();
    test_memory_cap_abort();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_arena: all checks passed\n");
    return 0;
}
