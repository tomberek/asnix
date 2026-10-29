#ifndef ASNIX_REGEX_ENGINE_H
#define ASNIX_REGEX_ENGINE_H

#include <stdint.h>

typedef struct AsnixCustomRegex AsnixCustomRegex;

typedef struct {
    int64_t so;
    int64_t eo;
} AsnixRegexGroup;

int asnix_regex_needs_custom_engine(const char *pattern, int64_t len);

AsnixCustomRegex *asnix_regex_compile(const char *pattern, int64_t len, int64_t *nsub_out);

void asnix_regex_free(AsnixCustomRegex *re);

int asnix_regex_match_anchored(const AsnixCustomRegex *re, const char *subject, int64_t slen,
                                int64_t start, int64_t end, int notbol, AsnixRegexGroup *groups_out);

int asnix_regex_search(const AsnixCustomRegex *re, const char *subject, int64_t slen,
                        int64_t from_pos, int notbol, int64_t *out_start, int64_t *out_end,
                        AsnixRegexGroup *groups_out);

#endif

