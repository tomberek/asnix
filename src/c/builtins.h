#ifndef ASNIX_BUILTINS_H
#define ASNIX_BUILTINS_H

#include "asnix/value.h"

/* Builds the `builtins` attrset (sorted by symbol id, matching OP_SELECT's
 * expectations) containing every native primop this milestone ships. The
 * compiler binds this as the single root-level scope slot (see
 * compiler.c/main.c) so user programs reference it like any other
 * variable: `builtins.toString 5`. */
Value asnix_make_builtins_attrs(void);

/* `import` is one of real Nix's bare-global names, exposed in addition
 * to `builtins.import`. The actual rule (read directly from cppnix's
 * source, `EvalState::addPrimOp`, `~/nix/src/libexpr/eval.cc`, after
 * M18's original piecemeal-testing-against-nix-instantiate approach
 * turned out to have missed names -- see M27's scope notes,
 * docs/architecture.md): *every* primop registered without a `__`
 * prefix is automatically exposed bare under that exact name, not a
 * small hand-picked list. Grepping every `RegisterPrimOp`/`.name = ...`
 * site for a non-`__`-prefixed name gives the complete set: `import
 * abort throw map removeAttrs isNull toString baseNameOf dirOf
 * fetchTarball fetchGit fetchMercurial fetchTree fetchFinalTree fromTOML
 * placeholder scopedImport derivationStrict break` -- plus `derivation`/
 * `builtins`/`true`/`false`/`null`/`__curPos`, which aren't ordinary
 * `addPrimOp`-registered primops and are handled separately. asnix
 * currently exposes `import`/`map`/`abort`/`throw`/`removeAttrs`/
 * `isNull`/`toString`/`derivation`/`baseNameOf`/`dirOf`/`fromTOML` bare
 * (see eval_file.c's root Env); the fetch-family (needs real network
 * I/O), `scopedImport`/`derivationStrict` (moderate undertakings), and
 * `break`/`placeholder` (not yet hit by anything evaluated) remain
 * documented, deliberate follow-ups, not attempted speculatively.
 * Returns a fresh PrimOp Value wrapping the exact same native function
 * `builtins.import` uses -- same behavior, just reachable two ways. */
Value asnix_make_import_primop(void);

/* Same idea, for `abort` -- also one of real Nix's bare-global names. */
Value asnix_make_abort_primop(void);

/* Same idea, for `throw` -- also one of real Nix's bare-global names. */
Value asnix_make_throw_primop(void);

/* Same idea, for `toString` -- also one of real Nix's bare-global names. */
Value asnix_make_toString_primop(void);

/* Same idea, for `derivation` -- also one of real Nix's bare-global
 * names (M23: found needed by numtide/nix-conformance's
 * eval-okay-drv-fixed-output.nix/eval-okay-drv-structured-attrs.nix,
 * both of which call bare `derivation {...}`, not
 * `builtins.derivation {...}`). */
Value asnix_make_derivation_primop(void);

#endif /* ASNIX_BUILTINS_H */
