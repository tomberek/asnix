#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/env.h"
#include "asnix/irbuild.h"
#include "asnix/value.h"
#include "bytecode.h"
#include "value_util.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static Env *root_env(void) {
    return asnix_env_alloc(NULL, 0);
}

static void check_push_int_roundtrip(int64_t v) {
    IrBuilder *b = ib_new();
    ib_push_int(b, v);
    ib_halt(b);
    IrProgram *orig = ib_finish(b);

    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = asnix_bytecode_serialize(orig, &buf, &len);
    CHECK(rc == 0, "serialize succeeds for a plain push_int/halt program");
    if (rc != 0) return;

    IrProgram *rt = asnix_bytecode_deserialize(buf, len);
    CHECK(rt != NULL, "deserialize succeeds on what was just serialized");
    if (!rt) {
        free(buf);
        return;
    }

    for (size_t n = 0; n < 2; n++) {
        CHECK(orig->ops[n].handler == rt->ops[n].handler, "op handler matches after round-trip");
        CHECK(orig->ops[n].a == rt->ops[n].a, "op operand `a` matches after round-trip");
        CHECK(orig->ops[n].b == rt->ops[n].b, "op operand `b` matches after round-trip");
    }

    if (v >= ASNIX_INT_MIN && v <= ASNIX_INT_MAX) {
        Value result = asnix_eval(rt, root_env());
        CHECK(asnix_tag_of(result) == ASNIX_TAG_INT, "round-tripped push_int program still yields an INT");
        CHECK(asnix_int_value(result) == v, "round-tripped push_int program evaluates to the exact original value");
    }

    free(buf);
}

static void fixture_small_values(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 5);
    ib_push_int(b, 37);
    ib_add(b);
    ib_halt(b);
    IrProgram *orig = ib_finish(b);

    uint8_t *buf = NULL;
    size_t len = 0;
    CHECK(asnix_bytecode_serialize(orig, &buf, &len) == 0, "serialize succeeds (small-value program)");
    CHECK(len < 8  + 3 * (1 + 1 + 1 + 1) + 16 ,
          "small-value program's serialized size reflects variable-width encoding, not fixed 8-byte fields");

    IrProgram *rt = asnix_bytecode_deserialize(buf, len);
    CHECK(rt != NULL, "deserialize succeeds (small-value program)");
    if (rt) {
        Value result = asnix_eval(rt, root_env());
        CHECK(asnix_tag_of(result) == ASNIX_TAG_INT, "small-value program yields an INT");
        CHECK(asnix_int_value(result) == 42, "5 + 37 == 42 after a round trip through the cache format");
    }
    free(buf);
}

static void fixture_large_values(void) {
    static const int64_t edge_values[] = {
        0,
        1,
        -1,
        63,
        64,
        -64,
        -65,
        127,
        128,
        -128,
        1000000007,
        -1000000007,
        ASNIX_INT_MAX,
        ASNIX_INT_MIN,
        INT64_MAX,
        INT64_MIN,
        INT64_MAX - 1,
        INT64_MIN + 1,
    };
    for (size_t i = 0; i < sizeof(edge_values) / sizeof(edge_values[0]); i++) {
        check_push_int_roundtrip(edge_values[i]);
    }
}

static void fixture_string_and_select(void) {
    IrBuilder *b = ib_new();
    Value s = asnix_make_string_value("hello world, this is a longer-than-one-byte-length string", 59);
    int64_t idx = ib_push_const(b, (int64_t)s.raw);
    (void)idx;
    ib_halt(b);
    IrProgram *orig = ib_finish(b);

    uint8_t *buf = NULL;
    size_t len = 0;
    CHECK(asnix_bytecode_serialize(orig, &buf, &len) == 0, "serialize succeeds (string-const program)");
    IrProgram *rt = asnix_bytecode_deserialize(buf, len);
    CHECK(rt != NULL, "deserialize succeeds (string-const program)");
    if (rt) {
        Value result = asnix_eval(rt, root_env());
        CHECK(asnix_tag_of(result) == ASNIX_TAG_PTR, "round-tripped string constant is still a heap PTR value");
        int64_t rlen;
        const char *bytes = asnix_ptr_bytes(result, &rlen);
        CHECK(rlen == 59 && memcmp(bytes, "hello world, this is a longer-than-one-byte-length string", 59) == 0,
              "round-tripped string constant has the exact original content (length varint round-trips too)");
    }
    free(buf);
}

static void fixture_old_format_is_rejected_cleanly(void) {
    uint8_t old[64];
    memcpy(old, "ASNXBC01", 8);
    memset(old + 8, 0, sizeof(old) - 8);
    old[8] = 1;
    CHECK(asnix_bytecode_deserialize(old, sizeof(old)) == NULL,
          "an ASNXBC01 (pre-M102) entry is rejected outright, never misread as ASNXBC03 data");

    CHECK(asnix_bytecode_deserialize((const uint8_t *)"ASNXBC01", 8) == NULL,
          "bare ASNXBC01 magic with no payload is still a clean miss");

    CHECK(asnix_bytecode_deserialize((const uint8_t *)"ASNXBC02", 8) == NULL,
          "bare ASNXBC02 magic with no payload is still a clean miss (now a stale pre-M169 format too)");

    CHECK(asnix_bytecode_deserialize((const uint8_t *)"ASNXBC03", 8) == NULL,
          "bare current magic with no payload is a clean miss, not a crash");

    CHECK(asnix_bytecode_deserialize((const uint8_t *)"AB", 2) == NULL, "a too-short buffer is a clean miss");
    CHECK(asnix_bytecode_deserialize(NULL, 0) == NULL, "a zero-length buffer is a clean miss");

    uint8_t garbage[32];
    memset(garbage, 0xAA, sizeof(garbage));
    CHECK(asnix_bytecode_deserialize(garbage, sizeof(garbage)) == NULL, "garbage bytes are a clean miss, not a crash");
}

static void fixture_new_format_roundtrips(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 42);
    ib_halt(b);
    IrProgram *orig = ib_finish(b);
    uint8_t *buf = NULL;
    size_t len = 0;
    CHECK(asnix_bytecode_serialize(orig, &buf, &len) == 0, "serialize succeeds");
    CHECK(len >= 8 && memcmp(buf, "ASNXBC03", 8) == 0, "serialized stream starts with the current ASNXBC03 magic");
    IrProgram *rt = asnix_bytecode_deserialize(buf, len);
    CHECK(rt != NULL, "a genuine current-format entry still deserializes");
    free(buf);
}

int main(void) {
    asnix_arena_init(0);
    fixture_small_values();
    fixture_large_values();
    fixture_string_and_select();
    fixture_old_format_is_rejected_cleanly();
    fixture_new_format_roundtrips();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_bytecode: all checks passed\n");
    return 0;
}

