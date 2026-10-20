#ifndef ASNIX_BUILTINS_MISC_H
#define ASNIX_BUILTINS_MISC_H

#include <stddef.h>

#include "asnix/value.h"

/* "Misc" builtins (string ops + type predicates), kept in their own
 * translation unit so this agent's work doesn't textually collide with
 * sibling agents also extending builtins.c's BUILTINS[] table -- a later
 * integration step merges ASNIX_BUILTINS_MISC into the shared table (see
 * builtins.c's asnix_make_builtins_attrs).
 *
 * Each entry's `build` is called once, AFTER asnix_arena_init() has run
 * (mirrors asnix_make_builtins_attrs's own lazy-build-at-use pattern in
 * builtins.c), and returns the fully-tagged Value to insert into the
 * `builtins` attrset under `name`. Most entries build a plain arity-N
 * PrimOp the usual way; the two genuinely multi-argument ones
 * (`substring`, `concatStringsSep`) build a short chain of real Closures
 * instead of a PrimOp -- see the long comment above make_curried_native()
 * in builtins_misc.c for why a PrimOp-only curry (hashString's
 * finite-enum trick) can't generalize to arbitrary-domain arguments like
 * an integer start/length or an arbitrary separator string. */
typedef Value (*AsnixBuiltinMiscBuilder)(void);

typedef struct {
    const char *name;
    AsnixBuiltinMiscBuilder build;
} AsnixBuiltinMiscDef;

extern const AsnixBuiltinMiscDef ASNIX_BUILTINS_MISC[];
extern const size_t ASNIX_BUILTINS_MISC_COUNT;

#endif /* ASNIX_BUILTINS_MISC_H */
