#ifndef ASNIX_BUILTINS_FLAKEREF_H
#define ASNIX_BUILTINS_FLAKEREF_H

#include <stddef.h>

#include "asnix/value.h"

typedef Value (*AsnixBuiltinFlakerefBuilder)(void);

typedef struct {
    const char *name;
    AsnixBuiltinFlakerefBuilder build;
} AsnixBuiltinFlakerefDef;

extern const AsnixBuiltinFlakerefDef ASNIX_FLAKEREF_BUILTINS[];
extern const size_t ASNIX_FLAKEREF_BUILTINS_COUNT;

#endif

