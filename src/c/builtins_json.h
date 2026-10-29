#ifndef ASNIX_BUILTINS_JSON_H
#define ASNIX_BUILTINS_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "builtins_misc.h"

extern const AsnixBuiltinMiscDef ASNIX_JSON_BUILTINS[];
extern const size_t ASNIX_JSON_BUILTINS_COUNT;

Value asnix_json_parse_structured_attrs(const char *bytes, int64_t len);

#endif

