#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH_VEC(name, T)                                                                                           \
    static T *name##_buf = NULL;                                                                                       \
    static size_t name##_len = 0, name##_cap = 0;                                                                      \
    static size_t name##_mark(void) { return name##_len; }                                                             \
    static void name##_push(T item) {                                                                                  \
        if (name##_len == name##_cap) {                                                                                \
                                                                 \
            name##_cap = name##_cap ? name##_cap * 2 : 24;                                                              \
            name##_buf = realloc(name##_buf, name##_cap * sizeof(T));                                                   \
        }                                                                                                               \
        name##_buf[name##_len++] = item;                                                                                \
    }                                                                                                                   \
           \
    static T *name##_commit(size_t mark, size_t *out_count) {                                                          \
        size_t n = name##_len - mark;                                                                                   \
        name##_len = mark;                                                                                              \
        *out_count = n;                                                                                                 \
        if (n == 0) return NULL;                                                                                        \
        T *out = malloc(n * sizeof(T));                                                                                 \
        memcpy(out, name##_buf + mark, n * sizeof(T));                                                                  \
        return out;                                                                                                     \
    }

SCRATCH_VEC(astvec, Ast *)

typedef struct {
    AstBinding *items;
    size_t len, cap;
} BindingVec;

static void bindingvec_push(BindingVec *v, AstBinding item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 24;
        v->items = realloc(v->items, v->cap * sizeof(AstBinding));
    }
    v->items[v->len++] = item;
}

#define VEC_TRIM(v, T)                                                                                               \
    do {                                                                                                             \
        if ((v).len > 0 && (v).len < (v).cap) (v).items = realloc((v).items, (v).len * sizeof(T));                   \
    } while (0)

typedef struct {
    Ast *name_expr;
    Ast *value;
} DynBinding;

typedef struct {
    DynBinding *items;
    size_t len, cap;
} DynBindingVec;

static void dynbindingvec_push(DynBindingVec *v, DynBinding item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->items = realloc(v->items, v->cap * sizeof(DynBinding));
    }
    v->items[v->len++] = item;
}

static AstDynBinding *finalize_dyn_vec(DynBindingVec *vec) {
    if (vec->len == 0) return NULL;
    AstDynBinding *out = malloc(vec->len * sizeof(AstDynBinding));
    for (size_t i = 0; i < vec->len; i++) {
        out[i] = (AstDynBinding){ .name_expr = vec->items[i].name_expr, .value = vec->items[i].value };
    }
    return out;
}

typedef struct {
    int is_dynamic;
    Ast *name_expr;
    const char *attr;
    size_t attr_len;
} AttrSeg;

typedef struct BTNode BTNode;
typedef struct { BTNode *items; size_t len, cap; DynBindingVec dyn; size_t *index; size_t index_cap; } BTVec;

#define BTVEC_EMPTY_SLOT SIZE_MAX

static void merge_bt_dyn_into(BTVec *bt, DynBindingVec *dyn_out) {
    for (size_t i = 0; i < bt->dyn.len; i++) dynbindingvec_push(dyn_out, bt->dyn.items[i]);
}

struct BTNode {
    const char *name;
    size_t name_len;
    int is_leaf;
    Ast *leaf_value;
    int is_inherit;
    int is_hidden;
    int rec;
    BTVec children;
};

static uint64_t btvec_hash_name(const char *name, size_t len) {
    uint64_t h = 0;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)name[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static void btvec_index_rehash(BTVec *v, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = BTVEC_EMPTY_SLOT;
    for (size_t i = 0; i < v->len; i++) {
        size_t slot = btvec_hash_name(v->items[i].name, v->items[i].name_len) & (new_cap - 1);
        while (new_table[slot] != BTVEC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(v->index);
    v->index = new_table;
    v->index_cap = new_cap;
}

static void btvec_push(BTVec *v, BTNode item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 24;
        v->items = realloc(v->items, v->cap * sizeof(BTNode));
    }
    if (v->index_cap == 0) btvec_index_rehash(v, 16);
    else if ((v->len + 1) * 4 > v->index_cap * 3) btvec_index_rehash(v, v->index_cap * 2);
    v->items[v->len] = item;
    size_t slot = btvec_hash_name(item.name, item.name_len) & (v->index_cap - 1);
    while (v->index[slot] != BTVEC_EMPTY_SLOT) slot = (slot + 1) & (v->index_cap - 1);
    v->index[slot] = v->len;
    v->len++;
}

static BTNode *btvec_find(BTVec *v, const char *name, size_t name_len) {
    if (v->index_cap == 0) return NULL;
    size_t slot = btvec_hash_name(name, name_len) & (v->index_cap - 1);
    for (;;) {
        size_t idx = v->index[slot];
        if (idx == BTVEC_EMPTY_SLOT) return NULL;
        if (v->items[idx].name_len == name_len && memcmp(v->items[idx].name, name, name_len) == 0) {
            return &v->items[idx];
        }
        slot = (slot + 1) & (v->index_cap - 1);
    }
}

static void fatal_dup_attr(const char *name, size_t name_len) {
    fprintf(stderr, "asnix: attribute '%.*s' already defined, aborting\n", (int)name_len, name);
    exit(1);
}

static void insert_attrpath(BTVec *vec, AttrSeg *segs, size_t seg_count, size_t idx,
                             Ast *leaf_value, int is_inherit);

static void reopen_attrset_into(BTVec *dest, Ast *attrs) {
    for (size_t i = 0; i < attrs->as.attrset.dyn_count; i++) {
        AstDynBinding *db = &attrs->as.attrset.dyn_bindings[i];
        dynbindingvec_push(&dest->dyn, (DynBinding){ .name_expr = db->name_expr, .value = db->value });
    }
    for (size_t i = 0; i < attrs->as.attrset.count; i++) {
        AstBinding *b = &attrs->as.attrset.bindings[i];
        btvec_push(dest, (BTNode){
            .name = b->name, .name_len = b->name_len,
            .is_leaf = 1, .leaf_value = b->value, .is_inherit = b->is_inherit, .is_hidden = b->is_hidden });
    }
}

static void insert_attrpath(BTVec *vec, AttrSeg *segs, size_t seg_count, size_t idx,
                             Ast *leaf_value, int is_inherit) {
    AttrSeg seg = segs[idx];
    BTNode *existing = btvec_find(vec, seg.attr, seg.attr_len);
    int is_last = (idx == seg_count - 1);
    if (is_last) {
        if (!existing) {
            btvec_push(vec, (BTNode){
                .name = seg.attr, .name_len = seg.attr_len,
                .is_leaf = 1, .leaf_value = leaf_value, .is_inherit = is_inherit });
            return;
        }
        Ast *existing_attrs_val = existing->is_leaf ? existing->leaf_value : NULL;
        int existing_is_attrs_literal = existing->is_leaf && existing_attrs_val &&
                                         existing_attrs_val->kind == AST_ATTRSET;
        int new_is_attrs_literal = !is_inherit && leaf_value && leaf_value->kind == AST_ATTRSET;
        if (!existing->is_leaf && new_is_attrs_literal) {
            reopen_attrset_into(&existing->children, leaf_value);
            return;
        }
        if (existing_is_attrs_literal && new_is_attrs_literal) {
            BTVec merged = { 0 };
            reopen_attrset_into(&merged, existing_attrs_val);
            existing->is_leaf = 0;
            existing->rec = existing_attrs_val->as.attrset.is_rec;
            existing->children = merged;
            reopen_attrset_into(&existing->children, leaf_value);
            return;
        }
        fatal_dup_attr(seg.attr, seg.attr_len);
    }
    if (!existing) {
        BTNode node = { .name = seg.attr, .name_len = seg.attr_len, .is_leaf = 0, .rec = 0 };
        btvec_push(vec, node);
        existing = &vec->items[vec->len - 1];
        insert_attrpath(&existing->children, segs, seg_count, idx + 1, leaf_value, is_inherit);
        return;
    }
    if (existing->is_leaf) {
        if (!existing->leaf_value || existing->leaf_value->kind != AST_ATTRSET) {
            fatal_dup_attr(seg.attr, seg.attr_len);
        }
        Ast *reopened = existing->leaf_value;
        existing->is_leaf = 0;
        existing->rec = reopened->as.attrset.is_rec;
        existing->children = (BTVec){ 0 };
        reopen_attrset_into(&existing->children, reopened);
    }
    insert_attrpath(&existing->children, segs, seg_count, idx + 1, leaf_value, is_inherit);
}

static AstBinding finalize_bt_node(BTNode *node) {
    if (node->is_leaf) {
        return (AstBinding){
            .name = node->name, .name_len = node->name_len,
            .value = node->leaf_value, .is_inherit = node->is_inherit, .is_hidden = node->is_hidden };
    }
    BindingVec out = { 0 };
    for (size_t i = 0; i < node->children.len; i++) {
        bindingvec_push(&out, finalize_bt_node(&node->children.items[i]));
    }
    VEC_TRIM(out, AstBinding);
    Ast *attrs = asnix_ast_new(AST_ATTRSET);
    attrs->as.attrset.bindings = out.items;
    attrs->as.attrset.count = out.len;
    attrs->as.attrset.is_rec = node->rec;
    attrs->as.attrset.dyn_bindings = finalize_dyn_vec(&node->children.dyn);
    attrs->as.attrset.dyn_count = node->children.dyn.len;
    free(node->children.items);
    free(node->children.dyn.items);
    free(node->children.index);
    return (AstBinding){ .name = node->name, .name_len = node->name_len, .value = attrs, .is_inherit = 0 };
}

static void finalize_bt_vec(BTVec *vec, BindingVec *out) {
    for (size_t i = 0; i < vec->len; i++) {
        bindingvec_push(out, finalize_bt_node(&vec->items[i]));
    }
    free(vec->items);
    free(vec->index);
}

SCRATCH_VEC(partvec, AstStringPart)

SCRATCH_VEC(attrsegvec, AttrSeg)

SCRATCH_VEC(formalvec, AstFormal)

typedef struct {
    int is_expr;
    const char *text;
    size_t text_len;
    int has_indentation;
    Ast *expr;
} RawIndPart;

SCRATCH_VEC(rawind, RawIndPart)

static AstStringPart *strip_indentation(RawIndPart *raw, size_t n, size_t *out_count) {
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
        RawIndPart *p = &raw[idx];
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

    AstStringPart *out = malloc(n * sizeof(AstStringPart));
    size_t result_count = 0;
    at_start = 1;
    size_t cur_dropped = 0;
    for (size_t idx = 0; idx < n; idx++) {
        RawIndPart *p = &raw[idx];
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
    Lexer *lx;
    Token tok;
    Token next;
    const char *src;
    const char *curpos_base;
} Parser;

static void curpos_line_col(const char *src, const char *at, int *out_line, int *out_col) {
    int line = 1, col = 1;
    for (const char *c = src; c < at; c++) {
        if (*c == '\n') { line++; col = 1; }
        else col++;
    }
    *out_line = line;
    *out_col = col;
}

static const char *tok_text(Parser *p, Token t) { return p->src + t.start; }
static size_t tok_len(Token t) { return (size_t)(t.end - t.start); }

static Token cur(Parser *p) {
    return p->tok;
}
static Token peek2(Parser *p) {
    return p->next;
}
static TokKind peek3(Parser *p) {
    LexerSave save = lexer_save(p->lx);
    TokKind k = lexer_next(p->lx).kind;
    lexer_restore(p->lx, save);
    return k;
}
static void advance(Parser *p) {
    p->tok = p->next;
    p->next = lexer_next(p->lx);
}
static Token expect(Parser *p, TokKind kind, const char *what) {
    if (cur(p).kind != kind) {
        fprintf(stderr, "asnix: parse error: expected %s\n", what);
        exit(1);
    }
    Token t = cur(p);
    advance(p);
    return t;
}

static int starts_atom(TokKind k) {
    return k == TOK_INT || k == TOK_FLOAT || k == TOK_IDENT || k == TOK_TRUE || k == TOK_FALSE || k == TOK_NULL ||
           k == TOK_LPAREN || k == TOK_STR_START || k == TOK_ISTR_START || k == TOK_LBRACKET ||
           k == TOK_LBRACE || k == TOK_REC || k == TOK_PATH || k == TOK_SEARCH_PATH || k == TOK_OR ||
           k == TOK_URI;
}

static Ast *parse_expr(Parser *p);
static Ast *parse_dquote_string(Parser *p);

static Ast *parse_select(Parser *p);

static AttrSeg parse_attr_segment(Parser *p, const char *what) {
    if (cur(p).kind == TOK_DOLLAR_LBRACE) {
        advance(p);
        Ast *name_expr = parse_expr(p);
        expect(p, TOK_RBRACE, "'}' closing a dynamic attribute name");
        if (name_expr->kind == AST_STRING) {
            if (name_expr->as.string.count == 0) {
                return (AttrSeg){ .is_dynamic = 0, .attr = "", .attr_len = 0 };
            }
            if (name_expr->as.string.count == 1 && !name_expr->as.string.parts[0].is_expr) {
                return (AttrSeg){ .is_dynamic = 0, .attr = name_expr->as.string.parts[0].text,
                                   .attr_len = name_expr->as.string.parts[0].text_len };
            }
        }
        return (AttrSeg){ .is_dynamic = 1, .name_expr = name_expr };
    }
    if (cur(p).kind != TOK_STR_START) {
        if (cur(p).kind == TOK_OR || cur(p).kind == TOK_NULL || cur(p).kind == TOK_TRUE || cur(p).kind == TOK_FALSE) {
            Token t = cur(p);
            advance(p);
            return (AttrSeg){ .is_dynamic = 0, .attr = tok_text(p, t), .attr_len = tok_len(t) };
        }
        Token t = expect(p, TOK_IDENT, what);
        return (AttrSeg){ .is_dynamic = 0, .attr = tok_text(p, t), .attr_len = tok_len(t) };
    }
    Ast *str = parse_dquote_string(p);
    if (str->as.string.count == 0) {
        return (AttrSeg){ .is_dynamic = 0, .attr = "", .attr_len = 0 };
    }
    if (str->as.string.count == 1 && !str->as.string.parts[0].is_expr) {
        return (AttrSeg){ .is_dynamic = 0, .attr = str->as.string.parts[0].text,
                           .attr_len = str->as.string.parts[0].text_len };
    }
    return (AttrSeg){ .is_dynamic = 1, .name_expr = str };
}

static void set_has_attr_segment(Ast *n, AttrSeg seg) {
    if (seg.is_dynamic) {
        n->as.has_attr.attr_expr = seg.name_expr;
        n->as.has_attr.attr = NULL;
        n->as.has_attr.attr_len = 0;
    } else {
        n->as.has_attr.attr_expr = NULL;
        n->as.has_attr.attr = seg.attr;
        n->as.has_attr.attr_len = seg.attr_len;
    }
}

static void set_select_segment(Ast *n, AttrSeg seg) {
    if (seg.is_dynamic) {
        n->as.select.attr_expr = seg.name_expr;
        n->as.select.attr = NULL;
        n->as.select.attr_len = 0;
    } else {
        n->as.select.attr_expr = NULL;
        n->as.select.attr = seg.attr;
        n->as.select.attr_len = seg.attr_len;
    }
}

static Ast *build_dynamic_chain_value(AttrSeg *segs, size_t start, size_t count, Ast *value) {
    if (count == 0) return value;
    AttrSeg seg = segs[start];
    Ast *inner = build_dynamic_chain_value(segs, start + 1, count - 1, value);
    Ast *attrs = asnix_ast_new(AST_ATTRSET);
    attrs->as.attrset.is_rec = 0;
    if (seg.is_dynamic) {
        attrs->as.attrset.bindings = NULL;
        attrs->as.attrset.count = 0;
        AstDynBinding *dyn = malloc(sizeof(AstDynBinding));
        dyn[0] = (AstDynBinding){ .name_expr = seg.name_expr, .value = inner };
        attrs->as.attrset.dyn_bindings = dyn;
        attrs->as.attrset.dyn_count = 1;
    } else {
        AstBinding *b = malloc(sizeof(AstBinding));
        b[0] = (AstBinding){ .name = seg.attr, .name_len = seg.attr_len, .value = inner, .is_inherit = 0 };
        attrs->as.attrset.bindings = b;
        attrs->as.attrset.count = 1;
        attrs->as.attrset.dyn_bindings = NULL;
        attrs->as.attrset.dyn_count = 0;
    }
    return attrs;
}

static int g_inherit_hidden_counter = 0;

extern int asnix_lazy_thunk_enabled;

static void parse_binding_stmt_inner(Parser *p, BTVec *out, DynBindingVec *dyn_out);

static void parse_binding_stmt(Parser *p, BTVec *out, DynBindingVec *dyn_out) {
    int prev = asnix_ast_enter_retained();
    parse_binding_stmt_inner(p, out, dyn_out);
    asnix_ast_leave_retained(prev);
}

static void parse_binding_stmt_inner(Parser *p, BTVec *out, DynBindingVec *dyn_out) {
    if (cur(p).kind == TOK_INHERIT) {
        advance(p);
        Ast *src_expr = NULL;
        if (cur(p).kind == TOK_LPAREN) {
            advance(p);
            src_expr = parse_expr(p);
            expect(p, TOK_RPAREN, "')'");
        }
        size_t names_mark = attrsegvec_mark();
        while (cur(p).kind == TOK_IDENT || cur(p).kind == TOK_STR_START || cur(p).kind == TOK_DOLLAR_LBRACE ||
               cur(p).kind == TOK_OR || cur(p).kind == TOK_NULL || cur(p).kind == TOK_TRUE || cur(p).kind == TOK_FALSE) {
            AttrSeg name = parse_attr_segment(p, "an inherited name");
            if (name.is_dynamic) {
                fprintf(stderr, "asnix: parse error: dynamic attributes not allowed in inherit\n");
                exit(1);
            }
            attrsegvec_push(name);
        }
        size_t names_count;
        AttrSeg *names_items = attrsegvec_commit(names_mark, &names_count);
        Ast *hidden_var = NULL;
        if (src_expr && names_count > 1) {
            char *hidden_name = malloc(32);
            int hlen = snprintf(hidden_name, 32, "\x01inherit#%d", g_inherit_hidden_counter++);
            BTNode hidden_node = {
                .name = hidden_name, .name_len = (size_t)hlen,
                .is_leaf = 1, .leaf_value = src_expr, .is_inherit = 0, .is_hidden = 1 };
            btvec_push(out, hidden_node);
            Ast *v = asnix_ast_new_at(AST_VAR, cur(p).start);
            v->as.var.name = hidden_name;
            v->as.var.name_len = (size_t)hlen;
            hidden_var = v;
        }
        for (size_t ni = 0; ni < names_count; ni++) {
            AttrSeg name = names_items[ni];
            Ast *value;
            if (src_expr) {
                Ast *sel = asnix_ast_new_at(AST_SELECT, cur(p).start);
                sel->as.select.target = hidden_var ? hidden_var : src_expr;
                sel->as.select.attr = name.attr;
                sel->as.select.attr_len = name.attr_len;
                sel->as.select.attr_expr = NULL;
                sel->as.select.default_ = NULL;
                value = sel;
            } else {
                Ast *v = asnix_ast_new_at(AST_VAR, cur(p).start);
                v->as.var.name = name.attr;
                v->as.var.name_len = name.attr_len;
                value = v;
            }
            insert_attrpath(out, &name, 1, 0, value, (src_expr == NULL));
        }
        free(names_items);
        expect(p, TOK_SEMI, "';'");
        return;
    }
    size_t segs_mark = attrsegvec_mark();
    attrsegvec_push(parse_attr_segment(p, "a binding name"));
    while (cur(p).kind == TOK_DOT) {
        advance(p);
        attrsegvec_push(parse_attr_segment(p, "an attribute name"));
    }
    expect(p, TOK_EQUALS, "'='");
    uint32_t value_src_pos = cur(p).start;
    Ast *value = parse_expr(p);
    if (asnix_lazy_thunk_enabled) value->src_pos = value_src_pos;
    expect(p, TOK_SEMI, "';'");

    size_t segs_count;
    AttrSeg *segs_items = attrsegvec_commit(segs_mark, &segs_count);
    size_t first_dyn = segs_count;
    for (size_t i = 0; i < segs_count; i++) {
        if (segs_items[i].is_dynamic) { first_dyn = i; break; }
    }
    if (first_dyn == segs_count) {
        insert_attrpath(out, segs_items, segs_count, 0, value, 0);
    } else if (first_dyn == 0) {
        if (!dyn_out) {
            fprintf(stderr, "asnix: parse error: dynamic attributes not allowed in let\n");
            exit(1);
        }
        Ast *inner = build_dynamic_chain_value(segs_items, 1, segs_count - 1, value);
        dynbindingvec_push(dyn_out, (DynBinding){ .name_expr = segs_items[0].name_expr, .value = inner });
    } else {
        Ast *inner = build_dynamic_chain_value(segs_items, first_dyn, segs_count - first_dyn, value);
        insert_attrpath(out, segs_items, first_dyn, 0, inner, 0);
    }
    free(segs_items);
}

static Ast *parse_path_literal_interp(Parser *p, Token t) {
    Ast *n = asnix_ast_new_at(AST_PATH, cur(p).start);
    n->as.path.text = tok_text(p, t);
    n->as.path.len = tok_len(t);
    n->as.path.is_search = 0;
    n->as.path.is_home = (p->src[t.start] == '~');

    size_t parts_mark = partvec_mark();
    while (cur(p).kind != TOK_PATH_END) {
        if (cur(p).kind == TOK_PATH_PART) {
            Token pt = cur(p);
            partvec_push((AstStringPart){ .is_expr = 0, .text = tok_text(p, pt), .text_len = tok_len(pt) });
            advance(p);
        } else if (cur(p).kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a path interpolation");
            partvec_push((AstStringPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "asnix: parse error: malformed path literal\n");
            exit(1);
        }
    }
    advance(p);
    size_t parts_count;
    AstStringPart *parts_items = partvec_commit(parts_mark, &parts_count);
    n->as.path.interp_parts = parts_items;
    n->as.path.interp_count = parts_count;
    return n;
}

static Ast *parse_dquote_string(Parser *p) {
    advance(p);
    size_t parts_mark = partvec_mark();
    while (cur(p).kind != TOK_STR_END) {
        if (cur(p).kind == TOK_STR_PART) {
            Token t = cur(p);
            size_t dlen;
            char *dtext = asnix_decode_dquote_span(p->src, t.start, t.end, &dlen);
            partvec_push((AstStringPart){ .is_expr = 0, .text = dtext, .text_len = dlen });
            advance(p);
        } else if (cur(p).kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a string interpolation");
            partvec_push((AstStringPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "asnix: parse error: malformed string literal\n");
            exit(1);
        }
    }
    advance(p);
    size_t parts_count;
    AstStringPart *parts_items = partvec_commit(parts_mark, &parts_count);
    Ast *n = asnix_ast_new_at(AST_STRING, cur(p).start);
    n->as.string.parts = parts_items;
    n->as.string.count = parts_count;
    return n;
}

static Ast *parse_indented_string(Parser *p) {
    advance(p);
    size_t raw_mark = rawind_mark();
    while (cur(p).kind != TOK_ISTR_END) {
        if (cur(p).kind == TOK_ISTR_PART) {
            Token t = cur(p);
            rawind_push((RawIndPart){ .is_expr = 0, .text = tok_text(p, t), .text_len = tok_len(t),
                                       .has_indentation = 1 });
            advance(p);
        } else if (cur(p).kind == TOK_ISTR_ESC) {
            Token t = cur(p);
            char decoded[2];
            size_t dlen;
            asnix_decode_istr_esc(p->src, t.start, decoded, &dlen);
            char *owned = malloc(dlen);
            memcpy(owned, decoded, dlen);
            rawind_push((RawIndPart){ .is_expr = 0, .text = owned, .text_len = dlen, .has_indentation = 0 });
            advance(p);
        } else if (cur(p).kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a string interpolation");
            rawind_push((RawIndPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "asnix: parse error: malformed indented string literal\n");
            exit(1);
        }
    }
    advance(p);

    size_t raw_count;
    RawIndPart *raw_items = rawind_commit(raw_mark, &raw_count);
    size_t count;
    AstStringPart *parts = strip_indentation(raw_items, raw_count, &count);
    free(raw_items);

    Ast *n = asnix_ast_new_at(AST_STRING, cur(p).start);
    n->as.string.parts = parts;
    n->as.string.count = count;
    return n;
}

static Ast *parse_atom(Parser *p) {
    Token t = cur(p);
    switch (t.kind) {
        case TOK_INT: {
            advance(p);
            Ast *n = asnix_ast_new_at(AST_INT, cur(p).start);
            n->as.int_val = strtoll(p->src + t.start, NULL, 10);
            return n;
        }
        case TOK_FLOAT: {
            advance(p);
            Ast *n = asnix_ast_new_at(AST_FLOAT, cur(p).start);
            n->as.float_val = asnix_decode_float_literal(p->src, t.start, t.end);
            return n;
        }
        case TOK_TRUE: {
            advance(p);
            Ast *n = asnix_ast_new_at(AST_BOOL, cur(p).start);
            n->as.bool_val = 1;
            return n;
        }
        case TOK_FALSE: {
            advance(p);
            Ast *n = asnix_ast_new_at(AST_BOOL, cur(p).start);
            n->as.bool_val = 0;
            return n;
        }
        case TOK_NULL: {
            advance(p);
            return asnix_ast_new_at(AST_NULL, cur(p).start);
        }
        case TOK_STR_START:
            return parse_dquote_string(p);
        case TOK_ISTR_START:
            return parse_indented_string(p);
        case TOK_IDENT:
        case TOK_OR: {
            if (t.kind == TOK_IDENT && tok_len(t) == 8 && memcmp(tok_text(p, t), "__curPos", 8) == 0) {
                advance(p);
                Ast *n = asnix_ast_new_at(AST_CURPOS, cur(p).start);
                curpos_line_col(p->curpos_base, p->src + t.start, &n->as.curpos.line, &n->as.curpos.column);
                return n;
            }
            advance(p);
            Ast *n = asnix_ast_new_at(AST_VAR, cur(p).start);
            n->as.var.name = tok_text(p, t);
            n->as.var.name_len = tok_len(t);
            return n;
        }
        case TOK_PATH: {
            advance(p);
            if (cur(p).kind == TOK_DOLLAR_LBRACE) {
                return parse_path_literal_interp(p, t);
            }
            Ast *n = asnix_ast_new_at(AST_PATH, cur(p).start);
            n->as.path.text = tok_text(p, t);
            n->as.path.len = tok_len(t);
            n->as.path.is_search = 0;
            n->as.path.is_home = (p->src[t.start] == '~');
            n->as.path.interp_parts = NULL;
            n->as.path.interp_count = 0;
            return n;
        }
        case TOK_URI: {
            advance(p);
            Ast *n = asnix_ast_new_at(AST_STRING, cur(p).start);
            AstStringPart *parts = malloc(sizeof(AstStringPart));
            parts[0].is_expr = 0;
            parts[0].text = tok_text(p, t);
            parts[0].text_len = tok_len(t);
            parts[0].expr = NULL;
            n->as.string.parts = parts;
            n->as.string.count = 1;
            return n;
        }
        case TOK_SEARCH_PATH: {
            advance(p);
            Ast *n = asnix_ast_new_at(AST_PATH, cur(p).start);
            n->as.path.text = tok_text(p, t);
            n->as.path.len = tok_len(t);
            n->as.path.is_search = 1;
            n->as.path.is_home = 0;
            n->as.path.interp_parts = NULL;
            n->as.path.interp_count = 0;
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
            size_t elems_mark = astvec_mark();
            while (cur(p).kind != TOK_RBRACKET) astvec_push(parse_select(p));
            advance(p);
            size_t elems_count;
            Ast **elems_items = astvec_commit(elems_mark, &elems_count);
            Ast *n = asnix_ast_new_at(AST_LIST, cur(p).start);
            n->as.list.elems = elems_items;
            n->as.list.count = elems_count;
            return n;
        }
        case TOK_LBRACE: {
            advance(p);
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            int lt_prev_lbrace = asnix_ast_enter_retained();
            while (cur(p).kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p);
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            VEC_TRIM(bindings, AstBinding);
            Ast *n = asnix_ast_new_at(AST_ATTRSET, cur(p).start);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 0;
            merge_bt_dyn_into(&bt, &dyn);
            n->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            n->as.attrset.dyn_count = dyn.len;
            asnix_ast_leave_retained(lt_prev_lbrace);
            return n;
        }
        case TOK_REC: {
            advance(p);
            expect(p, TOK_LBRACE, "'{'");
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            int lt_prev_rec = asnix_ast_enter_retained();
            while (cur(p).kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p);
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            VEC_TRIM(bindings, AstBinding);
            Ast *n = asnix_ast_new_at(AST_ATTRSET, cur(p).start);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 1;
            merge_bt_dyn_into(&bt, &dyn);
            n->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            n->as.attrset.dyn_count = dyn.len;
            asnix_ast_leave_retained(lt_prev_rec);
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
    while (cur(p).kind == TOK_DOT) {
        advance(p);
        Ast *n = asnix_ast_new_at(AST_SELECT, cur(p).start);
        n->as.select.target = e;
        n->as.select.default_ = NULL;
        AttrSeg seg = parse_attr_segment(p, "an attribute name");
        set_select_segment(n, seg);
        e = n;
        had_select = 1;
    }
    if (had_select && cur(p).kind == TOK_OR) {
        advance(p);
        Ast *default_ast = parse_select(p);
        Ast *chain_node = e;
        while (chain_node->kind == AST_SELECT && chain_node->as.select.default_ == NULL) {
            chain_node->as.select.default_ = default_ast;
            Ast *next = chain_node->as.select.target;
            if (next->kind != AST_SELECT) break;
            chain_node = next;
        }
    }
    return e;
}

static Ast *parse_app(Parser *p) {
    Ast *fn = parse_select(p);
    while (starts_atom(cur(p).kind)) {
        Ast *arg = parse_select(p);
        Ast *app = asnix_ast_new_at(AST_APP, cur(p).start);
        app->as.app.fn = fn;
        app->as.app.arg = arg;
        fn = app;
    }
    return fn;
}

static Ast *parse_binop(Parser *p, int min_prec);
#define PREC_ADD 6
static Ast *parse_negate(Parser *p) {
    if (cur(p).kind == TOK_MINUS) {
        advance(p);
        Ast *operand = parse_negate(p);
        Ast *zero = asnix_ast_new_at(AST_INT, cur(p).start);
        zero->as.int_val = 0;
        Ast *n = asnix_ast_new_at(AST_BINOP, cur(p).start);
        n->as.binop.op = '-';
        n->as.binop.lhs = zero;
        n->as.binop.rhs = operand;
        return n;
    }
    if (cur(p).kind == TOK_BANG) {
        advance(p);
        Ast *operand = parse_binop(p, PREC_ADD);
        Ast *n = asnix_ast_new_at(AST_NOT, cur(p).start);
        n->as.not1.operand = operand;
        return n;
    }
    return parse_app(p);
}

static Ast *parse_has_attr(Parser *p) {
    Ast *e = parse_negate(p);
    while (cur(p).kind == TOK_QUESTION) {
        advance(p);
        AttrSeg seg = parse_attr_segment(p, "an attribute name");
        Ast *n = asnix_ast_new_at(AST_HAS_ATTR, cur(p).start);
        n->as.has_attr.target = e;
        set_has_attr_segment(n, seg);
        Ast *result = n;
        Ast *running_target = e;
        while (cur(p).kind == TOK_DOT) {
            advance(p);
            Ast *sel = asnix_ast_new_at(AST_SELECT, cur(p).start);
            sel->as.select.target = running_target;
            set_select_segment(sel, seg);
            Ast *empty_attrs = asnix_ast_new_at(AST_ATTRSET, cur(p).start);
            empty_attrs->as.attrset.bindings = NULL;
            empty_attrs->as.attrset.count = 0;
            empty_attrs->as.attrset.is_rec = 0;
            empty_attrs->as.attrset.dyn_bindings = NULL;
            empty_attrs->as.attrset.dyn_count = 0;
            sel->as.select.default_ = empty_attrs;
            running_target = sel;

            seg = parse_attr_segment(p, "an attribute name");
            Ast *next_check = asnix_ast_new_at(AST_HAS_ATTR, cur(p).start);
            next_check->as.has_attr.target = running_target;
            set_has_attr_segment(next_check, seg);

            Ast *and_node = asnix_ast_new_at(AST_AND, cur(p).start);
            and_node->as.logic.lhs = result;
            and_node->as.logic.rhs = next_check;
            result = and_node;
        }
        e = result;
    }
    return e;
}

typedef enum { BINOP_K_BINOP, BINOP_K_AND, BINOP_K_OR, BINOP_K_IMPL } BinOpKind;
typedef struct {
    TokKind tok;
    int prec;
    int right_assoc;
    int non_assoc;
    BinOpKind kind;
    char op;
} BinOpInfo;

static const BinOpInfo BINOPS[] = {
    { TOK_ARROW,      0, 1, 0, BINOP_K_IMPL,  0   },
    { TOK_OROR,       1, 0, 0, BINOP_K_OR,    0   },
    { TOK_ANDAND,     2, 0, 0, BINOP_K_AND,   0   },
    { TOK_EQEQ,       3, 0, 1, BINOP_K_BINOP, 'e' },
    { TOK_NEQ,        3, 0, 1, BINOP_K_BINOP, 'n' },
    { TOK_LT,         4, 0, 1, BINOP_K_BINOP, 'l' },
    { TOK_GT,         4, 0, 1, BINOP_K_BINOP, 'g' },
    { TOK_LE,         4, 0, 1, BINOP_K_BINOP, 'L' },
    { TOK_GE,         4, 0, 1, BINOP_K_BINOP, 'G' },
    { TOK_SLASHSLASH, 5, 1, 0, BINOP_K_BINOP, 'u' },
    { TOK_PLUS,       6, 0, 0, BINOP_K_BINOP, '+' },
    { TOK_MINUS,      6, 0, 0, BINOP_K_BINOP, '-' },
    { TOK_STAR,       7, 0, 0, BINOP_K_BINOP, '*' },
    { TOK_SLASH,      7, 0, 0, BINOP_K_BINOP, '/' },
    { TOK_PLUSPLUS,   8, 1, 0, BINOP_K_BINOP, 'c' },
};
#define BINOPS_COUNT (sizeof(BINOPS) / sizeof(BINOPS[0]))

static const BinOpInfo *lookup_binop(TokKind k) {
    for (size_t i = 0; i < BINOPS_COUNT; i++) {
        if (BINOPS[i].tok == k) return &BINOPS[i];
    }
    return NULL;
}

static Ast *make_binop_node(const BinOpInfo *info, Ast *lhs, Ast *rhs) {
    switch (info->kind) {
        case BINOP_K_AND: {
            Ast *n = asnix_ast_new(AST_AND);
            n->as.logic.lhs = lhs;
            n->as.logic.rhs = rhs;
            return n;
        }
        case BINOP_K_OR: {
            Ast *n = asnix_ast_new(AST_OR);
            n->as.logic.lhs = lhs;
            n->as.logic.rhs = rhs;
            return n;
        }
        case BINOP_K_IMPL: {
            Ast *not_lhs = asnix_ast_new(AST_NOT);
            not_lhs->as.not1.operand = lhs;
            Ast *n = asnix_ast_new(AST_OR);
            n->as.logic.lhs = not_lhs;
            n->as.logic.rhs = rhs;
            return n;
        }
        case BINOP_K_BINOP:
        default: {
            Ast *n = asnix_ast_new(AST_BINOP);
            n->as.binop.op = info->op;
            n->as.binop.lhs = lhs;
            n->as.binop.rhs = rhs;
            return n;
        }
    }
}

static Ast *parse_binop(Parser *p, int min_prec);

static Ast *parse_binop_primary(Parser *p) {
    if (cur(p).kind == TOK_BANG) {
        advance(p);
        Ast *operand = parse_binop(p, PREC_ADD);
        Ast *n = asnix_ast_new_at(AST_NOT, cur(p).start);
        n->as.not1.operand = operand;
        return n;
    }
    return parse_has_attr(p);
}

static Ast *parse_binop(Parser *p, int min_prec) {
    Ast *lhs = parse_binop_primary(p);
    int last_non_assoc_prec = -1;
    for (;;) {
        const BinOpInfo *info = lookup_binop(cur(p).kind);
        if (!info || info->prec < min_prec) break;
        if (info->non_assoc && info->prec == last_non_assoc_prec) break;
        advance(p);
        int next_min_prec = info->right_assoc ? info->prec : info->prec + 1;
        Ast *rhs = parse_binop(p, next_min_prec);
        lhs = make_binop_node(info, lhs, rhs);
        if (info->non_assoc) last_non_assoc_prec = info->prec;
    }
    return lhs;
}

static Ast *parse_impl(Parser *p) {
    return parse_binop(p, 0);
}

static Ast *parse_let_bindings(Parser *p) {
    BTVec bt = { 0 };
    while (cur(p).kind != TOK_IN) parse_binding_stmt(p, &bt, NULL);
    advance(p);
    Ast *body = parse_expr(p);

    if (bt.dyn.len > 0) {
        fprintf(stderr, "asnix: parse error: dynamic attributes not allowed in let\n");
        exit(1);
    }
    BindingVec bindings = { 0 };
    int lt_prev_let_finalize = asnix_ast_enter_retained();
    finalize_bt_vec(&bt, &bindings);
    asnix_ast_leave_retained(lt_prev_let_finalize);
    VEC_TRIM(bindings, AstBinding);

    Ast *n = asnix_ast_new_at(AST_LET, cur(p).start);
    n->as.let1.bindings = bindings.items;
    n->as.let1.count = bindings.len;
    n->as.let1.body = body;
    return n;
}

static int is_formals_pattern(Parser *p) {
    TokKind t1 = peek2(p).kind;
    if (t1 == TOK_ELLIPSIS) return 1;
    if (t1 == TOK_RBRACE) {
        TokKind t2 = peek3(p);
        return t2 == TOK_COLON || t2 == TOK_AT;
    }
    if (t1 == TOK_IDENT) {
        TokKind t2 = peek3(p);
        return t2 == TOK_COMMA || t2 == TOK_QUESTION || t2 == TOK_RBRACE;
    }
    return 0;
}

static void parse_formals(Parser *p, AstFormal **out_items, size_t *out_count, int *out_ellipsis) {
    size_t formals_mark = formalvec_mark();
    int ellipsis = 0;
    while (cur(p).kind != TOK_RBRACE) {
        if (cur(p).kind == TOK_ELLIPSIS) {
            advance(p);
            ellipsis = 1;
            break;
        }
        Token name = expect(p, TOK_IDENT, "a formal name");
        Ast *default_ = NULL;
        if (cur(p).kind == TOK_QUESTION) {
            advance(p);
            default_ = parse_expr(p);
        }
        formalvec_push((AstFormal){ .name = tok_text(p, name), .name_len = tok_len(name), .default_ = default_ });
        if (cur(p).kind == TOK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }
    expect(p, TOK_RBRACE, "'}'");
    *out_items = formalvec_commit(formals_mark, out_count);
    *out_ellipsis = ellipsis;
}

static Ast *parse_expr(Parser *p) {
    if (cur(p).kind == TOK_LET) {
        advance(p);
        if (cur(p).kind == TOK_LBRACE) {
            advance(p);
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            int lt_prev_letbrace = asnix_ast_enter_retained();
            while (cur(p).kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p);
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            VEC_TRIM(bindings, AstBinding);
            Ast *attrs = asnix_ast_new_at(AST_ATTRSET, cur(p).start);
            attrs->as.attrset.bindings = bindings.items;
            attrs->as.attrset.count = bindings.len;
            attrs->as.attrset.is_rec = 1;
            merge_bt_dyn_into(&bt, &dyn);
            attrs->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            attrs->as.attrset.dyn_count = dyn.len;
            asnix_ast_leave_retained(lt_prev_letbrace);
            Ast *n = asnix_ast_new_at(AST_SELECT, cur(p).start);
            n->as.select.target = attrs;
            n->as.select.attr = "body";
            n->as.select.attr_len = 4;
            n->as.select.attr_expr = NULL;
            n->as.select.default_ = NULL;
            return n;
        }
        return parse_let_bindings(p);
    }
    if (cur(p).kind == TOK_WITH) {
        advance(p);
        Ast *attrs = parse_expr(p);
        expect(p, TOK_SEMI, "';'");
        Ast *body = parse_expr(p);
        Ast *n = asnix_ast_new_at(AST_WITH, cur(p).start);
        n->as.with1.attrs = attrs;
        n->as.with1.body = body;
        return n;
    }
    if (cur(p).kind == TOK_IF) {
        advance(p);
        Ast *cond = parse_expr(p);
        expect(p, TOK_THEN, "'then'");
        Ast *then_ = parse_expr(p);
        expect(p, TOK_ELSE, "'else'");
        Ast *else_ = parse_expr(p);
        Ast *n = asnix_ast_new_at(AST_IF, cur(p).start);
        n->as.if1.cond = cond;
        n->as.if1.then_ = then_;
        n->as.if1.else_ = else_;
        return n;
    }
    if (cur(p).kind == TOK_IDENT && peek2(p).kind == TOK_COLON) {
        Token param = cur(p);
        advance(p);
        advance(p);
        Ast *body = parse_expr(p);
        Ast *n = asnix_ast_new_at(AST_LAMBDA, cur(p).start);
        n->as.lambda.param = tok_text(p, param);
        n->as.lambda.param_len = tok_len(param);
        n->as.lambda.body = body;
        return n;
    }
    if (cur(p).kind == TOK_ASSERT) {
        advance(p);
        Token start_tok = cur(p);
        Ast *cond = parse_expr(p);
        Token semi_tok = cur(p);
        expect(p, TOK_SEMI, "';'");
        size_t span_len = (size_t)(semi_tok.start - start_tok.start);
        while (span_len > 0 && isspace((unsigned char)p->src[start_tok.start + span_len - 1])) span_len--;
        Ast *body = parse_expr(p);
        Ast *n = asnix_ast_new_at(AST_ASSERT, cur(p).start);
        n->as.assert1.cond = cond;
        n->as.assert1.body = body;
        n->as.assert1.cond_text = p->src + start_tok.start;
        n->as.assert1.cond_text_len = span_len;
        return n;
    }
    if (cur(p).kind == TOK_IDENT && peek2(p).kind == TOK_AT) {
        Token asname = cur(p);
        advance(p);
        advance(p);
        expect(p, TOK_LBRACE, "'{' after '@'");
        AstFormal *formals;
        size_t fcount;
        int ellipsis;
        parse_formals(p, &formals, &fcount, &ellipsis);
        expect(p, TOK_COLON, "':'");
        Ast *body = parse_expr(p);
        Ast *n = asnix_ast_new_at(AST_LAMBDA, cur(p).start);
        n->as.lambda.is_pattern = 1;
        n->as.lambda.formals = formals;
        n->as.lambda.formals_count = fcount;
        n->as.lambda.has_ellipsis = ellipsis;
        n->as.lambda.has_as_name = 1;
        n->as.lambda.as_name = tok_text(p, asname);
        n->as.lambda.as_name_len = tok_len(asname);
        n->as.lambda.body = body;
        return n;
    }
    if (cur(p).kind == TOK_LBRACE) {
        if (is_formals_pattern(p)) {
            advance(p);
            AstFormal *formals;
            size_t fcount;
            int ellipsis;
            parse_formals(p, &formals, &fcount, &ellipsis);
            int has_as = 0;
            const char *as_name = NULL;
            size_t as_name_len = 0;
            if (cur(p).kind == TOK_AT) {
                advance(p);
                Token asname = expect(p, TOK_IDENT, "a name after '@'");
                has_as = 1;
                as_name = tok_text(p, asname);
                as_name_len = tok_len(asname);
            }
            expect(p, TOK_COLON, "':'");
            Ast *body = parse_expr(p);
            Ast *n = asnix_ast_new_at(AST_LAMBDA, cur(p).start);
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
    }
    return parse_impl(p);
}

Ast *asnix_parse(const char *src) {
    Lexer *lx = lexer_new(src);
    Parser p = { .lx = lx, .src = src, .curpos_base = src };
    p.tok = lexer_next(lx);
    p.next = lexer_next(lx);
    Ast *result = parse_expr(&p);
    expect(&p, TOK_EOF, "end of input");
    lexer_free(lx);
    return result;
}

Ast *asnix_parse_subexpr(const char *src, size_t start_offset) {
    Lexer *lx = lexer_new_lenient(src + start_offset);
    Parser p = { .lx = lx, .src = src + start_offset, .curpos_base = src };
    p.tok = lexer_next(lx);
    p.next = lexer_next(lx);
    Ast *result = parse_expr(&p);
    lexer_free(lx);
    return result;
}

