#ifndef ASNIX_BUILTINS_LIST2_H
#define ASNIX_BUILTINS_LIST2_H

#include <stddef.h>

#include "builtins_misc.h"

extern const AsnixBuiltinMiscDef ASNIX_LIST2_BUILTINS[];
extern const size_t ASNIX_LIST2_BUILTINS_COUNT;

#include "asnix/value.h"
#include <stdint.h>

Value asnix_prim_elem2_impl(const Value *args, int64_t argc);
Value asnix_prim_any2_impl(const Value *args, int64_t argc);
Value asnix_prim_all2_impl(const Value *args, int64_t argc);

#endif

