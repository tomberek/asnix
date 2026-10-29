#ifndef ASNIX_SYMTAB_H
#define ASNIX_SYMTAB_H

#include <stddef.h>
#include <stdint.h>

uint32_t asnix_intern(const char *name, size_t len);

const char *asnix_sym_name(uint32_t id, size_t *out_len);

#endif

