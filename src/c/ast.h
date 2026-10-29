#ifndef ASNIX_AST_H
#define ASNIX_AST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AST_INT,
    AST_FLOAT,
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
    AST_AND,
    AST_OR,
    AST_NOT,
    AST_ASSERT,
    AST_WITH,
    AST_HAS_ATTR,
    AST_PATH,
    AST_CURPOS,
} AstKind;

typedef struct Ast Ast;

typedef struct {
    int is_expr;
    const char *text;
    size_t text_len;
    Ast *expr;
} AstStringPart;

typedef struct {
    const char *name;
    size_t name_len;
    Ast *default_;
} AstFormal;

typedef struct {
    const char *name;
    size_t name_len;
    Ast *value;
    int is_inherit;
    int is_hidden;
} AstBinding;

typedef struct {
    Ast *name_expr;
    Ast *value;
} AstDynBinding;

struct Ast {
    AstKind kind;
    uint32_t src_pos;
    union {
        int64_t int_val;
        double float_val;
        int bool_val;
        struct {
            const char *name;
            size_t name_len;
        } var;
        struct {
            AstBinding *bindings;
            size_t count;
            Ast *body;
        } let1;
        struct {
            Ast *cond, *then_, *else_;
        } if1;
        struct {
            const char *param;
            size_t param_len;
            Ast *body;
            int is_pattern;
            AstFormal *formals;
            size_t formals_count;
            int has_ellipsis;
            int has_as_name;
            const char *as_name;
            size_t as_name_len;
        } lambda;
        struct {
            Ast *fn, *arg;
        } app;
        struct {
            char op;
            Ast *lhs, *rhs;
        } binop;
        struct {
            AstStringPart *parts;
            size_t count;
        } string;
        struct {
            Ast **elems;
            size_t count;
        } list;
        struct {
            AstBinding *bindings;
            size_t count;
            int is_rec;
            AstDynBinding *dyn_bindings;
            size_t dyn_count;
        } attrset;
        struct {
            Ast *target;
            const char *attr;
            size_t attr_len;
            Ast *attr_expr;
            Ast *default_;
        } select;
        struct {
            Ast *lhs, *rhs;
        } logic;
        struct {
            Ast *operand;
        } not1;
        struct {
            Ast *cond, *body;
            const char *cond_text;
            size_t cond_text_len;
        } assert1;
        struct {
            Ast *attrs;
            Ast *body;
        } with1;
        struct {
            Ast *target;
            const char *attr;
            size_t attr_len;
            Ast *attr_expr;
        } has_attr;
        struct {
            const char *text;
            size_t len;
            int is_search;
            int is_home;
            AstStringPart *interp_parts;
            size_t interp_count;
        } path;
        struct {
            int line;
            int column;
        } curpos;
    } as;
};

Ast *asnix_ast_new(AstKind kind);

#ifdef ASNIX_IRDUMP_LINE_TRACKING
static inline Ast *asnix_ast_new_at(AstKind kind, uint32_t src_pos) {
    Ast *n = asnix_ast_new(kind);
    n->src_pos = src_pos;
    return n;
}
#else
#define asnix_ast_new_at(kind, src_pos) asnix_ast_new(kind)
#endif

void asnix_ast_arena_reset(void);

int asnix_ast_enter_retained(void);
void asnix_ast_leave_retained(int prev_state);

#endif

