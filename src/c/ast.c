#include "ast.h"

#include "asnix/arena.h"

Ast *asnix_ast_new(AstKind kind) {
    Ast *n = asnix_arena_alloc(sizeof(Ast));
    n->kind = kind;
    return n;
}
