#ifndef ASNIX_BUILTINS_M23_H
#define ASNIX_BUILTINS_M23_H

#include <stddef.h>
#include <stdint.h>

#include "builtins_misc.h"

extern const AsnixBuiltinMiscDef ASNIX_M23_BUILTINS[];
extern const size_t ASNIX_M23_BUILTINS_COUNT;

Value asnix_prim_pathExists(const Value *args, int64_t argc);
Value asnix_prim_readFile(const Value *args, int64_t argc);
Value asnix_prim_readFileType(const Value *args, int64_t argc);
Value asnix_prim_readDir(const Value *args, int64_t argc);
Value asnix_prim_toPath(const Value *args, int64_t argc);
Value asnix_prim_getEnv(const Value *args, int64_t argc);
Value asnix_prim_parseDrvName(const Value *args, int64_t argc);

Value asnix_prim_intersectAttrs2_impl(const Value *args, int64_t argc);

#endif

