# Conformance tests (mined from numtide/nix-conformance)

These `.nix`/`.expected` pairs are copied from
[numtide/nix-conformance](https://github.com/numtide/nix-conformance)'s
`lang/eval-okay-*` test suite (LGPL-2.1, itself largely derived from Nix's
own functional test suite), not written by this project. `.expected`
holds the exact output Nix 2.34.8 produces.

Only a handful of nix-conformance's 532 `eval-okay-*` cases are copied
here: asnix's current language subset (M0-M7: integer arithmetic,
non-recursive `let`/attrsets, `if`, curried lambdas/application, lists,
attribute selection, 3 builtins) is tiny compared to full Nix, so most
cases need unimplemented features (string concatenation, `++`, comparison/
logical operators, `with`, `rec`, `inherit`, `?`, floats, paths, escape
sequences, most builtins) and would fail for that reason alone -- not
informative as a conformance check yet. The cases here were manually
screened to use *only* currently-implemented syntax; see
`docs/architecture.md`'s conformance notes for how they were found and
what fixing this screening turned up.

As asnix's language coverage grows, re-screen nix-conformance's `lang/`
directory (cloned separately, not vendored into this repo) for more
compatible cases rather than trying to build the full adapter protocol
(`SPEC.md`) prematurely -- that protocol (deep-forcing with
`nix-instantiate --eval --strict`-style `«repeated»` cycle notation,
structured JSON errors, a real store, experimental-feature flags) is a
real undertaking in its own right, worth it once there's enough language
surface for the investment to pay off (see the M9 roadmap milestone).
