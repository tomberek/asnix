#ifndef ASNIX_ENV_H
#define ASNIX_ENV_H

#define ASNIX_ENV_OFF_UP     0
#define ASNIX_ENV_OFF_VALUES 4

#define ASNIX_ENV_WITH_TAG   1

#define ASNIX_ENV_ARENA_SIZE 1073741824

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "asnix/value.h"

typedef struct __attribute__((packed)) Env {
    uint32_t up;
    Value values[1];
} Env;

extern void *asnix_env_arena_base;

Env *asnix_env_alloc(Env *up, int64_t nslots);
Value asnix_env_lookup(Env *env, int64_t depth, int64_t slot);

#endif

#endif

