#include "string_context.h"

#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

void asnix_ctx_init(AsnixStringContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void asnix_ctx_add_built(AsnixStringContext *ctx, const char *drv_path, const char *output_name) {
    for (size_t i = 0; i < ctx->ndrvs; i++) {
        if (strcmp(ctx->drvs[i].drv_path, drv_path) == 0 && strcmp(ctx->drvs[i].output_name, output_name) == 0)
            return;
    }
    if (ctx->ndrvs == ctx->drvs_cap) {
        ctx->drvs_cap = ctx->drvs_cap ? ctx->drvs_cap * 2 : 8;
        ctx->drvs = realloc(ctx->drvs, ctx->drvs_cap * sizeof(*ctx->drvs));
    }
    ctx->drvs[ctx->ndrvs].drv_path = dup_cstr(drv_path);
    ctx->drvs[ctx->ndrvs].output_name = dup_cstr(output_name);
    ctx->ndrvs++;
}

void asnix_ctx_add_opaque(AsnixStringContext *ctx, const char *path) {
    for (size_t i = 0; i < ctx->nsrcs; i++) {
        if (strcmp(ctx->srcs[i], path) == 0) return;
    }
    if (ctx->nsrcs == ctx->srcs_cap) {
        ctx->srcs_cap = ctx->srcs_cap ? ctx->srcs_cap * 2 : 8;
        ctx->srcs = realloc(ctx->srcs, ctx->srcs_cap * sizeof(*ctx->srcs));
    }
    ctx->srcs[ctx->nsrcs++] = dup_cstr(path);
}

int asnix_ctx_is_empty(const AsnixStringContext *ctx) {
    return !ctx || (ctx->ndrvs == 0 && ctx->nsrcs == 0);
}

void asnix_ctx_union_into(AsnixStringContext *dst, const AsnixStringContext *src) {
    if (asnix_ctx_is_empty(src)) return;
    for (size_t i = 0; i < src->ndrvs; i++) asnix_ctx_add_built(dst, src->drvs[i].drv_path, src->drvs[i].output_name);
    for (size_t i = 0; i < src->nsrcs; i++) asnix_ctx_add_opaque(dst, src->srcs[i]);
}

typedef struct {
    const void *ptr;
    AsnixStringContext ctx;
} ContextTableEntry;
static ContextTableEntry *g_table = NULL;
static size_t g_count = 0, g_cap = 0;

void asnix_string_attach_context(Value v, const AsnixStringContext *ctx) {
    if (asnix_ctx_is_empty(ctx)) return;
    if (g_count == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_table = realloc(g_table, g_cap * sizeof(*g_table));
    }
    ContextTableEntry *entry = &g_table[g_count++];
    entry->ptr = (const void *)(v.raw & ~0x7ULL);
    asnix_ctx_init(&entry->ctx);
    asnix_ctx_union_into(&entry->ctx, ctx);
}

const AsnixStringContext *asnix_string_context_of(Value v) {
    const void *ptr = (const void *)(v.raw & ~0x7ULL);
    for (size_t i = 0; i < g_count; i++) {
        if (g_table[i].ptr == ptr) return &g_table[i].ctx;
    }
    return NULL;
}
