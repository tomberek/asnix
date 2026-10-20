#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

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
           k == TOK_LPAREN || k == TOK_STRING || k == TOK_LBRACKET || k == TOK_LBRACE || k == TOK_REC;
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
        case TOK_STRING: {
            advance(p);
            Ast *n = asnix_ast_new(AST_STRING);
            n->as.string.text = t->start;
            n->as.string.len = t->len;
            return n;
        }
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
 *   parse_or    ||              left-assoc, chainable
 *   parse_and   &&              left-assoc, chainable
 *   parse_eq    == !=           non-assoc (single occurrence only)
 *   parse_cmp   < > <= >=       non-assoc (single occurrence only)
 *   parse_update //             right-assoc, chainable
 *   parse_not   ! (prefix)      binds the rest of this chain down to parse_add
 *   parse_add   + -             (already existed, M4)
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
    return parse_or(p);
}

Ast *asnix_parse(Token *toks, size_t count) {
    (void)count;
    Parser p = { .toks = toks, .pos = 0 };
    Ast *result = parse_expr(&p);
    expect(&p, TOK_EOF, "end of input");
    return result;
}
