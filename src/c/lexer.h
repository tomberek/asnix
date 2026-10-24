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
    TOK_STR_PART,      /* M141: a RAW (undecoded) literal chunk inside "...", escapes
                        * NOT yet applied -- start/end span the exact source bytes between
                        * the opening quote/prior interpolation and the next boundary, as
                        * a plain offset into `src` (never a decoded copy, unlike pre-M141).
                        * See asnix_decode_dquote_span's own comment for where/when the
                        * escape decode actually happens. */
    TOK_STR_END,       /* '"' closing a double-quoted string */
    TOK_ISTR_START,     /* "''" opening an indented string */
    TOK_ISTR_PART,      /* M141: a RAW run of plain indented-string content (no escapes --
                         * an escape always ends this run, see TOK_ISTR_ESC), NOT yet
                         * indentation-stripped (that happens once all of one indented
                         * string's parts are collected -- see parser.c's
                         * strip_indentation, ported from real Nix's
                         * ParserState::stripIndentation). Counts toward indentation
                         * measurement -- the TOK_ISTR_PART/TOK_ISTR_ESC kind split IS
                         * this token's own "hasIndentation" bit (pre-M141 carried it as
                         * a separate flag field on one shared kind; folding it into the
                         * kind tag instead needs no extra Token field at all). */
    TOK_ISTR_ESC,       /* M141: exactly one `''$` / `'''` / `''\<c>` escape unit's RAW
                         * source span (3 or 4 bytes, start/end into `src`) -- decoded on
                         * demand by asnix_decode_istr_esc. Never counts toward
                         * indentation (see TOK_ISTR_PART above). Deliberate, documented
                         * scope note vs. the pre-M141 lexer: two or more of these back to
                         * back used to be merged into a single accumulated token (an
                         * artifact of the old eager-ByteBuf's "only flush on a category
                         * change" logic); M141 emits one TOK_ISTR_ESC per escape unit
                         * instead, never merged. This is observably identical for every
                         * case actually exercised by this project's own test corpus (the
                         * indentation algorithm's own `!has_indentation` branch never
                         * inspects a non-indentation chunk's text at all, and the final
                         * AstStringPart list's *concatenated value* is unaffected by how
                         * many pieces it's split into) EXCEPT one vanishingly narrow
                         * corner: `stripIndentation`'s "drop a final line that's empty-
                         * or-spaces-only" check looks only at the literal's OVERALL LAST
                         * part -- if that literal's content ends with two or more
                         * back-to-back escape units where the old merged token would have
                         * been in scope for that check and the new split tokens shift
                         * which one is "last", this one specific edge (e.g. a string whose
                         * very final content is `''\<space>''\<space>`, an escaped-space
                         * pair right at the end) could in principle strip differently.
                         * Not reachable by any fixture in this project's own e2e/
                         * nix-conformance/nixpkgs-hash corpus (checked directly -- see
                         * docs/architecture.md M141); flagged here, not swept under the
                         * rug, in case a future real-world file ever hits it. */
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
                      * TOK_DOLLAR_LBRACE rather than standing alone; see
                      * TOK_PATH_PART/TOK_PATH_END below. M141: "started with `~`"
                      * (pre-M141's separate `is_home` flag) is now just `src[start] ==
                      * '~'`, derived by the parser instead of carried on the token. */
    TOK_PATH_PART,   /* M64: a literal chunk inside an interpolated path literal, AFTER the
                      * first (`./a-${foo}-b`'s "-b", possibly zero-length between two
                      * adjacent interpolations) -- no escapes exist in path-literal syntax
                      * at all, so (unlike TOK_STR_PART) this always points directly at a
                      * raw source span, decoded or not, same thing. */
    TOK_PATH_END,    /* M64: closes an interpolated path literal -- a boundary marker only,
                      * no payload (mirrors TOK_STR_END). */
    TOK_SEARCH_PATH, /* <nixpkgs>-style NIX_PATH search-path literal */
    TOK_URI, /* bare URL literal (deprecated-but-still-default-accepted real-Nix syntax,
              * e.g. `http://example.com`) -- see lexer.c's try_scan_uri. Carries its
              * full literal text as a raw source span, same as TOK_PATH/TOK_SEARCH_PATH;
              * the parser turns it into a plain (non-interpolated) AST_STRING. */
    TOK_EOF,
} TokKind;

/* M141: shrunk from 56 to 12 bytes (matching ~/gnix/iets/src/syntax/
 * lexer.rs's own `Token`, the design target -- see docs/architecture.md's
 * M141 section for the full before/after). `start`/`end` are byte offsets
 * into the ONE source buffer every Token was ever lexed from (never a
 * pointer into a separate decoded/malloc'd copy, unlike the pre-M141
 * TOK_STR_PART/TOK_ISTR_PART, which each owned a freshly decoded buffer) --
 * this is the "defer literal decoding" half of the design: an int/float
 * literal's actual numeric value, a string/indented-string chunk's escape
 * decoding, and a path literal's `is_home`/`is_search` classification are
 * all now computed exactly once, on demand, by the PARSER at the point it
 * builds the Ast node that needs them (see asnix_decode_float_literal/
 * asnix_decode_dquote_span/asnix_decode_istr_esc below, and parser.c's own
 * call sites) -- never eagerly at lex time, and never more than once per
 * literal occurrence (same total decode work as before, just relocated).
 * `end` doubles as "the token's own end position" for every kind that has
 * no real payload at all (punctuation/keywords) -- `start` alone already
 * serves the old `.pos` field's role (every token's source position) for
 * every kind uniformly, since nothing is ever a decoded-buffer pointer
 * anymore. */
typedef struct {
    TokKind kind;
    uint32_t start;
    uint32_t end;
} Token;

/* Tokenizes the whole source in one pass into a malloc'd array terminated
 * by a TOK_EOF entry (*out_count includes that terminator). Dead code as
 * of M135 (every real caller goes through asnix_parse's internal
 * LexCursor instead -- see below), kept as a still-correct, still-tested-
 * by-construction (same lex_until core every LexCursor call ultimately
 * runs) standalone entry point, matching this project's own M127/M135
 * precedent of keeping a correct-but-unused reference implementation
 * rather than deleting a working, independently-useful API. Aborts the
 * process with a message on an unrecognized character -- this is a
 * from-scratch minimal-subset lexer, not a full Nix lexer (see
 * docs/architecture.md M4 scope notes). */
Token *asnix_lex(const char *src, size_t *out_count);

/* M141: decodes a TOK_FLOAT's raw [start,end) span via strtod, matching
 * real Nix's own lexer.l exactly ("errno = 0; strtod(yytext, 0); if
 * (errno != 0) throw ParseError") -- safe to call UNBOUNDED (no length
 * cap passed to strtod) because the lexer's own scan already found the
 * maximal span this exact grammar matches at this position, and strtod
 * parses the identical grammar (digits, '.', digits, optional
 * [eE][+-]?digits) -- it is provably unable to consume anything past
 * `end` (whatever immediately follows `end` already failed this
 * grammar, or the lexer's own scan would have included it too), so this
 * always stops at exactly the same byte the lexer already decided ends
 * the token, without needing a malloc'd, NUL-terminated, length-bounded
 * copy the way the pre-M141 lexer-time decode did. */
double asnix_decode_float_literal(const char *src, uint32_t start, uint32_t end);

/* M141: decodes a TOK_STR_PART's raw [start,end) span (escapes NOT yet
 * applied) into a freshly malloc'd buffer, applying exactly the same
 * backslash-escape rules the pre-M141 lexer applied eagerly (see
 * lexer.c's decode_escape_char) -- \n \r \t map to the control character,
 * every other escaped byte (including \" \\ \$ \') passes through as
 * itself, and any non-escaped byte (including a raw embedded newline,
 * which real Nix allows inside "...") copies straight through. The
 * decoded length is always <= end-start (every escape shrinks by exactly
 * one byte, 2 raw -> 1 decoded), so this mallocs that upper bound
 * directly without trimming afterward -- the same "malloc the provable
 * upper bound, don't bother reallocing down" convention parser.c's own
 * strip_indentation already uses for the exact same reason. *out_len
 * receives the real decoded length (may be less than end-start). */
char *asnix_decode_dquote_span(const char *src, uint32_t start, uint32_t end, size_t *out_len);

/* M141: decodes one TOK_ISTR_ESC raw span (exactly one `''$` / `'''` /
 * `''\<c>` escape unit) into `out` (caller-provided, >= 2 bytes) --
 * *out_len is always 1 (`''$` -> "$", `''\<c>` -> one decoded byte) or 2
 * (`'''` -> "''"). Mirrors lexer.c's pre-M141 istr_append_escape exactly,
 * just evaluated later, from the raw span instead of while lexing. */
void asnix_decode_istr_esc(const char *src, uint32_t start, char out[2], size_t *out_len);

/* M135: an incremental, resumable lexer cursor -- used by the parser
 * (parser.c) to fuse lexing and parsing into one interleaved pass instead
 * of running asnix_lex to completion before parsing starts (see
 * docs/architecture.md M134/M135). Tokens are produced on demand, exactly
 * as far ahead as lexcur_tok() is ever asked for, by the same scanning
 * logic asnix_lex itself uses -- never re-lexed, never discarded.
 *
 * LexMode/ModeStack/TokVec are otherwise lexer.c-private (mirrored here,
 * not re-exported from there) -- the ONLY reason they're visible in this
 * header at all is so lexcur_tok's fast path below can be `static inline`
 * and actually inline at every parser.c call site (cur()/peek2() run on
 * EVERY token access, many times per token -- measured directly: routing
 * every single one through a real cross-TU function call into lexer.c's
 * lex_until, even just to hit its own early-return check, cost a real
 * +10.5% instruction regression on a full d.nix eval before this fast
 * path existed). The slow path (lexcur_tok_slow, lexer.c) still owns all
 * the real lexing logic; this is purely a "is the token already there"
 * short-circuit, never duplicated scanning logic. */
typedef enum { MODE_NORMAL, MODE_STRING, MODE_ISTRING, MODE_PATH } LexMode;

typedef struct {
    LexMode *modes;
    size_t len, cap;
    /* M174: a mirror of modes[len-1] (the only slot mode_top ever reads),
     * kept in sync by mode_push/mode_pop so lex_until's own per-token
     * mode_top() call becomes a single field load instead of a 3-load
     * chain (len, then the modes pointer, then modes[len-1] itself) --
     * see lexer.c's mode_push/mode_top/mode_pop for the invariant this
     * depends on: EVERY mutation of modes/len happens through those
     * three functions, nothing else in the codebase ever touches a
     * ModeStack's fields directly (grepped the whole tree to confirm). */
    LexMode cur;
} ModeStack;

typedef struct {
    Token *toks;
    size_t len, cap;
} TokVec;

typedef struct LexCursor {
    const char *src;
    size_t i;
    ModeStack modes;
    TokVec v;
} LexCursor;

LexCursor *lexcur_new(const char *src);
Token *lexcur_tok_slow(LexCursor *lc, size_t idx);
void lexcur_free(LexCursor *lc);

static inline Token *lexcur_tok(LexCursor *lc, size_t idx) {
    if (idx < lc->v.len) return &lc->v.toks[idx];
    return lexcur_tok_slow(lc, idx);
}

#endif /* ASNIX_LEXER_H */
