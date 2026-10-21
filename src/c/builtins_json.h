#ifndef ASNIX_BUILTINS_JSON_H
#define ASNIX_BUILTINS_JSON_H

#include <stddef.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef {name, build()} shape */

/*
 * `builtins.fromJSON` -- M51. Own translation unit, same rationale as
 * builtins_toml.c's fromTOML parser: a large-enough, self-contained
 * feature (a real recursive-descent JSON parser) deserves its own file
 * rather than crowding an unrelated module. Unlike fromTOML, real Nix
 * does NOT expose this one as a bare root-scope global (confirmed
 * directly: `nix-instantiate --eval -E 'fromJSON "1"'` -> "undefined
 * variable 'fromJSON'", only `builtins.fromJSON` works) -- so there's no
 * `asnix_make_fromJSON_primop`-style bare wrapper here, unlike
 * builtins_toml.h's `asnix_make_fromTOML_primop`.
 */

extern const AsnixBuiltinMiscDef ASNIX_JSON_BUILTINS[];
extern const size_t ASNIX_JSON_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_JSON_H */
