#ifndef ASNIX_AST_H
#define ASNIX_AST_H

#include <stddef.h>
#include <stdint.h>

/* `let` is genuinely recursive (letrec), matching real Nix: every binding
 * in one `let ... in` is visible to itself and to every sibling binding,
 * regardless of source order. AST_LET therefore carries its whole binding
 * list (like AST_ATTRSET), not just one name -- see compiler.c's AST_LET
 * case for how that list becomes a single multi-slot Env frame built
 * *before* any binding's thunk is constructed. `rec { ... }` gives an
 * attrset literal the same self/sibling visibility (AST_ATTRSET.is_rec);
 * plain `{ ... }` keeps the old non-recursive behavior (bindings only see
 * the enclosing scope). */

typedef enum {
    AST_INT,
    AST_BOOL,
    AST_NULL,
    AST_STRING,
    AST_VAR,
    AST_LET,
    AST_IF,
    AST_LAMBDA,
    AST_APP,
    AST_BINOP,
    AST_LIST,
    AST_ATTRSET,
    AST_SELECT,
    AST_AND,    /* short-circuit `&&`: as.logic */
    AST_OR,     /* short-circuit `||`: as.logic */
    AST_NOT,    /* unary `!`: as.not1 */
    AST_ASSERT, /* `assert cond; body`: as.assert1 */
    AST_WITH,
    AST_HAS_ATTR,
    AST_PATH,
} AstKind;

typedef struct Ast Ast;

/* One piece of an AST_STRING literal: either a literal chunk of text
 * (escapes already decoded, and -- for a '' ... '' literal -- indentation
 * already stripped by the time the parser builds this) or an interpolated
 * expression ("${ ... }"). Plain-malloc'd, same rationale as AstBinding
 * below. */
typedef struct {
    int is_expr;
    const char *text;   /* valid when !is_expr */
    size_t text_len;
    Ast *expr;           /* valid when is_expr */
} AstStringPart;

/* One formal in a lambda-pattern (`{a, b ? default, ...}: body`) parameter
 * list. Plain-malloc'd, same rationale as AstBinding below. `default_`
 * is NULL for a required field (no `? expr`). */
typedef struct {
    const char *name;
    size_t name_len;
    Ast *default_;
} AstFormal;

/* One binding in a `let`/`rec {}`/`{}` binding list. Plain-malloc'd (not
 * arena-allocated): it's a compile-time-only auxiliary array, not part of
 * the runtime Value graph the arena exists to serve -- same rationale as
 * IrBuilder's growable arrays in irbuild.c.
 *
 * `inherit x;` / `inherit (expr) x y;` desugar at parse time (parser.c)
 * into ordinary bindings whose `value` is an AST_VAR(x) (bare form) or
 * AST_SELECT(expr, x) (`(expr)` form). Only the BARE form sets
 * `is_inherit`: verified against real nix-instantiate, `inherit (expr)
 * x;`'s `expr` resolves against the enclosing block's own (possibly
 * recursive) scope just like any ordinary binding's value -- siblings
 * are visible (`let inherit (pt) x; pt = {...}; in ...` sees `pt`) -- so
 * it needs no special treatment. A bare `inherit x;`, however, must
 * resolve `x` against the scope *enclosing* the let/rec/attrset, never
 * against that construct's own scope, so `rec { inherit x; }` can't
 * accidentally self-reference the `x` it's trying to pull in from
 * outside (also verified against real nix-instantiate). */
typedef struct {
    const char *name;
    size_t name_len;
    Ast *value;
    int is_inherit;
} AstBinding;

struct Ast {
    AstKind kind;
    union {
        int64_t int_val;   /* AST_INT */
        int bool_val;       /* AST_BOOL */
        struct {
            const char *name;
            size_t name_len;
        } var; /* AST_VAR */
        struct {
            AstBinding *bindings;
            size_t count;
            Ast *body;
        } let1; /* AST_LET */
        struct {
            Ast *cond, *then_, *else_;
        } if1; /* AST_IF */
        struct {
            const char *param;    /* plain-identifier form (`a: body`); unused when is_pattern */
            size_t param_len;
            Ast *body;
            int is_pattern;        /* 1 for `{...}: body` / `{...}@n: body` / `n@{...}: body` */
            /* Pattern-only fields below (see docs/architecture.md's lambda-patterns
             * notes): a destructuring lambda is still arity-1 at the OP_CALL ABI
             * level -- the compiler desugars the pattern into synthetic
             * AST_IF/AST_HAS_ATTR/AST_SELECT nodes over the one real attrset
             * argument (see compiler.c's AST_LAMBDA case). */
            AstFormal *formals;
            size_t formals_count;
            int has_ellipsis;      /* `...` present: extra fields beyond `formals` are allowed */
            int has_as_name;       /* an `@name` alias was given, either position */
            const char *as_name;
            size_t as_name_len;
        } lambda; /* AST_LAMBDA */
        struct {
            Ast *fn, *arg;
        } app; /* AST_APP */
        struct {
            /* '+' '-' '*'                  arithmetic (M4)
             * 'e' '==' 'n' '!='            equality/inequality
             * 'l' '<' 'g' '>' 'L' '<=' 'G' '>='   ordering (INT-only, see compiler.c)
             * 'u' '//'                     attrset update/merge */
            char op;
            Ast *lhs, *rhs;
        } binop; /* AST_BINOP */
        struct {
            AstStringPart *parts;
            size_t count;
        } string; /* AST_STRING */
        struct {
            Ast **elems;
            size_t count;
        } list; /* AST_LIST */
        struct {
            AstBinding *bindings;
            size_t count;
            int is_rec; /* `rec { ... }` vs plain `{ ... }` */
        } attrset; /* AST_ATTRSET */
        struct {
            Ast *target;
            const char *attr;
            size_t attr_len;
        } select; /* AST_SELECT */
        struct {
            Ast *lhs, *rhs;
        } logic; /* AST_AND / AST_OR -- compiled branch-shaped (if/then/else), not as an
                   * eager binop, so the untaken side is never evaluated (see compiler.c). */
        struct {
            Ast *operand;
        } not1; /* AST_NOT */
        struct {
            Ast *cond, *body;
            const char *cond_text; /* raw source span of `cond`, for the failure message */
            size_t cond_text_len;
        } assert1; /* AST_ASSERT */
        struct {
            Ast *attrs;
            Ast *body;
        } with1; /* AST_WITH */
        struct {
            Ast *target;
            const char *attr;
            size_t attr_len;
        } has_attr; /* AST_HAS_ATTR: `target ? attr` */
        struct {
            const char *text; /* raw lexeme: e.g. "./foo.nix", or the bare name for a search path */
            size_t len;
            int is_search; /* 1 for `<name>`-style search-path literals, 0 for ordinary path literals */
        } path; /* AST_PATH */
    } as;
};

/* Arena-allocated (never individually freed), same as every other runtime
 * structure in this process -- see src/asm/arena.S. */
Ast *asnix_ast_new(AstKind kind);

#endif /* ASNIX_AST_H */
