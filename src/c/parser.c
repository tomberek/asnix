#include "parser.h"

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
           k == TOK_LPAREN || k == TOK_STRING || k == TOK_LBRACKET || k == TOK_LBRACE;
}

static Ast *parse_expr(Parser *p);

/* List elements and application arguments are parsed at this same
 * "select" precedence (atom plus postfix `.attr` chains), not full
 * `expr` -- matching real Nix, where `[ f x ]` is a 2-element list and
 * putting a function call in a list needs parens: `[ (f x) ]`. */
static Ast *parse_select(Parser *p);

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

static Ast *parse_mul(Parser *p) {
    Ast *lhs = parse_has_attr(p);
    while (cur(p)->kind == TOK_STAR) {
        advance(p);
        Ast *rhs = parse_has_attr(p);
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
        /* Not a pattern: fall through to parse_add, whose parse_atom
         * handles '{' as an attrset literal. */
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
