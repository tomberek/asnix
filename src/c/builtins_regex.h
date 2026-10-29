#ifndef ASNIX_BUILTINS_REGEX_H
#define ASNIX_BUILTINS_REGEX_H

#include <stddef.h>

#include "builtins_misc.h"

extern const AsnixBuiltinMiscDef ASNIX_REGEX_BUILTINS[];
extern const size_t ASNIX_REGEX_BUILTINS_COUNT;

#include "asnix/value.h"
#include <stdint.h>

Value asnix_prim_match2_impl(const Value *args, int64_t argc);

#endif

