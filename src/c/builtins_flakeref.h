#ifndef ASNIX_BUILTINS_FLAKEREF_H
#define ASNIX_BUILTINS_FLAKEREF_H

#include <stddef.h>

#include "asnix/value.h"

/*
 * M61: `builtins.parseFlakeRef` / `builtins.flakeRefToString` -- pure
 * string<->attrset parsing/formatting of the flake-ref URL-like syntax
 * (`github:owner/repo/ref?dir=...`, `git+https://...`, `path:/...`, bare
 * flake ids like `nixpkgs`, tarball/file URLs, ...). No filesystem/network
 * access, no actual flake resolution/fetching/registry lookup -- see
 * docs/architecture.md's M61 scope notes for the full spec derivation
 * (reverse-engineered from numtide/nix-conformance's
 * eval-okay-*flake-ref*.nix/.exp fixtures, cross-checked directly against
 * real `nix-instantiate --eval` for every ambiguous case) and the list of
 * deliberate scope cuts. Kept in its own translation unit, same
 * "no textual collision with a sibling agent's own builtins module"
 * rationale as every prior builtins-expansion pass (see builtins_m54.h).
 */
typedef Value (*AsnixBuiltinFlakerefBuilder)(void);

typedef struct {
    const char *name;
    AsnixBuiltinFlakerefBuilder build;
} AsnixBuiltinFlakerefDef;

extern const AsnixBuiltinFlakerefDef ASNIX_FLAKEREF_BUILTINS[];
extern const size_t ASNIX_FLAKEREF_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_FLAKEREF_H */
