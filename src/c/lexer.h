#ifndef ASNIX_LEXER_H
#define ASNIX_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_INT,
    TOK_IDENT,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_LET,
    TOK_IN,
    TOK_IF,
    TOK_THEN,
    TOK_ELSE,
    TOK_WITH,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_EQUALS,
    TOK_SEMI,
    TOK_COLON,
    TOK_DOT,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_STRING,
    TOK_EOF,
} TokKind;

typedef struct {
    TokKind kind;
    int64_t int_val;    /* TOK_INT */
    const char *start;  /* TOK_IDENT / TOK_STRING: pointer into the source buffer */
    size_t len;          /* TOK_IDENT / TOK_STRING: length (TOK_STRING excludes the quotes) */
} Token;

/* Tokenizes the whole source in one pass into a malloc'd array terminated
 * by a TOK_EOF entry (*out_count includes that terminator), so the parser
 * can peek arbitrarily far ahead by plain array indexing. Aborts the
 * process with a message on an unrecognized character -- this is a
 * from-scratch minimal-subset lexer, not a full Nix lexer (see
 * docs/architecture.md M4 scope notes). */
Token *asnix_lex(const char *src, size_t *out_count);

#endif /* ASNIX_LEXER_H */
