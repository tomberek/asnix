#ifndef ASNIX_DERIVATION_H
#define ASNIX_DERIVATION_H

#include <stddef.h>

#include "asnix/value.h"
#include "builtins_misc.h"

Value asnix_prim_derivation(const Value *args, int64_t argc);

void asnix_compute_fixed_output_path(const char *output_hash_mode, const uint8_t raw[64], size_t raw_len,
                                      const char *algo, size_t algo_len, const char *name, char *out,
                                      size_t out_cap);

Value asnix_make_derivation_strict_primop(void);

Value asnix_make_derivation_wrapper_value(void);

extern const AsnixBuiltinMiscDef ASNIX_DERIVATION_BUILTINS[];
extern const size_t ASNIX_DERIVATION_BUILTINS_COUNT;

#endif

