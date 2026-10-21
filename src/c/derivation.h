#ifndef ASNIX_DERIVATION_H
#define ASNIX_DERIVATION_H

#include "asnix/value.h"
#include "builtins_misc.h" /* AsnixBuiltinMiscDef -- reused for ASNIX_DERIVATION_BUILTINS below,
                             * same shared-shape precedent builtins_attrs.h already set for
                             * builtins_list.h's AsnixBuiltinDef. */

/*
 * `derivationStrict { ... }` (M8's minimal slice, see
 * docs/architecture.md): a single fixed output named "out", zero input
 * derivations, zero input sources. Required attrs: `name`, `system`,
 * `builder`. Optional: `args` (list of strings). Every other attr
 * becomes an environment variable (coerced to a string via
 * `asnix_coerce_to_string`, i.e. the same rules as `builtins.toString`).
 *
 * Returns `{ outPath; drvPath; type = "derivation"; out = outPath; }` --
 * does NOT merge the original input attrs back into the result or build
 * the self-referential multi-output object (`drv.out.outPath`, etc.) --
 * that's exactly what real Nix's OWN `derivation` does on top of this
 * primop, via a Nix-level wrapper (`~/nix/src/libexpr/primops/
 * derivation.nix`) that this project now ports verbatim (M58, see
 * asnix_make_derivation_wrapper_value below) rather than reimplementing
 * in C. Does not track string context (so a derivation's outPath can't
 * yet be used as another derivation's input -- that needs the recursive
 * `hashDerivationModulo` masking real Nix does for input derivations,
 * out of scope here along with `rec`/multiple real outputs/fixed-output
 * (content-addressed) derivations).
 */
Value asnix_prim_derivation(const Value *args, int64_t argc);

/* The raw primop above, wrapped as a Value -- bound to the bare global
 * `derivationStrict` (and `builtins.derivationStrict`), exactly matching
 * real Nix's own split between the low-level primop and the `derivation`
 * wrapper below that's actually built on top of it. */
Value asnix_make_derivation_strict_primop(void);

/* M58: real Nix's `derivation` is NOT a primop -- it's a short Nix-level
 * function (ported verbatim below from cppnix's own
 * `~/nix/src/libexpr/primops/derivation.nix`) that calls
 * `derivationStrict` and then wraps the result in a self-referential
 * "nice" object: `drv.out` is the WHOLE output's own attrset (so
 * `drv.out.outPath`/`drv.out.drvPath`/`drv.out.type` all work, not just
 * `drv.outPath`), and `drv.all`/`drv.drvAttrs`/the original input attrs
 * (`name`, `system`, `builder`, ...) are all merged back in too.
 * Verified directly against real Nix (`(derivation {...}).out` has
 * `builtins.typeOf == "set"`, not "string" -- asnix's previous
 * `out = outPath` string was simply wrong). Found via `d.nix`: nixpkgs'
 * `lib.extendDerivation` (used by every `mkDerivation`-built package,
 * and by minimal-bootstrap's own `derivationWithMeta`) does
 * `inherit (drv.${outputName}) type outputName;` and
 * `drv.${outputName}.outPath` -- both require `drv.out` to be attrset-
 * shaped, which crashed with "selection target is not an attrset" once
 * `out`'s own `.outPath` got selected a second time. Lazily compiled and
 * evaluated once per process (same write-a-fixed-temp-path-once pattern
 * compiler.c's `<nix/fetchurl.nix>` support already established, M46)
 * and the resulting Closure Value is cached -- it's a pure function with
 * no caller-specific state, so reusing one across every root Env's
 * `derivation` binding is behaviorally identical to rebuilding it fresh
 * each time, just without paying the lex/parse/compile cost repeatedly. */
Value asnix_make_derivation_wrapper_value(void);

/* `builtins.derivation`/`builtins.derivationStrict` -- same two Values
 * as the bare globals above, wired into asnix_make_builtins_attrs
 * (builtins.c) via the same {name,build()} integration step every other
 * builtins-expansion module uses. */
extern const AsnixBuiltinMiscDef ASNIX_DERIVATION_BUILTINS[];
extern const size_t ASNIX_DERIVATION_BUILTINS_COUNT;

#endif /* ASNIX_DERIVATION_H */
