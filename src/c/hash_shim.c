#include "hash_shim.h"

#include <dlfcn.h>
#include <stdlib.h>

typedef void *(*asnix_hash_init_fn)(void *ctx);
typedef void *(*asnix_hash_update_fn)(void *ctx, const void *data, size_t len);
typedef void *(*asnix_hash_final_fn)(unsigned char *md, void *ctx);

typedef union {
    unsigned char raw[256];
    uint64_t align_marker;
} asnix_hash_ctx;

static void *asnix_libcrypto_handle = 0;

static void *asnix_load_libcrypto(void) {
    if (asnix_libcrypto_handle) return asnix_libcrypto_handle;

    const char *env_path = getenv("ASNIX_LIBCRYPTO_LIB");
    if (env_path) {
        void *h = dlopen(env_path, RTLD_NOW);
        if (h) { asnix_libcrypto_handle = h; return h; }
    }

    static const char *candidates[] = {
        "libcrypto.so.3",
        "libcrypto.so",
        "/usr/lib/x86_64-linux-gnu/libcrypto.so.3",
        "/usr/lib64/libcrypto.so.3",
        "/usr/lib/libcrypto.so.3",
        "/lib/x86_64-linux-gnu/libcrypto.so.3",
        "/usr/local/lib/libcrypto.so.3",
        0,
    };
    for (int i = 0; candidates[i]; i++) {
        void *h = dlopen(candidates[i], RTLD_NOW);
        if (h) { asnix_libcrypto_handle = h; return h; }
    }
    return 0;
}

typedef struct {
    asnix_hash_init_fn init;
    asnix_hash_update_fn update;
    asnix_hash_final_fn final_fn;
    int resolved;
} AsnixHashFnCache;

static int64_t asnix_hash_one(const char *init_sym, const char *update_sym,
                               const char *final_sym, const void *input,
                               size_t input_len, unsigned char *out,
                               AsnixHashFnCache *cache) {
    if (!cache->resolved) {
        void *h = asnix_load_libcrypto();
        if (!h) return -1;
        cache->init = (asnix_hash_init_fn)dlsym(h, init_sym);
        cache->update = (asnix_hash_update_fn)dlsym(h, update_sym);
        cache->final_fn = (asnix_hash_final_fn)dlsym(h, final_sym);
        cache->resolved = 1;
    }
    if (!cache->init || !cache->update || !cache->final_fn) return -2;
    asnix_hash_ctx ctx;
    cache->init(&ctx);
    cache->update(&ctx, input, input_len);
    cache->final_fn(out, &ctx);
    return 0;
}

int64_t asnix_hash_md5(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("MD5_Init", "MD5_Update", "MD5_Final", input, input_len, out, &cache);
}
int64_t asnix_hash_sha1(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("SHA1_Init", "SHA1_Update", "SHA1_Final", input, input_len, out, &cache);
}
int64_t asnix_hash_sha256(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("SHA256_Init", "SHA256_Update", "SHA256_Final", input, input_len, out, &cache);
}
int64_t asnix_hash_sha512(const void *input, size_t input_len, unsigned char *out) {
    static AsnixHashFnCache cache = { 0 };
    return asnix_hash_one("SHA512_Init", "SHA512_Update", "SHA512_Final", input, input_len, out, &cache);
}

void asnix_hash_to_hex(const unsigned char *bytes, size_t n, char *out_hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out_hex[2 * i] = digits[bytes[i] >> 4];
        out_hex[2 * i + 1] = digits[bytes[i] & 0xf];
    }
}

