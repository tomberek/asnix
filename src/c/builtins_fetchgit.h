#ifndef ASNIX_BUILTINS_FETCHGIT_H
#define ASNIX_BUILTINS_FETCHGIT_H

#include <stdint.h>

#include "asnix/value.h"

Value asnix_prim_fetchGit(const Value *args, int64_t argc);
Value asnix_make_fetchGit_primop(void);

#endif

