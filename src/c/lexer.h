#ifndef ASNIX_LEXER_H
#define ASNIX_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_INT,
    TOK_FLOAT,
    TOK_IDENT,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_LET,
    TOK_IN,
    TOK_IF,
    TOK_THEN,
    TOK_ELSE,
    TOK_REC,
    TOK_INHERIT,
    TOK_ASSERT,
    TOK_WITH,
    TOK_OR,
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
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_ANDAND,
    TOK_OROR,
    TOK_BANG,
    TOK_SLASHSLASH,
    TOK_ARROW,
    TOK_PLUSPLUS,
    TOK_SLASH,
    TOK_STR_START,
    TOK_STR_PART,
    TOK_STR_END,
    TOK_ISTR_START,
    TOK_ISTR_PART,
    TOK_ISTR_ESC,
    TOK_ISTR_END,
    TOK_DOLLAR_LBRACE,
    TOK_QUESTION,
    TOK_COMMA,
    TOK_AT,
    TOK_ELLIPSIS,
    TOK_PATH,
    TOK_PATH_PART,
    TOK_PATH_END,
    TOK_SEARCH_PATH,
    TOK_URI,
    TOK_EOF,
} TokKind;

typedef struct {
    TokKind kind;
    uint32_t start;
    uint32_t end;
} Token;

Token *asnix_lex(const char *src, size_t *out_count);

double asnix_decode_float_literal(const char *src, uint32_t start, uint32_t end);

char *asnix_decode_dquote_span(const char *src, uint32_t start, uint32_t end, size_t *out_len);

void asnix_decode_istr_esc(const char *src, uint32_t start, char out[2], size_t *out_len);

typedef struct Lexer Lexer;

Lexer *lexer_new(const char *src);

Lexer *lexer_new_lenient(const char *src);

Token lexer_next(Lexer *lx);
void lexer_free(Lexer *lx);

typedef struct {
    size_t pos;
    size_t modes_len;
    Token pending;
    int has_pending;
} LexerSave;

LexerSave lexer_save(Lexer *lx);
void lexer_restore(Lexer *lx, LexerSave save);

#endif

