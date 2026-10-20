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
    { "false", TOK_FALSE }, { "null", TOK_NULL },
};

/* Lexer mode stack -- a from-scratch equivalent of real Nix's flex state
 * stack (see ~/nix's src/libexpr/lexer.l, %option stack / PUSH_STATE /
 * POP_STATE): entering a string pushes STRING/ISTRING on top; every "${"
 * (and every literal "{", for exactly the same reason: `${ { a = 1; }.a }`
 * needs the inner attrset's own "}" to NOT prematurely resume string
 * lexing) pushes NORMAL; every "}" pops one level. Only when a pop reveals
 * STRING/ISTRING underneath does the "}" that closed an interpolation
 * actually mean "resume string-content lexing" -- the asm/dispatch loop
 * terminology doesn't apply here, this is purely a lexer-internal stack. */
typedef enum { MODE_NORMAL, MODE_STRING, MODE_ISTRING } LexMode;

typedef struct {
    LexMode *modes;
    size_t len, cap;
} ModeStack;

static void mode_push(ModeStack *m, LexMode mode) {
    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->modes = realloc(m->modes, m->cap * sizeof(LexMode));
    }
    m->modes[m->len++] = mode;
}
static LexMode mode_top(ModeStack *m) {
    return m->modes[m->len - 1];
}
static void mode_pop(ModeStack *m) {
    /* Every push above is paired 1:1 with a pop at a matching "}"/closing
     * quote, driven entirely by well-formed-by-construction scan logic
     * below -- an underflow here would be a lexer bug, not malformed
     * input (malformed `}`-without-`{` is caught before this is ever
     * called, see the MODE_NORMAL '}' case). */
    m->len--;
}

/* Growable byte buffer for accumulating decoded string-literal content:
 * escape processing can shrink/change bytes, so a literal chunk can't
 * just be a pointer+length into the original source the way TOK_IDENT is. */
typedef struct {
    char *buf;
    size_t len, cap;
} ByteBuf;

static void bb_push(ByteBuf *bb, char c) {
    if (bb->len == bb->cap) {
        bb->cap = bb->cap ? bb->cap * 2 : 32;
        bb->buf = realloc(bb->buf, bb->cap);
    }
    bb->buf[bb->len++] = c;
}

/* Hands ownership of a malloc'd copy of bb's current contents to the
 * token being pushed, then resets bb (keeping its underlying buffer, if
 * any, for the next chunk) -- tokens must outlive the ByteBuf that built
 * them, since asnix_lex returns the whole token array back to the caller. */
static char *bb_take_copy(ByteBuf *bb, size_t *out_len) {
    char *copy = malloc(bb->len ? bb->len : 1);
    if (bb->len) memcpy(copy, bb->buf, bb->len);
    *out_len = bb->len;
    bb->len = 0;
    return copy;
}

static void fatal_unterminated(const char *what) {
    fprintf(stderr, "asnix: unterminated %s\n", what);
    exit(1);
}

/* Decodes one backslash-escape's trailing character, matching real Nix's
 * unescapeStr (~/nix's src/libexpr/lexer.l): \n \r \t map to the control
 * character, anything else (including \" \\ \$ \') passes through as the
 * literal character itself. */
static char decode_escape_char(char c) {
    switch (c) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        default: return c;
    }
}

/* Scans one run of double-quoted string content starting at *i (which
 * must be positioned just inside the opening '"' or just after a closed
 * "${ ... }" interpolation), up to and including the next boundary --
 * either the closing '"' or the next "${" -- and pushes exactly two
 * tokens: a TOK_STR_PART (possibly zero-length, e.g. between two adjacent
 * interpolations) followed by the boundary token (TOK_STR_END or
 * TOK_DOLLAR_LBRACE). Mutates *i and `modes` to reflect the boundary
 * crossed (pop back to NORMAL on close, push a fresh NORMAL on "${").
 * Verified against real `nix-instantiate` for raw embedded newlines
 * (allowed, unlike this lexer's pre-interpolation M5 behavior -- see
 * docs/architecture.md's Known Bugs list) and the \n \r \t \" \\ \$
 * escapes. */
static void lex_dquote_run(const char *src, size_t *i, ModeStack *modes, TokVec *v) {
    ByteBuf bb = { 0 };
    for (;;) {
        char c = src[*i];
        if (c == '\0') fatal_unterminated("string literal");
        if (c == '"') {
            (*i)++;
            size_t len;
            char *text = bb_take_copy(&bb, &len);
            push_tok(v, (Token){ .kind = TOK_STR_PART, .start = text, .len = len });
            mode_pop(modes);
            push_tok(v, (Token){ .kind = TOK_STR_END });
            free(bb.buf);
            return;
        }
        if (c == '$' && src[*i + 1] == '{') {
            *i += 2;
            size_t len;
            char *text = bb_take_copy(&bb, &len);
            push_tok(v, (Token){ .kind = TOK_STR_PART, .start = text, .len = len });
            mode_push(modes, MODE_NORMAL);
            push_tok(v, (Token){ .kind = TOK_DOLLAR_LBRACE });
            free(bb.buf);
            return;
        }
        if (c == '\\') {
            char e = src[*i + 1];
            if (e == '\0') fatal_unterminated("string literal");
            bb_push(&bb, decode_escape_char(e));
            *i += 2;
            continue;
        }
        bb_push(&bb, c); /* includes raw '\n' -- real Nix allows literal newlines in "..." */
        (*i)++;
    }
}

/* Scans one or more runs of indented ('' ... '') string content starting
 * at *i, up to and including the next boundary (closing "''" or "${"),
 * pushing one TOK_ISTR_PART per plain-content/escape-sequence run (so
 * indentation stripping, done later over the whole collected sequence,
 * can tell real content from an escape the same way real Nix's
 * StringToken::hasIndentation does -- see ~/nix's ParserState::
 * stripIndentation) followed by the boundary token. Implements the
 * escapes verified against real nix-instantiate: ''$ -> literal "$"
 * (combined with plain "{" right after, this is how ''${ becomes a
 * literal "${" without starting an interpolation -- there is no separate
 * "''${" rule, it falls out of this one plus ordinary content scanning),
 * ''' -> literal "''", ''\<c> -> a backslash-style escape of <c>, and
 * bare '' (not followed by one of the above) always closes the string.
 *
 * Deliberate, documented scope cut vs. real Nix's lexer: a lone "'"
 * immediately followed by "$" is treated here as two ordinary content
 * characters (real Nix special-cases it as a hasIndentation=false
 * boundary token). This only affects the indentation-*counting* boundary
 * in the vanishingly rare case of a leading "'$" run of whitespace, never
 * the produced string's actual bytes -- not exercised by any case in
 * tests/e2e, and not worth the extra complexity here. */
static void istr_flush_if_nonempty(ByteBuf *bb, int acc_is_content, TokVec *v) {
    if (bb->len == 0) return;
    size_t len;
    char *text = bb_take_copy(bb, &len);
    push_tok(v, (Token){ .kind = TOK_ISTR_PART, .start = text, .len = len, .istr_has_indentation = acc_is_content });
}

static void istr_flush_always(ByteBuf *bb, int acc_is_content, TokVec *v) {
    size_t len;
    char *text = bb_take_copy(bb, &len);
    push_tok(v, (Token){ .kind = TOK_ISTR_PART, .start = text, .len = len, .istr_has_indentation = acc_is_content });
}

/* Appends one escape sequence's decoded bytes to `bb`, flushing first if
 * the buffer currently holds a plain-content run (category changed).
 * `acc_is_content` is the caller's running category variable: always 0
 * (not content) after this returns. */
static void istr_append_escape(ByteBuf *bb, int *acc_is_content, TokVec *v, const char *bytes, size_t n) {
    if (bb->len > 0 && *acc_is_content != 0) istr_flush_if_nonempty(bb, *acc_is_content, v);
    *acc_is_content = 0;
    for (size_t k = 0; k < n; k++) bb_push(bb, bytes[k]);
}

static void lex_indented_run(const char *src, size_t *i, ModeStack *modes, TokVec *v) {
    ByteBuf bb = { 0 };
    int acc_is_content = 0; /* only meaningful once bb.len > 0 */

    for (;;) {
        char c = src[*i];
        if (c == '\0') fatal_unterminated("indented string literal (''...'')");

        if (c == '$' && src[*i + 1] == '{') {
            istr_flush_always(&bb, acc_is_content, v); /* always emit a (possibly empty) PART before the boundary */
            *i += 2;
            mode_push(modes, MODE_NORMAL);
            push_tok(v, (Token){ .kind = TOK_DOLLAR_LBRACE });
            free(bb.buf);
            return;
        }

        if (c == '\'' && src[*i + 1] == '\'') {
            char third = src[*i + 2];
            if (third == '\'') { /* ''' -> literal "''" */
                istr_append_escape(&bb, &acc_is_content, v, "''", 2);
                *i += 3;
                continue;
            }
            if (third == '$') { /* ''$ -> literal "$" (combined with a following plain '{', this is
                                  * how ''${ reads as a literal "${" without starting an
                                  * interpolation -- there's no separate "''${" rule, it falls out
                                  * of this escape plus ordinary content scanning of the '{'). */
                istr_append_escape(&bb, &acc_is_content, v, "$", 1);
                *i += 3;
                continue;
            }
            if (third == '\\') { /* ''\<c> -> one escaped character, same decode rules as "..." */
                char e = src[*i + 3];
                if (e == '\0') fatal_unterminated("indented string literal (''...'')");
                char decoded = decode_escape_char(e);
                istr_append_escape(&bb, &acc_is_content, v, &decoded, 1);
                *i += 4;
                continue;
            }
            /* Bare '' not followed by ', $, or \\ -- always the closing delimiter. */
            istr_flush_always(&bb, acc_is_content, v);
            *i += 2;
            mode_pop(modes);
            push_tok(v, (Token){ .kind = TOK_ISTR_END });
            free(bb.buf);
            return;
        }

        /* Plain content: everything else, including a lone '$' not
         * followed by '{', a lone '\'' not followed by another '\'', and
         * raw newlines (which indentation-stripping treats specially). */
        if (bb.len > 0 && acc_is_content != 1) istr_flush_if_nonempty(&bb, acc_is_content, v);
        acc_is_content = 1;
        bb_push(&bb, c);
        (*i)++;
    }
}

Token *asnix_lex(const char *src, size_t *out_count) {
    TokVec v = { 0 };
    ModeStack modes = { 0 };
    mode_push(&modes, MODE_NORMAL);
    size_t i = 0;

    for (;;) {
        LexMode mode = mode_top(&modes);

        if (mode == MODE_STRING) {
            lex_dquote_run(src, &i, &modes, &v);
            continue;
        }
        if (mode == MODE_ISTRING) {
            lex_indented_run(src, &i, &modes, &v);
            continue;
        }

        /* mode == MODE_NORMAL: ordinary expression/identifier/operator lexing. */
        char c = src[i];

        if (c == '\0') {
            if (modes.len != 1) {
                fprintf(stderr, "asnix: unexpected end of input inside '${ ... }'\n");
                exit(1);
            }
            break;
        }

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
            Token t = { .kind = TOK_INT, .int_val = strtoll(src + start, NULL, 10) };
            push_tok(&v, t);
            continue;
        }

        if (c == '"') {
            i++;
            mode_push(&modes, MODE_STRING);
            push_tok(&v, (Token){ .kind = TOK_STR_START });
            continue;
        }

        if (c == '\'' && src[i + 1] == '\'') {
            i += 2;
            /* \'\'(\ *\n)? -- swallow an immediate (optionally space-padded)
             * newline right after the opening delimiter; it contributes
             * nothing to the string's content or its indentation
             * computation. Only consumed if a newline actually follows the
             * spaces -- otherwise none of it is consumed (matches the `?`
             * on the whole group, not on the spaces alone). */
            size_t j = i;
            while (src[j] == ' ') j++;
            if (src[j] == '\n') i = j + 1;
            mode_push(&modes, MODE_ISTRING);
            push_tok(&v, (Token){ .kind = TOK_ISTR_START });
            continue;
        }

        if (c == '{') {
            i++;
            mode_push(&modes, MODE_NORMAL);
            push_tok(&v, (Token){ .kind = TOK_LBRACE });
            continue;
        }
        if (c == '}') {
            if (modes.len == 1) {
                fprintf(stderr, "asnix: unmatched '}'\n");
                exit(1);
            }
            i++;
            mode_pop(&modes);
            push_tok(&v, (Token){ .kind = TOK_RBRACE });
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
    free(modes.modes);
    *out_count = v.len;
    return v.toks;
}
