#ifndef ASNIX_BUILTINS_COMPARE_H
#define ASNIX_BUILTINS_COMPARE_H

#include <stddef.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * Compare/string-domain builtins added in this pass: lessThan,
 * splitVersion, compareVersions, replaceStrings, stringToCharacters,
 * toJSON. `splitVersion`'s component-splitting rule and
 * `compareVersions`'s component-ordering rule are ported line-for-line
 * from the real implementation (~/nix/src/libstore/names.cc's
 * nextComponent/componentsLT/compareVersions), the same "port from the
 * real source, don't guess" discipline this project already uses for
 * base32.c/store_path.c (see docs/architecture.md's M8 notes) -- these
 * two in particular have enough genuinely surprising edge cases (e.g.
 * `"1pre" < "1"`, `"1.0" > "1"`) that re-deriving them from a description
 * would be asking to get them wrong.
 */

extern const AsnixBuiltinMiscDef ASNIX_COMPARE_BUILTINS[];
extern const size_t ASNIX_COMPARE_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_COMPARE_H */
