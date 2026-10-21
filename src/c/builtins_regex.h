#ifndef ASNIX_BUILTINS_REGEX_H
#define ASNIX_BUILTINS_REGEX_H

#include <stddef.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * `builtins.match`/`builtins.split` -- POSIX Extended Regular Expression
 * (ERE) support, built on glibc's <regex.h> (`regcomp`/`regexec`/
 * `regfree`) for the common case -- it's both correct and fast for any
 * pattern with no capture groups and none of a handful of bracket-
 * expression extensions glibc can't be trusted for -- plus
 * `regex_engine.c`, a small hand-rolled POSIX ERE backtracking matcher
 * (M59), used whenever a pattern actually needs it: to recompute
 * capture groups (and, for `split`, sometimes the match boundary
 * itself) the way real Nix's libstdc++ `std::regex` would, which
 * glibc's regexec doesn't always agree with for a specific, identified
 * family of ambiguous/repeated subexpressions -- see regex_engine.c's
 * own header comment and docs/architecture.md's M59 scope notes for the
 * full story (M55's scope notes cover the triage that preceded it).
 * See builtins_regex.c for the match/split algorithms and routing
 * logic themselves.
 */

extern const AsnixBuiltinMiscDef ASNIX_REGEX_BUILTINS[];
extern const size_t ASNIX_REGEX_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_REGEX_H */
