#ifndef ASNIX_NAR_H
#define ASNIX_NAR_H

#include <stddef.h>
#include <stdint.h>

void asnix_copy_path_to_store(const char *path, char *out, size_t out_cap);

const char *asnix_copy_path_to_store_cache_lookup(const char *path);

typedef int (*AsnixNarFilter)(void *ctx, const char *path, const char *type);

void asnix_copy_path_to_store_ext(const char *path, const char *name, int recursive, AsnixNarFilter filter,
                                   void *filter_ctx, const uint8_t expected_sha256[32], char *out, size_t out_cap);

int asnix_hash_path_nar_sha256(const char *path, AsnixNarFilter filter, void *filter_ctx, uint8_t out[32]);

#endif

