#ifndef ASNIX_ENV_H
#define ASNIX_ENV_H

/* Env{up; values[]}: a flat per-scope frame, chained via `up` for lexical
 * lookup (De Bruijn depth,slot resolved at compile time). One frame per
 * let/lambda-call, matching how cppnix/iets structure Env. */

#define ASNIX_ENV_OFF_UP     0
#define ASNIX_ENV_OFF_VALUES 8

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "asnix/value.h"

typedef struct Env {
    struct Env *up;
    Value values[1]; /* real length is nslots; always arena-allocated, never sizeof()'d */
} Env;

/* Implemented in src/asm/env.S. Provided as tested, C-callable primitives;
 * the hot dispatch handlers (M3 op_var_ref/op_let/op_call) inline the same
 * walk/bind logic directly rather than paying a `call` per use. */
Env *asnix_env_alloc(Env *up, int64_t nslots);
Value asnix_env_lookup(Env *env, int64_t depth, int64_t slot);

#endif /* __ASSEMBLER__ */

#endif /* ASNIX_ENV_H */
