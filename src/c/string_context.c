#include "string_context.h"

#include <stdint.h>
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

static uint64_t dcc_hash_str(const char *s, size_t len) {
    uint64_t h = 0;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t word;
        memcpy(&word, s + i, 8);
        h ^= word;
        h *= 0x517cc1b727220a95ULL;
    }
    for (; i < len; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

#define DCC_EMPTY_SLOT SIZE_MAX

#define SC_INDEX_THRESHOLD 16

static void ctx_drvs_index_rehash(AsnixStringContext *ctx, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < ctx->ndrvs; i++) {
        const char *dp = ctx->drvs[i].drv_path, *on = ctx->drvs[i].output_name;
        uint64_t h = dcc_hash_str(dp, strlen(dp)) ^ (dcc_hash_str(on, strlen(on)) * 0x9e3779b97f4a7c15ULL);
        size_t slot = h & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(ctx->drvs_index);
    ctx->drvs_index = new_table;
    ctx->drvs_index_cap = new_cap;
}

void asnix_ctx_add_built(AsnixStringContext *ctx, const char *drv_path, const char *output_name) {
    size_t slot = 0;
    int have_slot = 0;
    if (ctx->ndrvs < SC_INDEX_THRESHOLD) {
        for (size_t i = 0; i < ctx->ndrvs; i++) {
            if (strcmp(ctx->drvs[i].drv_path, drv_path) == 0 && strcmp(ctx->drvs[i].output_name, output_name) == 0)
                return;
        }
    } else {
        size_t dp_len = strlen(drv_path), on_len = strlen(output_name);
        if (ctx->drvs_index_cap == 0) ctx_drvs_index_rehash(ctx, 32);
        else if ((ctx->ndrvs + 1) * 4 > ctx->drvs_index_cap * 3) ctx_drvs_index_rehash(ctx, ctx->drvs_index_cap * 2);
        uint64_t h = dcc_hash_str(drv_path, dp_len) ^ (dcc_hash_str(output_name, on_len) * 0x9e3779b97f4a7c15ULL);
        slot = h & (ctx->drvs_index_cap - 1);
        for (;;) {
            size_t idx = ctx->drvs_index[slot];
            if (idx == DCC_EMPTY_SLOT) break;
            if (strcmp(ctx->drvs[idx].drv_path, drv_path) == 0 && strcmp(ctx->drvs[idx].output_name, output_name) == 0) return;
            slot = (slot + 1) & (ctx->drvs_index_cap - 1);
        }
        have_slot = 1;
    }
    if (ctx->ndrvs == ctx->drvs_cap) {
        ctx->drvs_cap = ctx->drvs_cap ? ctx->drvs_cap * 2 : 8;
        ctx->drvs = realloc(ctx->drvs, ctx->drvs_cap * sizeof(*ctx->drvs));
    }
    ctx->drvs[ctx->ndrvs].drv_path = dup_cstr(drv_path);
    ctx->drvs[ctx->ndrvs].output_name = dup_cstr(output_name);
    if (have_slot) ctx->drvs_index[slot] = ctx->ndrvs;
    ctx->ndrvs++;
}

static void ctx_srcs_index_rehash(AsnixStringContext *ctx, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < ctx->nsrcs; i++) {
        size_t slot = dcc_hash_str(ctx->srcs[i], strlen(ctx->srcs[i])) & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(ctx->srcs_index);
    ctx->srcs_index = new_table;
    ctx->srcs_index_cap = new_cap;
}

void asnix_ctx_add_opaque(AsnixStringContext *ctx, const char *path) {
    size_t slot = 0;
    int have_slot = 0;
    if (ctx->nsrcs < SC_INDEX_THRESHOLD) {
        for (size_t i = 0; i < ctx->nsrcs; i++) {
            if (strcmp(ctx->srcs[i], path) == 0) return;
        }
    } else {
        size_t len = strlen(path);
        if (ctx->srcs_index_cap == 0) ctx_srcs_index_rehash(ctx, 32);
        else if ((ctx->nsrcs + 1) * 4 > ctx->srcs_index_cap * 3) ctx_srcs_index_rehash(ctx, ctx->srcs_index_cap * 2);
        slot = dcc_hash_str(path, len) & (ctx->srcs_index_cap - 1);
        for (;;) {
            size_t idx = ctx->srcs_index[slot];
            if (idx == DCC_EMPTY_SLOT) break;
            if (strcmp(ctx->srcs[idx], path) == 0) return;
            slot = (slot + 1) & (ctx->srcs_index_cap - 1);
        }
        have_slot = 1;
    }
    if (ctx->nsrcs == ctx->srcs_cap) {
        ctx->srcs_cap = ctx->srcs_cap ? ctx->srcs_cap * 2 : 8;
        ctx->srcs = realloc(ctx->srcs, ctx->srcs_cap * sizeof(*ctx->srcs));
    }
    ctx->srcs[ctx->nsrcs] = dup_cstr(path);
    if (have_slot) ctx->srcs_index[slot] = ctx->nsrcs;
    ctx->nsrcs++;
}

static void ctx_deep_index_rehash(AsnixStringContext *ctx, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < ctx->ndeep; i++) {
        size_t slot = dcc_hash_str(ctx->deep[i], strlen(ctx->deep[i])) & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(ctx->deep_index);
    ctx->deep_index = new_table;
    ctx->deep_index_cap = new_cap;
}

void asnix_ctx_add_deep(AsnixStringContext *ctx, const char *drv_path) {
    size_t slot = 0;
    int have_slot = 0;
    if (ctx->ndeep < SC_INDEX_THRESHOLD) {
        for (size_t i = 0; i < ctx->ndeep; i++) {
            if (strcmp(ctx->deep[i], drv_path) == 0) return;
        }
    } else {
        size_t len = strlen(drv_path);
        if (ctx->deep_index_cap == 0) ctx_deep_index_rehash(ctx, 32);
        else if ((ctx->ndeep + 1) * 4 > ctx->deep_index_cap * 3) ctx_deep_index_rehash(ctx, ctx->deep_index_cap * 2);
        slot = dcc_hash_str(drv_path, len) & (ctx->deep_index_cap - 1);
        for (;;) {
            size_t idx = ctx->deep_index[slot];
            if (idx == DCC_EMPTY_SLOT) break;
            if (strcmp(ctx->deep[idx], drv_path) == 0) return;
            slot = (slot + 1) & (ctx->deep_index_cap - 1);
        }
        have_slot = 1;
    }
    if (ctx->ndeep == ctx->deep_cap) {
        ctx->deep_cap = ctx->deep_cap ? ctx->deep_cap * 2 : 8;
        ctx->deep = realloc(ctx->deep, ctx->deep_cap * sizeof(*ctx->deep));
    }
    ctx->deep[ctx->ndeep] = dup_cstr(drv_path);
    if (have_slot) ctx->deep_index[slot] = ctx->ndeep;
    ctx->ndeep++;
}

int asnix_ctx_is_empty(const AsnixStringContext *ctx) {
    return !ctx || (ctx->ndrvs == 0 && ctx->nsrcs == 0 && ctx->ndeep == 0);
}

void asnix_ctx_union_into(AsnixStringContext *dst, const AsnixStringContext *src) {
    if (asnix_ctx_is_empty(src)) return;
    for (size_t i = 0; i < src->ndrvs; i++) asnix_ctx_add_built(dst, src->drvs[i].drv_path, src->drvs[i].output_name);
    for (size_t i = 0; i < src->nsrcs; i++) asnix_ctx_add_opaque(dst, src->srcs[i]);
    for (size_t i = 0; i < src->ndeep; i++) asnix_ctx_add_deep(dst, src->deep[i]);
}

typedef struct {
    const void *ptr;
    AsnixStringContext ctx;
} ContextTableEntry;
static ContextTableEntry *g_table = NULL;
static size_t g_count = 0, g_cap = 0;

static size_t ctx_hash_ptr(const void *ptr) {
    uint64_t x = (uint64_t)(uintptr_t)ptr;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

static ContextTableEntry *ctx_table_find_slot(ContextTableEntry *table, size_t cap, const void *ptr) {
    size_t mask = cap - 1;
    size_t idx = ctx_hash_ptr(ptr) & mask;
    for (;;) {
        ContextTableEntry *e = &table[idx];
        if (e->ptr == NULL || e->ptr == ptr) return e;
        idx = (idx + 1) & mask;
    }
}

static void ctx_table_grow(void) {
    size_t new_cap = g_cap ? g_cap * 2 : 64;
    ContextTableEntry *new_table = calloc(new_cap, sizeof(*new_table));
    for (size_t i = 0; i < g_cap; i++) {
        if (g_table[i].ptr == NULL) continue;
        ContextTableEntry *slot = ctx_table_find_slot(new_table, new_cap, g_table[i].ptr);
        *slot = g_table[i];
    }
    free(g_table);
    g_table = new_table;
    g_cap = new_cap;
}

void asnix_string_attach_context(Value v, const AsnixStringContext *ctx) {
    if (asnix_ctx_is_empty(ctx)) return;
    if (g_cap == 0 || g_count * 4 >= g_cap * 3) ctx_table_grow();
    const void *ptr = (const void *)(v.raw & ~0x7ULL);
    ContextTableEntry *entry = ctx_table_find_slot(g_table, g_cap, ptr);
    if (entry->ptr == NULL) {
        entry->ptr = ptr;
        asnix_ctx_init(&entry->ctx);
        asnix_ctx_union_into(&entry->ctx, ctx);
        g_count++;
    }
}

const AsnixStringContext *asnix_string_context_of(Value v) {
    if (g_cap == 0) return NULL;
    const void *ptr = (const void *)(v.raw & ~0x7ULL);
    ContextTableEntry *entry = ctx_table_find_slot(g_table, g_cap, ptr);
    return entry->ptr == NULL ? NULL : &entry->ctx;
}

