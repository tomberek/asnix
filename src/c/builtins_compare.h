#ifndef ASNIX_BUILTINS_COMPARE_H
#define ASNIX_BUILTINS_COMPARE_H

#include <stddef.h>

#include "builtins_misc.h"

extern const AsnixBuiltinMiscDef ASNIX_COMPARE_BUILTINS[];
extern const size_t ASNIX_COMPARE_BUILTINS_COUNT;

char *asnix_toJSON_cstr(Value v);

#endif

