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

/* `${expr} = value;` -- a dynamic attrset-literal key. Collected
 * separately from BTVec/BTNode (the static-key tree): the name isn't
 * known until runtime, so it can't participate in that tree's compile-
 * time dup-detection/nested-attrpath merging at all -- see compiler.c's
 * AST_ATTRSET case for how these are applied (after the static part is
 * built, via a runtime insert-with-collision-check op). Real Nix allows
 * this in a plain `{...}`/`rec {...}` literal but explicitly rejects it
 * in `let` ("dynamic attributes not allowed in let", confirmed directly
 * against cppnix's own grammar, `~/nix/src/libexpr/parser.y`'s `LET
 * binds IN_KW ...` action) -- `parse_binding_stmt`'s `dyn_out` parameter
 * is NULL at the one call site (`let`) where that rejection applies.
 * A dynamic segment followed by more attrpath segments (`${k}.a = v;`)
 * or one after a leading static segment (`a.${k} = v;`) is also
 * supported (M41) -- see parse_binding_stmt's attrpath-sugar branch and
 * build_dynamic_chain_value, which handle every position uniformly;
 * this vector only ever receives the entries that end up as *this*
 * block's own direct dynamic bindings (a dynamic chain nested under a
 * static prefix instead becomes that prefix's leaf value via
 * build_dynamic_chain_value, never pushed here directly). */
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

/* Converts a parse-time DynBindingVec into the plain AstDynBinding array
 * AST_ATTRSET actually stores (ast.h has no reason to know this file's
 * own growable-vector type). */
static AstDynBinding *finalize_dyn_vec(DynBindingVec *vec) {
    if (vec->len == 0) return NULL;
    AstDynBinding *out = malloc(vec->len * sizeof(AstDynBinding));
    for (size_t i = 0; i < vec->len; i++) {
        out[i] = (AstDynBinding){ .name_expr = vec->items[i].name_expr, .value = vec->items[i].value };
    }
    return out;
}

/* Attrpath binding sugar (`a.b.c = val;`, real Nix's `attrpath '=' expr
 * ';'` grammar rule -- see cppnix's parser.y/parser-state.hh's addAttr):
 * a *tree* of these is built incrementally while parsing one binds-block
 * (one `{...}`/`rec{...}`/`let`'s immediate binding list), then flattened
 * into ordinary nested AST_ATTRSET Ast nodes only once the whole block is
 * done (finalize_bt_vec below) -- an attrpath statement anywhere in the
 * block can still extend a sibling attrpath statement's nested attrset
 * parsed earlier in the same block (`a.b = 1; a.c = 2;` => `a = { b = 1;
 * c = 2; };`, confirmed order-independent against nix-instantiate), so
 * nothing can be finalized to a flat Ast until the whole block is known.
 *
 * Each node is either a LEAF (name has a definitive value: an ordinary
 * `name = expr;`, or an `inherit`ed name) or a BRANCH (name was only ever
 * used as a non-final attrpath segment, so it implicitly denotes a nested
 * attrset -- never both. This tree only ever holds STATIC segments
 * (`AttrSeg.is_dynamic == 0`) -- a dynamic segment anywhere in an
 * attrpath (`a.${k} = v;`, `${k}.b = v;`, `a.${k1}.${k2} = v;`, M41) is
 * handled by splitting the chain at the first dynamic segment:
 * everything before it still flows through this static tree as normal;
 * everything from the first dynamic segment onward becomes a synthetic
 * nested plain-attrset literal with a `${...} = ...;` dynamic binding
 * (reusing M28's existing DynBindingVec/OP_INSERT_DYNAMIC_ATTR
 * machinery) as that static prefix's leaf value -- see
 * parse_binding_stmt's attrpath-sugar branch below for exactly where
 * that split happens. */
typedef struct {
    int is_dynamic;
    Ast *name_expr;      /* valid when is_dynamic */
    const char *attr;    /* valid when !is_dynamic */
    size_t attr_len;
} AttrSeg;

typedef struct BTNode BTNode;
/* M67: `dyn` -- dynamic (`${expr} = value;`) bindings absorbed into THIS
 * level of the tree by `reopen_attrset_into` (below), carried alongside
 * the ordinary static `items`/`len`/`cap` since a BTNode has no static
 * name to merge them under. Empty/zero for the overwhelming majority of
 * BTVecs, which never go through a merge involving a dynamic-keyed
 * literal at all. */
typedef struct { BTNode *items; size_t len, cap; DynBindingVec dyn; } BTVec;

/* M67: folds a BTVec's own absorbed-via-merge dynamic bindings (see
 * reopen_attrset_into's comment) into this BLOCK's top-level explicit
 * `${expr} = value;` DynBindingVec, so one `finalize_dyn_vec` call at
 * each `{...}`/`rec {...}` call site below picks up both kinds. */
static void merge_bt_dyn_into(BTVec *bt, DynBindingVec *dyn_out) {
    for (size_t i = 0; i < bt->dyn.len; i++) dynbindingvec_push(dyn_out, bt->dyn.items[i]);
}

struct BTNode {
    const char *name;
    size_t name_len;
    int is_leaf;
    Ast *leaf_value;   /* valid iff is_leaf */
    int is_inherit;    /* valid iff is_leaf */
    int rec;           /* valid iff !is_leaf: whether the attrset this branch finalizes
                         * into should be `rec`. Always 0 for a branch created purely by
                         * attrpath-sugar descent (`a.b = 1;` makes an ordinary, non-rec
                         * nested `a`, confirmed against nix-instantiate: ExprAttrs
                         * defaults non-recursive and addAttr's own intermediate-node
                         * creation never sets ->recursive). Only set to 1 when a branch
                         * was instead "reopened" from a pre-existing literal `rec {...}`
                         * value below (the regrettable-merge path) -- it then keeps that
                         * literal's original rec-ness, per real Nix's own documented
                         * quirk (NixOS/nix#9020: "any rec marker on [the earlier value]
                         * will apply to the attributes in [the later one]"). */
    BTVec children;    /* valid iff !is_leaf */
};

static void btvec_push(BTVec *v, BTNode item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof(BTNode));
    }
    v->items[v->len++] = item;
}

static BTNode *btvec_find(BTVec *v, const char *name, size_t name_len) {
    for (size_t i = 0; i < v->len; i++) {
        if (v->items[i].name_len == name_len && memcmp(v->items[i].name, name, name_len) == 0) {
            return &v->items[i];
        }
    }
    return NULL;
}

static void fatal_dup_attr(const char *name, size_t name_len) {
    fprintf(stderr, "asnix: attribute '%.*s' already defined, aborting\n", (int)name_len, name);
    exit(1);
}

static void insert_attrpath(BTVec *vec, AttrSeg *segs, size_t seg_count, size_t idx,
                             Ast *leaf_value, int is_inherit);

/* Re-opens a pre-existing, already-finalized AST_ATTRSET literal (`attrs`)
 * as a mutable BTVec of leaf nodes, so a later attrpath statement in the
 * same binds-block can still extend or merge into it -- the mechanism
 * behind both the plain "same name, both attrset-literal values" dup-key
 * merge (`inner = {...}; inner = {...};`) and the "regrettable" attrpath-
 * into-a-literal merge (`a = rec {...}; a.c = ...;`, NixOS/nix#9020) via
 * the exact same code path real Nix's addAttr uses for both. */
static void reopen_attrset_into(BTVec *dest, Ast *attrs) {
    /* M67: `a."${k}" = 1; a."${k2}" = 2;` / `a.b = 1; a."${k}" = 2;`-style
     * regrettable merges (NixOS/nix#9020) of a literal that itself had a
     * dynamic key -- previously fatal unconditionally ("cannot merge an
     * attrpath into a literal attrset that has a dynamic key"), found
     * actually hit by numtide/nix-conformance's eval-okay-dynamic-attrs-2/
     * eval-okay-merge-dynamic-attrs.nix, not just a hypothetical. Carried
     * into `dest->dyn` (BTVec's own comment) instead of being dropped or
     * fataling -- `finalize_bt_node`/the top-level block parser (below)
     * re-attach it to whichever AST_ATTRSET this BTVec ultimately
     * finalizes into, same as this project's own direct `${k} = v;`
     * dynamic bindings already were. Verified against real
     * `nix-instantiate --eval`: both files' listed cases evaluate as
     * their own `.exp` says (plain merge of the static and dynamic
     * attributes, duplicate-key rules applying across the two kinds
     * exactly like real Nix's `addAttr` -- this project doesn't attempt
     * to replicate the "two dynamic keys that happen to collide at
     * runtime" dup-attr check, same OP_INSERT_DYNAMIC_ATTR behavior as
     * every other dynamic binding already had before this fix). */
    for (size_t i = 0; i < attrs->as.attrset.dyn_count; i++) {
        AstDynBinding *db = &attrs->as.attrset.dyn_bindings[i];
        dynbindingvec_push(&dest->dyn, (DynBinding){ .name_expr = db->name_expr, .value = db->value });
    }
    for (size_t i = 0; i < attrs->as.attrset.count; i++) {
        AstBinding *b = &attrs->as.attrset.bindings[i];
        btvec_push(dest, (BTNode){
            .name = b->name, .name_len = b->name_len,
            .is_leaf = 1, .leaf_value = b->value, .is_inherit = b->is_inherit });
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
        /* Collision at the final segment: only mergeable if BOTH the
         * existing value and the new value are themselves literal
         * attrsets (real Nix's exact condition -- see addAttr's
         * `jAttrs && ae` check) -- anything else is a hard dup-attr
         * error, matching real Nix. */
        Ast *existing_attrs_val = existing->is_leaf ? existing->leaf_value : NULL;
        int existing_is_attrs_literal = existing->is_leaf && existing_attrs_val &&
                                         existing_attrs_val->kind == AST_ATTRSET;
        int new_is_attrs_literal = !is_inherit && leaf_value && leaf_value->kind == AST_ATTRSET;
        if (!existing->is_leaf && new_is_attrs_literal) {
            /* existing is already a branch (from earlier attrpath sugar) -- merge the
             * new literal attrset's own bindings into it. */
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
    /* Not the last segment: this name must denote (or become) a branch. */
    if (!existing) {
        BTNode node = { .name = seg.attr, .name_len = seg.attr_len, .is_leaf = 0, .rec = 0 };
        btvec_push(vec, node);
        existing = &vec->items[vec->len - 1];
        insert_attrpath(&existing->children, segs, seg_count, idx + 1, leaf_value, is_inherit);
        return;
    }
    if (existing->is_leaf) {
        /* Only mergeable if the existing leaf's value is itself a literal
         * attrset (the "regrettable" merge path -- real Nix's addAttr
         * dynamic_casts the existing value to ExprAttrs here too). */
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
            .value = node->leaf_value, .is_inherit = node->is_inherit };
    }
    BindingVec out = { 0 };
    for (size_t i = 0; i < node->children.len; i++) {
        bindingvec_push(&out, finalize_bt_node(&node->children.items[i]));
    }
    Ast *attrs = asnix_ast_new(AST_ATTRSET);
    attrs->as.attrset.bindings = out.items;
    attrs->as.attrset.count = out.len;
    attrs->as.attrset.is_rec = node->rec;
    /* M67: re-attach whatever dynamic bindings this branch absorbed via
     * reopen_attrset_into (see that function's own comment) -- a nested
     * synthetic attrset needs its own dyn_bindings just like a top-level
     * one does. */
    attrs->as.attrset.dyn_bindings = finalize_dyn_vec(&node->children.dyn);
    attrs->as.attrset.dyn_count = node->children.dyn.len;
    return (AstBinding){ .name = node->name, .name_len = node->name_len, .value = attrs, .is_inherit = 0 };
}

static void finalize_bt_vec(BTVec *vec, BindingVec *out) {
    for (size_t i = 0; i < vec->len; i++) {
        bindingvec_push(out, finalize_bt_node(&vec->items[i]));
    }
}

typedef struct {
    AstStringPart *items;
    size_t len, cap;
} PartVec;

/* A growable array of AttrSeg -- used by parse_binding_stmt to collect an
 * attrpath's segments (`a.b.c`/`a.${k}.c`) before handing them to
 * insert_attrpath (the static prefix) and/or build_dynamic_chain_value
 * (everything from the first dynamic segment onward). */
typedef struct {
    AttrSeg *items;
    size_t len, cap;
} AttrSegVec;

static void attrsegvec_push(AttrSegVec *v, AttrSeg item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->items = realloc(v->items, v->cap * sizeof(AttrSeg));
    }
    v->items[v->len++] = item;
}

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

/* M67: `or` is lexed as its own keyword token (TOK_OR, for the `.attr or
 * default` select-with-default form -- see parse_select), but real Nix's
 * grammar still accepts it as an ORDINARY IDENTIFIER in any position that
 * can't be confused with that form -- a binding/formal name, or a bare
 * variable reference (deprecated/"cursed" there with a warning, per
 * NixOS/nix#11121, but still accepted, not rejected -- confirmed against
 * real `nix-instantiate --eval`: `let or = 1; in or` evaluates to `1`
 * with only a warning on stderr). `starts_atom`/`parse_atom` below treat
 * a bare TOK_OR exactly like TOK_IDENT for this reason; found via
 * numtide/nix-conformance's eval-okay-attrs5.nix (`let or = x: y: x ||
 * y; in ...`, a plain binding name) and eval-okay-deprecate-cursed-or.nix
 * (bare `or` as a function-application argument) -- both previously hit
 * "parse error: expected a binding name"/failed to parse `or` as an
 * atom at all. */
static int starts_atom(TokKind k) {
    return k == TOK_INT || k == TOK_FLOAT || k == TOK_IDENT || k == TOK_TRUE || k == TOK_FALSE || k == TOK_NULL ||
           k == TOK_LPAREN || k == TOK_STR_START || k == TOK_ISTR_START || k == TOK_LBRACKET ||
           k == TOK_LBRACE || k == TOK_REC || k == TOK_PATH || k == TOK_SEARCH_PATH || k == TOK_OR;
}

static Ast *parse_expr(Parser *p);
static Ast *parse_dquote_string(Parser *p);

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

/* Parses one attrpath segment -- shared by `inherit`'s attribute-name
 * list, attrpath binding sugar (`a."b c".${k}.d = val;`), `.`-select
 * chains, and `?`-chains. Needed for names that collide with a reserved
 * keyword and so can't be written bare (e.g. nixpkgs's own
 * `lib/trivial.nix`: `"or" = x: y: x || y;`, and `lib/default.nix`:
 * `inherit (self.trivial) ... "or" ...;`) -- found evaluating real
 * nixpkgs. A dynamic segment (bare `${expr}` or an interpolated quoted
 * string) is likewise a real, supported case in every one of these
 * contexts EXCEPT `inherit` -- that one rejects it unconditionally,
 * confirmed in cppnix's own grammar (`attrs: attrs string_attr`'s
 * visitor throws "dynamic attributes not allowed in inherit" for the
 * `Expr*`/dynamic case specifically, not the plain-string case) -- see
 * that call site's own dedicated rejection check, not here, since a
 * static quoted-string segment must still be accepted for `inherit`.
 * Found needed evaluating real nixpkgs: `lib/systems/architectures.nix`
 * (or a sibling file reached from `d.nix`) has `"${ident}Fallback" =
 * fallback;`-shaped bindings. */
static AttrSeg parse_attr_segment(Parser *p, const char *what) {
    if (cur(p)->kind == TOK_DOLLAR_LBRACE) {
        advance(p);
        Ast *name_expr = parse_expr(p);
        expect(p, TOK_RBRACE, "'}' closing a dynamic attribute name");
        /* M67: a bare `${"literal"}` (no interpolation inside the string
         * literal itself -- `${"a" + "b"}` or `${foo}` still count as
         * genuinely dynamic) is a compile-time-constant name, same as
         * the quoted-string branch below already folds. Real Nix does
         * this too, generally (not just for `let`): `~/nix/src/libexpr/
         * include/nix/expr/parser-state.hh`'s `ToBeStringyExpr::visit`,
         * "consistently handle simple StringExpr ${"string"} as non-
         * dynamic attributes" (NixOS/nix#14642) -- used for exactly this
         * AttrPath-component position. Found via numtide/nix-
         * conformance's eval-okay-dynamic-attrs-3.nix (`let ${"a"} = 1;
         * in ...`, its own comment: "dynamic attrs are not generally
         * allowed in `let`... but they are if they only contain a
         * string") -- confirmed against real `nix-instantiate --eval`:
         * `let ${"a"} = 1; in a` == `1`, but `let ${"a"+""} = 1; in a`
         * still fails "dynamic attributes not allowed in let". */
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
    if (cur(p)->kind != TOK_STR_START) {
        /* M67: `or` as a bare (unquoted) binding/attribute name -- see
         * starts_atom's own comment above for the full rationale;
         * `"or"` (quoted) was already accepted here (this function's own
         * header comment), only the unquoted keyword-token form was
         * missing. */
        if (cur(p)->kind == TOK_OR) {
            Token *t = cur(p);
            advance(p);
            return (AttrSeg){ .is_dynamic = 0, .attr = t->start, .attr_len = t->len };
        }
        Token *t = expect(p, TOK_IDENT, what);
        return (AttrSeg){ .is_dynamic = 0, .attr = t->start, .attr_len = t->len };
    }
    /* Quoted-string segment -- reuses parse_dquote_string's own chunk-
     * collecting loop (same TOK_STR_PART/TOK_DOLLAR_LBRACE/TOK_STR_END
     * grammar as an ordinary string literal), then classifies the
     * result: zero parts (`""`) or exactly one non-interpolated literal
     * part is a compile-time-constant name (static); anything else
     * (an interpolation chunk present) is genuinely dynamic. Verified
     * against real `nix-instantiate --eval`: `let bar = "x"; in {
     * "foo${bar}" = 1; }` == `{ foox = 1; }`. */
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

/* Builds the value for an attrpath-sugar binding's segments
 * `segs[start..start+count)`, given the final `value` expression --
 * used for the portion of an attrpath STARTING AT the first dynamic
 * segment (M41: `a.${k}.b = v;`, `${k}.b = v;`, `a.${k1}.${k2} = v;`).
 * `count == 0` is the base case (the chain is exhausted; just return
 * `value` itself). Otherwise wraps the recursively-built inner value in
 * one more layer: a plain (non-`rec`) one-binding attrset literal, using
 * either an ordinary static binding (`{ segs[start] = inner; }`) or
 * M28's existing dynamic-binding machinery (`{ ${segs[start]} = inner;
 * }`) depending on that segment's own kind -- so a chain with multiple
 * dynamic segments (`a.${k1}.${k2} = v;`) composes correctly, each
 * nested dynamic binding getting its own OP_INSERT_DYNAMIC_ATTR at
 * runtime. Verified against real `nix-instantiate --eval`: `let k =
 * "a"; in { x.${k} = 1; }` == `{ x = { a = 1; }; }` and `let k = "a"; in
 * { ${k}.b = 1; }` == `{ a = { b = 1; }; }`. */
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

/* `dyn_out`: NULL rejects a dynamic `${...}` key outright ("dynamic
 * attributes not allowed in let", matching real Nix's own explicit
 * rejection there); non-NULL (plain `{...}`/`rec {...}`) collects it
 * instead of fataling. See DynBindingVec's own comment above. */
static void parse_binding_stmt(Parser *p, BTVec *out, DynBindingVec *dyn_out) {
    if (cur(p)->kind == TOK_INHERIT) {
        advance(p);
        Ast *src_expr = NULL;
        if (cur(p)->kind == TOK_LPAREN) {
            advance(p);
            src_expr = parse_expr(p);
            expect(p, TOK_RPAREN, "')'");
        }
        while (cur(p)->kind == TOK_IDENT || cur(p)->kind == TOK_STR_START || cur(p)->kind == TOK_DOLLAR_LBRACE) {
            AttrSeg name = parse_attr_segment(p, "an inherited name");
            /* real Nix's grammar lets a bare `${...}`/interpolated
             * string reach this position syntactically (`attrs: attrs
             * attr | attrs string_attr`), but rejects it unconditionally
             * once parsed -- confirmed in cppnix's own source (the
             * `string_attr` visitor throws "dynamic attributes not
             * allowed in inherit" specifically for the dynamic/`Expr*`
             * case, not the plain-string case, so `inherit (x) "or";`
             * still works -- see parse_attr_segment's own comment). */
            if (name.is_dynamic) {
                fprintf(stderr, "asnix: parse error: dynamic attributes not allowed in inherit\n");
                exit(1);
            }
            Ast *value;
            if (src_expr) {
                Ast *sel = asnix_ast_new(AST_SELECT);
                sel->as.select.target = src_expr;
                sel->as.select.attr = name.attr;
                sel->as.select.attr_len = name.attr_len;
                sel->as.select.attr_expr = NULL;
                sel->as.select.default_ = NULL;
                value = sel;
            } else {
                Ast *v = asnix_ast_new(AST_VAR);
                v->as.var.name = name.attr;
                v->as.var.name_len = name.attr_len;
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
             * scope, even though plain `x` would otherwise self-match.
             * An inherited name is always a single segment in real Nix's
             * own grammar (`attrs: attrs attr | ...`, never `attrpath`),
             * so it always goes through insert_attrpath as a length-1
             * path -- still subject to the same dup-attr/merge rules as
             * an ordinary binding if it collides with one. */
            insert_attrpath(out, &name, 1, 0, value, (src_expr == NULL));
        }
        expect(p, TOK_SEMI, "';'");
        return;
    }
    /* Attrpath binding sugar (`a.b.c = val;`, real Nix's `attrpath '='
     * expr ';'`) -- see the BTNode/insert_attrpath comment above for the
     * full merge semantics. Each segment may be a plain identifier, a
     * static quoted-string key, or (M41) a dynamic one (bare `${expr}`
     * or an interpolated quoted string) anywhere in the chain, including
     * as the very first segment -- see parse_attr_segment. Collect every
     * segment first, then split at the first dynamic one (if any): the
     * static prefix (possibly empty) still flows through insert_attrpath
     * as before; everything from the first dynamic segment onward
     * becomes a synthetic value via build_dynamic_chain_value, which
     * either becomes that static prefix's leaf value (non-empty prefix)
     * or -- when the very first segment is itself dynamic -- a direct
     * push to `dyn_out` (same `NULL` rejection as a bare `${expr} =
     * value;` binding: "dynamic attributes not allowed in let"). Found
     * needed evaluating real nixpkgs: `lib/systems/architectures.nix`
     * has `"${ident}Fallback" = fallback;`-shaped bindings. */
    AttrSegVec segs = { 0 };
    attrsegvec_push(&segs, parse_attr_segment(p, "a binding name"));
    while (cur(p)->kind == TOK_DOT) {
        advance(p);
        attrsegvec_push(&segs, parse_attr_segment(p, "an attribute name"));
    }
    expect(p, TOK_EQUALS, "'='");
    Ast *value = parse_expr(p);
    expect(p, TOK_SEMI, "';'");

    size_t first_dyn = segs.len;
    for (size_t i = 0; i < segs.len; i++) {
        if (segs.items[i].is_dynamic) { first_dyn = i; break; }
    }
    if (first_dyn == segs.len) {
        /* Fully static -- unchanged original behavior. */
        insert_attrpath(out, segs.items, segs.len, 0, value, 0);
    } else if (first_dyn == 0) {
        if (!dyn_out) {
            fprintf(stderr, "asnix: parse error: dynamic attributes not allowed in let\n");
            exit(1);
        }
        Ast *inner = build_dynamic_chain_value(segs.items, 1, segs.len - 1, value);
        dynbindingvec_push(dyn_out, (DynBinding){ .name_expr = segs.items[0].name_expr, .value = inner });
    } else {
        Ast *inner = build_dynamic_chain_value(segs.items, first_dyn, segs.len - first_dyn, value);
        insert_attrpath(out, segs.items, first_dyn, 0, inner, 0);
    }
    free(segs.items);
}

/* M64: an interpolated path literal's tail -- `t` is the already-consumed
 * first TOK_PATH chunk (e.g. "./" in "./${foo}", "~/" in "~/${foo}"),
 * known by the caller to be immediately followed by TOK_DOLLAR_LBRACE.
 * Collects zero or more TOK_PATH_PART literal chunks interleaved with
 * TOK_DOLLAR_LBRACE ... TOK_RBRACE interpolated expressions (same shape
 * parse_dquote_string uses for TOK_STR_PART, reusing AstStringPart
 * directly) up to the closing TOK_PATH_END, and builds an AST_PATH node
 * whose `text`/`len`/`is_home` are the first chunk and whose
 * `interp_parts`/`interp_count` hold everything after it -- see
 * compiler.c's AST_PATH case (resolve_path_chunk) for how the first
 * chunk alone becomes a Path constant and the rest becomes a runtime
 * OP_PATH_CONCAT chain. */
static Ast *parse_path_literal_interp(Parser *p, Token *t) {
    Ast *n = asnix_ast_new(AST_PATH);
    n->as.path.text = t->start;
    n->as.path.len = t->len;
    n->as.path.is_search = 0;
    n->as.path.is_home = t->is_home;

    PartVec parts = { 0 };
    while (cur(p)->kind != TOK_PATH_END) {
        if (cur(p)->kind == TOK_PATH_PART) {
            Token *pt = cur(p);
            partvec_push(&parts, (AstStringPart){ .is_expr = 0, .text = pt->start, .text_len = pt->len });
            advance(p);
        } else if (cur(p)->kind == TOK_DOLLAR_LBRACE) {
            advance(p);
            Ast *e = parse_expr(p);
            expect(p, TOK_RBRACE, "'}' closing a path interpolation");
            partvec_push(&parts, (AstStringPart){ .is_expr = 1, .expr = e });
        } else {
            fprintf(stderr, "asnix: parse error: malformed path literal\n");
            exit(1);
        }
    }
    advance(p); /* TOK_PATH_END */
    n->as.path.interp_parts = parts.items;
    n->as.path.interp_count = parts.len;
    return n;
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
        case TOK_FLOAT: {
            advance(p);
            Ast *n = asnix_ast_new(AST_FLOAT);
            n->as.float_val = t->float_val;
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
        case TOK_IDENT:
        case TOK_OR: { /* M67: bare `or` as an ordinary (deprecated/"cursed") variable -- see starts_atom's comment */
            advance(p);
            Ast *n = asnix_ast_new(AST_VAR);
            n->as.var.name = t->start;
            n->as.var.name_len = t->len;
            return n;
        }
        case TOK_PATH: {
            advance(p);
            /* M64: the lexer only ever follows a TOK_PATH with
             * TOK_DOLLAR_LBRACE when this chunk is the first (possibly
             * only) part of a path literal that continues into
             * interpolation (`./${foo}`, `~/${foo}`, ...) -- see
             * lexer.c's at_dollar_brace checks in asnix_lex's main loop.
             * A bare "${" can't otherwise appear here at all (it's not
             * valid Nix syntax outside a string/path literal), so this
             * peek is unambiguous. */
            if (cur(p)->kind == TOK_DOLLAR_LBRACE) {
                return parse_path_literal_interp(p, t);
            }
            Ast *n = asnix_ast_new(AST_PATH);
            n->as.path.text = t->start;
            n->as.path.len = t->len;
            n->as.path.is_search = 0;
            n->as.path.is_home = t->is_home;
            n->as.path.interp_parts = NULL;
            n->as.path.interp_count = 0;
            return n;
        }
        case TOK_SEARCH_PATH: {
            advance(p);
            Ast *n = asnix_ast_new(AST_PATH);
            n->as.path.text = t->start;
            n->as.path.len = t->len;
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
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            while (cur(p)->kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p); /* '}' */
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            Ast *n = asnix_ast_new(AST_ATTRSET);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 0;
            merge_bt_dyn_into(&bt, &dyn); /* M67 */
            n->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            n->as.attrset.dyn_count = dyn.len;
            return n;
        }
        case TOK_REC: {
            advance(p);
            expect(p, TOK_LBRACE, "'{'");
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            while (cur(p)->kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p); /* '}' */
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            Ast *n = asnix_ast_new(AST_ATTRSET);
            n->as.attrset.bindings = bindings.items;
            n->as.attrset.count = bindings.len;
            n->as.attrset.is_rec = 1;
            merge_bt_dyn_into(&bt, &dyn); /* M67 */
            n->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            n->as.attrset.dyn_count = dyn.len;
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
    while (cur(p)->kind == TOK_DOT) {
        advance(p);
        Ast *n = asnix_ast_new(AST_SELECT);
        n->as.select.target = e;
        n->as.select.default_ = NULL;
        /* Dynamic attribute name, `.${expr}` or an interpolated quoted
         * string (real Nix's grammar for a computed-at-runtime attrpath
         * segment, e.g. `set.${name}` -- found needed evaluating real
         * nixpkgs, pkgs/top-level/default.nix), or a plain identifier/
         * static quoted-string segment (`x."a b"`, verified against
         * `nix-instantiate --eval`: `let x = { "a b" = 5; }; in x."a
         * b"` => `5`) -- see parse_attr_segment's comment. `or` after a
         * dynamic segment (`set.${name} or default`) is a real,
         * supported combination (M28) -- handled uniformly below by the
         * same `had_select && TOK_OR` check a static segment uses,
         * compiler.c's AST_SELECT case picks OP_SELECT_DYNAMIC_OR
         * instead of the static OP_SELECT_OR when attr_expr is set. */
        AttrSeg seg = parse_attr_segment(p, "an attribute name");
        set_select_segment(n, seg);
        e = n;
        had_select = 1;
    }
    /* `expr.attr or default` -- real Nix's grammar attaches `or` to the
     * whole preceding `expr '.' attrpath`, not per-segment (confirmed
     * against nix-instantiate's own parser.y: `expr_simple '.' attrpath
     * OR_KW expr_select`), and `ExprSelect::eval` (eval.cc) evaluates
     * that whole multi-segment attrpath in ONE loop, falling through to
     * `default` the instant ANY segment is missing or the current value
     * isn't an attrset -- not just the last one. asnix's parser instead
     * builds a chain of nested single-segment AST_SELECT nodes (one per
     * `.segment`, `target` pointing at the previous node), so matching
     * that semantics means sharing the SAME `default_` Ast* across every
     * node of this one unbroken chain, not just the outermost (last) one
     * -- walk back down `target` while it's itself a freshly-built
     * AST_SELECT (default_ == NULL, true for every node of THIS chain
     * and only this chain, by construction: an earlier chain's `or`
     * would already have set it, and this loop built every node here
     * from scratch) and stamp all of them with the same pointer. Sharing
     * one Ast* subtree across multiple parent nodes is safe here --
     * compiler.c only ever READS `default_` to emit a fresh, independent
     * copy of its bytecode at each compile site, never mutates the AST --
     * verified by testing a chain with a side-effecting default
     * (`a.x.y or (builtins.trace "eval" default)`) evaluates it exactly
     * once, not once per missing segment. `default` itself is parsed at
     * this same parse_select tightness (matching real Nix: the default
     * is NOT a full `expr`, e.g. `+`/`==` outside are not absorbed into
     * it), recursively, so `a.b or c.d or e` chains sensibly. No `or` is
     * possible without at least one preceding `.select` (real Nix's
     * grammar agrees -- a bare `x or y` is the separate, unsupported
     * "cursed or" rule). */
    if (had_select && cur(p)->kind == TOK_OR) {
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
    while (starts_atom(cur(p)->kind)) {
        Ast *arg = parse_select(p);
        Ast *app = asnix_ast_new(AST_APP);
        app->as.app.fn = fn;
        app->as.app.arg = arg;
        fn = app;
    }
    return fn;
}

/* Unary `-` (negate): binds tighter than every binary/`?`/`++` operator
 * but looser than application/select -- `-f 3` == `-(f 3)` (not `(-f)
 * 3`), `-x ? "a"` == `(-x) ? "a"` (not `-(x ? "a")`), `-2 + 3` == `(-2) +
 * 3` == `1` -- all cross-checked against `nix-instantiate --eval` before
 * writing this (found entirely missing while chasing real nixpkgs:
 * `lib/trivial.nix` has `bitNot = builtins.sub (-1);`). Matches real
 * Nix's grammar (`~/nix/src/libexpr/parser.y`: `'-' expr_op %prec
 * NEGATE` desugars to `sub 0 $2`, i.e. `0 - expr_op` -- NEGATE is the
 * single tightest-declared operator in the whole table, but still
 * looser than implicit application/select, which aren't part of that
 * table at all). Reuses the existing binary `-` (AST_BINOP op='-') with
 * a literal `0` lhs, same desugaring real Nix itself uses -- no new AST
 * node or IR op needed. */
static Ast *parse_negate(Parser *p) {
    if (cur(p)->kind == TOK_MINUS) {
        advance(p);
        Ast *operand = parse_negate(p); /* right-recurse: allows `--x`, harmless */
        Ast *zero = asnix_ast_new(AST_INT);
        zero->as.int_val = 0;
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = '-';
        n->as.binop.lhs = zero;
        n->as.binop.rhs = operand;
        return n;
    }
    return parse_app(p);
}

/* `expr ? attr` (has-attr): binds tighter than arithmetic but looser than
 * application, matching real Nix's precedence table. Confirmed against
 * `nix-instantiate --eval '1 ? a'` => false (not an error) -- a non-
 * attrset left-hand side is simply "no", see op_has_attr.S. */
static Ast *parse_has_attr(Parser *p) {
    Ast *e = parse_negate(p);
    while (cur(p)->kind == TOK_QUESTION) {
        advance(p);
        /* `expr ? attrpath`, real Nix's grammar for the right-hand side
         * (`~/nix/src/libexpr/parser.y`: `expr_op '?' attrpath`) is a
         * full DOTTED chain, not a single segment -- `x ? a.b.c` checks
         * nested existence, confirmed against `nix-instantiate --eval`
         * (found needed evaluating real nixpkgs: `lib/systems/
         * default.nix` has `platform ? gcc.arch`). Each segment may be a
         * plain identifier, a static quoted-string, or a dynamic
         * `${expr}` (see parse_attr_segment/AttrSeg above). Desugars
         * directly to `x ? a && (x.a or {}) ? b && ((x.a or {}).b or {})
         * ? c ...` -- reusing the already-correct AST_HAS_ATTR/
         * AST_SELECT-with-`or`/AST_AND nodes, no new AST node needed
         * (dynamic segments needed one new IR op, OP_HAS_ATTR_DYNAMIC,
         * since AST_SELECT already had its own dynamic op). Verified
         * this desugaring matches real Nix exactly for the three cases
         * that actually distinguish it from a naive "just select, no
         * fallback" version: a missing intermediate attr, an
         * intermediate value that exists but isn't an attrset, and a
         * fully-present chain -- all graceful (`false`), none fatal. */
        AttrSeg seg = parse_attr_segment(p, "an attribute name");
        Ast *n = asnix_ast_new(AST_HAS_ATTR);
        n->as.has_attr.target = e;
        set_has_attr_segment(n, seg);
        Ast *result = n;
        Ast *running_target = e;
        while (cur(p)->kind == TOK_DOT) {
            advance(p);
            Ast *sel = asnix_ast_new(AST_SELECT);
            sel->as.select.target = running_target;
            set_select_segment(sel, seg);   /* the segment just checked above */
            Ast *empty_attrs = asnix_ast_new(AST_ATTRSET);
            empty_attrs->as.attrset.bindings = NULL;
            empty_attrs->as.attrset.count = 0;
            empty_attrs->as.attrset.is_rec = 0;
            empty_attrs->as.attrset.dyn_bindings = NULL;
            empty_attrs->as.attrset.dyn_count = 0;
            sel->as.select.default_ = empty_attrs;
            running_target = sel; /* == the previous target's `.attr or {}` */

            seg = parse_attr_segment(p, "an attribute name");
            Ast *next_check = asnix_ast_new(AST_HAS_ATTR);
            next_check->as.has_attr.target = running_target;
            set_has_attr_segment(next_check, seg);

            Ast *and_node = asnix_ast_new(AST_AND);
            and_node->as.logic.lhs = result;
            and_node->as.logic.rhs = next_check;
            result = and_node;
        }
        e = result;
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
    while (cur(p)->kind == TOK_STAR || cur(p)->kind == TOK_SLASH) {
        char op = cur(p)->kind == TOK_STAR ? '*' : '/';
        advance(p);
        Ast *rhs = parse_concat(p);
        Ast *n = asnix_ast_new(AST_BINOP);
        n->as.binop.op = op;
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
 *   parse_mul   * /             (already existed, M4)
 *   parse_concat ++             right-assoc, chainable -- binds TIGHTER than
 *                                * / + -, looser than ? and application (M23,
 *                                see parse_concat's own comment above parse_mul)
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
    BTVec bt = { 0 };
    while (cur(p)->kind != TOK_IN) parse_binding_stmt(p, &bt, NULL); /* NULL: reject dynamic attrs */
    advance(p); /* 'in' */
    Ast *body = parse_expr(p);

    /* M67: `let` has nowhere to put a dynamic binding absorbed via
     * reopen_attrset_into (AST_LET has no dyn_bindings field at all,
     * unlike AST_ATTRSET) -- fatal here with the same message a direct
     * top-level `let ${k} = v;` already gets (parse_binding_stmt's own
     * `dyn_out == NULL` check), rather than silently dropping it. Not
     * believed reachable today (every path into this would need a `let`
     * binding whose attrpath has a dynamic *non-first* segment that then
     * collides with a sibling attrpath statement -- e.g. `let a.${k} =
     * 1; a.b = 2; in ...` -- a shape nothing in this project's own
     * fixtures or numtide/nix-conformance exercises), kept purely as a
     * loud-not-silent guard. */
    if (bt.dyn.len > 0) {
        fprintf(stderr, "asnix: parse error: dynamic attributes not allowed in let\n");
        exit(1);
    }
    BindingVec bindings = { 0 };
    finalize_bt_vec(&bt, &bindings);

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
 * balanced in valid source independent of their semantic role -- EXCEPT a
 * string interpolation/dynamic-select's `${` lexes as its own distinct
 * TOK_DOLLAR_LBRACE (not TOK_LBRACE), while its closing brace is an
 * ordinary TOK_RBRACE like any other -- so it must also increment depth
 * here, or every `${...}`/`.${...}` inside the scanned range (e.g. a
 * formal's default-value expression) decrements depth one extra time
 * with no matching increment, closing the scan early at the WRONG brace
 * and misclassifying a genuine lambda pattern as a plain attrset literal
 * (found evaluating real nixpkgs: `pkgs/stdenv/linux/default.nix`'s
 * `bootstrapFiles ? let ... archLookupTable.${system} ... in ...`
 * default value has three `.${...}` dynamic selects before the formals
 * list's real closing `}`). */
static size_t find_matching_rbrace(Parser *p, size_t open_idx) {
    size_t depth = 0;
    size_t i = open_idx;
    for (;;) {
        TokKind k = p->toks[i].kind;
        if (k == TOK_LBRACE || k == TOK_DOLLAR_LBRACE) {
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
            BTVec bt = { 0 };
            DynBindingVec dyn = { 0 };
            while (cur(p)->kind != TOK_RBRACE) parse_binding_stmt(p, &bt, &dyn);
            advance(p); /* '}' */
            BindingVec bindings = { 0 };
            finalize_bt_vec(&bt, &bindings);
            Ast *attrs = asnix_ast_new(AST_ATTRSET);
            attrs->as.attrset.bindings = bindings.items;
            attrs->as.attrset.count = bindings.len;
            attrs->as.attrset.is_rec = 1;
            merge_bt_dyn_into(&bt, &dyn); /* M67 */
            attrs->as.attrset.dyn_bindings = finalize_dyn_vec(&dyn);
            attrs->as.attrset.dyn_count = dyn.len;
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
