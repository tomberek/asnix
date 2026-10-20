#ifndef ASNIX_BASE32_H
#define ASNIX_BASE32_H

#include <stddef.h>
#include <stdint.h>

/*
 * Nix's own base32 variant (NOT RFC4648): alphabet
 * "0123456789abcdfghijklmnpqrsvwxyz" (omits e, o, u, t to avoid spelling
 * words and avoid visual ambiguity), and a non-standard bit-packing order
 * (characters are emitted most-significant-chunk first, but the chunks
 * themselves are read least-significant-bit-first within each byte).
 * Ported directly from the real implementation at
 * ~/nix/src/libutil/base-nix-32.cc (`BaseNix32::encode`) and verified
 * against real `nix-instantiate`-computed store paths before being used
 * for anything -- see docs/architecture.md's M8 notes.
 */

/* Number of base32 characters needed to encode `n` raw bytes. */
size_t asnix_base32_encoded_length(size_t n);

/* Writes exactly asnix_base32_encoded_length(len) characters to `out`
 * (NOT null-terminated -- caller sizes/terminates the buffer). */
void asnix_base32_encode(const uint8_t *data, size_t len, char *out);

#endif /* ASNIX_BASE32_H */
