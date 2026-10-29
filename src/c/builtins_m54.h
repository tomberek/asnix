#ifndef ASNIX_BUILTINS_M54_H
#define ASNIX_BUILTINS_M54_H

#include <stddef.h>

#include "asnix/value.h"

typedef Value (*AsnixBuiltinM54Builder)(void);

typedef struct {
    const char *name;
    AsnixBuiltinM54Builder build;
} AsnixBuiltinM54Def;

extern const AsnixBuiltinM54Def ASNIX_M54_BUILTINS[];
extern const size_t ASNIX_M54_BUILTINS_COUNT;

Value asnix_make_scopedImport_primop(void);

#endif

