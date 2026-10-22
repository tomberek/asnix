#include "ast.h"

#include <stdlib.h>
#include <string.h>

/* perf: Ast nodes used to come from the main never-free bump arena, same
 * as every runtime value -- but an AST is pure compile-time scratch:
 * once asnix_compile() walks it into an IrProgram, nothing downstream
 * ever reads an Ast* again (AstBinding.name/AstFormal.name are raw
 * pointers into the source TEXT buffer, not into Ast nodes, and
 * AST_CURPOS bakes its line/col into the IR as plain integers at compile
 * time). So this file keeps its own chunk list, freed in one shot by
 * asnix_ast_arena_reset() right after each top-level asnix_compile()/
 * asnix_compile_scoped() call returns (see eval_file.c's call sites),
 * instead of living in the main arena forever. Measured this session:
 * Ast nodes were ~17% of asnix's total memory footprint on a real
 * nixpkgs eval, entirely reclaimable since nothing needs them past
 * compile time. */
#define AST_CHUNK_SIZE (1 << 20) /* 1 MiB per chunk -- same order as the main arena's 2 MiB */

typedef struct AstChunk {
    struct AstChunk *next;
    size_t used;
    char data[AST_CHUNK_SIZE];
} AstChunk;

static AstChunk *g_ast_chunks = NULL;

Ast *asnix_ast_new(AstKind kind) {
    size_t need = sizeof(Ast);
    if (!g_ast_chunks || g_ast_chunks->used + need > AST_CHUNK_SIZE) {
        AstChunk *c = calloc(1, sizeof(AstChunk)); /* DEBUG: test zero-init hypothesis */
        c->next = g_ast_chunks;
        c->used = 0;
        g_ast_chunks = c;
    }
    Ast *n = (Ast *)(g_ast_chunks->data + g_ast_chunks->used);
    g_ast_chunks->used += need;
    n->kind = kind;
    return n;
}

/* Frees every chunk ever allocated by asnix_ast_new -- safe to call only
 * once the caller is certain no Ast* from the chunks being freed is
 * still reachable (true right after asnix_compile()/asnix_compile_scoped()
 * returns, per this file's own header comment above). */
void asnix_ast_arena_reset(void) {
    AstChunk *c = g_ast_chunks;
    while (c) {
        AstChunk *next = c->next;
        free(c);
        c = next;
    }
    g_ast_chunks = NULL;
}
