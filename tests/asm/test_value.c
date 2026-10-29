#include <stdint.h>
#include <stdio.h>
#include "asnix/value.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void test_int_roundtrip(void) {
    int64_t samples[] = {
        0, 1, -1, 42, -42, 1000000, -1000000,
        ASNIX_INT_MAX, ASNIX_INT_MIN, ASNIX_INT_MAX - 1, ASNIX_INT_MIN + 1,
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        int64_t n = samples[i];
        Value v = asnix_make_int(n);
        CHECK(asnix_tag_of(v) == ASNIX_TAG_INT, "encoded int carries TAG_INT");
        CHECK(asnix_int_value(v) == n, "int round-trip preserves value/sign");
    }
}

static void test_imm_roundtrip(void) {
    int subtags[] = { ASNIX_IMM_NULL, ASNIX_IMM_FALSE, ASNIX_IMM_TRUE, ASNIX_IMM_BLACKHOLE };
    for (size_t i = 0; i < sizeof(subtags) / sizeof(subtags[0]); i++) {
        Value v = asnix_make_imm(subtags[i]);
        CHECK(asnix_tag_of(v) == ASNIX_TAG_IMM, "imm carries TAG_IMM");
        CHECK(asnix_imm_subtag(v) == subtags[i], "imm subtag round-trip");
    }
}

static void test_tag_extraction(void) {
    for (int t = ASNIX_TAG_PTR; t <= ASNIX_TAG_PRIMOP; t++) {
        Value v = { .raw = ((uint64_t)0x1000 << ASNIX_TAG_BITS) | (uint64_t)t };
        CHECK(asnix_tag_of(v) == t, "tag_of recovers the synthesized tag");
        CHECK(asnix_is_thunk(v) == (t == ASNIX_TAG_THUNK), "is_thunk matches only TAG_THUNK");
    }
}

static void test_well_known_constants(void) {
    CHECK(asnix_tag_of(ASNIX_NULL) == ASNIX_TAG_IMM, "ASNIX_NULL is an IMM");
    CHECK(asnix_imm_subtag(ASNIX_NULL) == ASNIX_IMM_NULL, "ASNIX_NULL subtag");
    CHECK(!asnix_values_equal(ASNIX_TRUE, ASNIX_FALSE), "true != false");
    CHECK(asnix_values_equal(ASNIX_NULL, asnix_make_imm(ASNIX_IMM_NULL)), "values_equal basic case");
}

int main(void) {
    test_int_roundtrip();
    test_imm_roundtrip();
    test_tag_extraction();
    test_well_known_constants();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_value: all checks passed\n");
    return 0;
}

