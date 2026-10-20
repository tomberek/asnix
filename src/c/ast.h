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
    AST_AND,    /* short-circuit `&&`: as.logic */
    AST_OR,     /* short-circuit `||`: as.logic */
    AST_NOT,    /* unary `!`: as.not1 */
    AST_ASSERT, /* `assert cond; body`: as.assert1 */
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
            /* '+' '-' '*'                  arithmetic (M4)
             * 'e' '==' 'n' '!='            equality/inequality
             * 'l' '<' 'g' '>' 'L' '<=' 'G' '>='   ordering (INT-only, see compiler.c)
             * 'u' '//'                     attrset update/merge */
            char op;
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
    } as;
};

/* Arena-allocated (never individually freed), same as every other runtime
 * structure in this process -- see src/asm/arena.S. */
Ast *asnix_ast_new(AstKind kind);

#endif /* ASNIX_AST_H */
