#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Ast **items;
    size_t len, cap;
} AstVec;

static void astvec_push(AstVec *v, Ast *item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof(Ast *));
    }
    v->items[v->len++] = item;
}

typedef struct {
    AstBinding *items;
    size_t len, cap;
} BindingVec;

static void bindingvec_push(BindingVec *v, AstBinding item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof(AstBinding));
    }
    v->items[v->len++] = item;
}

typedef struct {
    AstStringPart *items;
    size_t len, cap;
} PartVec;

static void partvec_push(PartVec *v, AstStringPart item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof(AstStringPart));
    }
    v->items[v->len++] = item;
}

typedef struct {
    AstFormal *items;
    size_t len, cap;
} FormalVec;

static void formalvec_push(FormalVec *v, AstFormal item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof(AstFormal));
    }
    v->items[v->len++] = item;
}

/* Raw piece of a '' ... '' literal as collected straight off the lexer,
 * before indentation stripping -- same shape as AstStringPart plus the
 * per-literal-chunk `has_indentation` flag (see lexer.h's TOK_ISTR_PART
 * comment). Kept separate from AstStringPart because that flag has no
 * meaning once stripping is done. */
typedef struct {
    int is_expr;
    const char *text;
    size_t text_len;
    int has_indentation;
    Ast *expr;
} RawIndPart;

typedef struct {
    RawIndPart *items;
    size_t len, cap;
} RawIndVec;

static void rawind_push(RawIndVec *v, RawIndPart item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof(RawIndPart));
    }
    v->items[v->len++] = item;
}

/* Ported directly from real Nix's ParserState::stripIndentation
 * (~/nix's src/libexpr/include/nix/expr/parser-state.hh), verified against
 * `nix-instantiate --eval` for plain indentation, varying per-line
 * indentation, and an all-whitespace trailing line (see tests/e2e). Two
 * passes over the raw parts: (1) find the minimum leading-space count
 * across every content line (a line ended by an escape or an
 * interpolation, not just a raw newline, still ends the "start of line"
 * run -- matching real Nix's `!str->hasIndentation` / expr handling); (2)
 * strip exactly that many leading spaces from every content line, and (for
 * a literal's very last raw part only) drop a final line that is entirely
 * spaces. */
static AstStringPart *strip_indentation(RawIndVec *raw, size_t *out_count) {
    size_t n = raw->len;
    if (n == 0) {
        AstStringPart *parts = malloc(sizeof(AstStringPart));
        parts[0] = (AstStringPart){ .is_expr = 0, .text = "", .text_len = 0 };
        *out_count = 1;
        return parts;
    }

    int at_start = 1;
    size_t min_indent = 1000000;
    size_t cur_indent = 0;
    for (size_t idx = 0; idx < n; idx++) {
        RawIndPart *p = &raw->items[idx];
        if (p->is_expr || !p->has_indentation) {
            if (at_start) {
                at_start = 0;
                if (cur_indent < min_indent) min_indent = cur_indent;
            }
            continue;
        }
        for (size_t j = 0; j < p->text_len; j++) {
            char ch = p->text[j];
            if (at_start) {
                if (ch == ' ') cur_indent++;
                else if (ch == '\n') cur_indent = 0;
                else {
                    at_start = 0;
                    if (cur_indent < min_indent) min_indent = cur_indent;
                }
            } else if (ch == '\n') {
                at_start = 1;
                cur_indent = 0;
            }
        }
    }

    AstStringPart *out = malloc(n * sizeof(AstStringPart)); /* upper bound: at most one output per input part */
    size_t result_count = 0;
    at_start = 1;
    size_t cur_dropped = 0;
    for (size_t idx = 0; idx < n; idx++) {
        RawIndPart *p = &raw->items[idx];
        if (p->is_expr) {
            at_start = 0;
            cur_dropped = 0;
            out[result_count++] = (AstStringPart){ .is_expr = 1, .expr = p->expr };
            continue;
        }

        char *s2 = malloc(p->text_len ? p->text_len : 1);
        size_t s2len = 0;
        for (size_t j = 0; j < p->text_len; j++) {
            char ch = p->text[j];
            if (at_start) {
                if (ch == ' ') {
                    if (cur_dropped++ >= min_indent) s2[s2len++] = ch;
                } else if (ch == '\n') {
                    cur_dropped = 0;
                    s2[s2len++] = ch;
                } else {
                    at_start = 0;
                    cur_dropped = 0;
                    s2[s2len++] = ch;
                }
            } else {
                s2[s2len++] = ch;
                if (ch == '\n') at_start = 1;
            }
        }

        /* Drop a final line that's empty-or-spaces-only, but only for the
         * literal's overall last raw part (matches upstream's `n == 1`
         * check, where its `n` is "parts remaining including this one",
         * i.e. true exactly on the last loop iteration). */
        if (idx == n - 1) {
            size_t last_nl = (size_t)-1;
            for (size_t k = 0; k < s2len; k++) if (s2[k] == '\n') last_nl = k;
            if (last_nl != (size_t)-1) {
                int all_spaces_after = 1;
                for (size_t k = last_nl + 1; k < s2len; k++) {
                    if (s2[k] != ' ') { all_spaces_after = 0; break; }
                }
                if (all_spaces_after) s2len = last_nl + 1;
            }
        }

        if (s2len > 0) {
            out[result_count++] = (AstStringPart){ .is_expr = 0, .text = s2, .text_len = s2len };
        } else {
            free(s2);
        }
    }

    if (result_count == 0) {
        out[0] = (AstStringPart){ .is_expr = 0, .text = "", .text_len = 0 };
        result_count = 1;
    }
    *out_count = result_count;
    return out;
}

typedef struct {
    Token *toks;
    size_t pos;
} Parser;

static Token *cur(Parser *p) {
    return &p->toks[p->pos];
}
static Token *peek2(Parser *p) {
    return &p->toks[p->pos + 1]; /* always in range: TOK_EOF is the final element */
}
static void advance(Parser *p) {
    p->pos++;
}
static Token *expect(Parser *p, TokKind kind, const char *what) {
    if (cur(p)->kind != kind) {
        fprintf(stderr, "asnix: parse error: expected %s\n", what);
        exit(1);
    }
    Token *t = cur(p);
    advance(p);
    return t;
}

static int starts_atom(TokKind k) {
    return k == TOK_INT || k == TOK_IDENT || k == TOK_TRUE || k == TOK_FALSE || k == TOK_NULL ||
           k == TOK_LPAREN || k == TOK_STR_START || k == TOK_ISTR_START || k == TOK_LBRACKET ||
           k == TOK_LBRACE || k == TOK_REC || k == TOK_PATH || k == TOK_SEARCH_PATH;
}

static Ast *parse_expr(Parser *p);

/* List elements and application arguments are parsed at this same
 * "select" precedence (atom plus postfix `.attr` chains), not full
 * `expr` -- matching real Nix, where `[ f x ]` is a 2-element list and
 * putting a function call in a list needs parens: `[ (f x) ]`. */
static Ast *parse_select(Parser *p);

/* Parses one binding-list statement (an ordinary `name = value;`, or an
 * `inherit x y;` / `inherit (expr) x y;` form) and appends the resulting
 * binding(s) to `out`. Shared by `let ... in`, `rec { ... }`, and plain
 * `{ ... }` -- all three use the same binding-list grammar (see ast.h's
 * note on AstBinding.is_inherit for why the desugared value differs by
 * form but `is_inherit` is attached regardless). */
static void parse_binding_stmt(Parser *p, BindingVec *out) {
    if (cur(p)->kind == TOK_INHERIT) {
        advance(p);
        Ast *src_expr = NULL;
        if (cur(p)->kind == TOK_LPAREN) {
            advance(p);
            src_expr = parse_expr(p);
            expect(p, TOK_RPAREN, "')'");
        }
        while (cur(p)->kind == TOK_IDENT) {
            Token *name = cur(p);
            advance(p);
            Ast *value;
            if (src_expr) {
                Ast *sel = asnix_ast_new(AST_SELECT);
                sel->as.select.target = src_expr;
                sel->as.select.attr = name->start;
                sel->as.select.attr_len = name->len;
                sel->as.select.attr_expr = NULL;
                sel->as.select.default_ = NULL;
                value = sel;
            } else {
                Ast *v = asnix_ast_new(AST_VAR);
                v->as.var.name = name->start;
                v->as.var.name_len = name->len;
                value = v;
            }
            /* `inherit (expr) x;` is sugar for an ordinary binding `x =
             * expr.x;` -- expr resolves against this block's own scope
             * just like any other binding's value (siblings visible,
             * confirmed against real nix-instantiate: `let inherit (pt)
             * x y; pt = { ... }; in ...` sees the sibling `pt`). Only the
             * bare `inherit x;` form needs the is_inherit escape-to-
             * outer-scope treatment below (see ast.h) -- real Nix
             * resolves a bare inherited name against the scope
             * *enclosing* this block, not the block's own recursive
             * scope, even though plain `x` would otherwise self-match. */
            bindingvec_push(out, (AstBinding){
                .name = name->start, .name_len = name->len, .value = value,
                .is_inherit = (src_expr == NULL) });
        }
        expect(p, TOK_SEMI, "';'");
        return;
    }
    Token *name = expect(p, TOK_IDENT, "a binding name");
    expect(p, TOK_EQUALS, "'='");
    Ast *value = parse_expr(p);
    expect(p, TOK_SEMI, "';'");
    bindingvec_push(out, (AstBinding){
        .name = name->start, .name_len = name->len, .value = value, .is_inherit = 0 });
}

/* "foo ${expr} bar": zero or more TOK_STR_PART literal chunks interleaved
 * with TOK_DOLLAR_LBRACE ... TOK_RBRACE interpolated expressions, parsed
 * at full `expr` precedence (so e.g. `"${a} ${b}"` or `"${if c then x else
 * y}"` both just work -- the lexer's mode stack is what makes re-entering
 * parse_expr here safe, see lexer.c). */
static Ast *parse_dquote_string(Parser *p) {
    advance(p); /* TOK_STR_START */
    PartVec parts = { 0 };
    while (cur(p)->kind != TOK_STR_END) {
        if (cur(p)->kind == TOK_STR_PART) {
            Token *t = cur(p);
            partvec_push(&parts, (AstStringPart){ .is_expr = 0, .text = t->start, .text_len = t->len });
            advance(p);
        } else if (cur(p)->kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a string interpolation");
            partvec_push(&parts, (AstStringPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "asnix: parse error: malformed string literal\n");
            exit(1);
        }
    }
    advance(p); /* TOK_STR_END */
    Ast *n = asnix_ast_new(AST_STRING);
    n->as.string.parts = parts.items;
    n->as.string.count = parts.len;
    return n;
}

/* '' ... '' indented string: same interpolation shape as above, but the
 * literal chunks are raw (un-indentation-stripped) and get run through
 * strip_indentation() once the whole literal is collected -- see that
 * function's comment for why this has to happen only after collecting
 * every part (the minimum indentation is computed across ALL of them). */
static Ast *parse_indented_string(Parser *p) {
    advance(p); /* TOK_ISTR_START */
    RawIndVec raw = { 0 };
    while (cur(p)->kind != TOK_ISTR_END) {
        if (cur(p)->kind == TOK_ISTR_PART) {
            Token *t = cur(p);
            rawind_push(&raw, (RawIndPart){ .is_expr = 0, .text = t->start, .text_len = t->len,
                                             .has_indentation = t->istr_has_indentation });
            advance(p);
        } else if (cur(p)->kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a string interpolation");
            rawind_push(&raw, (RawIndPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "asnix: parse error: malformed indented string literal\n");
            exit(1);
        }
    }
    advance(p); /* TOK_ISTR_END */

    size_t count;
    AstStringPart *parts = strip_indentation(&raw, &count);
    free(raw.items);

    Ast *n = asnix_ast_new(AST_STRING);
    n->as.string.parts = parts;
    n->as.string.count = count;
    return n;
}

static Ast *parse_atom(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
        case TOK_INT: {
            advance(p);
            Ast *n = asnix_ast_new(AST_INT);
            n->as.int_val = t->int_val;
            return n;
        }
        case TOK_TRUE: {
            advance(p);
            Ast *n = asnix_ast_new(AST_BOOL);
            n->as.bool_val = 1;
            return n;
        }
        case TOK_FALSE: {
            advance(p);
            Ast *n = asnix_ast_new(AST_BOOL);
            n->as.bool_val = 0;
            return n;
        }
        case TOK_NULL: {
            advance(p);
            return asnix_ast_new(AST_NULL);
        }
        case TOK_STR_START:
            return parse_dquote_string(p);
        case TOK_ISTR_START:
            return parse_indented_string(p);
        case TOK_IDENT: {
            advance(p);
            Ast *n = asnix_ast_new(AST_VAR);
            n->as.var.name = t->start;
            n->as.var.name_len = t->len;
            return n;
        }
        case TOK_PATH: {
            advance(p);
            Ast *n = asnix_ast_new(AST_PATH);
            n->as.path.text = t->start;
            n->as.path.len = t->len;
            n->as.path.is_search = 0;
            return n;
        }
        case TOK_SEARCH_PATH: {
            advance(p);
            Ast *n = asnix_ast_new(AST_PATH);
            n->as.path.text = t->start;
            n->as.path.len = t->len;
            n->as.path.is_search = 1;
            return n;
        }
        case TOK_LPAREN: {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RPAREN, "')'");
            return e;
        }
        case TOK_LBRACKET: {
            advance(p);
            AstVec elems = { 0 };
            while (cur(p)->kind != TOK_RBRACKET) astvec_push(&elems, parse_select(p));
            advance(p); /* ']' */
            Ast *n = asnix_ast_new(AST_LIST);
            n->as.list.elems = elems.items;
            n->as.list.count = elems.len;
            return n;
        }
        case TOK_LBRACE: {
            advance(p);
            BindingVec bindings = { 0 };
            while (cur(p)->kind != TOK_RBRACE) parse_binding_stmt(p, &bindings);
            advance(p); /* '}' */
            Ast *n = asnix_ast_new(AST_ATTRSET);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 0;
            return n;
        }
        case TOK_REC: {
            advance(p);
            expect(p, TOK_LBRACE, "'{'");
            BindingVec bindings = { 0 };
            while (cur(p)->kind != TOK_RBRACE) parse_binding_stmt(p, &bindings);
            advance(p); /* '}' */
            Ast *n = asnix_ast_new(AST_ATTRSET);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 1;
            return n;
        }
        default:
            fprintf(stderr, "asnix: parse error: unexpected token\n");
            exit(1);
    }
}

static Ast *parse_select(Parser *p) {
    Ast *e = parse_atom(p);
    int had_select = 0;
    int had_dynamic = 0;
    while (cur(p)->kind == TOK_DOT) {
        advance(p);
        Ast *n = asnix_ast_new(AST_SELECT);
        n->as.select.target = e;
        n->as.select.default_ = NULL;
        if (cur(p)->kind == TOK_DOLLAR_LBRACE) {
            /* Dynamic attribute name, `.${expr}` -- real Nix's grammar
             * for a computed-at-runtime attrpath segment, e.g.
             * `set.${name}` (found needed evaluating real nixpkgs,
             * pkgs/top-level/default.nix). */
            advance(p);
            Ast *name_expr = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a dynamic attribute name");
            n->as.select.attr_expr = name_expr;
            n->as.select.attr = NULL;
            n->as.select.attr_len = 0;
            had_dynamic = 1;
        } else {
            Token *attr = expect(p, TOK_IDENT, "an attribute name");
            n->as.select.attr_expr = NULL;
            n->as.select.attr = attr->start;
            n->as.select.attr_len = attr->len;
        }
        e = n;
        had_select = 1;
    }
    if (had_dynamic && cur(p)->kind == TOK_OR) {
        /* `set.${name} or default` -- a real, valid combination in real
         * Nix, not supported here: fatal rather than silently
         * mishandling it (documented, narrow scope cut -- see ast.h's
         * AstSelect.attr_expr comment). Not needed by anything this
         * project has hit yet. */
        fprintf(stderr, "asnix: parse error: 'or' after a dynamic '.${...}' attribute is not supported\n");
        exit(1);
    }
    /* `expr.attr or default` -- real Nix's grammar attaches `or` to the
     * whole preceding `expr '.' attrpath`, not per-segment (confirmed
     * against nix-instantiate's own parser.y: `expr_simple '.' attrpath
     * OR_KW expr_select`), so this is checked once, after the loop, on
     * the outermost (last) AST_SELECT node -- see that node's `default_`
     * field comment in ast.h for the resulting scope cut (only the last
     * segment's failure is caught, not an earlier one in a longer
     * chain). `default` itself is parsed at this same parse_select
     * tightness (matching real Nix: the default is NOT a full `expr`,
     * e.g. `+`/`==` outside are not absorbed into it), recursively, so
     * `a.b or c.d or e` chains sensibly. No `or` is possible without at
     * least one preceding `.select` (real Nix's grammar agrees -- a bare
     * `x or y` is the separate, unsupported "cursed or" rule). */
    if (had_select && cur(p)->kind == TOK_OR) {
        advance(p);
        e->as.select.default_ = parse_select(p);
    }
    return e;
}

static Ast *parse_app(Parser *p) {
    Ast *fn = parse_select(p);
    while (starts_atom(cur(p)->kind)) {
        Ast *arg = parse_select(p);
        Ast *app = asnix_ast_new(AST_APP);
        app->as.app.fn = fn;
        app->as.app.arg = arg;
        fn = app;
    }
    return fn;
}

/* `expr ? attr` (has-attr): binds tighter than arithmetic but looser than
 * application, matching real Nix's precedence table. Confirmed against
 * `nix-instantiate --eval '1 ? a'` => false (not an error) -- a non-
 * attrset left-hand side is simply "no", see op_has_attr.S. */
static Ast *parse_has_attr(Parser *p) {
    Ast *e = parse_app(p);
    while (cur(p)->kind == TOK_QUESTION) {
        advance(p);
        Token *attr = expect(p, TOK_IDENT, "an attribute name");
        Ast *n = asnix_ast_new(AST_HAS_ATTR);
        n->as.has_attr.target = e;
        n->as.has_attr.attr = attr->start;
        n->as.has_attr.attr_len = attr->len;
        e = n;
    }
    return e;
}

/* `++` list concatenation: binds tighter than `*`/`/` but looser than
 * `?` (has-attr) and unary `-` (negate), right-associative -- cross-
 * checked against cppnix's real grammar, `~/nix/src/libexpr/parser.y`'s
 * precedence table (`%left '+' '-'`, `%left '*' '/'`, `%right CONCAT`,
 * `%nonassoc '?'`, `%nonassoc NEGATE`, loosest to tightest in that
 * declaration order). Verified against `nix-instantiate --eval`:
 * `[1] ++ [2] ++ [3]` => `[ 1 2 3 ]` (chains), and the type error from
 * `1 ++ [2]` names the lhs ("expected a list but found an integer: 1"),
 * confirming no numeric-only interpretation competes here. */
static Ast *parse_concat(Parser *p) {
    Ast *lhs = parse_has_attr(p);
    if (cur(p)->kind == TOK_PLUSPLUS) {
        advance(p);
        Ast *rhs = parse_concat(p); /* right-recurse: right-associative */
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = 'c';
        n->as.binop.lhs = lhs;
        n->as.binop.rhs = rhs;
        return n;
    }
    return lhs;
}

static Ast *parse_mul(Parser *p) {
    Ast *lhs = parse_concat(p);
    while (cur(p)->kind == TOK_STAR) {
        advance(p);
        Ast *rhs = parse_concat(p);
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = '*';
        n->as.binop.lhs = lhs;
        n->as.binop.rhs = rhs;
        lhs = n;
    }
    return lhs;
}

static Ast *parse_add(Parser *p) {
    Ast *lhs = parse_mul(p);
    while (cur(p)->kind == TOK_PLUS || cur(p)->kind == TOK_MINUS) {
        char op = cur(p)->kind == TOK_PLUS ? '+' : '-';
        advance(p);
        Ast *rhs = parse_mul(p);
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = op;
        n->as.binop.lhs = lhs;
        n->as.binop.rhs = rhs;
        lhs = n;
    }
    return lhs;
}

/* Operator precedence below `parse_expr`'s keyword forms (`let`/`if`/
 * lambda/`assert`), lowest to highest, verified directly against real
 * `nix-instantiate` (see docs/architecture.md's "operators" milestone
 * notes -- this is NOT the same ordering a literal reading of the Nix
 * manual's summary table might suggest for `!`, which binds looser than
 * `+`/`-` but tighter than `//`; confirmed with
 * `! 1 + 1` (parses as `!(1 + 1)`, confirmed by the runtime type error
 * complaining about `2`, not `1`) and `! true // {}` (parses as
 * `(!true) // {}`, confirmed by the runtime error naming `false`, the
 * result of the `!`, not `true`)):
 *
 *   parse_impl  ->              right-assoc, chainable, LOWEST of all of these
 *   parse_or    ||              left-assoc, chainable
 *   parse_and   &&              left-assoc, chainable
 *   parse_eq    == !=           non-assoc (single occurrence only)
 *   parse_cmp   < > <= >=       non-assoc (single occurrence only)
 *   parse_update //             right-assoc, chainable
 *   parse_not   ! (prefix)      binds the rest of this chain down to parse_add
 *   parse_add   + -             (already existed, M4)
 *
 * `->`'s precedence/associativity (lowest, right-assoc) cross-checked
 * against cppnix's real grammar (~/nix/src/libexpr/parser.y: `%right
 * IMPL`, declared before `%left OR`, i.e. binding looser).
 */
static Ast *parse_not(Parser *p) {
    if (cur(p)->kind == TOK_BANG) {
        advance(p);
        Ast *operand = parse_not(p); /* right-recurse: allows `!!x` */
        Ast *n = asnix_ast_new(AST_NOT);
        n->as.not1.operand = operand;
        return n;
    }
    return parse_add(p);
}

static Ast *parse_update(Parser *p) {
    Ast *lhs = parse_not(p);
    if (cur(p)->kind == TOK_SLASHSLASH) {
        advance(p);
        Ast *rhs = parse_update(p); /* right-recurse: right-associative, matching real Nix */
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = 'u';
        n->as.binop.lhs = lhs;
        n->as.binop.rhs = rhs;
        return n;
    }
    return lhs;
}

static Ast *parse_cmp(Parser *p) {
    Ast *lhs = parse_update(p);
    TokKind k = cur(p)->kind;
    if (k == TOK_LT || k == TOK_GT || k == TOK_LE || k == TOK_GE) {
        advance(p);
        Ast *rhs = parse_update(p); /* no loop: non-associative, matching real Nix (`1 < 2 < 3` is a parse error there too) */
        char op = k == TOK_LT ? 'l' : k == TOK_GT ? 'g' : k == TOK_LE ? 'L' : 'G';
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = op;
        n->as.binop.lhs = lhs;
        n->as.binop.rhs = rhs;
        return n;
    }
    return lhs;
}

static Ast *parse_eq(Parser *p) {
    Ast *lhs = parse_cmp(p);
    TokKind k = cur(p)->kind;
    if (k == TOK_EQEQ || k == TOK_NEQ) {
        advance(p);
        Ast *rhs = parse_cmp(p); /* non-associative, same reasoning as parse_cmp */
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = k == TOK_EQEQ ? 'e' : 'n';
        n->as.binop.lhs = lhs;
        n->as.binop.rhs = rhs;
        return n;
    }
    return lhs;
}

static Ast *parse_and(Parser *p) {
    Ast *lhs = parse_eq(p);
    while (cur(p)->kind == TOK_ANDAND) {
        advance(p);
        Ast *rhs = parse_eq(p);
        Ast *n = asnix_ast_new(AST_AND);
        n->as.logic.lhs = lhs;
        n->as.logic.rhs = rhs;
        lhs = n;
    }
    return lhs;
}

static Ast *parse_or(Parser *p) {
    Ast *lhs = parse_and(p);
    while (cur(p)->kind == TOK_OROR) {
        advance(p);
        Ast *rhs = parse_and(p);
        Ast *n = asnix_ast_new(AST_OR);
        n->as.logic.lhs = lhs;
        n->as.logic.rhs = rhs;
        lhs = n;
    }
    return lhs;
}

/* `a -> b` desugars directly to `(!a) || b` -- exactly real Nix's own
 * ExprOpImpl::eval semantics (`!evalBool(e1) || evalBool(e2)`, cppnix's
 * eval.cc), so this gets short-circuiting (b is never evaluated when a is
 * false) for free by reusing the already-correct AST_OR/AST_NOT
 * compilation, no new AST node or IR op needed. */
static Ast *parse_impl(Parser *p) {
    Ast *lhs = parse_or(p);
    if (cur(p)->kind == TOK_ARROW) {
        advance(p);
        Ast *rhs = parse_impl(p); /* right-recurse: right-associative, matching real Nix */
        Ast *not_lhs = asnix_ast_new(AST_NOT);
        not_lhs->as.not1.operand = lhs;
        Ast *n = asnix_ast_new(AST_OR);
        n->as.logic.lhs = not_lhs;
        n->as.logic.rhs = rhs;
        return n;
    }
    return lhs;
}

/* Parses the bindings following `let` into one flat binding list (real Nix
 * `let ... in` is always implicitly `letrec`: every binding sees itself
 * and every sibling regardless of source order -- see ast.h/compiler.c). */
static Ast *parse_let_bindings(Parser *p) {
    BindingVec bindings = { 0 };
    while (cur(p)->kind != TOK_IN) parse_binding_stmt(p, &bindings);
    advance(p); /* 'in' */
    Ast *body = parse_expr(p);

    Ast *n = asnix_ast_new(AST_LET);
    n->as.let1.bindings = bindings.items;
    n->as.let1.count = bindings.len;
    n->as.let1.body = body;
    return n;
}

/* Scans forward from an TOK_LBRACE at `open_idx`, tracking brace depth, and
 * returns the index of the matching TOK_RBRACE -- used to look past an
 * entire `{ ... }` (lambda-pattern formals or an attrset literal, whichever
 * it turns out to be) without actually parsing it, just to disambiguate
 * the two forms by checking what follows the close brace (':' or '@' means
 * a lambda pattern; anything else means an ordinary attrset-literal
 * expression, parsed as such by parse_atom). Depth-counting on braces
 * alone is safe regardless of what's nested inside (parens, brackets, a
 * default expression that itself contains `{ }`) since braces are always
 * balanced in valid source independent of their semantic role. */
static size_t find_matching_rbrace(Parser *p, size_t open_idx) {
    size_t depth = 0;
    size_t i = open_idx;
    for (;;) {
        TokKind k = p->toks[i].kind;
        if (k == TOK_LBRACE) {
            depth++;
        } else if (k == TOK_RBRACE) {
            depth--;
            if (depth == 0) return i;
        } else if (k == TOK_EOF) {
            fprintf(stderr, "asnix: parse error: unterminated '{' (lambda pattern or attrset)\n");
            exit(1);
        }
        i++;
    }
}

/* Parses the comma-separated formals list of a lambda pattern, with the
 * parser positioned just after the opening '{'. Consumes through (and
 * including) the matching '}'. Trailing commas are accepted (including
 * right before `...`), matching real Nix. */
static void parse_formals(Parser *p, AstFormal **out_items, size_t *out_count, int *out_ellipsis) {
    FormalVec formals = { 0 };
    int ellipsis = 0;
    while (cur(p)->kind != TOK_RBRACE) {
        if (cur(p)->kind == TOK_ELLIPSIS) {
            advance(p);
            ellipsis = 1;
            break; /* `...` must be the last formal */
        }
        Token *name = expect(p, TOK_IDENT, "a formal name");
        Ast *default_ = NULL;
        if (cur(p)->kind == TOK_QUESTION) {
            advance(p);
            default_ = parse_expr(p); /* arbitrary lazy expression -- see compiler.c's AST_LAMBDA case */
        }
        formalvec_push(&formals, (AstFormal){ .name = name->start, .name_len = name->len, .default_ = default_ });
        if (cur(p)->kind == TOK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }
    expect(p, TOK_RBRACE, "'}'");
    *out_items = formals.items;
    *out_count = formals.len;
    *out_ellipsis = ellipsis;
}

static Ast *parse_expr(Parser *p) {
    if (cur(p)->kind == TOK_LET) {
        advance(p);
        if (cur(p)->kind == TOK_LBRACE) {
            /* Legacy `let { ..., body = ...; }` form -- real Nix desugars
             * this directly to `(rec { ... }).body` (cppnix's parser.y:
             * "Let expressions `let {..., body = ...}' are just desugared
             * into `(rec {..., body = ...}).body'."). Distinct from
             * ordinary `let <bindings> in <expr>` (no `{` immediately
             * after `let`) handled below. */
            advance(p); /* '{' */
            BindingVec bindings = { 0 };
            while (cur(p)->kind != TOK_RBRACE) parse_binding_stmt(p, &bindings);
            advance(p); /* '}' */
            Ast *attrs = asnix_ast_new(AST_ATTRSET);
            attrs->as.attrset.bindings = bindings.items;
            attrs->as.attrset.count = bindings.len;
            attrs->as.attrset.is_rec = 1;
            Ast *n = asnix_ast_new(AST_SELECT);
            n->as.select.target = attrs;
            n->as.select.attr = "body";
            n->as.select.attr_len = 4;
            n->as.select.attr_expr = NULL;
            n->as.select.default_ = NULL;
            return n;
        }
        return parse_let_bindings(p);
    }
    if (cur(p)->kind == TOK_WITH) {
        advance(p);
        Ast *attrs = parse_expr(p);
        expect(p, TOK_SEMI, "';'");
        Ast *body = parse_expr(p); /* recursing here is what lets further let/if/with/lambda follow */
        Ast *n = asnix_ast_new(AST_WITH);
        n->as.with1.attrs = attrs;
        n->as.with1.body = body;
        return n;
    }
    if (cur(p)->kind == TOK_IF) {
        advance(p);
        Ast *cond = parse_expr(p);
        expect(p, TOK_THEN, "'then'");
        Ast *then_ = parse_expr(p);
        expect(p, TOK_ELSE, "'else'");
        Ast *else_ = parse_expr(p);
        Ast *n = asnix_ast_new(AST_IF);
        n->as.if1.cond = cond;
        n->as.if1.then_ = then_;
        n->as.if1.else_ = else_;
        return n;
    }
    if (cur(p)->kind == TOK_IDENT && peek2(p)->kind == TOK_COLON) {
        Token *param = cur(p);
        advance(p);
        advance(p); /* ':' */
        Ast *body = parse_expr(p); /* recursing here is what makes `a: b: body` curry */
        Ast *n = asnix_ast_new(AST_LAMBDA);
        n->as.lambda.param = param->start;
        n->as.lambda.param_len = param->len;
        n->as.lambda.body = body;
        return n;
    }
    if (cur(p)->kind == TOK_ASSERT) {
        advance(p);
        Token *start_tok = cur(p);
        Ast *cond = parse_expr(p);
        Token *semi_tok = cur(p); /* captured before expect() consumes it */
        expect(p, TOK_SEMI, "';'");
        size_t span_len = (size_t)(semi_tok->pos - start_tok->pos);
        while (span_len > 0 && isspace((unsigned char)start_tok->pos[span_len - 1])) span_len--;
        Ast *body = parse_expr(p);
        Ast *n = asnix_ast_new(AST_ASSERT);
        n->as.assert1.cond = cond;
        n->as.assert1.body = body;
        n->as.assert1.cond_text = start_tok->pos;
        n->as.assert1.cond_text_len = span_len;
        return n;
    }
    if (cur(p)->kind == TOK_IDENT && peek2(p)->kind == TOK_AT) {
        /* `name@{...}: body` -- the whole-arg alias comes first. */
        Token *asname = cur(p);
        advance(p);
        advance(p); /* '@' */
        expect(p, TOK_LBRACE, "'{' after '@'");
        AstFormal *formals;
        size_t fcount;
        int ellipsis;
        parse_formals(p, &formals, &fcount, &ellipsis);
        expect(p, TOK_COLON, "':'");
        Ast *body = parse_expr(p);
        Ast *n = asnix_ast_new(AST_LAMBDA);
        n->as.lambda.is_pattern = 1;
        n->as.lambda.formals = formals;
        n->as.lambda.formals_count = fcount;
        n->as.lambda.has_ellipsis = ellipsis;
        n->as.lambda.has_as_name = 1;
        n->as.lambda.as_name = asname->start;
        n->as.lambda.as_name_len = asname->len;
        n->as.lambda.body = body;
        return n;
    }
    if (cur(p)->kind == TOK_LBRACE) {
        /* Could be a lambda pattern (`{...}: body` / `{...}@name: body`) or
         * an ordinary attrset-literal expression -- look past the matching
         * '}' to tell which, same disambiguation real Nix's own grammar
         * makes (see find_matching_rbrace's comment). */
        size_t close = find_matching_rbrace(p, p->pos);
        TokKind after = p->toks[close + 1].kind;
        if (after == TOK_COLON || after == TOK_AT) {
            advance(p); /* '{' */
            AstFormal *formals;
            size_t fcount;
            int ellipsis;
            parse_formals(p, &formals, &fcount, &ellipsis);
            int has_as = 0;
            const char *as_name = NULL;
            size_t as_name_len = 0;
            if (cur(p)->kind == TOK_AT) {
                advance(p);
                Token *asname = expect(p, TOK_IDENT, "a name after '@'");
                has_as = 1;
                as_name = asname->start;
                as_name_len = asname->len;
            }
            expect(p, TOK_COLON, "':'");
            Ast *body = parse_expr(p);
            Ast *n = asnix_ast_new(AST_LAMBDA);
            n->as.lambda.is_pattern = 1;
            n->as.lambda.formals = formals;
            n->as.lambda.formals_count = fcount;
            n->as.lambda.has_ellipsis = ellipsis;
            n->as.lambda.has_as_name = has_as;
            n->as.lambda.as_name = as_name;
            n->as.lambda.as_name_len = as_name_len;
            n->as.lambda.body = body;
            return n;
        }
        /* Not a pattern: fall through to parse_impl (the top of the full
         * operator-precedence chain), whose own fallthrough eventually
         * reaches parse_atom, which handles '{' as an attrset literal. */
    }
    return parse_impl(p);
}

Ast *asnix_parse(Token *toks, size_t count) {
    (void)count;
    Parser p = { .toks = toks, .pos = 0 };
    Ast *result = parse_expr(&p);
    expect(&p, TOK_EOF, "end of input");
    return result;
}
