#include "hash_shim.h"

#include <dlfcn.h>
#include <stdlib.h>

/* Matches OpenSSL's legacy one-shot digest signature exactly (see
 * hash_shim.h's header comment for which API/why). */
typedef unsigned char *(*asnix_hash_fn)(const unsigned char *d, size_t n, unsigned char *md);

static void *asnix_libcrypto_handle = 0;

static void *asnix_load_libcrypto(void) {
    if (asnix_libcrypto_handle) return asnix_libcrypto_handle;

    /* Tier 1: an explicit resolved store path, exported by flake.nix's
     * devShell shellHook as ASNIX_LIBCRYPTO_LIB (mirroring nixgoose's
     * NIXGOOSE_LIBBLAKE3_LIB/NIXGOOSE_TOMLC99_LIB convention) -- needed
     * because, unlike a system package manager, nixpkgs' openssl has no
     * single predictable path and isn't on the ambient dlopen search path
     * by soname alone in general. */
    const char *env_path = getenv("ASNIX_LIBCRYPTO_LIB");
    if (env_path) {
        void *h = dlopen(env_path, RTLD_NOW);
        if (h) { asnix_libcrypto_handle = h; return h; }
    }

    /* Tier 2: bare sonames / common absolute paths, tried in case
     * libcrypto happens to be reachable some other way (e.g. this
     * project's own dev machine also has a system libcrypto.so.3 at
     * /usr/lib/x86_64-linux-gnu -- true here, but not assumed to be true
     * on every machine this ever runs on, hence still trying the bare
     * soname first). */
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

static int64_t asnix_hash_one(const char *symbol, const void *input, size_t input_len, unsigned char *out) {
    void *h = asnix_load_libcrypto();
    if (!h) return -1;
    asnix_hash_fn fn = (asnix_hash_fn)dlsym(h, symbol);
    if (!fn) return -2;
    fn((const unsigned char *)input, input_len, out);
    return 0;
}

int64_t asnix_hash_md5(const void *input, size_t input_len, unsigned char *out) {
    return asnix_hash_one("MD5", input, input_len, out);
}
int64_t asnix_hash_sha1(const void *input, size_t input_len, unsigned char *out) {
    return asnix_hash_one("SHA1", input, input_len, out);
}
int64_t asnix_hash_sha256(const void *input, size_t input_len, unsigned char *out) {
    return asnix_hash_one("SHA256", input, input_len, out);
}
int64_t asnix_hash_sha512(const void *input, size_t input_len, unsigned char *out) {
    return asnix_hash_one("SHA512", input, input_len, out);
}

void asnix_hash_to_hex(const unsigned char *bytes, size_t n, char *out_hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out_hex[2 * i] = digits[bytes[i] >> 4];
        out_hex[2 * i + 1] = digits[bytes[i] & 0xf];
    }
}
