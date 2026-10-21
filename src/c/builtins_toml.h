#ifndef ASNIX_BUILTINS_TOML_H
#define ASNIX_BUILTINS_TOML_H

#include <stddef.h>

#include "asnix/value.h"
#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * `builtins.fromTOML` -- M23. Own translation unit (not folded into an
 * existing builtins_*.c) for the same reason builtins_closure.c is one:
 * a large-enough, self-contained feature (the real parser lives in
 * toml.c/toml.h) deserves its own file rather than crowding an unrelated
 * module. Uses the {name, build()} shape (builtins_misc.h) like
 * builtins_closure.c's genericClosure -- `fromTOML` is a true arity-1
 * native function (one string argument), so either shape would fit, but
 * this keeps the convention uniform with every other single-entry
 * module added this way.
 */

extern const AsnixBuiltinMiscDef ASNIX_TOML_BUILTINS[];
extern const size_t ASNIX_TOML_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_TOML_H */
