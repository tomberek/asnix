#ifndef ASNIX_HASH_SHIM_H
#define ASNIX_HASH_SHIM_H

#include <stddef.h>
#include <stdint.h>

#define ASNIX_HASH_MD5_LEN    16
#define ASNIX_HASH_SHA1_LEN   20
#define ASNIX_HASH_SHA256_LEN 32
#define ASNIX_HASH_SHA512_LEN 64

int64_t asnix_hash_md5(const void *input, size_t input_len, unsigned char *out);
int64_t asnix_hash_sha1(const void *input, size_t input_len, unsigned char *out);
int64_t asnix_hash_sha256(const void *input, size_t input_len, unsigned char *out);
int64_t asnix_hash_sha512(const void *input, size_t input_len, unsigned char *out);

void asnix_hash_to_hex(const unsigned char *bytes, size_t n, char *out_hex);

#endif

