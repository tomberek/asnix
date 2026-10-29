#ifndef ASNIX_PARSER_H
#define ASNIX_PARSER_H

#include "ast.h"
#include "lexer.h"

Ast *asnix_parse(const char *src);

Ast *asnix_parse_subexpr(const char *src, size_t start_offset);

#endif

