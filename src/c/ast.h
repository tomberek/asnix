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
    AST_WITH,
} AstKind;

typedef struct Ast Ast;

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
            const char *param;
            size_t param_len;
            Ast *body;
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
            Ast *attrs;
            Ast *body;
        } with1; /* AST_WITH */
    } as;
};

/* Arena-allocated (never individually freed), same as every other runtime
 * structure in this process -- see src/asm/arena.S. */
Ast *asnix_ast_new(AstKind kind);

#endif /* ASNIX_AST_H */
