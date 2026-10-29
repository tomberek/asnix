#ifndef ASNIX_IRPASS_H
#define ASNIX_IRPASS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "asnix/ir.h"

typedef int (*AsnixIrPass)(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap);

void asnix_irpass_register(const char *name, AsnixIrPass pass);

void asnix_irpass_clear(void);

int asnix_irpass_run(IrOp *ops, size_t *len, int64_t **pool, size_t *pool_len, size_t *pool_cap);

void asnix_irpass_remove(IrOp *ops, size_t *len, size_t start, size_t count);

#ifdef ASNIX_IRDUMP_LINE_TRACKING
void asnix_irpass_set_line_array(uint32_t *lines);
#endif

size_t asnix_irpass_pool_append(int64_t **pool, size_t *pool_len, size_t *pool_cap, int64_t v);

#define ASNIX_IRPASS_MAX_ITERS 64

size_t asnix_irpass_num_passes(void);
const char *asnix_irpass_name(size_t index);
uint64_t asnix_irpass_fire_count(size_t index);
void asnix_irpass_reset_stats(void);
void asnix_irpass_print_stats(FILE *out);

size_t asnix_irpass_num_firings(void);
const char *asnix_irpass_firing_pass_name(size_t i);
size_t asnix_irpass_firing_pass_index(size_t i);
size_t asnix_irpass_firing_op_index(size_t i);

void asnix_irpass_begin_program(void);
void asnix_irpass_mark_program(const void *prog);
int asnix_irpass_firing_belongs_to_program(size_t i, const void *prog);

#endif

