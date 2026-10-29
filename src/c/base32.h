#ifndef ASNIX_BASE32_H
#define ASNIX_BASE32_H

#include <stddef.h>
#include <stdint.h>

size_t asnix_base32_encoded_length(size_t n);

void asnix_base32_encode(const uint8_t *data, size_t len, char *out);

int asnix_base32_decode(const char *s, size_t len, uint8_t *out, size_t out_len);

#endif

