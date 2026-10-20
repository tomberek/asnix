# Conformance tests (mined from numtide/nix-conformance)

These `.nix`/`.expected` pairs are copied from
[numtide/nix-conformance](https://github.com/numtide/nix-conformance)'s
`lang/eval-okay-*` test suite (LGPL-2.1, itself largely derived from Nix's
own functional test suite), not written by this project. `.expected`
holds the exact output Nix 2.34.8 produces.

Only 19 of nix-conformance's 532 `eval-okay-*` cases are copied here (up
from an original 4, after re-screening once the language subset grew
through M10-M17: recursive `let`/`rec {}`/`inherit`, comparisons/`&&`/
`||`/`!`/`assert`/`//`, string interpolation + multi-line strings, `with`,
lambda-destructuring patterns, `import` + path literals, and an expanded
builtins set). Most of the remaining 513 still need unimplemented features
(string concatenation via `+`, `++`, floats, division, deep/structural
equality on lists/attrsets or content equality on strings, real Nix's
legacy `__overrides` mechanism, regex, JSON/TOML builtins, `genericClosure`,
CLI-level `--arg` autoargs, and more) and were excluded -- either because
asnix cleanly errors on them, or (more subtly, since asnix doesn't
type-check `+`/`==`) because they run to completion but produce a
*different* answer than the `.exp` sidecar, which is caught by actually
running and diffing every case rather than just grepping for disallowed
syntax. See `docs/architecture.md`'s conformance notes for the full
screening methodology, the complete list of what's included/excluded and
why, and one real printer bug (unescaped special characters in displayed
strings) this latest pass found and fixed.

As asnix's language coverage grows, re-screen nix-conformance's `lang/`
directory (cloned separately, not vendored into this repo) for more
compatible cases rather than trying to build the full adapter protocol
(`SPEC.md`) prematurely -- that protocol (deep-forcing with
`nix-instantiate --eval --strict`-style `«repeated»` cycle notation,
structured JSON errors, a real store, experimental-feature flags) is a
real undertaking in its own right, worth it once there's enough language
surface for the investment to pay off (see the M9 roadmap milestone).
