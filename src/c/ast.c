#include "ast.h"

#include <stdlib.h>
#include <string.h>

#include "compiler.h"

#define AST_CHUNK_SIZE (1 << 20)

typedef struct AstChunk {
    struct AstChunk *next;
    size_t used;
    char data[AST_CHUNK_SIZE];
} AstChunk;

static AstChunk *g_ast_chunks = NULL;

static AstChunk *g_ast_retained_chunks = NULL;
static int g_ast_redirect_retained = 0;

int asnix_ast_enter_retained(void) {
    int prev = g_ast_redirect_retained;
    if (asnix_lazy_thunk_enabled) g_ast_redirect_retained = 1;
    return prev;
}

void asnix_ast_leave_retained(int prev_state) {
    g_ast_redirect_retained = prev_state;
}

Ast *asnix_ast_new(AstKind kind) {
    size_t need = sizeof(Ast);
    AstChunk **chunks = g_ast_redirect_retained ? &g_ast_retained_chunks : &g_ast_chunks;
    if (!*chunks || (*chunks)->used + need > AST_CHUNK_SIZE) {
        AstChunk *c = malloc(sizeof(AstChunk));
        c->next = *chunks;
        c->used = 0;
        *chunks = c;
    }
    Ast *n = (Ast *)((*chunks)->data + (*chunks)->used);
    (*chunks)->used += need;
    memset(n, 0, need);
    n->kind = kind;
    return n;
}

void asnix_ast_arena_reset(void) {
    AstChunk *c = g_ast_chunks;
    while (c) {
        AstChunk *next = c->next;
        free(c);
        c = next;
    }
    g_ast_chunks = NULL;
}

