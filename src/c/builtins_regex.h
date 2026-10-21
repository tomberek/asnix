#ifndef ASNIX_BUILTINS_REGEX_H
#define ASNIX_BUILTINS_REGEX_H

#include <stddef.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * `builtins.match`/`builtins.split` -- POSIX Extended Regular Expression
 * (ERE) support, wired directly onto glibc's <regex.h> (`regcomp`/
 * `regexec`/`regfree`), which already implements POSIX leftmost-longest
 * semantics -- the same engine class real Nix's own `std::regex` (built
 * with `std::regex::extended`) uses, so this matches real Nix's behavior
 * for ordinary ERE patterns without needing a hand-rolled regex engine.
 * See builtins_regex.c for the match/split algorithms themselves and
 * docs/architecture.md's scope notes for what's deliberately not chased
 * (anything outside plain POSIX ERE -- backreferences aren't part of ERE
 * at all, so that's not a scope cut, just not a thing to support).
 */

extern const AsnixBuiltinMiscDef ASNIX_REGEX_BUILTINS[];
extern const size_t ASNIX_REGEX_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_REGEX_H */
