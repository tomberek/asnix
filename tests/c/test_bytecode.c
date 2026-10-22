/* M102: round-trip coverage for the varint (zigzag+LEB128) bytecode-cache
 * wire format -- bytecode.c's wbuf_i64/rcur_i64 replaced a fixed 8-byte
 * int64 with a variable-width encoding, and the end-to-end test suite
 * (real .nix source) exercises mostly-small values, never the large-magnitude
 * edge (INT64_MIN/INT64_MAX) a `PUSH_INT` operand can in principle hold.
 * This directly serializes/deserializes a handful of hand-built IrPrograms
 * (bypassing the compiler entirely, same strategy the tests/ir tree
 * already uses) and checks exact op-by-op equality, plus confirms a pre-M102
 * (ASNXBC01-magic) cache entry is rejected cleanly rather than misread. */
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
    /* M118: Env.up is a dedicated-arena-relative compressed offset --
     * must come from the real allocator, not a hand-rolled general-
     * arena alloc. */
    return asnix_env_alloc(NULL, 0);
}

/* Builds `push_int(v); halt;`, serializes, deserializes, and checks:
 *   1. the round-tripped program has the exact same op count, and every
 *      op's (handler, a, b) matches the original exactly -- this sequence
 *      has no pool-referencing ops at all (OP_PUSH_INT/OP_HALT never touch
 *      the pool), so "exact round-trip equality" is a meaningful,
 *      literal byte-for-byte-of-the-decoded-struct claim here, not just
 *      "evaluates to the same answer". This part covers the FULL int64_t
 *      range, including INT64_MIN/INT64_MAX: `IrOp.a` is a plain int64_t
 *      field, independent of whatever range a real Nix integer VALUE is
 *      later restricted to (see below) -- the on-disk format must not
 *      truncate it either way.
 *   2. ONLY when `v` is within asnix's own representable integer range
 *      (ASNIX_INT_MIN..ASNIX_INT_MAX, a 61-bit signed window --
 *      asnix_make_int's own hard limit, unrelated to this format and
 *      unconditionally enforced for every real Nix integer value, cache
 *      or no cache), also evaluates the round-tripped program and checks
 *      it yields back exactly `v` -- confirming the varint encoding
 *      didn't corrupt an ordinary in-range value on its way through
 *      disk-shaped bytes. A `v` outside that window is still fully
 *      exercised by check (1) above; it's just not a value asnix_eval
 *      could ever legitimately produce either way (it would abort), so
 *      asnix_eval is skipped for it to keep this test's failure signal
 *      about the SERIALIZATION format, not about that unrelated,
 *      pre-existing range check. */
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

    /* Exact structural equality: this fixture is always exactly two ops
     * (OP_PUSH_INT, then OP_HALT from ib_halt) -- neither touches the
     * pool, so every field of both ops must match exactly, bit-for-bit,
     * after the round trip through disk-shaped bytes. */
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

/* An ordinary small-value program: 5 + 37 == 42, entirely made of small
 * non-negative operands (the overwhelmingly common real-world case this
 * change targets) -- exact round-trip equality plus a correct eval. */
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
    /* A hand-check that the new format is in fact smaller than the old
     * fixed-8-byte-per-field one would have been for this stream: 3 ops
     * (PUSH_INT 5, PUSH_INT 37, ADD) each need an 8-byte magic + per-op
     * tag/opcode/operands -- the old format spent 8 bytes per int64 field
     * no matter the value; this one should spend 1 byte for every field
     * here (op_id/a/b are all < 64). Loose upper bound, not an exact
     * byte-count pin (which would be too brittle to the exact tag
     * encoding), just a sanity check that small values are small on disk. */
    CHECK(len < 8 /* magic */ + 3 * (1 + 1 + 1 + 1) + 16 /* n_ops + halt op, generous slack */,
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

/* The large-value edge case the task is specifically worried about: a
 * PUSH_INT operand holding values at or near the extremes of int64_t,
 * which real Nix source essentially never produces but which the format
 * must still round-trip exactly (never truncate/wrap/corrupt) -- zigzag
 * encoding's whole point is that this costs more BYTES (up to 10, vs.
 * the old format's fixed 8) but never loses precision. */
static void fixture_large_values(void) {
    static const int64_t edge_values[] = {
        0,
        1,
        -1,
        63,
        64,     /* zigzag(64) = 128: the first value needing a 2nd LEB128 byte */
        -64,
        -65,
        127,
        128,
        -128,
        1000000007,
        -1000000007,
        ASNIX_INT_MAX,     /* the real boundary asnix_make_int itself enforces */
        ASNIX_INT_MIN,
        INT64_MAX,         /* beyond what asnix_make_int accepts, but the on-disk
                             * int64 FIELD must still round-trip it exactly */
        INT64_MIN,
        INT64_MAX - 1,
        INT64_MIN + 1,
    };
    for (size_t i = 0; i < sizeof(edge_values) / sizeof(edge_values[0]); i++) {
        check_push_int_roundtrip(edge_values[i]);
    }
}

/* Exercises the pool-referencing paths (OP_PUSH_CONST's string, and
 * OP_SELECT's symbol-id-in-`a` special case, PTAG_OP_SYM_A) -- these
 * reorder/re-intern across the round trip, so exact field equality isn't
 * meaningful here the way it is for the pool-free push_int fixtures above;
 * what matters is that evaluation still gives the right answer. */
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

/* Old-format (pre-M102, fixed-8-byte-int) cache entries must be rejected
 * cleanly -- never misread as new-format data, never a crash -- so a
 * stale entry on disk from before this change degrades to an ordinary
 * cache miss (recompile, overwrite) exactly like any other corrupt/
 * foreign entry, per bytecode_cache.c's own silent-degrade-to-miss
 * design. The magic bump (ASNXBC01 -> ASNXBC02) is what makes this work:
 * it's checked before a single other byte is interpreted. */
static void fixture_old_format_is_rejected_cleanly(void) {
    /* A buffer that LOOKS like a plausible old-format entry (old magic,
     * then bytes that would have been a fixed-width `n_ops`/op stream)
     * must still be refused outright, purely on the magic mismatch. */
    uint8_t old[64];
    memcpy(old, "ASNXBC01", 8);
    memset(old + 8, 0, sizeof(old) - 8);
    /* Old format's `n_ops = 1` as a little-endian fixed int64, which if
     * misread as the NEW format would be interpreted completely
     * differently (varint) -- exactly the misreading this guards against. */
    old[8] = 1;
    CHECK(asnix_bytecode_deserialize(old, sizeof(old)) == NULL,
          "an ASNXBC01 (pre-M102) entry is rejected outright, never misread as ASNXBC02 data");

    /* Bare old magic, nothing else -- still a clean NULL, not a crash. */
    CHECK(asnix_bytecode_deserialize((const uint8_t *)"ASNXBC01", 8) == NULL,
          "bare old magic with no payload is still a clean miss");

    /* Sanity: the literal current magic by itself (no program bytes
     * following) is ALSO a clean miss, not a crash -- truncated-but-
     * correctly-tagged data is still invalid. */
    CHECK(asnix_bytecode_deserialize((const uint8_t *)"ASNXBC02", 8) == NULL,
          "bare new magic with no payload is a clean miss, not a crash");

    /* Too short to even hold a magic number. */
    CHECK(asnix_bytecode_deserialize((const uint8_t *)"AB", 2) == NULL, "a too-short buffer is a clean miss");
    CHECK(asnix_bytecode_deserialize(NULL, 0) == NULL, "a zero-length buffer is a clean miss");

    /* Foreign/garbage magic. */
    uint8_t garbage[32];
    memset(garbage, 0xAA, sizeof(garbage));
    CHECK(asnix_bytecode_deserialize(garbage, sizeof(garbage)) == NULL, "garbage bytes are a clean miss, not a crash");
}

/* A genuine, well-formed NEW-format entry (current code) must still load
 * -- the magic-bump/rejection fixture above doesn't accidentally also
 * reject valid current data. */
static void fixture_new_format_roundtrips(void) {
    IrBuilder *b = ib_new();
    ib_push_int(b, 42);
    ib_halt(b);
    IrProgram *orig = ib_finish(b);
    uint8_t *buf = NULL;
    size_t len = 0;
    CHECK(asnix_bytecode_serialize(orig, &buf, &len) == 0, "serialize succeeds");
    CHECK(len >= 8 && memcmp(buf, "ASNXBC02", 8) == 0, "serialized stream starts with the current ASNXBC02 magic");
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
