#ifndef ASNIX_ENV_H
#define ASNIX_ENV_H

/* Env{up; values[]}: a flat per-scope frame, chained via `up` for lexical
 * lookup (De Bruijn depth,slot resolved at compile time). One frame per
 * let/lambda-call, matching how cppnix/iets structure Env. */

#define ASNIX_ENV_OFF_UP     0
#define ASNIX_ENV_OFF_VALUES 8

/* `with`: a with-frame is an ordinary 1-slot Env{up;values[0]=attrs} with
 * one difference -- its OWN `up` field has this tag bit OR'd in (arena
 * pointers are always 8-byte aligned, exactly the same free-low-bits trick
 * `value.h` uses for Value tags), marking "I am a with-frame" so
 * OP_WITH_TRY's fallback search can recognize with-frames while walking
 * the Env chain without needing a separate parallel structure. Every
 * walk that blindly chases `up` across a *generic* hop (not specifically
 * looking for a with-frame -- e.g. OP_VAR_REF's depth walk, which may pass
 * straight through a with-frame on its way to an outer lexical binding)
 * must mask this bit back off before using the pointer, since nothing
 * else ever sets it. */
#define ASNIX_ENV_WITH_TAG   1

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
