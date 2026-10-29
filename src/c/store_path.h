#ifndef ASNIX_STORE_PATH_H
#define ASNIX_STORE_PATH_H

#include <stddef.h>
#include <stdint.h>

#define ASNIX_STORE_DIR "/nix/store"

size_t asnix_store_path_preimage(const char *type, const char *hash_with_algo, const char *name, char *out,
                                  size_t out_cap);

size_t asnix_make_store_path_from_digest(const uint8_t digest[32], const char *name, char *out, size_t out_cap);

void asnix_check_store_path_name(const char *name);

#endif

