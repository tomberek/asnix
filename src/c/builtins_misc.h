#ifndef ASNIX_BUILTINS_MISC_H
#define ASNIX_BUILTINS_MISC_H

#include <stddef.h>

#include "asnix/value.h"

typedef Value (*AsnixBuiltinMiscBuilder)(void);

typedef struct {
    const char *name;
    AsnixBuiltinMiscBuilder build;
} AsnixBuiltinMiscDef;

extern const AsnixBuiltinMiscDef ASNIX_BUILTINS_MISC[];
extern const size_t ASNIX_BUILTINS_MISC_COUNT;

Value asnix_make_isNull_primop(void);

Value asnix_make_baseNameOf_primop(void);
Value asnix_make_dirOf_primop(void);
Value asnix_make_placeholder_primop(void);

#endif

