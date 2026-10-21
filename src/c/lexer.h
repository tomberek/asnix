#ifndef ASNIX_LEXER_H
#define ASNIX_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_INT,
    TOK_FLOAT, /* M51: a decimal-point-or-exponent numeric literal -- see lexer.c's
                * scan_float_literal for the exact grammar (matches real Nix's
                * lexer.l FLOAT regex precisely, verified against nix-instantiate). */
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
    TOK_OR, /* `expr.attr or default` -- see parser.c's parse_select. Deliberate scope cut
             * (documented in docs/architecture.md): a genuine reserved keyword here, unlike
             * real Nix where `or` is contextually special ("cursed or") but can still be used
             * as an ordinary identifier elsewhere; asnix's `or` can't be used as a plain
             * identifier at all. */
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
    /* Operators (comparisons/booleans/update): see docs/architecture.md's
     * "operators" milestone notes for precedence rationale. */
    TOK_EQEQ,    /* == */
    TOK_NEQ,     /* != */
    TOK_LT,      /* < */
    TOK_GT,      /* > */
    TOK_LE,      /* <= */
    TOK_GE,      /* >= */
    TOK_ANDAND,  /* && */
    TOK_OROR,    /* || */
    TOK_BANG,    /* ! */
    TOK_SLASHSLASH, /* // */
    TOK_ARROW,   /* -> (logical implication) */
    TOK_PLUSPLUS, /* ++ (list concatenation) */
    TOK_SLASH,   /* / (integer division) -- only reached when try_scan_path (lexer.c)
                  * already determined this '/' isn't part of a path literal and it's
                  * not the first of a '//' pair either (see lexer.c's main loop). */
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
    TOK_QUESTION,  /* '?' -- has-attr operator / lambda-pattern default marker */
    TOK_COMMA,     /* ',' -- lambda-pattern formal separator */
    TOK_AT,        /* '@' -- lambda-pattern whole-arg alias (`n@{...}` / `{...}@n`) */
    TOK_ELLIPSIS,  /* '...' -- lambda-pattern "allow extra fields" marker */
    TOK_PATH,        /* bare path literal: ./foo, ../foo, /foo, foo/bar (see lexer.c's
                      * try_scan_path/try_scan_home_path), OR -- M64 -- the FIRST chunk of
                      * an interpolated path literal (`./${foo}`'s "./", `~/${foo}`'s "~/",
                      * ...), in which case this token is immediately followed by
                      * TOK_DOLLAR_LBRACE rather than standing alone; see Token.is_home and
                      * TOK_PATH_PART/TOK_PATH_END below. */
    TOK_PATH_PART,   /* M64: a literal chunk inside an interpolated path literal, AFTER the
                      * first (`./a-${foo}-b`'s "-b", possibly zero-length between two
                      * adjacent interpolations) -- no escapes exist in path-literal syntax
                      * at all, so (unlike TOK_STR_PART) this always points directly into
                      * the original source buffer, never a decoded copy. */
    TOK_PATH_END,    /* M64: closes an interpolated path literal -- a boundary marker only,
                      * no payload (mirrors TOK_STR_END). */
    TOK_SEARCH_PATH, /* <nixpkgs>-style NIX_PATH search-path literal */
    TOK_EOF,
} TokKind;

typedef struct {
    TokKind kind;
    int64_t int_val;    /* TOK_INT */
    double float_val;   /* TOK_FLOAT (M51) */
    const char *start;  /* TOK_IDENT / TOK_STR_PART / TOK_ISTR_PART / TOK_PATH / TOK_PATH_PART /
                          * TOK_SEARCH_PATH: pointer into a buffer (TOK_IDENT/TOK_PATH/
                          * TOK_PATH_PART/TOK_SEARCH_PATH point into the original source,
                          * TOK_SEARCH_PATH excluding the angle brackets; TOK_STR_PART/
                          * TOK_ISTR_PART point into a freshly malloc'd decoded buffer, since escape
                          * processing can shrink/change the bytes) */
    size_t len;          /* length of the above */
    const char *pos;     /* every token: pointer to its first byte in the source buffer --
                           * used to recover the exact source text of an `assert` condition
                           * for the failure message (see parser.c / compiler.c AST_ASSERT) */
    int istr_has_indentation; /* TOK_ISTR_PART only: 1 if this chunk came from plain
                                * string content (counts toward the indented string's
                                * indentation computation), 0 if it came from an escape
                                * sequence (''$, ''', ''\<c> -- treated as a boundary,
                                * not scanned for leading whitespace, exactly like real
                                * Nix's StringToken::hasIndentation). */
    int is_home; /* TOK_PATH only (M64): 1 if this path literal (or this interpolated
                   * literal's first chunk) started with `~` -- see try_scan_home_path and
                   * compiler.c's resolve_path_chunk/resolve_path_literal. Always 0 for
                   * TOK_SEARCH_PATH and every other TOK_PATH use. */
} Token;

/* Tokenizes the whole source in one pass into a malloc'd array terminated
 * by a TOK_EOF entry (*out_count includes that terminator), so the parser
 * can peek arbitrarily far ahead by plain array indexing. Aborts the
 * process with a message on an unrecognized character -- this is a
 * from-scratch minimal-subset lexer, not a full Nix lexer (see
 * docs/architecture.md M4 scope notes). */
Token *asnix_lex(const char *src, size_t *out_count);

#endif /* ASNIX_LEXER_H */
