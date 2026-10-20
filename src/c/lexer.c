#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Token *toks;
    size_t len, cap;
} TokVec;

static void push_tok(TokVec *v, Token t) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->toks = realloc(v->toks, v->cap * sizeof(Token));
    }
    v->toks[v->len++] = t;
}

static int ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}
static int ident_cont(char c) {
    /* Deliberately excludes '-': real Nix allows hyphenated identifiers,
     * but that collides with binary minus in this minimal lexer and isn't
     * needed by the M4 subset -- see docs/architecture.md M4 scope notes. */
    return isalnum((unsigned char)c) || c == '_' || c == '\'';
}

typedef struct {
    const char *kw;
    TokKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    { "let", TOK_LET },     { "in", TOK_IN },     { "if", TOK_IF },
    { "then", TOK_THEN },   { "else", TOK_ELSE },  { "true", TOK_TRUE },
    { "false", TOK_FALSE }, { "null", TOK_NULL }, { "assert", TOK_ASSERT },
};

Token *asnix_lex(const char *src, size_t *out_count) {
    TokVec v = { 0 };
    size_t i = 0;

    while (src[i]) {
        char c = src[i];
        size_t tok_start = i; /* byte offset of whatever token (if any) starts here */

        if (isspace((unsigned char)c)) {
            i++;
            continue;
        }
        if (c == '#') { /* line comment, a harmless real-Nix-ism to allow in fixtures */
            while (src[i] && src[i] != '\n') i++;
            continue;
        }

        if (isdigit((unsigned char)c)) {
            size_t start = i;
            while (isdigit((unsigned char)src[i])) i++;
            Token t = { .kind = TOK_INT, .int_val = strtoll(src + start, NULL, 10), .pos = src + tok_start };
            push_tok(&v, t);
            continue;
        }

        if (c == '"') {
            /* No escapes/interpolation yet -- a plain quoted run, matching
             * what src/c/compiler.c currently does with string Values (see
             * docs/architecture.md M5 scope notes). */
            i++;
            size_t start = i;
            while (src[i] && src[i] != '"') {
                if (src[i] == '\n') {
                    fprintf(stderr, "asnix: unterminated string literal\n");
                    exit(1);
                }
                i++;
            }
            if (!src[i]) {
                fprintf(stderr, "asnix: unterminated string literal\n");
                exit(1);
            }
            size_t len = i - start;
            i++; /* closing quote */
            push_tok(&v, (Token){ .kind = TOK_STRING, .start = src + start, .len = len, .pos = src + tok_start });
            continue;
        }

        if (ident_start(c)) {
            size_t start = i;
            while (ident_cont(src[i])) i++;
            size_t len = i - start;
            TokKind kind = TOK_IDENT;
            for (size_t k = 0; k < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); k++) {
                size_t kwlen = strlen(KEYWORDS[k].kw);
                if (kwlen == len && memcmp(KEYWORDS[k].kw, src + start, len) == 0) {
                    kind = KEYWORDS[k].kind;
                    break;
                }
            }
            Token t = { .kind = kind, .start = src + start, .len = len, .pos = src + tok_start };
            push_tok(&v, t);
            continue;
        }

        /* Two-character operators are checked before falling back to their
         * single-character meaning (or to "unrecognized character" for
         * '&'/'|'/'/' alone, none of which asnix gives a standalone
         * meaning to -- real Nix doesn't either, except '/' as part of a
         * path literal, not supported yet). */
        TokKind kind;
        size_t width = 1;
        if (c == '=' && src[i + 1] == '=') { kind = TOK_EQEQ; width = 2; }
        else if (c == '!' && src[i + 1] == '=') { kind = TOK_NEQ; width = 2; }
        else if (c == '<' && src[i + 1] == '=') { kind = TOK_LE; width = 2; }
        else if (c == '>' && src[i + 1] == '=') { kind = TOK_GE; width = 2; }
        else if (c == '&' && src[i + 1] == '&') { kind = TOK_ANDAND; width = 2; }
        else if (c == '|' && src[i + 1] == '|') { kind = TOK_OROR; width = 2; }
        else if (c == '/' && src[i + 1] == '/') { kind = TOK_SLASHSLASH; width = 2; }
        else {
            switch (c) {
                case '+': kind = TOK_PLUS; break;
                case '-': kind = TOK_MINUS; break;
                case '*': kind = TOK_STAR; break;
                case '(': kind = TOK_LPAREN; break;
                case ')': kind = TOK_RPAREN; break;
                case '=': kind = TOK_EQUALS; break;
                case ';': kind = TOK_SEMI; break;
                case ':': kind = TOK_COLON; break;
                case '.': kind = TOK_DOT; break;
                case '{': kind = TOK_LBRACE; break;
                case '}': kind = TOK_RBRACE; break;
                case '[': kind = TOK_LBRACKET; break;
                case ']': kind = TOK_RBRACKET; break;
                case '<': kind = TOK_LT; break;
                case '>': kind = TOK_GT; break;
                case '!': kind = TOK_BANG; break;
                default:
                    fprintf(stderr, "asnix: unrecognized character '%c' in source\n", c);
                    exit(1);
            }
        }
        push_tok(&v, (Token){ .kind = kind, .pos = src + tok_start });
        i += width;
    }

    push_tok(&v, (Token){ .kind = TOK_EOF, .pos = src + i });
    *out_count = v.len;
    return v.toks;
}
