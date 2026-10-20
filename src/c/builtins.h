#ifndef ASNIX_BUILTINS_H
#define ASNIX_BUILTINS_H

#include "asnix/value.h"

/* Builds the `builtins` attrset (sorted by symbol id, matching OP_SELECT's
 * expectations) containing every native primop this milestone ships. The
 * compiler binds this as the single root-level scope slot (see
 * compiler.c/main.c) so user programs reference it like any other
 * variable: `builtins.toString 5`. */
Value asnix_make_builtins_attrs(void);

/* `import` is one of a small, fixed set of names real Nix exposes as a
 * bare global in addition to `builtins.import` (confirmed directly
 * against `nix-instantiate`: `import`/`map`/`abort`/`throw`/`removeAttrs`/
 * `isNull`/`toString`/`baseNameOf`/`dirOf`/`fetchTarball`/`derivation`/
 * `builtins`/`__curPos` all resolve bare there, while e.g. `filter`/
 * `foldl`/`typeOf`/`length`/`hashString`/`genericClosure` do not -- a
 * small curated list, not "every builtin"). asnix exposes only `import`
 * bare for now (the one reported missing); the rest of that list is a
 * known, documented follow-up, not attempted here. Returns a fresh PrimOp
 * Value wrapping the exact same native function `builtins.import` uses --
 * same behavior, just reachable two ways. */
Value asnix_make_import_primop(void);

/* Same idea, for `abort` -- also one of real Nix's bare-global names. */
Value asnix_make_abort_primop(void);

/* Same idea, for `toString` -- also one of real Nix's bare-global names. */
Value asnix_make_toString_primop(void);

#endif /* ASNIX_BUILTINS_H */
