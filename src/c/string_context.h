#ifndef ASNIX_STRING_CONTEXT_H
#define ASNIX_STRING_CONTEXT_H

#include <stddef.h>

#include "asnix/value.h"

typedef struct {
    char *drv_path;
    char *output_name;
} AsnixCtxBuiltRef;

typedef struct {
    AsnixCtxBuiltRef *drvs;
    size_t ndrvs, drvs_cap;
    size_t *drvs_index;
    size_t drvs_index_cap;
    char **srcs;
    size_t nsrcs, srcs_cap;
    size_t *srcs_index;
    size_t srcs_index_cap;
    char **deep;
    size_t ndeep, deep_cap;
    size_t *deep_index;
    size_t deep_index_cap;
} AsnixStringContext;

void asnix_ctx_init(AsnixStringContext *ctx);
void asnix_ctx_add_built(AsnixStringContext *ctx, const char *drv_path, const char *output_name);
void asnix_ctx_add_opaque(AsnixStringContext *ctx, const char *path);
void asnix_ctx_add_deep(AsnixStringContext *ctx, const char *drv_path);
int asnix_ctx_is_empty(const AsnixStringContext *ctx);
void asnix_ctx_union_into(AsnixStringContext *dst, const AsnixStringContext *src);

void asnix_string_attach_context(Value v, const AsnixStringContext *ctx);

const AsnixStringContext *asnix_string_context_of(Value v);

#endif

