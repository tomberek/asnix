#include "builtins_regex.h"

#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/value.h"
#include "builtins_shared.h"
#include "regex_engine.h"
#include "value_util.h"

static char *nul_copy(const char *bytes, int64_t len) {
    char *buf = malloc((size_t)len + 1);
    if (len > 0) memcpy(buf, bytes, (size_t)len);
    buf[len] = '\0';
    return buf;
}

#define ASNIX_GROUP_REPEAT_SAFE_MAX 1000

static int group_repeat_bound_too_large(const char *p, int64_t len, int64_t i) {
    int64_t j = i + 1;
    int64_t bound1 = -1, bound2 = -1;
    int64_t start = j;
    while (j < len && p[j] >= '0' && p[j] <= '9') j++;
    if (j > start) bound1 = atoll(p + start);
    if (j < len && p[j] == ',') {
        j++;
        start = j;
        while (j < len && p[j] >= '0' && p[j] <= '9') j++;
        if (j > start) bound2 = atoll(p + start);
    }
    if (j >= len || p[j] != '}') return 0;
    return (bound1 > ASNIX_GROUP_REPEAT_SAFE_MAX) || (bound2 > ASNIX_GROUP_REPEAT_SAFE_MAX);
}
static int pattern_rejected_by_real_nix(const char *p, int64_t len) {
    static const char *valid_escapes = ".()*+?[{^$|\\";
    int in_bracket = 0, bracket_first = 0, paren_depth = 0;
    int64_t i = 0;
    while (i < len) {
        char c = p[i];
        if (!in_bracket) {
            if (c == '\\') {
                if (i + 1 >= len) return 0;
                if (!strchr(valid_escapes, p[i + 1])) return 1;
                i += 2;
                continue;
            }
            if (c == '(') { paren_depth++; i++; continue; }
            if (c == ')') {
                if (paren_depth == 0) return 1;
                paren_depth--;
                i++;
                if (i < len && p[i] == '{' && group_repeat_bound_too_large(p, len, i)) return 1;
                continue;
            }
            if (c == '{' && i + 1 < len && p[i + 1] == ',') return 1;
            if (c == '[') {
                in_bracket = 1;
                bracket_first = 1;
                i++;
                if (i < len && p[i] == '^') i++;
                continue;
            }
            i++;
            continue;
        }
        if (bracket_first && c == ']') {
            bracket_first = 0;
            i++;
            continue;
        }
        bracket_first = 0;
        if (c == '[' && i + 1 < len && (p[i + 1] == '.' || p[i + 1] == ':' || p[i + 1] == '=')) {
            char kind = p[i + 1];
            int64_t j = i + 2;
            while (j + 1 < len && !(p[j] == kind && p[j + 1] == ']')) j++;
            if (j + 1 >= len) return 0;
            i = j + 2;
            continue;
        }
        if (c == ']') {
            in_bracket = 0;
            i++;
            continue;
        }
        i++;
    }
    return 0;
}

typedef struct {
    char *pattern;
    int64_t len;

    int rejected;
    int needs_custom;

    int have_glibc;
    int glibc_bad;
    char *glibc_err;
    regex_t glibc_re;

    int custom_attempted;
    AsnixCustomRegex *custom_re;
    int64_t custom_nsub;

    int span_trusted;
    int span_verify_count;
} RegexCacheEntry;

static RegexCacheEntry **g_regex_cache = NULL;
static size_t g_regex_cache_count = 0, g_regex_cache_cap = 0;
static size_t *g_regex_cache_index = NULL;
static size_t g_regex_cache_index_cap = 0;
#define REGEX_CACHE_EMPTY_SLOT ((size_t)-1)

static uint64_t regex_cache_hash(const char *s, int64_t len) {
    uint64_t h = 0;
    for (int64_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 0x517cc1b727220a95ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static void regex_cache_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = REGEX_CACHE_EMPTY_SLOT;
    for (size_t i = 0; i < g_regex_cache_count; i++) {
        RegexCacheEntry *e = g_regex_cache[i];
        size_t slot = regex_cache_hash(e->pattern, e->len) & (new_cap - 1);
        while (new_table[slot] != REGEX_CACHE_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_regex_cache_index);
    g_regex_cache_index = new_table;
    g_regex_cache_index_cap = new_cap;
}

static RegexCacheEntry *regex_cache_find(const char *pattern, int64_t len) {
    if (g_regex_cache_index_cap == 0) return NULL;
    size_t slot = regex_cache_hash(pattern, len) & (g_regex_cache_index_cap - 1);
    for (;;) {
        size_t idx = g_regex_cache_index[slot];
        if (idx == REGEX_CACHE_EMPTY_SLOT) return NULL;
        RegexCacheEntry *e = g_regex_cache[idx];
        if (e->len == len && memcmp(e->pattern, pattern, (size_t)len) == 0) return e;
        slot = (slot + 1) & (g_regex_cache_index_cap - 1);
    }
}

static RegexCacheEntry *regex_cache_insert(const char *pattern, int64_t len) {
    RegexCacheEntry *e = malloc(sizeof *e);
    e->pattern = malloc(len > 0 ? (size_t)len : 1);
    if (len > 0) memcpy(e->pattern, pattern, (size_t)len);
    e->len = len;
    e->rejected = pattern_rejected_by_real_nix(pattern, len);
    e->needs_custom = asnix_regex_needs_custom_engine(pattern, len);
    e->have_glibc = 0;
    e->glibc_bad = 0;
    e->glibc_err = NULL;
    e->custom_attempted = 0;
    e->custom_re = NULL;
    e->custom_nsub = 0;
    e->span_trusted = 0;
    e->span_verify_count = 0;

    if (g_regex_cache_count == g_regex_cache_cap) {
        g_regex_cache_cap = g_regex_cache_cap ? g_regex_cache_cap * 2 : 32;
        g_regex_cache = realloc(g_regex_cache, g_regex_cache_cap * sizeof(RegexCacheEntry *));
    }
    if (g_regex_cache_index_cap == 0) regex_cache_index_rehash(64);
    else if ((g_regex_cache_count + 1) * 4 > g_regex_cache_index_cap * 3) regex_cache_index_rehash(g_regex_cache_index_cap * 2);
    size_t slot = regex_cache_hash(e->pattern, e->len) & (g_regex_cache_index_cap - 1);
    while (g_regex_cache_index[slot] != REGEX_CACHE_EMPTY_SLOT) slot = (slot + 1) & (g_regex_cache_index_cap - 1);
    g_regex_cache_index[slot] = g_regex_cache_count;
    g_regex_cache[g_regex_cache_count] = e;
    g_regex_cache_count++;
    return e;
}

static RegexCacheEntry *regex_cache_get(const char *pattern, int64_t len) {
    RegexCacheEntry *e = regex_cache_find(pattern, len);
    if (e) return e;
    return regex_cache_insert(pattern, len);
}

static regex_t *regex_cache_ensure_glibc(RegexCacheEntry *e, const char *pattern, const char *who) {
    if (e->rejected) {
        asnix_bshared_fatal(who, "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
    }
    if (e->glibc_bad) {
        asnix_bshared_fatal(who, e->glibc_err);
    }
    if (e->have_glibc) return &e->glibc_re;
    int rc = regcomp(&e->glibc_re, pattern, REG_EXTENDED);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, &e->glibc_re, errbuf, sizeof errbuf);
        e->glibc_bad = 1;
        size_t elen = strlen(errbuf);
        e->glibc_err = malloc(elen + 1);
        memcpy(e->glibc_err, errbuf, elen + 1);
        asnix_bshared_fatal(who, e->glibc_err);
    }
    e->have_glibc = 1;
    return &e->glibc_re;
}

static AsnixCustomRegex *regex_cache_ensure_custom(RegexCacheEntry *e, const char *pattern, int64_t plen) {
    if (e->custom_attempted) return e->custom_re;
    e->custom_attempted = 1;
    e->custom_re = asnix_regex_compile(pattern, plen, &e->custom_nsub);
    return e->custom_re;
}

static Value groups_to_list(const regmatch_t *pmatch, size_t nsub, const char *base) {
    Value *vals = nsub > 0 ? malloc(nsub * sizeof(Value)) : NULL;
    for (size_t i = 0; i < nsub; i++) {
        regmatch_t m = pmatch[i + 1];
        if (m.rm_so < 0) {
            vals[i] = ASNIX_NULL;
        } else {
            vals[i] = asnix_make_string_value(base + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
        }
    }
    Value result = asnix_bshared_make_list(vals, (int64_t)nsub);
    free(vals);
    return result;
}

static Value custom_groups_to_list(const AsnixRegexGroup *g, int64_t nsub, const char *subject) {
    Value *vals = nsub > 0 ? malloc((size_t)nsub * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < nsub; i++) {
        if (g[i].so < 0) {
            vals[i] = ASNIX_NULL;
        } else {
            vals[i] = asnix_make_string_value(subject + g[i].so, (size_t)(g[i].eo - g[i].so));
        }
    }
    Value result = asnix_bshared_make_list(vals, nsub);
    free(vals);
    return result;
}

#define ASNIX_REGEX_CUSTOM_MAX_SPAN 2000

#define ASNIX_REGEX_SPAN_VERIFY_CALLS 8

static Value groups_to_list_recomputed(const regmatch_t *pmatch, size_t nsub, const char *base,
                                        RegexCacheEntry *e, const char *pattern, int64_t plen,
                                        const char *subject, int64_t slen, int64_t match_so,
                                        int64_t match_eo, int notbol) {
    if (nsub == 0) return groups_to_list(pmatch, nsub, base);
    if (match_eo - match_so > ASNIX_REGEX_CUSTOM_MAX_SPAN) return groups_to_list(pmatch, nsub, base);
    AsnixCustomRegex *cre = regex_cache_ensure_custom(e, pattern, plen);
    if (cre && e->custom_nsub == (int64_t)nsub) {
        AsnixRegexGroup *g = malloc(nsub * sizeof *g);
        int ok = asnix_regex_match_anchored(cre, subject, slen, match_so, match_eo, notbol, g);
        if (ok) {
            Value r = custom_groups_to_list(g, (int64_t)nsub, subject);
            free(g);
            return r;
        }
        free(g);
    }
    return groups_to_list(pmatch, nsub, base);
}

Value asnix_prim_match2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    asnix_bshared_string_bytes(args[0], &pbytes, &plen, "match");
    asnix_bshared_string_bytes(args[1], &sbytes, &slen, "match");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    Value result;

    RegexCacheEntry *re = regex_cache_get(pattern, plen);

    if (re->needs_custom) {
        if (re->rejected) {
            asnix_bshared_fatal("match", "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
        }
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (!cre) asnix_bshared_fatal("match", "invalid regular expression");
        free(pattern);
        int64_t nsub = re->custom_nsub;

        AsnixRegexGroup *g = nsub > 0 ? malloc((size_t)nsub * sizeof *g) : NULL;
        int64_t mso, meo;
        int found = asnix_regex_search(cre, subject, slen, 0, 0, &mso, &meo, g);
        if (!found || mso != 0 || meo != slen) {
            result = ASNIX_NULL;
        } else {
            result = custom_groups_to_list(g, nsub, subject);
        }
        free(g);
        free(subject);
        return result;
    }

    regex_t *preg = regex_cache_ensure_glibc(re, pattern, "match");

    size_t nsub = preg->re_nsub;

    if (re->span_trusted == 1 && slen <= ASNIX_REGEX_CUSTOM_MAX_SPAN) {
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            AsnixRegexGroup *g = nsub > 0 ? malloc(nsub * sizeof *g) : NULL;
            int ok = asnix_regex_match_anchored(cre, subject, slen, 0, slen, 0, g);
            if (ok) {
                result = nsub > 0 ? custom_groups_to_list(g, (int64_t)nsub, subject) : asnix_bshared_make_list(NULL, 0);
            } else {
                result = ASNIX_NULL;
            }
            free(g);
            free(pattern);
            free(subject);
            return result;
        }
    }

    regmatch_t *pmatch = malloc((nsub + 1) * sizeof(regmatch_t));
    int mrc = regexec(preg, subject, nsub + 1, pmatch, 0);
    int glibc_ok = (mrc == 0 && pmatch[0].rm_so == 0 && pmatch[0].rm_eo == (regoff_t)slen);

    if (re->span_trusted == 0 && slen <= ASNIX_REGEX_CUSTOM_MAX_SPAN) {
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            AsnixRegexGroup *g = nsub > 0 ? malloc(nsub * sizeof *g) : NULL;
            int custom_ok = asnix_regex_match_anchored(cre, subject, slen, 0, slen, 0, g);
            if (custom_ok != glibc_ok) {
                re->span_trusted = -1;
            } else {
                re->span_verify_count++;
                if (re->span_verify_count >= ASNIX_REGEX_SPAN_VERIFY_CALLS) re->span_trusted = 1;
            }
            free(g);
        } else {
            re->span_trusted = -1;
        }
    }

    if (!glibc_ok) {
        result = ASNIX_NULL;
    } else {
        result = groups_to_list_recomputed(pmatch, nsub, subject, re, pattern, plen, subject, slen, 0, slen, 0);
    }

    free(pattern);
    free(pmatch);
    free(subject);
    return result;
}
static Value build_match(void) { return asnix_bshared_make_curried_native((void *)asnix_prim_match2_impl, 2); }

static Value split_via_custom_engine(const AsnixCustomRegex *cre, int64_t nsub, const char *subject, int64_t slen) {
    Value *out = NULL;
    int64_t cap = 0, n = 0;
    int64_t pos = 0, last_end = 0;
    AsnixRegexGroup *g = nsub > 0 ? malloc((size_t)nsub * sizeof *g) : NULL;

    while (pos <= slen) {
        int64_t abs_so, abs_eo;
        if (!asnix_regex_search(cre, subject, slen, pos, pos > 0, &abs_so, &abs_eo, g)) break;

        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = asnix_make_string_value(subject + last_end, (size_t)(abs_so - last_end));
        out[n++] = custom_groups_to_list(g, nsub, subject);

        last_end = abs_eo;
        pos = (abs_eo == abs_so) ? abs_eo + 1 : abs_eo;
    }
    free(g);

    if (n + 1 > cap) {
        cap = cap ? cap + 1 : 1;
        out = realloc(out, (size_t)cap * sizeof(Value));
    }
    out[n++] = asnix_make_string_value(subject + last_end, (size_t)(slen - last_end));

    Value result = asnix_bshared_make_list(out, n);
    free(out);
    return result;
}

static Value split2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    asnix_bshared_string_bytes(args[0], &pbytes, &plen, "split");
    asnix_bshared_string_bytes(args[1], &sbytes, &slen, "split");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    RegexCacheEntry *re = regex_cache_get(pattern, plen);

    if (re->needs_custom) {
        if (re->rejected) {
            asnix_bshared_fatal("split", "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
        }
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (!cre) asnix_bshared_fatal("split", "invalid regular expression");
        free(pattern);
        Value result = split_via_custom_engine(cre, re->custom_nsub, subject, slen);
        free(subject);
        return result;
    }

    regex_t *preg = regex_cache_ensure_glibc(re, pattern, "split");
    size_t nsub = preg->re_nsub;

    if (nsub > 0 && slen <= ASNIX_REGEX_CUSTOM_MAX_SPAN) {
        AsnixCustomRegex *cre = regex_cache_ensure_custom(re, pattern, plen);
        if (cre && re->custom_nsub == (int64_t)nsub) {
            Value result = split_via_custom_engine(cre, re->custom_nsub, subject, slen);
            free(pattern);
            free(subject);
            return result;
        }
    }

    Value *out = NULL;
    int64_t cap = 0, n = 0;
    int64_t pos = 0, last_end = 0;
    regmatch_t *pmatch = malloc((nsub + 1) * sizeof(regmatch_t));

    while (pos <= slen) {
        int eflags = pos > 0 ? REG_NOTBOL : 0;
        int mrc = regexec(preg, subject + pos, nsub + 1, pmatch, eflags);
        if (mrc != 0) break;

        int64_t abs_so = pos + pmatch[0].rm_so;
        int64_t abs_eo = pos + pmatch[0].rm_eo;

        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = asnix_make_string_value(subject + last_end, (size_t)(abs_so - last_end));
        out[n++] = groups_to_list_recomputed(pmatch, nsub, subject + pos, re, pattern, plen, subject, slen,
                                              abs_so, abs_eo, pos > 0);

        last_end = abs_eo;
        pos = (abs_eo == abs_so) ? abs_eo + 1 : abs_eo;
    }

    if (n + 1 > cap) {
        cap = cap ? cap + 1 : 1;
        out = realloc(out, (size_t)cap * sizeof(Value));
    }
    out[n++] = asnix_make_string_value(subject + last_end, (size_t)(slen - last_end));

    Value result = asnix_bshared_make_list(out, n);
    free(out);
    free(pattern);
    free(pmatch);
    free(subject);
    return result;
}
static Value build_split(void) { return asnix_bshared_make_curried_native((void *)split2_impl, 2); }

const AsnixBuiltinMiscDef ASNIX_REGEX_BUILTINS[] = {
    { "match", build_match },
    { "split", build_split },
};

const size_t ASNIX_REGEX_BUILTINS_COUNT = sizeof(ASNIX_REGEX_BUILTINS) / sizeof(ASNIX_REGEX_BUILTINS[0]);

