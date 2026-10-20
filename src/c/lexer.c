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

/* Path-literal scanning (import-paths). Real Nix's lexer recognizes PATH
 * tokens with the (flex) regex `[A-Za-z0-9._+-]*(\/[A-Za-z0-9._+-]+)+\/?`
 * -- an optional prefix of "path chars", followed by one-or-more `/segment`
 * groups, with an optional trailing slash (which real Nix's *parser*, not
 * the lexer, then rejects -- "path has a trailing slash"; checked against
 * nix-instantiate). The `+` group is what makes this unambiguous against
 * every other token in this grammar: nothing else in the language ever
 * uses `/` (there's no division operator), so any occurrence of `/` that
 * isn't inside a string is a path. That's also why this is tried *before*
 * digit/ident scanning below, not after: `foo/bar.nix` must lex as one
 * TOK_PATH, not TOK_IDENT("foo") followed by an unrecognized '/'. */
static int path_char(char c) {
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+';
}

/* Returns the length of a path literal starting at src[i], or 0 if this
 * position isn't one (in which case the caller falls through to ordinary
 * digit/ident/single-char scanning). */
static size_t try_scan_path(const char *src, size_t i) {
    size_t j = i;
    while (path_char(src[j])) j++;
    if (src[j] != '/') return 0; /* no `/segment` group at all -- not a path */
    int saw_group = 0;
    while (src[j] == '/') {
        size_t seg_start = j + 1;
        size_t k = seg_start;
        while (path_char(src[k])) k++;
        if (k == seg_start) break; /* '/' not followed by any path chars -- stop before consuming it */
        j = k;
        saw_group = 1;
    }
    if (!saw_group) return 0;
    if (src[j] == '/') j++; /* optional trailing slash -- rejected later, in the compiler */
    return j - i;
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
    { "false", TOK_FALSE }, { "null", TOK_NULL },
};

Token *asnix_lex(const char *src, size_t *out_count) {
    TokVec v = { 0 };
    size_t i = 0;

    while (src[i]) {
        char c = src[i];

        if (isspace((unsigned char)c)) {
            i++;
            continue;
        }
        if (c == '#') { /* line comment, a harmless real-Nix-ism to allow in fixtures */
            while (src[i] && src[i] != '\n') i++;
            continue;
        }

        /* Tried before digit/string/ident/single-char below -- see
         * try_scan_path's comment for why this has to come first. */
        {
            size_t plen = try_scan_path(src, i);
            if (plen > 0) {
                push_tok(&v, (Token){ .kind = TOK_PATH, .start = src + i, .len = plen });
                i += plen;
                continue;
            }
        }

        if (c == '<') {
            /* `<nixpkgs>`-style NIX_PATH search-path literal. Not ambiguous
             * with anything else in this grammar: there's no `<` comparison
             * operator implemented (yet), so `<` has no other meaning. */
            size_t start = i + 1;
            size_t k = start;
            while (path_char(src[k]) || src[k] == '/') k++;
            if (k > start && src[k] == '>') {
                push_tok(&v, (Token){ .kind = TOK_SEARCH_PATH, .start = src + start, .len = k - start });
                i = k + 1;
                continue;
            }
            fprintf(stderr, "asnix: unrecognized character '%c' in source\n", c);
            exit(1);
        }

        if (isdigit((unsigned char)c)) {
            size_t start = i;
            while (isdigit((unsigned char)src[i])) i++;
            Token t = { .kind = TOK_INT, .int_val = strtoll(src + start, NULL, 10) };
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
            push_tok(&v, (Token){ .kind = TOK_STRING, .start = src + start, .len = len });
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
            Token t = { .kind = kind, .start = src + start, .len = len };
            push_tok(&v, t);
            continue;
        }

        TokKind single;
        switch (c) {
            case '+': single = TOK_PLUS; break;
            case '-': single = TOK_MINUS; break;
            case '*': single = TOK_STAR; break;
            case '(': single = TOK_LPAREN; break;
            case ')': single = TOK_RPAREN; break;
            case '=': single = TOK_EQUALS; break;
            case ';': single = TOK_SEMI; break;
            case ':': single = TOK_COLON; break;
            case '.': single = TOK_DOT; break;
            case '{': single = TOK_LBRACE; break;
            case '}': single = TOK_RBRACE; break;
            case '[': single = TOK_LBRACKET; break;
            case ']': single = TOK_RBRACKET; break;
            default:
                fprintf(stderr, "asnix: unrecognized character '%c' in source\n", c);
                exit(1);
        }
        push_tok(&v, (Token){ .kind = single });
        i++;
    }

    push_tok(&v, (Token){ .kind = TOK_EOF });
    *out_count = v.len;
    return v.toks;
}
