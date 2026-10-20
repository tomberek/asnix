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
    /* Strings: "..." and ''...'' no longer lex to one flat TOK_STRING --
     * "${" inside either kind re-enters normal expression lexing until the
     * matching "}" (TOK_RBRACE, reused -- see asnix_lex's mode-stack
     * comment), then resumes string-content lexing. A string literal is
     * now a TOK_STR_START/TOK_ISTR_START, zero or more of (TOK_STR_PART |
     * TOK_DOLLAR_LBRACE ... TOK_RBRACE), and a matching TOK_STR_END/
     * TOK_ISTR_END. */
    TOK_STR_START,     /* '"' opening a double-quoted string */
    TOK_STR_PART,      /* a literal chunk inside "...", escapes already applied */
    TOK_STR_END,       /* '"' closing a double-quoted string */
    TOK_ISTR_START,     /* "''" opening an indented string */
    TOK_ISTR_PART,      /* a raw literal chunk inside ''...'', NOT yet indentation-
                         * stripped (that happens once all of one indented string's
                         * parts are collected -- see parser.c's strip_indentation,
                         * ported from real Nix's ParserState::stripIndentation) */
    TOK_ISTR_END,       /* "''" closing an indented string */
    TOK_DOLLAR_LBRACE,  /* "${" -- starts an interpolated expression */
    TOK_EOF,
} TokKind;

typedef struct {
    TokKind kind;
    int64_t int_val;    /* TOK_INT */
    const char *start;  /* TOK_IDENT / TOK_STR_PART / TOK_ISTR_PART: pointer into a buffer
                          * (TOK_IDENT points into the original source; TOK_STR_PART/
                          * TOK_ISTR_PART point into a freshly malloc'd decoded buffer,
                          * since escape processing can shrink/change the bytes) */
    size_t len;          /* length of the above */
    int istr_has_indentation; /* TOK_ISTR_PART only: 1 if this chunk came from plain
                                * string content (counts toward the indented string's
                                * indentation computation), 0 if it came from an escape
                                * sequence (''$, ''', ''\<c> -- treated as a boundary,
                                * not scanned for leading whitespace, exactly like real
                                * Nix's StringToken::hasIndentation). */
} Token;

/* Tokenizes the whole source in one pass into a malloc'd array terminated
 * by a TOK_EOF entry (*out_count includes that terminator), so the parser
 * can peek arbitrarily far ahead by plain array indexing. Aborts the
 * process with a message on an unrecognized character -- this is a
 * from-scratch minimal-subset lexer, not a full Nix lexer (see
 * docs/architecture.md M4 scope notes). */
Token *asnix_lex(const char *src, size_t *out_count);

#endif /* ASNIX_LEXER_H */
