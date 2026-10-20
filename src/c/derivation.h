#ifndef ASNIX_DERIVATION_H
#define ASNIX_DERIVATION_H

#include "asnix/value.h"

/*
 * `derivation { ... }` -- M8's minimal slice (see docs/architecture.md):
 * a single fixed output named "out", zero input derivations, zero input
 * sources. Required attrs: `name`, `system`, `builder`. Optional: `args`
 * (list of strings). Every other attr becomes an environment variable
 * (coerced to a string via `asnix_coerce_to_string`, i.e. the same rules
 * as `builtins.toString`).
 *
 * Returns `{ outPath; drvPath; type = "derivation"; out = outPath; }` --
 * deliberately does NOT merge the original input attrs back into the
 * result (real Nix's `derivation` does, via a Nix-level wrapper around
 * the `derivationStrict` primop that uses `//`, which this project
 * doesn't have yet) or track string context (so a derivation's outPath
 * can't yet be used as another derivation's input -- that needs the
 * recursive `hashDerivationModulo` masking real Nix does for input
 * derivations, out of scope here along with `//`/`rec`/multiple outputs/
 * fixed-output (content-addressed) derivations).
 */
Value asnix_prim_derivation(const Value *args, int64_t argc);

#endif /* ASNIX_DERIVATION_H */
