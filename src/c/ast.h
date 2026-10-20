#ifndef ASNIX_AST_H
#define ASNIX_AST_H

#include <stddef.h>
#include <stdint.h>

/* A multi-binding `let` desugars at parse time into a chain of nested
 * single-binding AST_LET nodes (see parser.c) -- so every scope-introducing
 * node (AST_LET, AST_LAMBDA) binds exactly one name, which keeps name
 * resolution trivial (every frame has exactly one slot, always index 0).
 * This is a deliberate M4-scope simplification: it doesn't support mutual
 * recursion between sibling bindings in one `let`, which isn't exercised
 * by the M4 fixture set (see docs/architecture.md). */

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
    AST_HAS_ATTR,
} AstKind;

typedef struct Ast Ast;

/* One formal in a lambda-pattern (`{a, b ? default, ...}: body`) parameter
 * list. Plain-malloc'd, same rationale as AstBinding above. `default_`
 * is NULL for a required field (no `? expr`). */
typedef struct {
    const char *name;
    size_t name_len;
    Ast *default_;
} AstFormal;

/* One `name = value;` binding in an AST_ATTRSET literal. Plain-malloc'd
 * (not arena-allocated): it's a compile-time-only auxiliary array, not
 * part of the runtime Value graph the arena exists to serve -- same
 * rationale as IrBuilder's growable arrays in irbuild.c. */
typedef struct {
    const char *name;
    size_t name_len;
    Ast *value;
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
            const char *name;
            size_t name_len;
            Ast *value;
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
            char op; /* '+', '-', '*' */
            Ast *lhs, *rhs;
        } binop; /* AST_BINOP */
        struct {
            const char *text;
            size_t len;
        } string; /* AST_STRING */
        struct {
            Ast **elems;
            size_t count;
        } list; /* AST_LIST */
        struct {
            AstBinding *bindings;
            size_t count;
        } attrset; /* AST_ATTRSET */
        struct {
            Ast *target;
            const char *attr;
            size_t attr_len;
        } select; /* AST_SELECT */
        struct {
            Ast *target;
            const char *attr;
            size_t attr_len;
        } has_attr; /* AST_HAS_ATTR: `target ? attr` */
    } as;
};

/* Arena-allocated (never individually freed), same as every other runtime
 * structure in this process -- see src/asm/arena.S. */
Ast *asnix_ast_new(AstKind kind);

#endif /* ASNIX_AST_H */
