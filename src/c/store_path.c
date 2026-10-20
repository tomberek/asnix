#include "store_path.h"

#include <stdio.h>
#include <string.h>

#include "base32.h"

size_t asnix_store_path_preimage(const char *type, const char *hash_with_algo, const char *name, char *out,
                                  size_t out_cap) {
    int n = snprintf(out, out_cap, "%s:%s:%s:%s", type, hash_with_algo, ASNIX_STORE_DIR, name);
    if (n < 0 || (size_t)n >= out_cap) return 0;
    return (size_t)n;
}

static void compress_hash(const uint8_t *digest, size_t digest_len, uint8_t *out, size_t out_len) {
    memset(out, 0, out_len);
    for (size_t i = 0; i < digest_len; i++) {
        out[i % out_len] ^= digest[i];
    }
}

size_t asnix_make_store_path_from_digest(const uint8_t digest[32], const char *name, char *out, size_t out_cap) {
    uint8_t compressed[20];
    compress_hash(digest, 32, compressed, sizeof(compressed));

    char hash32[32]; /* asnix_base32_encoded_length(20) == 32, fixed by the format */
    asnix_base32_encode(compressed, sizeof(compressed), hash32);

    int n = snprintf(out, out_cap, "%s/%.32s-%s", ASNIX_STORE_DIR, hash32, name);
    if (n < 0 || (size_t)n >= out_cap) return 0;
    return (size_t)n;
}
