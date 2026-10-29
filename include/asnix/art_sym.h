#ifndef ASNIX_ART_SYM_H
#define ASNIX_ART_SYM_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"

typedef struct AsnixArtSymNode AsnixArtSymNode;

typedef struct {
    AsnixArtSymNode *root;
} AsnixArtSymSet;

static inline void asnix_art_sym_init(AsnixArtSymSet *set) { set->root = NULL; }

void asnix_art_sym_insert(AsnixArtSymSet *set, uint32_t sym, Value val);

int64_t asnix_art_sym_collect_sorted(const AsnixArtSymSet *set, uint32_t *out_syms, Value *out_vals);

#endif

