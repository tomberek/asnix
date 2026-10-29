#include "lexer.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CC_SPACE      = 1 << 0,
    CC_DIGIT      = 1 << 1,
    CC_ALPHA      = 1 << 2,
    CC_IDENT_CONT = 1 << 3,
    CC_PATH_CHAR  = 1 << 4,
    CC_URI_SCHEME = 1 << 5,
    CC_URI_TAIL   = 1 << 6,
};

static const unsigned char CHAR_CLASS[256] = {
    ['\t'] = CC_SPACE, ['\n'] = CC_SPACE, ['\v'] = CC_SPACE, ['\f'] = CC_SPACE, ['\r'] = CC_SPACE, [' '] = CC_SPACE,

    ['0' ... '9'] = CC_DIGIT | CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['A' ... 'Z'] = CC_ALPHA | CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['a' ... 'z'] = CC_ALPHA | CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,

    ['\''] = CC_IDENT_CONT | CC_URI_TAIL,
    ['_']  = CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_TAIL,
    ['-']  = CC_IDENT_CONT | CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['.']  = CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,
    ['+']  = CC_PATH_CHAR | CC_URI_SCHEME | CC_URI_TAIL,

    ['!'] = CC_URI_TAIL, ['$'] = CC_URI_TAIL, ['%'] = CC_URI_TAIL, ['&'] = CC_URI_TAIL,
    ['*'] = CC_URI_TAIL, [','] = CC_URI_TAIL, ['/'] = CC_URI_TAIL, [':'] = CC_URI_TAIL,
    ['='] = CC_URI_TAIL, ['?'] = CC_URI_TAIL, ['@'] = CC_URI_TAIL, ['~'] = CC_URI_TAIL,
};

static int cc_digit(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_DIGIT) != 0; }
static int cc_space(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_SPACE) != 0; }
static int cc_alpha(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_ALPHA) != 0; }

static Token mk_tok(TokKind k, size_t start, size_t end) {
    return (Token){ .kind = k, .start = (uint32_t)start, .end = (uint32_t)end };
}

static int path_char(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_PATH_CHAR) != 0;
}

static int at_dollar_brace(const char *src, size_t j) {
    return src[j] == '$' && src[j + 1] == '{';
}

static size_t scan_path_groups(const char *src, size_t j, int *saw_group) {
    for (;;) {
        if (src[j] != '/') break;
        size_t seg_start = j + 1;
        size_t k = seg_start;
        while (path_char(src[k])) k++;
        if (k == seg_start) {
            if (at_dollar_brace(src, k)) {
                j = k;
                *saw_group = 1;
            }
            break;
        }
        j = k;
        *saw_group = 1;
    }
    return j;
}

static size_t try_scan_path(const char *src, size_t i) {
    size_t j = i;
    while (path_char(src[j])) j++;
    int saw_group = 0;
    j = scan_path_groups(src, j, &saw_group);
    if (!saw_group) return 0;
    if (src[j] == '/') j++;
    return j - i;
}

static size_t try_scan_home_path(const char *src, size_t i) {
    if (src[i] != '~' || src[i + 1] != '/') return 0;
    int saw_group = 0;
    size_t j = scan_path_groups(src, i + 1, &saw_group);
    if (!saw_group) return 0;
    if (src[j] == '/') j++;
    return j - i;
}

static int uri_scheme_char(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_URI_SCHEME) != 0;
}
static int uri_tail_char(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_URI_TAIL) != 0;
}
static size_t try_scan_uri(const char *src, size_t i) {
    if (!cc_alpha(src[i])) return 0;
    size_t j = i + 1;
    while (uri_scheme_char(src[j])) j++;
    if (src[j] != ':') return 0;
    size_t k = j + 1;
    size_t tail_start = k;
    while (uri_tail_char(src[k])) k++;
    if (k == tail_start) return 0;
    return k - i;
}

static int ident_start(char c) {
    return cc_alpha(c) || c == '_';
}
static int ident_cont(char c) {
    return (CHAR_CLASS[(unsigned char)c] & CC_IDENT_CONT) != 0;
}

typedef struct {
    const char *kw;
    uint8_t kwlen;
    TokKind kind;
} Keyword;

#define KW(lit, k) { lit, sizeof(lit) - 1, k }

static const Keyword KEYWORDS[] = {
    KW("let", TOK_LET),         KW("in", TOK_IN),     KW("if", TOK_IF),
    KW("then", TOK_THEN),       KW("else", TOK_ELSE),  KW("true", TOK_TRUE),
    KW("false", TOK_FALSE),     KW("null", TOK_NULL),
    KW("rec", TOK_REC),         KW("inherit", TOK_INHERIT),
    KW("assert", TOK_ASSERT),   KW("with", TOK_WITH),
    KW("or", TOK_OR),
};

static TokKind keyword_lookup(const char *s, size_t len) {
    switch (len) {
    case 2:
        if (memcmp(s, "in", 2) == 0) return TOK_IN;
        if (memcmp(s, "if", 2) == 0) return TOK_IF;
        if (memcmp(s, "or", 2) == 0) return TOK_OR;
        return TOK_IDENT;
    case 3:
        if (memcmp(s, "let", 3) == 0) return TOK_LET;
        if (memcmp(s, "rec", 3) == 0) return TOK_REC;
        return TOK_IDENT;
    case 4:
        if (memcmp(s, "then", 4) == 0) return TOK_THEN;
        if (memcmp(s, "else", 4) == 0) return TOK_ELSE;
        if (memcmp(s, "true", 4) == 0) return TOK_TRUE;
        if (memcmp(s, "null", 4) == 0) return TOK_NULL;
        if (memcmp(s, "with", 4) == 0) return TOK_WITH;
        return TOK_IDENT;
    case 5:
        if (memcmp(s, "false", 5) == 0) return TOK_FALSE;
        return TOK_IDENT;
    case 6:
        if (memcmp(s, "assert", 6) == 0) return TOK_ASSERT;
        return TOK_IDENT;
    case 7:
        if (memcmp(s, "inherit", 7) == 0) return TOK_INHERIT;
        return TOK_IDENT;
    default:
        return TOK_IDENT;
    }
}

static void keyword_lookup_selfcheck(void) {
    static int checked = 0;
    if (checked) return;
    checked = 1;
    for (size_t k = 0; k < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); k++) {
        TokKind got = keyword_lookup(KEYWORDS[k].kw, KEYWORDS[k].kwlen);
        assert(got == KEYWORDS[k].kind);
    }
}

#undef KW

typedef enum { MODE_NORMAL, MODE_STRING, MODE_ISTRING, MODE_PATH } LexMode;

struct Lexer {
    const char *src;
    size_t pos;
    LexMode *modes;
    size_t modes_len, modes_cap;
    LexMode cur;
    Token pending;
    int has_pending;
    int lenient;
};

static void mode_push(Lexer *lx, LexMode mode) {
    if (lx->modes_len == lx->modes_cap) {
        lx->modes_cap = lx->modes_cap ? lx->modes_cap * 2 : 16;
        lx->modes = realloc(lx->modes, lx->modes_cap * sizeof(LexMode));
    }
    lx->modes[lx->modes_len++] = mode;
    lx->cur = mode;
}
static LexMode mode_top(Lexer *lx) {
    return lx->cur;
}
static void mode_pop(Lexer *lx) {
    lx->modes_len--;
    lx->cur = lx->modes[lx->modes_len - 1];
}

static void fatal_unterminated(const char *what) {
    fprintf(stderr, "asnix: unterminated %s\n", what);
    exit(1);
}

static char decode_escape_char(char c) {
    switch (c) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        default: return c;
    }
}

double asnix_decode_float_literal(const char *src, uint32_t start, uint32_t end) {
    (void)end;
    errno = 0;
    double d = strtod(src + start, NULL);
    if (errno == ERANGE) {
        fprintf(stderr, "asnix: invalid float '%.*s'\n", (int)(end - start), src + start);
        exit(1);
    }
    return d;
}

char *asnix_decode_dquote_span(const char *src, uint32_t start, uint32_t end, size_t *out_len) {
    size_t cap = (size_t)(end - start);
    char *out = malloc(cap ? cap : 1);
    size_t n = 0;
    uint32_t i = start;
    while (i < end) {
        char c = src[i];
        if (c == '\\') {
            out[n++] = decode_escape_char(src[i + 1]);
            i += 2;
        } else {
            out[n++] = c;
            i += 1;
        }
    }
    *out_len = n;
    return out;
}

void asnix_decode_istr_esc(const char *src, uint32_t start, char out[2], size_t *out_len) {
    char third = src[start + 2];
    if (third == '\'') {
        out[0] = '\''; out[1] = '\'';
        *out_len = 2;
    } else if (third == '$') {
        out[0] = '$';
        *out_len = 1;
    } else {
        out[0] = decode_escape_char(src[start + 3]);
        *out_len = 1;
    }
}

static Token lex_dquote_run(Lexer *lx) {
    const char *src = lx->src;
    size_t start = lx->pos;
    size_t i = lx->pos;
    for (;;) {
        char c = src[i];
        if (c == '\0') fatal_unterminated("string literal");
        if (c == '"') {
            Token part = mk_tok(TOK_STR_PART, start, i);
            i++;
            mode_pop(lx);
            lx->pos = i;
            lx->pending = mk_tok(TOK_STR_END, i, i);
            lx->has_pending = 1;
            return part;
        }
        if (c == '$' && src[i + 1] == '$') {
            i += 2;
            continue;
        }
        if (c == '$' && src[i + 1] == '{') {
            Token part = mk_tok(TOK_STR_PART, start, i);
            i += 2;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            lx->pending = mk_tok(TOK_DOLLAR_LBRACE, i - 2, i);
            lx->has_pending = 1;
            return part;
        }
        if (c == '\\') {
            char e = src[i + 1];
            if (e == '\0') fatal_unterminated("string literal");
            i += 2;
            continue;
        }
        i++;
    }
}

static Token lex_indented_run(Lexer *lx) {
    const char *src = lx->src;
    size_t content_start = lx->pos;
    size_t i = lx->pos;

    for (;;) {
        char c = src[i];
        if (c == '\0') fatal_unterminated("indented string literal (''...'')");

        if (c == '$' && src[i + 1] == '$') {
            i += 2;
            continue;
        }
        if (c == '$' && src[i + 1] == '{') {
            Token part = mk_tok(TOK_ISTR_PART, content_start, i);
            i += 2;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            lx->pending = mk_tok(TOK_DOLLAR_LBRACE, i - 2, i);
            lx->has_pending = 1;
            return part;
        }

        if (c == '\'' && src[i + 1] == '\'') {
            char third = src[i + 2];
            if (third == '\'' || third == '$' || third == '\\') {
                size_t esc_start = i;
                size_t esc_end;
                if (third == '\\') {
                    if (src[i + 3] == '\0') fatal_unterminated("indented string literal (''...'')");
                    esc_end = i + 4;
                } else {
                    esc_end = i + 3;
                }
                if (i > content_start) {
                    lx->pos = i;
                    return mk_tok(TOK_ISTR_PART, content_start, i);
                }
                lx->pos = esc_end;
                return mk_tok(TOK_ISTR_ESC, esc_start, esc_end);
            }
            Token part = mk_tok(TOK_ISTR_PART, content_start, i);
            i += 2;
            mode_pop(lx);
            lx->pos = i;
            lx->pending = mk_tok(TOK_ISTR_END, i, i);
            lx->has_pending = 1;
            return part;
        }

        i++;
    }
}

static Token lex_path_run(Lexer *lx) {
    const char *src = lx->src;
    size_t start = lx->pos;
    size_t j = lx->pos;
    while (path_char(src[j])) j++;
    int saw_group = 0;
    j = scan_path_groups(src, j, &saw_group);
    if (saw_group && src[j] == '/') j++;
    size_t i = j;

    if (at_dollar_brace(src, i)) {
        Token part = mk_tok(TOK_PATH_PART, start, i);
        i += 2;
        mode_push(lx, MODE_NORMAL);
        lx->pos = i;
        lx->pending = mk_tok(TOK_DOLLAR_LBRACE, i - 2, i);
        lx->has_pending = 1;
        return part;
    }
    Token part = mk_tok(TOK_PATH_PART, start, i);
    mode_pop(lx);
    lx->pos = i;
    lx->pending = mk_tok(TOK_PATH_END, i, i);
    lx->has_pending = 1;
    return part;
}

static void scan_float_tail(const char *src, size_t *i) {
    while (cc_digit(src[*i])) (*i)++;
    if (src[*i] == 'e' || src[*i] == 'E') {
        size_t k = *i + 1;
        if (src[k] == '+' || src[k] == '-') k++;
        if (cc_digit(src[k])) {
            *i = k;
            while (cc_digit(src[*i])) (*i)++;
        }
    }
}

static Token lex_normal(Lexer *lx) {
    const char *src = lx->src;
    for (;;) {
        size_t i = lx->pos;
        char c = src[i];
        size_t tok_start = i;

        if (c == '\0') {
            if (lx->modes_len != 1) {
                fprintf(stderr, "asnix: unexpected end of input inside '${ ... }'\n");
                exit(1);
            }
            lx->pos = i;
            return mk_tok(TOK_EOF, i, i);
        }

        if (cc_space(c)) {
            lx->pos = i + 1;
            continue;
        }
        if (c == '#') {
            while (src[i] && src[i] != '\n') i++;
            lx->pos = i;
            continue;
        }
        if (c == '/' && src[i + 1] == '*') {
            i += 2;
            while (src[i] && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (!src[i]) {
                fprintf(stderr, "asnix: unterminated block comment\n");
                exit(1);
            }
            i += 2;
            lx->pos = i;
            continue;
        }

        {
            size_t hlen = try_scan_home_path(src, i);
            if (hlen > 0) {
                size_t end = i + hlen;
                if (at_dollar_brace(src, end)) {
                    mode_push(lx, MODE_PATH);
                    mode_push(lx, MODE_NORMAL);
                    lx->pos = end + 2;
                    lx->pending = mk_tok(TOK_DOLLAR_LBRACE, end, end + 2);
                    lx->has_pending = 1;
                    return mk_tok(TOK_PATH, i, end);
                }
                lx->pos = end;
                return mk_tok(TOK_PATH, i, end);
            }
        }

        if (ident_start(c)) {
            size_t j = i + 1;
            while (ident_cont(src[j])) j++;
            char nb = src[j];
            if (nb == '/' || nb == ':' || nb == '.' || nb == '+') {
                size_t plen = try_scan_path(src, i);
                if (plen > 0) {
                    size_t end = i + plen;
                    if (at_dollar_brace(src, end)) {
                        mode_push(lx, MODE_PATH);
                        mode_push(lx, MODE_NORMAL);
                        lx->pos = end + 2;
                        lx->pending = mk_tok(TOK_DOLLAR_LBRACE, end, end + 2);
                        lx->has_pending = 1;
                        return mk_tok(TOK_PATH, i, end);
                    }
                    lx->pos = end;
                    return mk_tok(TOK_PATH, i, end);
                }
                size_t ulen = try_scan_uri(src, i);
                if (ulen > 0) {
                    lx->pos = i + ulen;
                    return mk_tok(TOK_URI, i, i + ulen);
                }
            }
            size_t start = i;
            size_t len = j - start;
            TokKind kind = keyword_lookup(src + start, len);
            lx->pos = j;
            return mk_tok(kind, start, j);
        }

        {
            size_t plen = try_scan_path(src, i);
            if (plen > 0) {
                size_t end = i + plen;
                if (at_dollar_brace(src, end)) {
                    mode_push(lx, MODE_PATH);
                    mode_push(lx, MODE_NORMAL);
                    lx->pos = end + 2;
                    lx->pending = mk_tok(TOK_DOLLAR_LBRACE, end, end + 2);
                    lx->has_pending = 1;
                    return mk_tok(TOK_PATH, i, end);
                }
                lx->pos = end;
                return mk_tok(TOK_PATH, i, end);
            }
        }

        if (c == '<') {
            size_t start = i + 1;
            size_t k = start;
            while (path_char(src[k]) || src[k] == '/') k++;
            if (k > start && src[k] == '>') {
                lx->pos = k + 1;
                return mk_tok(TOK_SEARCH_PATH, start, k);
            }
        }

        if (cc_digit(c)) {
            size_t start = i;
            while (cc_digit(src[i])) i++;
            size_t int_len = i - start;
            int is_float = 0;
            if (src[i] == '.') {
                if (int_len == 1 && src[start] == '0') {
                    if (cc_digit(src[i + 1])) is_float = 1;
                } else if (src[start] != '0') {
                    is_float = 1;
                }
            }
            if (is_float) {
                i++;
                scan_float_tail(src, &i);
                lx->pos = i;
                return mk_tok(TOK_FLOAT, start, i);
            }
            lx->pos = i;
            return mk_tok(TOK_INT, start, i);
        }

        if (c == '.' && cc_digit(src[i + 1])) {
            size_t start = i;
            i++;
            scan_float_tail(src, &i);
            lx->pos = i;
            return mk_tok(TOK_FLOAT, start, i);
        }

        if (c == '"') {
            i++;
            mode_push(lx, MODE_STRING);
            lx->pos = i;
            return mk_tok(TOK_STR_START, tok_start, i);
        }

        if (c == '\'' && src[i + 1] == '\'') {
            i += 2;
            size_t j = i;
            while (src[j] == ' ') j++;
            if (src[j] == '\n') i = j + 1;
            mode_push(lx, MODE_ISTRING);
            lx->pos = i;
            return mk_tok(TOK_ISTR_START, tok_start, i);
        }

        if (c == '$' && src[i + 1] == '{') {
            i += 2;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            return mk_tok(TOK_DOLLAR_LBRACE, tok_start, i);
        }

        if (c == '{') {
            i++;
            mode_push(lx, MODE_NORMAL);
            lx->pos = i;
            return mk_tok(TOK_LBRACE, tok_start, i);
        }
        if (c == '}') {
            if (lx->modes_len == 1) {
                if (lx->lenient) {
                    i++;
                    lx->pos = i;
                    return mk_tok(TOK_RBRACE, tok_start, i);
                }
                fprintf(stderr, "asnix: unmatched '}'\n");
                exit(1);
            }
            i++;
            mode_pop(lx);
            lx->pos = i;
            return mk_tok(TOK_RBRACE, tok_start, i);
        }

        if (c == '.' && src[i + 1] == '.' && src[i + 2] == '.') {
            lx->pos = i + 3;
            return mk_tok(TOK_ELLIPSIS, tok_start, tok_start + 3);
        }

        TokKind kind;
        size_t width = 1;
        if (c == '=' && src[i + 1] == '=') { kind = TOK_EQEQ; width = 2; }
        else if (c == '!' && src[i + 1] == '=') { kind = TOK_NEQ; width = 2; }
        else if (c == '<' && src[i + 1] == '=') { kind = TOK_LE; width = 2; }
        else if (c == '>' && src[i + 1] == '=') { kind = TOK_GE; width = 2; }
        else if (c == '&' && src[i + 1] == '&') { kind = TOK_ANDAND; width = 2; }
        else if (c == '|' && src[i + 1] == '|') { kind = TOK_OROR; width = 2; }
        else if (c == '/' && src[i + 1] == '/') { kind = TOK_SLASHSLASH; width = 2; }
        else if (c == '-' && src[i + 1] == '>') { kind = TOK_ARROW; width = 2; }
        else if (c == '+' && src[i + 1] == '+') { kind = TOK_PLUSPLUS; width = 2; }
        else {
            switch (c) {
                case '+': kind = TOK_PLUS; break;
                case '-': kind = TOK_MINUS; break;
                case '*': kind = TOK_STAR; break;
                case '/': kind = TOK_SLASH; break;
                case '(': kind = TOK_LPAREN; break;
                case ')': kind = TOK_RPAREN; break;
                case '=': kind = TOK_EQUALS; break;
                case ';': kind = TOK_SEMI; break;
                case ':': kind = TOK_COLON; break;
                case '.': kind = TOK_DOT; break;
                case '[': kind = TOK_LBRACKET; break;
                case ']': kind = TOK_RBRACKET; break;
                case '<': kind = TOK_LT; break;
                case '>': kind = TOK_GT; break;
                case '!': kind = TOK_BANG; break;
                case '?': kind = TOK_QUESTION; break;
                case ',': kind = TOK_COMMA; break;
                case '@': kind = TOK_AT; break;
                default:
                    fprintf(stderr, "asnix: unrecognized character '%c' in source\n", c);
                    exit(1);
            }
        }
        lx->pos = tok_start + width;
        return mk_tok(kind, tok_start, tok_start + width);
    }
}

Token lexer_next(Lexer *lx) {
    if (lx->has_pending) {
        lx->has_pending = 0;
        return lx->pending;
    }
    LexMode mode = mode_top(lx);
    if (mode == MODE_NORMAL) return lex_normal(lx);
    if (mode == MODE_STRING) return lex_dquote_run(lx);
    if (mode == MODE_ISTRING) return lex_indented_run(lx);
    return lex_path_run(lx);
}

static Lexer *lexer_new_ex(const char *src, int lenient) {
    Lexer *lx = malloc(sizeof(Lexer));
    lx->src = src;
    lx->pos = 0;
    lx->modes = NULL;
    lx->modes_len = 0;
    lx->modes_cap = 0;
    mode_push(lx, MODE_NORMAL);
    lx->has_pending = 0;
    lx->lenient = lenient;
    return lx;
}

Lexer *lexer_new(const char *src) {
    return lexer_new_ex(src, 0);
}

Lexer *lexer_new_lenient(const char *src) {
    return lexer_new_ex(src, 1);
}

void lexer_free(Lexer *lx) {
    free(lx->modes);
    free(lx);
}

LexerSave lexer_save(Lexer *lx) {
    return (LexerSave){
        .pos = lx->pos, .modes_len = lx->modes_len,
        .pending = lx->pending, .has_pending = lx->has_pending };
}

void lexer_restore(Lexer *lx, LexerSave save) {
    lx->pos = save.pos;
    lx->modes_len = save.modes_len;
    lx->cur = lx->modes[lx->modes_len - 1];
    lx->pending = save.pending;
    lx->has_pending = save.has_pending;
}

Token *asnix_lex(const char *src, size_t *out_count) {
    keyword_lookup_selfcheck();
    Lexer *lx = lexer_new(src);
    size_t src_len = strlen(src);
    size_t cap = src_len / 3 + 32, len = 0;
    Token *out = malloc(cap * sizeof(Token));
    for (;;) {
        Token t = lexer_next(lx);
        if (len == cap) {
            cap *= 2;
            out = realloc(out, cap * sizeof(Token));
        }
        out[len++] = t;
        if (t.kind == TOK_EOF) break;
    }
    lexer_free(lx);
    *out_count = len;
    return out;
}

