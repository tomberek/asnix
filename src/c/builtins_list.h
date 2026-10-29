#ifndef ASNIX_BUILTINS_LIST_H
#define ASNIX_BUILTINS_LIST_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"

typedef Value (*AsnixPrimFn)(const Value *args, int64_t argc);

typedef struct {
    const char *name;
    AsnixPrimFn fn;
    int64_t arity;
} AsnixBuiltinDef;

extern const AsnixBuiltinDef asnix_builtins_list[];
extern const size_t asnix_builtins_list_count;

Value asnix_make_map_primop(void);

#endif

