#ifndef ASNIX_BUILTINS_ATTRS_H
#define ASNIX_BUILTINS_ATTRS_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"
#include "builtins_list.h"

extern const AsnixBuiltinDef ASNIX_ATTRS_BUILTINS[];
extern const size_t ASNIX_ATTRS_BUILTINS_COUNT;

Value asnix_make_removeAttrs_primop(void);

#endif

