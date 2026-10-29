#ifndef ASNIX_VALUE_H
#define ASNIX_VALUE_H

#define ASNIX_TAG_MASK   0x7
#define ASNIX_TAG_BITS   3

#define ASNIX_TAG_PTR      0x0
#define ASNIX_TAG_INT      0x1
#define ASNIX_TAG_IMM      0x2
#define ASNIX_TAG_ATTRS    0x3
#define ASNIX_TAG_LIST     0x4
#define ASNIX_TAG_CLOSURE  0x5
#define ASNIX_TAG_THUNK    0x6
#define ASNIX_TAG_PRIMOP   0x7

#define ASNIX_IMM_NULL      0
#define ASNIX_IMM_FALSE     1
#define ASNIX_IMM_TRUE      2
#define ASNIX_IMM_BLACKHOLE 3

#define ASNIX_INT_MAX 1152921504606846975
#define ASNIX_INT_MIN (-1152921504606846976)

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct {
    uint64_t raw;
} Value;

int asnix_tag_of(Value v);
Value asnix_make_int(int64_t n);
int64_t asnix_int_value(Value v);
Value asnix_make_imm(int subtag);
int asnix_imm_subtag(Value v);
int asnix_is_thunk(Value v);

static inline int asnix_values_equal(Value a, Value b) {
    return a.raw == b.raw;
}

#define ASNIX_NULL      (asnix_make_imm(ASNIX_IMM_NULL))
#define ASNIX_TRUE      (asnix_make_imm(ASNIX_IMM_TRUE))
#define ASNIX_FALSE     (asnix_make_imm(ASNIX_IMM_FALSE))
#define ASNIX_BLACKHOLE (asnix_make_imm(ASNIX_IMM_BLACKHOLE))

#endif

#endif

