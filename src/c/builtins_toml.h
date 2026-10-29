#ifndef ASNIX_BUILTINS_TOML_H
#define ASNIX_BUILTINS_TOML_H

#include <stddef.h>

#include "asnix/value.h"
#include "builtins_misc.h"

extern const AsnixBuiltinMiscDef ASNIX_TOML_BUILTINS[];
extern const size_t ASNIX_TOML_BUILTINS_COUNT;

Value asnix_make_fromTOML_primop(void);

#endif

