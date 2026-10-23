#include "lexer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* perf: one 256-entry character-classification table, replacing the
 * isalnum()/isalpha()/isdigit()/isspace() calls (each a ctype-table load
 * plus a bit test) chained with up to four more byte-compares that
 * path_char/ident_cont/uri_scheme_char/uri_tail_char below used to run
 * from scratch on every single call. Confirmed via `perf annotate
 * asnix_lex` on a standalone lex-only microbenchmark (all-packages.nix,
 * 300+ iterations): path_char alone (inlined into try_scan_path's
 * per-character scan loop, which runs on every leading character of
 * every identifier/number/operator token, not just real path literals)
 * was ~15-20% of asnix_lex's own self time, and try_scan_uri/
 * uri_scheme_char another ~9-10% -- almost entirely from re-deriving
 * each byte's classification one comparison at a time instead of a
 * single table lookup. Every CC_* bit here mirrors one of this file's
 * EXISTING predicates byte-for-byte (not a new/changed character set --
 * a pure speed change): this project never calls setlocale(), so the
 * "C" locale's ASCII-only isalnum/isalpha/isdigit/isspace behavior these
 * predicates relied on is a fixed fact for the whole process lifetime,
 * safe to precompute once into a table instead of recomputing per call.
 * Verified by an exhaustive 256-value comparison against the original
 * isalnum()-based bodies (every byte 0-255) before this table replaced
 * them -- see this milestone's own commit message for the standalone
 * check used (a throwaway harness, not shipped, since this file has no
 * existing unit-test binary of its own to extend; the real regression
 * net here is this file's own thorough use by every test in `make
 * test-e2e`/`test-conformance`/`test-nix-conformance`, which lex every
 * byte of every fixture through this exact table). */
enum {
    CC_SPACE      = 1 << 0, /* isspace: ' ' '\t' '\n' '\v' '\f' '\r' */
    CC_DIGIT      = 1 << 1, /* isdigit: '0'-'9' */
    CC_ALPHA      = 1 << 2, /* isalpha: 'A'-'Z' 'a'-'z' */
    CC_IDENT_CONT = 1 << 3, /* ident_cont's old body: alnum, '_', '\'', '-' */
    CC_PATH_CHAR  = 1 << 4, /* path_char's old body: alnum, '.', '_', '-', '+' */
    CC_URI_SCHEME = 1 << 5, /* uri_scheme_char's old body: alnum, '+', '-', '.' */
    CC_URI_TAIL   = 1 << 6, /* uri_tail_char's old body: alnum, or one of
                              * % / ? : @ & = + $ , _ . ! ~ * ' - */
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

/* Table-backed drop-in replacements for the plain isdigit()/isspace()/
 * isalpha() calls this file used at every digit-run/trivia/URI-entry
 * check -- same CHAR_CLASS table as path_char/ident_cont/uri_scheme_char/
 * uri_tail_char above, see this file's own top-of-file comment. */
static int cc_digit(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_DIGIT) != 0; }
static int cc_space(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_SPACE) != 0; }
static int cc_alpha(char c) { return (CHAR_CLASS[(unsigned char)c] & CC_ALPHA) != 0; }

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
    return (CHAR_CLASS[(unsigned char)c] & CC_PATH_CHAR) != 0;
}

/* True iff src[j..] is exactly "${" -- the interpolation-hole marker
 * (M64). Shared by scan_path_groups below and the main dispatch loop's
 * path-literal cases (plain, absolute, and `~`/home). */
static int at_dollar_brace(const char *src, size_t j) {
    return src[j] == '$' && src[j + 1] == '{';
}

/* Scans zero or more "/PATH_CHAR+" groups starting at *j (which must
 * already be positioned past any PATH_CHAR* prefix -- i.e. the very next
 * decision is "is src[j] a '/'?"), mirroring real Nix's own PATH/HPATH
 * lexer.l grammar's group-loop, generalized with ONE addition: a '/'
 * immediately followed by "${" ends the scan right there (consuming that
 * slash, even though zero path_chars followed it) instead of just
 * stopping -- matching real Nix's PATH_SEG/HPATH_START-before-
 * interpolation special lexer rule (needed for `./${foo}`/`~/${foo}`,
 * M64, see docs/architecture.md). Deliberately does NOT special-case a
 * trailing bare '/' with nothing useful after it (no "${", no further
 * path chars) -- that's left for the caller to optionally consume as the
 * grammar's own separate trailing `/?`, gated on `*saw_group` already
 * being true, so a `//` (update operator) immediately after an
 * as-yet-ungrouped position is never miscounted as a path (critical: if
 * this function consumed an ungrouped trailing '/' unconditionally, "a
 * // b" would mis-lex its first '/' as a 1-char path and its second '/'
 * as another, silently breaking TOK_SLASHSLASH detection). Returns the
 * new position. */
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

/* Returns the length of a path literal starting at src[i], or 0 if this
 * position isn't one (in which case the caller falls through to ordinary
 * digit/ident/single-char scanning). M64: also succeeds on a prefix that
 * ends right before "${" (e.g. "./" in "./${foo}", or "/" in "/${foo}"),
 * even though that prefix alone wouldn't otherwise satisfy the "at least
 * one full /PATH_CHAR+ group" rule -- see scan_path_groups. The caller
 * (asnix_lex's main loop) is responsible for noticing that case (checking
 * at_dollar_brace right after this returns) and switching into MODE_PATH
 * instead of emitting one complete, standalone TOK_PATH. */
static size_t try_scan_path(const char *src, size_t i) {
    size_t j = i;
    while (path_char(src[j])) j++;
    int saw_group = 0;
    j = scan_path_groups(src, j, &saw_group);
    if (!saw_group) return 0;
    if (src[j] == '/') j++; /* optional trailing slash -- rejected later, in the compiler,
                              * unless interpolation continues right after it */
    return j - i;
}

/* `~`-prefixed ("home") path literal (M64): unlike a plain path, there's
 * no PATH_CHAR* prefix allowed before the first '/' at all -- real Nix's
 * HPATH grammar is exactly `~(/PATH_CHAR+)+/?`, so this only looks for a
 * literal "~/" at src[i] and then reuses scan_path_groups starting right
 * at that '/'. Returns the full token length (including the leading
 * '~'), or 0 if src[i] isn't the start of a valid HPATH (e.g. a bare "~"
 * not followed by '/', or "~/" followed by neither a path char nor
 * "${" -- in which case the caller leaves `i` untouched and falls
 * through to ordinary single-char scanning, same as every other
 * not-actually-a-token probe in this lexer). */
static size_t try_scan_home_path(const char *src, size_t i) {
    if (src[i] != '~' || src[i + 1] != '/') return 0;
    int saw_group = 0;
    size_t j = scan_path_groups(src, i + 1, &saw_group);
    if (!saw_group) return 0;
    if (src[j] == '/') j++; /* optional trailing slash, same rule as try_scan_path */
    return j - i;
}

/* Bare URL literals (deprecated-but-default-accepted real-Nix syntax --
 * `eval-okay-url-literal-default.nix`'s own comment: "By default (no
 * flags), unquoted URL literals are accepted"). Ported directly from
 * real Nix's own lexer.l `URI` regex:
 *   [a-zA-Z][a-zA-Z0-9\+\-\.]*\:[a-zA-Z0-9\%\/\?\:\@\&\=\+\$\,\_\.\!\~\*\'\-]+
 * Tried in the main loop BEFORE ordinary identifier/keyword scanning,
 * same "longest match wins" precedence flex itself gives this rule over
 * ID: e.g. `x:x+1` lexes as ONE URI token "x:x+1", not as a lambda
 * `x: x+1` -- a well-known real-Nix footgun, verified directly against
 * `nix-instantiate --eval` (`(x:x+1) 5` fails with "attempt to call
 * something which is not a function but a string"). Checked the only
 * two *.nix drivers this project's own derivation-hashing work depends
 * on (d.nix/e.nix) for any whitespace-free `ident:ident` lambda that
 * this might newly misparse -- none found. */
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
    if (k == tail_start) return 0; /* need >=1 tail char after the ':' */
    return k - i;
}

static int ident_start(char c) {
    return cc_alpha(c) || c == '_';
}
static int ident_cont(char c) {
    /* M23: now INCLUDES '-' -- real Nix's own identifier regex is
     * `[a-zA-Z_][a-zA-Z0-9_'-]*`, i.e. a hyphen is an ordinary identifier
     * character (not just special attrpath-binding syntax), verified
     * directly against `nix-instantiate --eval`: `let flat-md5 = 1; in
     * flat-md5` succeeds (one identifier), and critically `a-b` (NO
     * surrounding whitespace) is itself one identifier/undefined-variable
     * lookup, NOT `a - b` subtraction -- subtraction only "wins" when at
     * least one side has separating whitespace (`a -b`, `a- b` is actually
     * `a-` as a dangling identifier, `a - b`), by ordinary maximal-munch
     * tokenization (same principle as C's `a+++b`). This was a deliberate
     * M4-era scope cut (previously excluded exactly to dodge this
     * ambiguity in the easy, unambiguous way) but real-world Nix source
     * uses hyphenated attribute names constantly (`drv-fixed-output.nix`'s
     * `flat-md5`, nixpkgs's own package/attribute names) -- found blocking
     * numtide/nix-conformance's eval-okay-drv-fixed-output.nix. No existing
     * asnix fixture relied on whitespace-free `a-b` meaning subtraction
     * (checked: every existing `-` arithmetic use already has at least one
     * space), so this is a pure feature add, not a documented-behavior
     * change to anything previously exercised. */
    return (CHAR_CLASS[(unsigned char)c] & CC_IDENT_CONT) != 0;
}

typedef struct {
    const char *kw;
    TokKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    { "let", TOK_LET },         { "in", TOK_IN },     { "if", TOK_IF },
    { "then", TOK_THEN },       { "else", TOK_ELSE },  { "true", TOK_TRUE },
    { "false", TOK_FALSE },     { "null", TOK_NULL },
    { "rec", TOK_REC },         { "inherit", TOK_INHERIT },
    { "assert", TOK_ASSERT },   { "with", TOK_WITH },
    { "or", TOK_OR },
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

/* M64: scans one run of path-literal content starting at *i (positioned
 * just after a closed "${ ... }" interpolation inside a path literal
 * whose FIRST chunk already established it continues past one -- see the
 * MODE_PATH push sites in asnix_lex's main loop above), up to the next
 * boundary: either another "${" or the first character that isn't a
 * path continuation at all (ending the whole literal). Pushes exactly
 * one TOK_PATH_PART (the literal chunk just scanned, possibly
 * zero-length -- e.g. between two adjacent interpolations, same
 * allowance lex_dquote_run makes for TOK_STR_PART) followed by the
 * boundary token (TOK_DOLLAR_LBRACE or TOK_PATH_END). Unlike
 * lex_dquote_run/lex_indented_run, there is no escape processing at all
 * in path-literal syntax (no backslash is ever a path_char), so the
 * chunk can point straight into the original source buffer instead of
 * a decoded copy -- no ByteBuf needed. A chunk may start with plain
 * path_chars, a '/'-group run, or both, exactly like a fresh path
 * literal's own scan (reuses scan_path_groups the same way
 * try_scan_path does), EXCEPT it's never required to contain any
 * group at all (a bare trailing `-b`-style run with no slash, like
 * `./a-${foo}-b`'s "-b", is perfectly valid here -- only the very
 * FIRST chunk of a path literal needs that requirement, to tell a path
 * apart from "not a path at all"). */
static void lex_path_run(const char *src, size_t *i, ModeStack *modes, TokVec *v) {
    size_t start = *i;
    size_t j = *i;
    while (path_char(src[j])) j++;
    int saw_group = 0;
    j = scan_path_groups(src, j, &saw_group);
    if (saw_group && src[j] == '/') j++; /* optional trailing slash, same rule as try_scan_path */
    *i = j;

    if (at_dollar_brace(src, *i)) {
        push_tok(v, (Token){ .kind = TOK_PATH_PART, .start = src + start, .len = *i - start });
        *i += 2;
        mode_push(modes, MODE_NORMAL); /* MODE_PATH underneath is left in place -- the matching
                                         * '}' pops back to it, re-entering this same function */
        push_tok(v, (Token){ .kind = TOK_DOLLAR_LBRACE });
        return;
    }
    push_tok(v, (Token){ .kind = TOK_PATH_PART, .start = src + start, .len = *i - start });
    mode_pop(modes);
    push_tok(v, (Token){ .kind = TOK_PATH_END });
}

/* Shared tail-scan + strtod for a float literal already known to match
 * (either a digit run followed by '.', or a bare '.' followed by a
 * digit) -- see asnix_lex's two call sites above. `*i` arrives already
 * positioned just past the decisive '.'; this scans the fractional
 * digits and an optional exponent, then hands the whole [tok_start, *i)
 * span to strtod, mirroring real Nix's own lexer.l exactly ("errno = 0;
 * strtod(yytext, 0); if (errno != 0) throw ParseError"). Checked
 * directly against glibc: "1.0e-400" sets ERANGE (underflows to exactly
 * 0.0), "5.0e-324" sets ERANGE too (a nonzero subnormal result still
 * counts as underflow), "1.0e400" sets ERANGE (overflows to +-HUGE_VAL),
 * while "0.0e-400" does NOT (the mantissa is already exactly zero, so
 * there's no actual underflow to report) -- all four cross-checked
 * against numtide/nix-conformance's parse-fail-float-{overflow,
 * underflow,subnormal}.nix fixtures (and eval-okay-float-range.nix's own
 * `0.0e-400`/`0.0e5` cases) before trusting this. */
static double scan_float_tail_and_parse(const char *src, size_t tok_start, size_t *i) {
    while (cc_digit(src[*i])) (*i)++;
    if (src[*i] == 'e' || src[*i] == 'E') {
        size_t k = *i + 1;
        if (src[k] == '+' || src[k] == '-') k++;
        if (cc_digit(src[k])) {
            *i = k;
            while (cc_digit(src[*i])) (*i)++;
        } /* else: 'e'/'E' not actually followed by a valid exponent -- leave *i unchanged, unconsumed */
    }
    size_t len = *i - tok_start;
    char *buf = malloc(len + 1);
    memcpy(buf, src + tok_start, len);
    buf[len] = '\0';
    errno = 0;
    double d = strtod(buf, NULL);
    if (errno == ERANGE) {
        fprintf(stderr, "asnix: invalid float '%s'\n", buf);
        exit(1);
    }
    free(buf);
    return d;
}

static void lex_until(const char *src, size_t *i_inout, ModeStack *modes, TokVec *v, size_t min_len);

Token *asnix_lex(const char *src, size_t *out_count) {
    /* perf: presize the token array from the source length instead of
     * letting push_tok's cap-doubling start from scratch (0->64->128->...)
     * on every single file. nixpkgs-style imports touch over a thousand
     * separate .nix files in one eval (1057 on the real d.nix/ffmpeg
     * closure), each paying its own doubling sequence -- every doubling
     * step mallocs a brand-new, bigger block and memcpy's the old live
     * tokens into it. One upfront malloc sized off strlen(src) (measured
     * ~8.6 real source bytes per token on that same corpus; /3 here is a
     * generous safety margin, not the measured average) cuts the
     * per-file realloc count from an average of ~3.5 down to 0 (measured
     * directly: ASNIX_LEX_STATS-instrumented build, 3688 reallocs across
     * 1057 files before this change, 0 after) and gives a real, repeated
     * ~10% wall-clock win on d.nix (perf stat -r 5: 1.893s -> 1.704s).
     * NOTE, confirmed by direct measurement, so a future reader doesn't
     * re-derive the wrong mental model from this comment alone: this is
     * NOT a page-fault fix -- total page faults barely move (174,407 ->
     * 173,303, -0.6%) because glibc's free-list reuse mostly satisfies a
     * growing buffer's reallocs from already-first-touched heap space,
     * not fresh pages. The win here is pure saved CPU (fewer realloc /
     * memcpy calls), not fewer faults. push_tok's existing doubling still
     * kicks in as a fallback for any file whose token density exceeds the
     * estimate, so this is purely an initial-capacity tweak, not a
     * behavior change. */
    size_t src_len = strlen(src);
    TokVec v = { .toks = malloc((src_len / 3 + 32) * sizeof(Token)), .len = 0, .cap = src_len / 3 + 32 };
    ModeStack modes = { 0 };
    mode_push(&modes, MODE_NORMAL);
    size_t i = 0;
    lex_until(src, &i, &modes, &v, SIZE_MAX);
    free(modes.modes);
    *out_count = v.len;
    return v.toks;
}

/* M135: fuses lexing and parsing into one interleaved pass instead of two
 * sequential ones (docs/architecture.md's M134 finding: asnix used to lex
 * the whole file into an array, THEN hand it to the parser -- a full write
 * pass followed by a full read pass over every token, with no equivalent
 * boundary in iets's pull-based lexer/parser). Called with min_len =
 * SIZE_MAX (asnix_lex above) this behaves exactly as the lexer always did:
 * run to EOF in one call. Called with a finite min_len (lexcur_tok below,
 * the parser's own on-demand driver) it stops the instant v->len tokens are
 * available and picks back up exactly where it left off on the next call,
 * via the same (i, modes, v) triple threaded through by the caller -- same
 * per-token scanning code, same output, just possibly paused partway
 * through. */
static void lex_until(const char *src, size_t *i_inout, ModeStack *modes, TokVec *v, size_t min_len) {
    size_t i = *i_inout;
    for (;;) {
        if (v->len > min_len) { *i_inout = i; return; }
        LexMode mode = mode_top(modes);

        if (mode == MODE_STRING) {
            lex_dquote_run(src, &i, modes, v);
            continue;
        }
        if (mode == MODE_ISTRING) {
            lex_indented_run(src, &i, modes, v);
            continue;
        }
        if (mode == MODE_PATH) {
            lex_path_run(src, &i, modes, v);
            continue;
        }

        /* mode == MODE_NORMAL: ordinary expression/identifier/operator lexing. */
        char c = src[i];
        size_t tok_start = i; /* byte offset of whatever token (if any) starts here */

        if (c == '\0') {
            if (modes->len != 1) {
                fprintf(stderr, "asnix: unexpected end of input inside '${ ... }'\n");
                exit(1);
            }
            break;
        }

        if (cc_space(c)) {
            i++;
            continue;
        }
        if (c == '#') { /* line comment, a harmless real-Nix-ism to allow in fixtures */
            while (src[i] && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && src[i + 1] == '*') { /* block comment -- real Nix files are full of these
                                               * (e.g. nixpkgs's pkgs/top-level/impure.nix opens with
                                               * one); previously unhandled entirely, so the lexer hit
                                               * "unrecognized character '/'" on the literal '/*' the
                                               * moment any real-world file was fed in -- found
                                               * evaluating real nixpkgs via d.nix, see
                                               * docs/architecture.md's Known Bugs list. Checked before
                                               * try_scan_path below: '/*' never matches a path literal
                                               * anyway ('*' isn't a path_char), but comments should
                                               * never compete with path-literal scanning regardless. */
            i += 2;
            while (src[i] && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (!src[i]) {
                fprintf(stderr, "asnix: unterminated block comment\n");
                exit(1);
            }
            i += 2;
            continue;
        }

        /* Tried before digit/string/single-char below -- see
         * try_scan_path's comment for why this has to come first. M64:
         * `~`-prefixed home paths are tried alongside (src[i]=='~' is
         * never itself a path_char, so try_scan_path can never fire at
         * this position on its own -- a separate probe is required). Both
         * branches below handle the "immediately followed by ${" case
         * identically: emit just this chunk as TOK_PATH (is_home set
         * accordingly), then push MODE_PATH (so the matching '}' that
         * closes the interpolation's expression returns here, not to
         * plain MODE_NORMAL) underneath a fresh MODE_NORMAL for the
         * expression itself, and emit TOK_DOLLAR_LBRACE -- exactly the
         * same push-two-modes-and-emit-the-boundary-token shape
         * lex_dquote_run/lex_indented_run already use for "${" inside a
         * string.
         *
         * M138: unlike the general try_scan_path call a few lines below
         * (kept unconditional, see that call site's own comment),
         * try_scan_home_path is left unconditional here too -- it's an
         * instant one-branch bail for any non-'~' leading byte, nothing
         * to guard. */
        {
            size_t hlen = try_scan_home_path(src, i);
            if (hlen > 0) {
                push_tok(v, (Token){ .kind = TOK_PATH, .start = src + i, .len = hlen, .pos = src + tok_start, .is_home = 1 });
                i += hlen;
                if (at_dollar_brace(src, i)) {
                    i += 2;
                    mode_push(modes, MODE_PATH);
                    mode_push(modes, MODE_NORMAL);
                    push_tok(v, (Token){ .kind = TOK_DOLLAR_LBRACE });
                }
                continue;
            }
        }

        /* M138: identifier-leading tokens get their own guarded path/URI
         * check instead of falling into the unconditional try_scan_path
         * (and, for alpha leads, try_scan_uri) calls below -- the second
         * lexer.c lever flagged as a follow-on in M129 (see that
         * section's "Checked against iets" paragraph) and in iets's own
         * `next_token` (~/gnix/iets/src/syntax/lexer.rs, the
         * `b'a'..=b'z' | b'A'..=b'Z' | b'_'` match arm): scan the
         * ordinary identifier's own ident_cont run ONCE (needed anyway to
         * produce a plain TOK_IDENT/keyword), then make the expensive
         * path_char-run-plus-group scan (try_scan_path) and URI scan
         * (try_scan_uri) conditional on a cheap single-byte peek at the
         * character immediately following that run, instead of always
         * attempting both scans first and then re-walking the exact same
         * bytes a second time via ident_cont moments later whenever they
         * fail (which is every time for an ordinary identifier/keyword --
         * by far the most common token in real Nix source).
         *
         * Correctness (the edge case M129 flagged and this milestone's
         * commit message verifies): ident_cont and path_char/
         * uri_scheme_char are NOT the same character set. '.' and '+' are
         * path_char/uri_scheme_char but NOT ident_cont -- e.g. a token
         * shaped like "a.b:foo" (a URI with a scheme containing '.') or
         * "foo.bar/baz" (a path literal with '.' in its prefix) needs the
         * real path_char/uri_scheme_char run, which keeps going past
         * where ident_cont already stopped, checked -- not just where the
         * identifier's own run ends. Conversely, '\'' is ident_cont but
         * NOT path_char/uri_scheme_char.
         *
         * The guard below fires the real (expensive) rescan -- from this
         * token's own start, not from the ident_cont run's end -- whenever
         * the byte right after the ident_cont run is '/' (direct
         * path-group signal), ':' (direct URI-colon signal), or '.'/'+'
         * (the ident_cont run stopped short of where path_char/
         * uri_scheme_char would keep going -- ambiguous without the real
         * rescan, exactly the "a.b:foo"/"foo.bar/baz" case above).
         * Whenever NONE of those four hold, skipping both scans entirely
         * is PROVABLY equivalent to running them and having them both
         * fail, not merely a fast-path approximation:
         *   - Neither '/' nor ':' can appear anywhere strictly before the
         *     peeked byte: both already stop an ident_cont run outright
         *     (neither is in CC_IDENT_CONT), so if either had appeared
         *     earlier, the run would never have reached this far.
         *   - If the ident_cont run contains no '\'' (the overwhelmingly
         *     common case), the path_char/uri_scheme_char run from this
         *     token's start tracks the ident_cont run exactly on their
         *     shared subset (alnum/'_'/'-') and can only diverge AT the
         *     peeked byte itself, via '.'/'+' -- already excluded by the
         *     guard's own condition, so it provably stops at that same
         *     byte too.
         *   - If the ident_cont run DOES contain a '\'' before the peeked
         *     byte (rare -- a primed identifier like `lib'`), the real
         *     path_char/uri_scheme_char run stops AT that '\'' instead,
         *     long before the peeked byte -- but scan_path_groups/the
         *     URI-colon check both require that exact stopping byte to be
         *     '/' or ':' respectively, and a '\'' is neither, so both
         *     scans are guaranteed to fail there regardless of what the
         *     peeked byte is. Skipping is still safe.
         * Every CC_* set used here is the same table M129 already built;
         * this milestone reuses it, not a new predicate. */
        if (ident_start(c)) {
            size_t j = i + 1;
            while (ident_cont(src[j])) j++;
            char nb = src[j];
            if (nb == '/' || nb == ':' || nb == '.' || nb == '+') {
                size_t plen = try_scan_path(src, i);
                if (plen > 0) {
                    push_tok(v, (Token){ .kind = TOK_PATH, .start = src + i, .len = plen, .pos = src + tok_start });
                    i += plen;
                    if (at_dollar_brace(src, i)) {
                        i += 2;
                        mode_push(modes, MODE_PATH);
                        mode_push(modes, MODE_NORMAL);
                        push_tok(v, (Token){ .kind = TOK_DOLLAR_LBRACE });
                    }
                    continue;
                }
                size_t ulen = try_scan_uri(src, i);
                if (ulen > 0) {
                    push_tok(v, (Token){ .kind = TOK_URI, .start = src + i, .len = ulen, .pos = src + tok_start });
                    i += ulen;
                    continue;
                }
            }
            /* Plain identifier or keyword: [i, j), with j already computed
             * above by the ident_cont scan this guard needed anyway -- no
             * second ident_cont walk later in this function (see the old
             * ident_start block this replaced, now removed). */
            size_t start = i;
            size_t len = j - start;
            TokKind kind = TOK_IDENT;
            for (size_t k = 0; k < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); k++) {
                size_t kwlen = strlen(KEYWORDS[k].kw);
                if (kwlen == len && memcmp(KEYWORDS[k].kw, src + start, len) == 0) {
                    kind = KEYWORDS[k].kind;
                    break;
                }
            }
            push_tok(v, (Token){ .kind = kind, .start = src + start, .len = len, .pos = src + tok_start });
            i = j;
            continue;
        }

        /* Not an identifier lead (handled, with its own guard, just above):
         * digit/'.'/'-'/'+'-led path scanning stays unconditional, exactly
         * as before this milestone -- iets doesn't bother guarding these
         * either (see its `or_float_or_path`'s own unconditional
         * `scan_path_token` call, and the direct `scan_path_token` calls in
         * its '.'/'-'/'+' match arms), since a digit/operator run is
         * typically only 1-4 bytes before failing path_char, unlike an
         * identifier's potentially much longer run -- not worth the extra
         * branch/complexity for a near-zero win. */
        {
            size_t plen = try_scan_path(src, i);
            if (plen > 0) {
                push_tok(v, (Token){ .kind = TOK_PATH, .start = src + i, .len = plen, .pos = src + tok_start });
                i += plen;
                if (at_dollar_brace(src, i)) {
                    i += 2;
                    mode_push(modes, MODE_PATH);
                    mode_push(modes, MODE_NORMAL);
                    push_tok(v, (Token){ .kind = TOK_DOLLAR_LBRACE });
                }
                continue;
            }
        }

        if (c == '<') {
            /* `<nixpkgs>`-style NIX_PATH search-path literal -- but only
             * when the lookahead actually matches `<name...>`; `<` is also
             * the less-than comparison operator (merged separately, see
             * the two-char-operator block below), so a non-matching `<`
             * must fall through to ordinary operator lexing instead of
             * erroring here. */
            size_t start = i + 1;
            size_t k = start;
            while (path_char(src[k]) || src[k] == '/') k++;
            if (k > start && src[k] == '>') {
                push_tok(v, (Token){ .kind = TOK_SEARCH_PATH, .start = src + start, .len = k - start, .pos = src + tok_start });
                i = k + 1;
                continue;
            }
        }

        if (cc_digit(c)) {
            size_t start = i;
            while (cc_digit(src[i])) i++;
            size_t int_len = i - start;
            /* M51: real Nix's FLOAT grammar (grepped directly from
             * ~/nix's src/libexpr/lexer.l):
             *   (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?
             * i.e. a float literal ALWAYS needs a literal '.' somewhere --
             * there is no bare-exponent float like "1e10" (confirmed:
             * `nix-instantiate --eval` on that input errors "undefined
             * variable 'e10'", i.e. it lexes as INT 1 then IDENT e10, NOT
             * a float). Applied to the digit run just scanned: a run
             * starting with a nonzero digit may be followed by '.' and
             * ANY number (including zero) of further digits ("10." is a
             * valid float with zero fractional digits -- verified
             * against nix-instantiate); a run that's the single digit
             * "0" may be followed by '.' only if at least one digit
             * follows the dot ("0." is NOT a float -- verified: that
             * lexes as INT 0 then DOT, same as before this milestone); a
             * multi-digit run starting with '0' (e.g. "00.5") matches
             * neither alternative and is left as plain INT "00" followed
             * by ordinary DOT/INT lexing, an edge case no sane digit
             * literal hits anyway. */
            int is_float = 0;
            if (src[i] == '.') {
                if (int_len == 1 && src[start] == '0') {
                    if (cc_digit(src[i + 1])) is_float = 1;
                } else if (src[start] != '0') {
                    is_float = 1;
                }
            }
            if (is_float) {
                i++; /* consume '.' */
                double d = scan_float_tail_and_parse(src, start, &i);
                push_tok(v, (Token){ .kind = TOK_FLOAT, .float_val = d, .pos = src + tok_start });
                continue;
            }
            Token t = { .kind = TOK_INT, .int_val = strtoll(src + start, NULL, 10), .pos = src + tok_start };
            push_tok(v, t);
            continue;
        }

        /* M51: a float literal with no leading digit at all -- the
         * `\.[0-9]+` shape of real Nix's `0?\.[0-9]+` alternative (the
         * "0?" is optional, so a bare ".5" is valid, verified against
         * nix-instantiate: `.5` == `0.5`). Checked before the ELLIPSIS/
         * plain-DOT handling below, since this starts with the same '.'
         * character. */
        if (c == '.' && cc_digit(src[i + 1])) {
            size_t start = i;
            i++; /* consume '.' */
            double d = scan_float_tail_and_parse(src, start, &i);
            push_tok(v, (Token){ .kind = TOK_FLOAT, .float_val = d, .pos = src + tok_start });
            continue;
        }

        if (c == '"') {
            i++;
            mode_push(modes, MODE_STRING);
            push_tok(v, (Token){ .kind = TOK_STR_START });
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
            mode_push(modes, MODE_ISTRING);
            push_tok(v, (Token){ .kind = TOK_ISTR_START });
            continue;
        }

        /* `${expr}` directly in normal-mode code (not inside a string) --
         * real Nix's dynamic attribute syntax, `set.${name}` (checked
         * before `{` below, which it would otherwise shadow). Reuses the
         * exact same TOK_DOLLAR_LBRACE/mode-push-MODE_NORMAL mechanism
         * string interpolation already uses (lex_dquote_run/
         * lex_indented_run above): the matching '}' pops back out via the
         * ordinary '}' handling just below, same as any other nested
         * '{'. Previously `$` had no meaning at all outside an active
         * string mode, so this hit "unrecognized character '$'" -- found
         * evaluating real nixpkgs (`pkgs/top-level/default.nix` uses
         * `set.${name}`), see docs/architecture.md's Known Bugs list. */
        if (c == '$' && src[i + 1] == '{') {
            i += 2;
            mode_push(modes, MODE_NORMAL);
            push_tok(v, (Token){ .kind = TOK_DOLLAR_LBRACE });
            continue;
        }

        if (c == '{') {
            i++;
            mode_push(modes, MODE_NORMAL);
            push_tok(v, (Token){ .kind = TOK_LBRACE });
            continue;
        }
        if (c == '}') {
            if (modes->len == 1) {
                fprintf(stderr, "asnix: unmatched '}'\n");
                exit(1);
            }
            i++;
            mode_pop(modes);
            push_tok(v, (Token){ .kind = TOK_RBRACE, .pos = src + tok_start });
            continue;
        }

        /* M138: the ident_start(c) branch that used to live here (plain
         * TOK_IDENT/keyword scanning, re-walking ident_cont from scratch)
         * was moved up above, merged into the new guarded path/URI check --
         * ident_start(c) can never reach this point anymore (that earlier
         * branch always `continue`s). */

        /* `...` (lambda-pattern ellipsis) is checked before any single- or
         * double-character operator, same lookahead style as the two-char
         * operators below. */
        if (c == '.' && src[i + 1] == '.' && src[i + 2] == '.') {
            push_tok(v, (Token){ .kind = TOK_ELLIPSIS, .pos = src + tok_start });
            i += 3;
            continue;
        }

        /* Two-character operators are checked before falling back to their
         * single-character meaning (or to "unrecognized character" for
         * '&'/'|'/'/' alone, none of which asnix gives a standalone
         * meaning to -- real Nix doesn't either, except '/' as part of a
         * path literal, not supported yet). `{`/`}` are handled earlier
         * (mode-stack-aware, see above), so they never reach this switch. */
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
                case '/': kind = TOK_SLASH; break; /* reached only when both try_scan_path
                                                     * (above) and the '//' two-char check
                                                     * just above already ruled this '/' out
                                                     * as a path literal or update operator --
                                                     * see TOK_SLASH's own comment (lexer.h). */
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
        push_tok(v, (Token){ .kind = kind, .pos = src + tok_start });
        i += width;
    }

    push_tok(v, (Token){ .kind = TOK_EOF, .pos = src + i });
    *i_inout = i;
}

/* M135: the parser's own on-demand lexing driver (see lex_until's comment
 * above) -- bundles exactly the (src, i, modes, v) state asnix_lex used to
 * keep as locals into one persistent, heap-allocated cursor so repeated
 * lexcur_tok() calls can resume lexing across many separate parser calls
 * instead of running to completion in one shot. */
LexCursor *lexcur_new(const char *src) {
    LexCursor *lc = malloc(sizeof(LexCursor));
    size_t src_len = strlen(src);
    lc->src = src;
    lc->i = 0;
    lc->modes = (ModeStack){ 0 };
    mode_push(&lc->modes, MODE_NORMAL);
    lc->v = (TokVec){ .toks = malloc((src_len / 3 + 32) * sizeof(Token)), .len = 0, .cap = src_len / 3 + 32 };
    return lc;
}

/* Ensures toks[0..idx] have all been lexed (lexing forward from wherever
 * the cursor last stopped, never backward, never re-lexing an already-
 * produced token) and returns a pointer to toks[idx]. idx must be at most
 * the eventual TOK_EOF's own index -- every caller (parser.c's cur/peek2/
 * find_matching_rbrace) already only ever asks for indices up to and
 * including whatever TOK_EOF it has already seen, exactly as it did
 * against the old eagerly-filled array. */
Token *lexcur_tok_slow(LexCursor *lc, size_t idx) {
    lex_until(lc->src, &lc->i, &lc->modes, &lc->v, idx);
    return &lc->v.toks[idx];
}

void lexcur_free(LexCursor *lc) {
    free(lc->modes.modes);
    free(lc->v.toks);
    free(lc);
}
