#ifndef ASNIX_FORCE_SCRATCH_H
#define ASNIX_FORCE_SCRATCH_H

#define ASNIX_FORCE_VALSTACK_BYTES 262144
#define ASNIX_FORCE_RETSTACK_BYTES 65536

#ifndef __ASSEMBLER__

#include <stdint.h>

void *asnix_force_scratch_valstack(int64_t depth);
void *asnix_force_scratch_retstack(int64_t depth);

void *asnix_apply_scratch_valstack(int64_t depth);
void *asnix_apply_scratch_retstack(int64_t depth);

#endif

#define ASNIX_EVAL_VALSTACK_BYTES 1048576
#define ASNIX_EVAL_RETSTACK_BYTES 1048576

#ifndef __ASSEMBLER__
void *asnix_eval_scratch_valstack(int64_t depth);
void *asnix_eval_scratch_retstack(int64_t depth);
#endif

#endif

