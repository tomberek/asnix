#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/irbuild.h"

/* Every scope-introducing AST node (AST_LET, AST_LAMBDA) binds exactly one
 * name at slot 0 (see ast.h), so resolution is just "how many scope links
 * up is the matching name" -- a single combined resolve+emit walk, no
 * separate annotation pass needed at this scope. A real attrset/`with`-
 * capable resolver (M5+) will need one; this is a deliberate M4 shortcut. */
typedef struct ScopeFrame {
    struct ScopeFrame *up;
    const char *name;
    size_t name_len;
} ScopeFrame;

static int name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
}

static void compile_expr(IrBuilder *b, Ast *node, ScopeFrame *scope) {
    switch (node->kind) {
        case AST_INT:
            ib_push_int(b, node->as.int_val);
            return;
        case AST_BOOL:
            if (node->as.bool_val)
                ib_push_true(b);
            else
                ib_push_false(b);
            return;
        case AST_NULL:
            ib_push_null(b);
            return;
        case AST_VAR: {
            int depth = 0;
            for (ScopeFrame *f = scope; f; f = f->up, depth++) {
                if (name_eq(f->name, f->name_len, node->as.var.name, node->as.var.name_len)) {
                    ib_var_ref(b, depth, 0);
                    return;
                }
            }
            fprintf(stderr, "asnix: undefined variable '%.*s'\n", (int)node->as.var.name_len,
                    node->as.var.name);
            exit(1);
        }
        case AST_LET: {
            compile_expr(b, node->as.let1.value, scope);
            ib_let(b, 1);
            ScopeFrame frame = { .up = scope, .name = node->as.let1.name, .name_len = node->as.let1.name_len };
            compile_expr(b, node->as.let1.body, &frame);
            return;
        }
        case AST_IF: {
            compile_expr(b, node->as.if1.cond, scope);
            size_t if_site = ib_if(b);
            compile_expr(b, node->as.if1.then_, scope);
            size_t jmp_site = ib_jmp(b);
            size_t else_idx = ib_len(b);
            ib_patch_delta(b, if_site, else_idx);
            compile_expr(b, node->as.if1.else_, scope);
            size_t end_idx = ib_len(b);
            ib_patch_delta(b, jmp_site, end_idx);
            return;
        }
        case AST_LAMBDA: {
            IrBuilder *body_b = ib_new();
            ScopeFrame frame = { .up = scope, .name = node->as.lambda.param, .name_len = node->as.lambda.param_len };
            compile_expr(body_b, node->as.lambda.body, &frame);
            ib_ret(body_b);
            IrProgram *body_prog = ib_finish(body_b);
            ib_lambda(b, 1, body_prog);
            return;
        }
        case AST_APP:
            compile_expr(b, node->as.app.fn, scope);
            compile_expr(b, node->as.app.arg, scope);
            ib_call(b, 1);
            return;
        case AST_BINOP:
            compile_expr(b, node->as.binop.lhs, scope);
            compile_expr(b, node->as.binop.rhs, scope);
            switch (node->as.binop.op) {
                case '+': ib_add(b); return;
                case '-': ib_sub(b); return;
                case '*': ib_mul(b); return;
            }
            return;
    }
}

IrProgram *asnix_compile(Ast *root) {
    IrBuilder *b = ib_new();
    compile_expr(b, root, NULL);
    ib_halt(b);
    return ib_finish(b);
}
