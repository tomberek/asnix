#ifndef ASNIX_SYMTAB_H
#define ASNIX_SYMTAB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Dense u32 symbol interning. Hashing is a cheap multiply-based mix (FxHash
 * shape) finished with a splitmix64-style avalanche, not SipHash -- these
 * are compiler-internal identifiers (attr names, let/lambda bindings),
 * never adversarial input, and `~/gnix/iets` specifically measured SipHash
 * as a profiler-visible cost for exactly this kind of short, high-volume
 * string hashing (see docs/architecture.md's prior-art notes).
 */

uint32_t asnix_intern(const char *name, size_t len);

/* Reverse lookup, mainly for error messages/debugging. */
const char *asnix_sym_name(uint32_t id, size_t *out_len);

#endif /* ASNIX_SYMTAB_H */
