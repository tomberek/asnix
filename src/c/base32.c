#include "base32.h"

static const char ALPHABET[] = "0123456789abcdfghijklmnpqrsvwxyz"; /* 32 chars + NUL */

size_t asnix_base32_encoded_length(size_t n) {
    if (n == 0) return 0;
    return (n * 8 - 1) / 5 + 1;
}

void asnix_base32_encode(const uint8_t *data, size_t len, char *out) {
    if (len == 0) return;
    size_t enc_len = asnix_base32_encoded_length(len);
    for (size_t idx = 0; idx < enc_len; idx++) {
        size_t n = enc_len - 1 - idx;
        unsigned int b = (unsigned int)(n * 5);
        unsigned int i = b / 8;
        unsigned int j = b % 8;
        unsigned int c = data[i] >> j;
        if (i < len - 1) {
            c |= (unsigned int)(data[i + 1] << (8 - j));
        }
        out[idx] = ALPHABET[c & 0x1f];
    }
}
