#ifndef ASNIX_BYTECODE_CACHE_H
#define ASNIX_BYTECODE_CACHE_H

#include <stdint.h>

#include "asnix/ir.h"

typedef struct Ast Ast;

int asnix_bytecode_cache_enabled(void);

IrProgram *asnix_bytecode_cache_lookup_fast(const char *resolved_path, int64_t mtime_sec, int64_t mtime_nsec,
                                             int64_t size, int *out_entry_exists);
IrProgram *asnix_bytecode_cache_lookup_verify(const char *resolved_path, const char *source, size_t source_len,
                                               int64_t mtime_sec, int64_t mtime_nsec, int64_t size);

void asnix_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, int64_t mtime_sec,
                                 int64_t mtime_nsec, int64_t size, IrProgram *prog);

void asnix_bytecode_prefetch_imports(Ast *root, const char *base_dir);

void asnix_prefetch_compile_and_cache_buf(const char *path, const char *src, size_t src_len);

void asnix_bytecode_prefetch_wait(const char *resolved_path);

#endif

