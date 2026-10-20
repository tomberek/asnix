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
 * Aborts the process with a message on a syntax error. */
Ast *asnix_parse(Token *toks, size_t count);

#endif /* ASNIX_PARSER_H */
