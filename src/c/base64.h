#ifndef ASNIX_BASE64_H
#define ASNIX_BASE64_H

#include <stddef.h>
#include <stdint.h>

size_t asnix_base64_encoded_length(size_t n);

void asnix_base64_encode(const uint8_t *data, size_t len, char *out);

int64_t asnix_base64_decode(const char *s, size_t len, uint8_t *out, size_t out_cap);

#endif

