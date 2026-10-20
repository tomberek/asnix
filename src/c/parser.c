#include "parser.h"

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
           k == TOK_LBRACE;
}

static Ast *parse_expr(Parser *p);

/* List elements and application arguments are parsed at this same
 * "select" precedence (atom plus postfix `.attr` chains), not full
 * `expr` -- matching real Nix, where `[ f x ]` is a 2-element list and
 * putting a function call in a list needs parens: `[ (f x) ]`. */
static Ast *parse_select(Parser *p);

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
            while (cur(p)->kind != TOK_RBRACE) {
                Token *name = expect(p, TOK_IDENT, "an attribute name");
                expect(p, TOK_EQUALS, "'='");
                Ast *value = parse_expr(p);
                expect(p, TOK_SEMI, "';'");
                bindingvec_push(&bindings, (AstBinding){ .name = name->start, .name_len = name->len, .value = value });
            }
            advance(p); /* '}' */
            Ast *n = asnix_ast_new(AST_ATTRSET);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            return n;
        }
        default:
            fprintf(stderr, "asnix: parse error: unexpected token\n");
            exit(1);
    }
}

static Ast *parse_select(Parser *p) {
    Ast *e = parse_atom(p);
    while (cur(p)->kind == TOK_DOT) {
        advance(p);
        Token *attr = expect(p, TOK_IDENT, "an attribute name");
        Ast *n = asnix_ast_new(AST_SELECT);
        n->as.select.target = e;
        n->as.select.attr = attr->start;
        n->as.select.attr_len = attr->len;
        e = n;
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

static Ast *parse_mul(Parser *p) {
    Ast *lhs = parse_app(p);
    while (cur(p)->kind == TOK_STAR) {
        advance(p);
        Ast *rhs = parse_app(p);
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

/* Parses the bindings following `let`, recursing once per binding so that
 * N bindings desugar into N nested single-binding AST_LET nodes (see
 * ast.h). Base case: `in expr`. */
static Ast *parse_let_bindings(Parser *p) {
    if (cur(p)->kind == TOK_IN) {
        advance(p);
        return parse_expr(p);
    }
    Token *name = expect(p, TOK_IDENT, "a binding name");
    expect(p, TOK_EQUALS, "'='");
    Ast *value = parse_expr(p);
    expect(p, TOK_SEMI, "';'");
    Ast *body = parse_let_bindings(p);

    Ast *n = asnix_ast_new(AST_LET);
    n->as.let1.name = name->start;
    n->as.let1.name_len = name->len;
    n->as.let1.value = value;
    n->as.let1.body = body;
    return n;
}

static Ast *parse_expr(Parser *p) {
    if (cur(p)->kind == TOK_LET) {
        advance(p);
        return parse_let_bindings(p);
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
    return parse_add(p);
}

Ast *asnix_parse(Token *toks, size_t count) {
    (void)count;
    Parser p = { .toks = toks, .pos = 0 };
    Ast *result = parse_expr(&p);
    expect(&p, TOK_EOF, "end of input");
    return result;
}
