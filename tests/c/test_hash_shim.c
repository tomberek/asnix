#include <stdio.h>
#include <string.h>

#include "hash_shim.h"

/*
 * M8: direct unit test of hash_shim.c's dlopen'd libcrypto wrappers,
 * independent of builtins.c/the evaluator entirely -- same bug-isolation
 * strategy as every other M* milestone's "validate the primitive with
 * hand-fed input before the layer above it is touched" approach (see
 * docs/architecture.md). Oracle values below were generated directly on
 * this dev machine with `printf '%s' <input> | md5sum/sha1sum/sha256sum`
 * and `openssl dgst -sha512 -r` (not transcribed from memory/documentation
 * -- see the task notes this was built from for why that matters).
 */

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

typedef int64_t (*hash_fn_t)(const void *, size_t, unsigned char *);

static void check_one(const char *label, hash_fn_t fn, size_t raw_len,
                       const void *input, size_t input_len, const char *expected_hex) {
    unsigned char raw[ASNIX_HASH_SHA512_LEN];
    int64_t rc = fn(input, input_len, raw);
    if (rc != 0) {
        fprintf(stderr, "FAIL: %s: hash_fn returned %lld (libcrypto not loadable -- "
                         "run inside `nix develop`, see flake.nix's ASNIX_LIBCRYPTO_LIB)\n",
                label, (long long)rc);
        failures++;
        return;
    }
    char hex[2 * ASNIX_HASH_SHA512_LEN];
    asnix_hash_to_hex(raw, raw_len, hex);
    size_t expected_len = strlen(expected_hex);
    if (expected_len != 2 * raw_len || memcmp(hex, expected_hex, expected_len) != 0) {
        fprintf(stderr, "FAIL: %s: got %.*s want %s\n", label, (int)(2 * raw_len), hex, expected_hex);
        failures++;
    }
}

int main(void) {
    /* "" */
    check_one("md5(\"\")", asnix_hash_md5, ASNIX_HASH_MD5_LEN, "", 0,
              "d41d8cd98f00b204e9800998ecf8427e");
    check_one("sha1(\"\")", asnix_hash_sha1, ASNIX_HASH_SHA1_LEN, "", 0,
              "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    check_one("sha256(\"\")", asnix_hash_sha256, ASNIX_HASH_SHA256_LEN, "", 0,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_one("sha512(\"\")", asnix_hash_sha512, ASNIX_HASH_SHA512_LEN, "", 0,
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

    /* "hello" */
    check_one("md5(\"hello\")", asnix_hash_md5, ASNIX_HASH_MD5_LEN, "hello", 5,
              "5d41402abc4b2a76b9719d911017c592");
    check_one("sha1(\"hello\")", asnix_hash_sha1, ASNIX_HASH_SHA1_LEN, "hello", 5,
              "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
    check_one("sha256(\"hello\")", asnix_hash_sha256, ASNIX_HASH_SHA256_LEN, "hello", 5,
              "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    check_one("sha512(\"hello\")", asnix_hash_sha512, ASNIX_HASH_SHA512_LEN, "hello", 5,
              "9b71d224bd62f3785d96d46ad3ea3d73319bfbc2890caadae2dff72519673ca72323c3d99ba5c11d7c7acc6e14b8c5da0c4663475c2e5c3adef46f73bcdec043");

    /* "hello world" */
    check_one("md5(\"hello world\")", asnix_hash_md5, ASNIX_HASH_MD5_LEN, "hello world", 11,
              "5eb63bbbe01eeed093cb22bb8f5acdc3");
    check_one("sha1(\"hello world\")", asnix_hash_sha1, ASNIX_HASH_SHA1_LEN, "hello world", 11,
              "2aae6c35c94fcfb415dbe95f408b9ce91ee846ed");
    check_one("sha256(\"hello world\")", asnix_hash_sha256, ASNIX_HASH_SHA256_LEN, "hello world", 11,
              "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    check_one("sha512(\"hello world\")", asnix_hash_sha512, ASNIX_HASH_SHA512_LEN, "hello world", 11,
              "309ecc489c12d6eb4cc40f50c902f2b4d0ed77ee511a7c7a9bcd3ca86d4cd86f989dd35bc5ff499670da34255b45b0cfd830e81f605dcf7dc5542e93ae9cd76f");

    /* "nix" */
    check_one("md5(\"nix\")", asnix_hash_md5, ASNIX_HASH_MD5_LEN, "nix", 3,
              "393b7e38038f12c5fb4c92e81a1b5a01");
    check_one("sha1(\"nix\")", asnix_hash_sha1, ASNIX_HASH_SHA1_LEN, "nix", 3,
              "e56549c11b18ab5edb0a3cf6b33638466fdb9723");
    check_one("sha256(\"nix\")", asnix_hash_sha256, ASNIX_HASH_SHA256_LEN, "nix", 3,
              "a0745f7ed88cfac6058d1d4cfb57bc71127a28ad7c6756004142a90aab5ee99f");
    check_one("sha512(\"nix\")", asnix_hash_sha512, ASNIX_HASH_SHA512_LEN, "nix", 3,
              "01804f534bb03e9175e0d1fadc297410648d4fe2bdbb3add95d3621ad5d0e5b1b45d4cf21f471cf02df46f72085cb5f7f69e46537f86427bc950f7ec1e67a56e");

    /* Embedded-NUL input ("ab\0cd", 5 bytes) -- the whole reason this shim
     * takes an explicit (pointer,len) pair instead of a null-terminated
     * C string: asnix's own String heap object (heap.h) is never null-
     * terminated, so a byte-length mismatch here would silently truncate
     * every real Nix string containing an embedded NUL. */
    {
        const char nul_input[5] = { 'a', 'b', '\0', 'c', 'd' };
        check_one("md5(embedded-NUL)", asnix_hash_md5, ASNIX_HASH_MD5_LEN, nul_input, 5,
                  "5d622b3d4d8cc07754fd386e7f29326e");
        check_one("sha1(embedded-NUL)", asnix_hash_sha1, ASNIX_HASH_SHA1_LEN, nul_input, 5,
                  "cf76bf81edbf067731a98c6b7d2eb28824d38027");
        check_one("sha256(embedded-NUL)", asnix_hash_sha256, ASNIX_HASH_SHA256_LEN, nul_input, 5,
                  "1bd95cf6379b94fd3b6ceb1390b70b822c76442c4bfb8273b941e09d8dfd9b56");
        check_one("sha512(embedded-NUL)", asnix_hash_sha512, ASNIX_HASH_SHA512_LEN, nul_input, 5,
                  "6f74b9701ed4220859f879533dbb4b6602dc381acabc47353ee26d09413057c2ffbc4be94f5be09ab3a3fe78ee875035f904fe4a5fad3807c5928819e97be9e7");
    }

    /* asnix_hash_to_hex itself, directly, independent of any real digest
     * (a trivial but worth-having check that nibble order/case is right). */
    {
        unsigned char bytes[3] = { 0x00, 0x1f, 0xff };
        char hex[6];
        asnix_hash_to_hex(bytes, 3, hex);
        CHECK(memcmp(hex, "001fff", 6) == 0, "asnix_hash_to_hex: lowercase, correct nibble order");
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_hash_shim: all checks passed\n");
    return 0;
}
