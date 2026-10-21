#ifndef ASNIX_PARSER_H
#define ASNIX_PARSER_H

#include "ast.h"
#include "lexer.h"

/* Recursive-descent parser for the M4 minimal subset plus the "operators"
 * milestone's additions: int/bool/null literals, arithmetic (+ - *),
 * comparisons (== != < > <= >=), booleans (&& || !, short-circuit),
 * attrset update (//), `assert cond; body`, function application by
 * juxtaposition, single-param lambdas (`a: body`, curried for multiple
 * params exactly like real Nix), `let`/`in`, and `if`/`then`/`else`.
 * Aborts the process with a message on a syntax error.
 *
 * `src`: M80 (see docs/architecture.md) -- the SAME source buffer `toks`
 * was lexed from (every `Token.pos`/`.start` is a pointer INTO it), needed
 * ONLY to compute a real line/column for `__curPos` (by counting newlines
 * from `src` up to the token's own `.pos` -- see parser.c's
 * curpos_line_col). Every other token field already carries everything
 * the rest of the parser needs without ever touching `src` directly, so
 * this one extra parameter is the full cost of that one feature -- NOT
 * the start of a broader "thread position through every AST/IR node"
 * change (`builtins.unsafeGetAttrPos`'s real per-ATTRIBUTE position
 * remains the disproportionate, deliberately out-of-scope one -- see
 * builtins_m54.c's own `unsafeGetAttrPos2_impl` comment). */
Ast *asnix_parse(Token *toks, size_t count, const char *src);

#endif /* ASNIX_PARSER_H */
