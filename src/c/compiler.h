#ifndef ASNIX_COMPILER_H
#define ASNIX_COMPILER_H

#include <stddef.h>

#include "ast.h"
#include "asnix/ir.h"

IrProgram *asnix_compile(Ast *root, const char *base_dir, const char *file_path);

IrProgram *asnix_compile_scoped(Ast *root, const char *base_dir, const char *file_path,
                                 const char **extra_names, size_t *extra_lens, size_t extra_count);

extern int asnix_lazy_thunk_enabled;

IrProgram *asnix_compile_lazy_thunk_body(void *desc);

int asnix_lazy_thunk_descriptor_src_pos(void *desc, unsigned int *out_pos);
const char *asnix_lazy_thunk_descriptor_file_path(void *desc);
const char *asnix_lazy_thunk_descriptor_base_dir(void *desc);
void *asnix_lazy_thunk_descriptor_scope(void *desc);
void *asnix_scope_frame_up(void *frame);
int asnix_scope_frame_count(void *frame);
int asnix_scope_frame_is_with(void *frame);
int asnix_scope_frame_has_overrides(void *frame);
const char *asnix_scope_frame_name(void *frame, int i, size_t *out_len);

void *asnix_lazy_scope_push_frame(void *up_chain, int count, int is_with, int has_overrides,
                                   const char **names, const size_t *name_lens);

void *asnix_make_cached_lazy_thunk_descriptor(unsigned int reparse_pos, const char *file_path,
                                               const char *base_dir, void *scope_chain);
void asnix_lazy_thunk_descriptor_set_self(void *desc, IrProgram *self);

#endif

