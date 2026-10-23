/* M146 isolated microbenchmark: asnix's own SHA256 path (hash_shim.c's
 * asnix_hash_sha256, i.e. the dlopen'd libcrypto SHA256() one-shot) in a
 * tight loop, over buffer sizes representative of derivation.c's masked
 * ATerm hashing (real .drv text in /nix/store runs ~2-4KB; sampled
 * directly, not guessed -- see docs/architecture.md M146).
 *
 * Also benchmarks the low-level SHA256_Init/Update/Final path (dlsym'd
 * directly, bypassing the legacy one-shot SHA256() wrapper) to isolate
 * how much of the one-shot call's cost is OpenSSL 3.x's EVP/provider
 * fetch machinery versus the actual SHA-NI compression work.
 *
 * Usage: bench_hash_shim <iterations> <buflen>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>

#include "../src/c/hash_shim.h"

typedef void *(*sha256_init_fn)(void *c);
typedef void *(*sha256_update_fn)(void *c, const void *data, size_t len);
typedef void *(*sha256_final_fn)(unsigned char *md, void *c);

/* SHA256_CTX is opaque to us here -- over-allocate generously (real struct
 * is well under 256 bytes: 8 uint32 state + 2 uint32 Nl/Nh + 16 uint32
 * data buffer + num, all well within this). */
typedef struct { unsigned char opaque[256]; } fake_sha256_ctx;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 1000000;
    size_t buflen = argc > 2 ? (size_t)atol(argv[2]) : 2048;

    unsigned char *buf = malloc(buflen);
    for (size_t i = 0; i < buflen; i++) buf[i] = (unsigned char)(i * 2654435761u);
    unsigned char out[64];

    /* Warm the dlopen'd handle + dlsym cache before timing. */
    if (asnix_hash_sha256(buf, buflen, out) != 0) {
        fprintf(stderr, "asnix_hash_sha256 failed (libcrypto not found)\n");
        return 1;
    }

    double t0 = now_sec();
    for (long i = 0; i < iters; i++) {
        asnix_hash_sha256(buf, buflen, out);
    }
    double t1 = now_sec();
    double onshot_sec = t1 - t0;
    printf("one-shot SHA256() [asnix_hash_sha256]: %ld iters, buflen=%zu, "
           "%.6f s total, %.2f ns/call\n",
           iters, buflen, onshot_sec, onshot_sec * 1e9 / iters);

    /* Now the low-level Init/Update/Final path, dlsym'd straight from the
     * same handle hash_shim.c already opened (re-dlopen is idempotent/
     * cheap once loaded by the loader's own refcount, this is just to get
     * at the symbols without duplicating hash_shim's internal state). */
    void *h = dlopen("libcrypto.so.3", RTLD_NOW);
    if (!h) h = dlopen("/usr/lib/x86_64-linux-gnu/libcrypto.so.3", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen failed for low-level path\n"); return 1; }
    sha256_init_fn init = (sha256_init_fn)dlsym(h, "SHA256_Init");
    sha256_update_fn update = (sha256_update_fn)dlsym(h, "SHA256_Update");
    sha256_final_fn final_fn = (sha256_final_fn)dlsym(h, "SHA256_Final");
    if (!init || !update || !final_fn) {
        fprintf(stderr, "dlsym failed for low-level path\n");
        return 1;
    }

    fake_sha256_ctx ctx;
    init(&ctx); update(&ctx, buf, buflen); final_fn(out, &ctx); /* warm */

    t0 = now_sec();
    for (long i = 0; i < iters; i++) {
        init(&ctx);
        update(&ctx, buf, buflen);
        final_fn(out, &ctx);
    }
    t1 = now_sec();
    double lowlevel_sec = t1 - t0;
    printf("low-level Init/Update/Final:            %ld iters, buflen=%zu, "
           "%.6f s total, %.2f ns/call\n",
           iters, buflen, lowlevel_sec, lowlevel_sec * 1e9 / iters);

    printf("one-shot overhead vs low-level: %.2f ns/call (%.1f%%)\n",
           (onshot_sec - lowlevel_sec) * 1e9 / iters,
           100.0 * (onshot_sec - lowlevel_sec) / lowlevel_sec);

    return 0;
}
