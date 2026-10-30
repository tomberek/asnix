#ifndef ASNIX_VALUE_UTIL_H
#define ASNIX_VALUE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/heap.h"
#include "asnix/value.h"

Value asnix_make_string_value(const char *s, size_t len);

Value asnix_make_path_value(const char *abs_path, size_t len);

#define ASNIX_PTR_BASE_MASK 0x0000FFFFFFFFFFF8ULL

static inline __attribute__((always_inline)) int asnix_value_is_path(Value v) {
    if ((v.raw & ASNIX_TAG_MASK) != ASNIX_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & ASNIX_PTR_BASE_MASK);
    return *(const int64_t *)(base + ASNIX_PATH_OFF_MARKER) == ASNIX_PATH_MARKER;
}

Value asnix_make_float_value(double d);

static inline __attribute__((always_inline)) int asnix_value_is_float(Value v) {
    if ((v.raw & ASNIX_TAG_MASK) != ASNIX_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & ASNIX_PTR_BASE_MASK);
    return *(const int64_t *)(base + ASNIX_FLOAT_OFF_MARKER) == ASNIX_FLOAT_MARKER;
}

double asnix_float_value(Value v);

Value asnix_make_cell_value(Value *target);

static inline __attribute__((always_inline)) int asnix_value_is_cell(Value v) {
    if ((v.raw & ASNIX_TAG_MASK) != ASNIX_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & ASNIX_PTR_BASE_MASK);
    /* M199: marker is now a 32-bit field (see heap.h) -- read it as
     * int32_t, not int64_t, or we'd read 4 bytes of the packed target
     * offset along with it. */
    return *(const int32_t *)(base + ASNIX_CELL_OFF_MARKER) == ASNIX_CELL_MARKER;
}

Value *asnix_cell_target(Value v);

Value asnix_resolve_cell_chain(Value v);

Value asnix_make_lazyapp1(Value fn, Value arg1);
Value asnix_make_lazyapp2(Value fn, Value arg1, Value arg2);

int asnix_value_is_lazyapp(Value v);

Value asnix_resolve_lazyapp(Value v);

void *asnix_thunkmap_make_desc(Value f, const void *src_entries);

Value asnix_thunkmap_pack(void *desc, int64_t idx);

void asnix_thunkmap_set_out(void *desc, Value out);

int asnix_value_is_thunkmap(Value v);

Value asnix_resolve_thunkmap(Value v);

Value *asnix_thunkmap_slot(Value v);

const char *asnix_ptr_bytes(Value v, int64_t *out_len);

Value asnix_coerce_to_string(Value v);

Value asnix_coerce_to_string_for_derivation(Value v);

Value asnix_coerce_to_string_for_concat_sep(Value v);

Value asnix_concat_strings(const Value *parts, int64_t count);

Value asnix_concat_path(const Value *parts, int64_t count);

Value asnix_add_values(Value lhs, Value rhs);

char *asnix_canon_path(const char *in);

Value asnix_sub_values(Value lhs, Value rhs);
Value asnix_mul_values(Value lhs, Value rhs);
Value asnix_div_values(Value lhs, Value rhs);

Value asnix_cmp_lt_values(Value lhs, Value rhs);
Value asnix_cmp_gt_values(Value lhs, Value rhs);
Value asnix_cmp_le_values(Value lhs, Value rhs);
Value asnix_cmp_ge_values(Value lhs, Value rhs);

Value asnix_concat_lists(Value lhs, Value rhs);

char *asnix_string_value_to_cstr(Value string_val);

#endif

