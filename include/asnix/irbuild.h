#ifndef ASNIX_TEST_IRBUILD_H
#define ASNIX_TEST_IRBUILD_H

#include <stddef.h>
#include <stdint.h>
#include "asnix/ir.h"

typedef struct {
    IrOp *ops;
    size_t len, cap;
    int64_t *pool;
    size_t pool_len, pool_cap;
#ifdef ASNIX_IRDUMP_LINE_TRACKING
    uint32_t *lines;
    size_t lines_cap;
#endif
} IrBuilder;

IrBuilder *ib_new(void);
size_t ib_len(IrBuilder *b);

#ifdef ASNIX_IRDUMP_LINE_TRACKING
void ib_enable_line_tracking(void);
void ib_set_current_src_pos(uint32_t pos);

const uint32_t *asnix_ib_lookup_program_lines(const IrProgram *prog);
#else
static inline void ib_enable_line_tracking(void) {}
static inline void ib_set_current_src_pos(uint32_t pos) { (void)pos; }
#endif

size_t ib_push_int(IrBuilder *b, int64_t v);
size_t ib_push_true(IrBuilder *b);
size_t ib_push_false(IrBuilder *b);
size_t ib_push_null(IrBuilder *b);
size_t ib_var_ref(IrBuilder *b, int64_t depth, int64_t slot);
size_t ib_var_ref_cell(IrBuilder *b, int64_t depth, int64_t slot);
size_t ib_var_ref_raw(IrBuilder *b, int64_t depth, int64_t slot);
size_t ib_add(IrBuilder *b);
size_t ib_sub(IrBuilder *b);
size_t ib_mul(IrBuilder *b);
size_t ib_if(IrBuilder *b);
size_t ib_jmp(IrBuilder *b);
void ib_patch_delta(IrBuilder *b, size_t site, size_t target_idx);
size_t ib_let(IrBuilder *b, int64_t nslots);
size_t ib_lambda(IrBuilder *b, int64_t arity, IrProgram *body_prog, int64_t formals_raw);
size_t ib_call(IrBuilder *b, int64_t argc);
size_t ib_ret(IrBuilder *b);
size_t ib_halt(IrBuilder *b);

size_t ib_push_const(IrBuilder *b, int64_t raw_value);
size_t ib_mk_list(IrBuilder *b, int64_t count);
size_t ib_mk_attrs(IrBuilder *b, int64_t count, const uint32_t *sorted_syms);
size_t ib_select(IrBuilder *b, uint32_t sym);

size_t ib_select_dynamic(IrBuilder *b);

size_t ib_mk_thunk(IrBuilder *b, IrProgram *body_prog);

size_t ib_mk_lazy_thunk_stub(IrBuilder *b, void *desc);

size_t ib_frame_alloc(IrBuilder *b, int64_t nslots);
size_t ib_frame_set(IrBuilder *b, int64_t slot);
size_t ib_frame_get(IrBuilder *b, int64_t slot);
size_t ib_frame_exit(IrBuilder *b);

size_t ib_rec_overrides_merge(IrBuilder *b, int64_t n, const uint32_t *syms, int64_t overrides_slot);

size_t ib_eq(IrBuilder *b);
size_t ib_neq(IrBuilder *b);
size_t ib_lt(IrBuilder *b);
size_t ib_gt(IrBuilder *b);
size_t ib_le(IrBuilder *b);
size_t ib_ge(IrBuilder *b);
size_t ib_not(IrBuilder *b);
size_t ib_update(IrBuilder *b);
size_t ib_concat(IrBuilder *b);
size_t ib_div(IrBuilder *b);
size_t ib_insert_dynamic_attr(IrBuilder *b);

size_t ib_select_dynamic_or(IrBuilder *b);
size_t ib_resolve_search_path(IrBuilder *b);
size_t ib_assert_fail(IrBuilder *b, const char *msg, size_t len);

size_t ib_str_concat(IrBuilder *b, int64_t count);

size_t ib_path_concat(IrBuilder *b, int64_t count);

size_t ib_with(IrBuilder *b);
size_t ib_with_try(IrBuilder *b, uint32_t sym);
size_t ib_with_miss(IrBuilder *b);
size_t ib_with_exit(IrBuilder *b);

size_t ib_has_attr(IrBuilder *b, uint32_t sym);

size_t ib_has_attr_dynamic(IrBuilder *b);

size_t ib_check_formals(IrBuilder *b, int64_t required_count, const uint32_t *required_syms_sorted,
                         int64_t total_count, const uint32_t *all_syms_sorted);

size_t ib_select_or(IrBuilder *b, uint32_t sym);

size_t ib_eq_if(IrBuilder *b);
size_t ib_neq_if(IrBuilder *b);
size_t ib_lt_if(IrBuilder *b);
size_t ib_ge_if(IrBuilder *b);
size_t ib_has_attr_if(IrBuilder *b, uint32_t sym);

IrProgram *ib_finish(IrBuilder *b);

#endif

