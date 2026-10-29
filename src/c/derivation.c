#include "derivation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "aterm.h"
#include "base32.h"
#include "base64.h"
#include "builtins_compare.h"
#include "builtins_json.h"
#include "builtins_shared.h"
#include "eval_file.h"
#include "hash_shim.h"
#include "nar.h"
#include "store_path.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

#define ASNIX_DRV_MAX_ARGS    64
#define ASNIX_DRV_MAX_ENV     256
#define ASNIX_DRV_MAX_OUTPUTS 16
#define ASNIX_DRV_MAX_INPUT_DRVS 1024

static void sha256_or_die(const char *s, size_t len, uint8_t out[32]) {
    int64_t rc = asnix_hash_sha256(s, len, out);
    if (rc != 0) {
        fprintf(stderr,
                "asnix: derivation: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }
}

static int envvar_cmp(const void *a, const void *b) {
    return strcmp(((const AtermEnvVar *)a)->key, ((const AtermEnvVar *)b)->key);
}

static int atermoutput_cmp(const void *a, const void *b) {
    return strcmp(((const AtermOutput *)a)->name, ((const AtermOutput *)b)->name);
}

static int strptr_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void output_path_name(const char *drv_name, const char *output_name, char *out, size_t out_cap) {
    if (strcmp(output_name, "out") == 0) {
        snprintf(out, out_cap, "%s", drv_name);
    } else {
        snprintf(out, out_cap, "%s-%s", drv_name, output_name);
    }
}

static int valid_hash_algo_str(const char *s, size_t len) {
    return (len == 3 && memcmp(s, "md5", 3) == 0) || (len == 4 && memcmp(s, "sha1", 4) == 0) ||
           (len == 6 && memcmp(s, "sha256", 6) == 0) || (len == 6 && memcmp(s, "sha512", 6) == 0);
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hash_algo_digest_bytes(const char *s, size_t len) {
    if (len == 3) return 16;
    if (len == 4) return 20;
    if (len == 6 && s[3] == '2') return 32;
    return 64;
}

static void decode_output_hash(const char *output_hash, const char *output_hash_algo,
                                const char *output_hash_mode, int *out_has_hash, uint8_t out_raw[64],
                                size_t *out_raw_len, const char **out_algo, size_t *out_algo_len) {
    *out_has_hash = 0;
    if (output_hash_mode && strcmp(output_hash_mode, "flat") != 0 && strcmp(output_hash_mode, "recursive") != 0) {
        fprintf(stderr, "asnix: unknown hash mode '%s', expected 'flat' or 'recursive'\n", output_hash_mode);
        exit(1);
    }
    if (output_hash_algo && output_hash_algo[0] == '\0') output_hash_algo = NULL;
    if (output_hash_algo && !valid_hash_algo_str(output_hash_algo, strlen(output_hash_algo))) {
        fprintf(stderr, "asnix: unknown hash algorithm '%s'\n", output_hash_algo);
        exit(1);
    }
    if (!output_hash) return;
    const char *algo = output_hash_algo;
    size_t algo_len = algo ? strlen(algo) : 0;
    const char *hash_body = output_hash;
    const char *sep = strchr(output_hash, '-');
    const char *sep2 = strchr(output_hash, ':');
    if (sep2 && (!sep || sep2 < sep)) sep = sep2;
    if (sep && valid_hash_algo_str(output_hash, (size_t)(sep - output_hash))) {
        algo = output_hash;
        algo_len = (size_t)(sep - output_hash);
        hash_body = sep + 1;
    }
    if (!algo) return;
    size_t n = hash_algo_digest_bytes(algo, algo_len);
    size_t hash_len = strlen(hash_body);
    size_t hex_len = 2 * n;
    size_t b32_len = asnix_base32_encoded_length(n);
    size_t b64_len = ((n + 2) / 3) * 4;
    size_t b64_unpadded_len = (n * 8 + 5) / 6;
    if (hash_len != hex_len && hash_len != b32_len && hash_len != b64_len && hash_len != b64_unpadded_len) {
        fprintf(stderr, "asnix: hash '%s' has wrong length for hash algorithm '%.*s'\n",
                output_hash, (int)algo_len, algo);
        exit(1);
    }

    if (hash_len == hex_len) {
        for (size_t i = 0; i < n; i++) {
            int hi = hex_digit_value(hash_body[2 * i]);
            int lo = hex_digit_value(hash_body[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                fprintf(stderr, "asnix: hash '%s' is not valid hex\n", output_hash);
                exit(1);
            }
            out_raw[i] = (uint8_t)((hi << 4) | lo);
        }
    } else if (hash_len == b32_len) {
        if (asnix_base32_decode(hash_body, hash_len, out_raw, n) != 0) {
            fprintf(stderr, "asnix: hash '%s' is not valid base32\n", output_hash);
            exit(1);
        }
    } else {
        if (asnix_base64_decode(hash_body, hash_len, out_raw, 64) != (int64_t)n) {
            fprintf(stderr, "asnix: hash '%s' is not valid base64\n", output_hash);
            exit(1);
        }
    }
    *out_has_hash = 1;
    *out_raw_len = n;
    *out_algo = algo;
    *out_algo_len = algo_len;
}

static void compute_store_path(const char *type, const uint8_t inner_digest[32], const char *name, char *out,
                                size_t out_cap) {
    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < 32; i++) {
        hashstr[7 + 2 * i] = hexdigits[inner_digest[i] >> 4];
        hashstr[7 + 2 * i + 1] = hexdigits[inner_digest[i] & 0xf];
    }
    hashstr[7 + 64] = '\0';

    size_t preimage_cap = strlen(type) + strlen(hashstr) + strlen(ASNIX_STORE_DIR) + strlen(name) + 8;
    char *preimage = malloc(preimage_cap);
    size_t n = asnix_store_path_preimage(type, hashstr, name, preimage, preimage_cap);
    if (n == 0) {
        fprintf(stderr, "asnix: derivation: preimage too long\n");
        exit(1);
    }

    uint8_t outer_digest[32];
    sha256_or_die(preimage, n, outer_digest);
    free(preimage);

    if (asnix_make_store_path_from_digest(outer_digest, name, out, out_cap) == 0) {
        fprintf(stderr, "asnix: derivation: computed store path too long\n");
        exit(1);
    }
}

void asnix_compute_fixed_output_path(const char *output_hash_mode, const uint8_t raw[64], size_t raw_len,
                                       const char *algo, size_t algo_len, const char *name, char *out,
                                       size_t out_cap) {
    int is_recursive = output_hash_mode && strcmp(output_hash_mode, "recursive") == 0;
    if (is_recursive && algo_len == 6 && memcmp(algo, "sha256", 6) == 0) {
        uint8_t digest32[32];
        memcpy(digest32, raw, 32);
        compute_store_path("source", digest32, name, out, out_cap);
        return;
    }
    static const char hexdigits[] = "0123456789abcdef";
    char hexhash[128 + 1];
    for (size_t i = 0; i < raw_len; i++) {
        hexhash[2 * i] = hexdigits[raw[i] >> 4];
        hexhash[2 * i + 1] = hexdigits[raw[i] & 0xf];
    }
    hexhash[2 * raw_len] = '\0';
    char payload[256];
    int n = snprintf(payload, sizeof(payload), "fixed:out:%s%.*s:%s:", is_recursive ? "r:" : "", (int)algo_len,
                      algo, hexhash);
    uint8_t digest[32];
    sha256_or_die(payload, (size_t)n, digest);
    compute_store_path("output:out", digest, name, out, out_cap);
}

static uint64_t dcc_hash_str(const char *s, size_t len) {
    uint64_t h = 0;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t word;
        memcpy(&word, s + i, 8);
        h ^= word;
        h *= 0x517cc1b727220a95ULL;
    }
    for (; i < len; i++) {
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

#define DCC_EMPTY_SLOT SIZE_MAX

typedef struct {
    char *drv_path;
    int is_fixed_output;
    uint8_t hash[32];
} MaskedHashCacheEntry;
static MaskedHashCacheEntry *g_masked_hash_cache = NULL;
static size_t g_masked_hash_cache_count = 0, g_masked_hash_cache_cap = 0;
static size_t *g_masked_hash_index = NULL;
static size_t g_masked_hash_index_cap = 0;

static void masked_hash_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < g_masked_hash_cache_count; i++) {
        const char *dp = g_masked_hash_cache[i].drv_path;
        size_t slot = dcc_hash_str(dp, strlen(dp)) & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_masked_hash_index);
    g_masked_hash_index = new_table;
    g_masked_hash_index_cap = new_cap;
}

static int masked_hash_cache_find(const char *drv_path, MaskedHashCacheEntry *out) {
    if (g_masked_hash_index_cap == 0) return 0;
    size_t len = strlen(drv_path);
    size_t slot = dcc_hash_str(drv_path, len) & (g_masked_hash_index_cap - 1);
    for (;;) {
        size_t idx = g_masked_hash_index[slot];
        if (idx == DCC_EMPTY_SLOT) return 0;
        if (strcmp(g_masked_hash_cache[idx].drv_path, drv_path) == 0) {
            *out = g_masked_hash_cache[idx];
            return 1;
        }
        slot = (slot + 1) & (g_masked_hash_index_cap - 1);
    }
}

static void masked_hash_cache_insert(const char *drv_path, int is_fixed_output, const uint8_t hash[32]) {
    if (g_masked_hash_cache_count == g_masked_hash_cache_cap) {
        g_masked_hash_cache_cap = g_masked_hash_cache_cap ? g_masked_hash_cache_cap * 2 : 64;
        g_masked_hash_cache = realloc(g_masked_hash_cache, g_masked_hash_cache_cap * sizeof(*g_masked_hash_cache));
    }
    if (g_masked_hash_index_cap == 0) {
        masked_hash_index_rehash(128);
    } else if ((g_masked_hash_cache_count + 1) * 4 > g_masked_hash_index_cap * 3) {
        masked_hash_index_rehash(g_masked_hash_index_cap * 2);
    }
    size_t len = strlen(drv_path);
    char *copy = malloc(len + 1);
    memcpy(copy, drv_path, len + 1);
    size_t idx = g_masked_hash_cache_count;
    g_masked_hash_cache[idx].drv_path = copy;
    g_masked_hash_cache[idx].is_fixed_output = is_fixed_output;
    memcpy(g_masked_hash_cache[idx].hash, hash, 32);
    g_masked_hash_cache_count++;
    size_t slot = dcc_hash_str(drv_path, len) & (g_masked_hash_index_cap - 1);
    while (g_masked_hash_index[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (g_masked_hash_index_cap - 1);
    g_masked_hash_index[slot] = idx;
}

typedef struct {
    char *drv_path;
    char *output_name;
} BuiltRef;

typedef struct {
    BuiltRef *drvs;
    size_t ndrvs, drvs_cap;
    char **srcs;
    size_t nsrcs, srcs_cap;
    size_t *drvs_index;
    size_t drvs_index_cap;
    size_t *srcs_index;
    size_t srcs_index_cap;
} InputRefSet;

static void input_ref_set_init(InputRefSet *s) {
    memset(s, 0, sizeof(*s));
}

static char *dup_cstr(const char *s, size_t len) {
    char *out = malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void input_ref_drvs_index_rehash(InputRefSet *s, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < s->ndrvs; i++) {
        const char *dp = s->drvs[i].drv_path, *on = s->drvs[i].output_name;
        uint64_t h = dcc_hash_str(dp, strlen(dp)) ^ (dcc_hash_str(on, strlen(on)) * 0x9e3779b97f4a7c15ULL);
        size_t slot = h & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(s->drvs_index);
    s->drvs_index = new_table;
    s->drvs_index_cap = new_cap;
}

static void input_ref_add_built(InputRefSet *s, const char *drv_path, const char *output_name) {
    size_t dp_len = strlen(drv_path), on_len = strlen(output_name);
    if (s->drvs_index_cap == 0) input_ref_drvs_index_rehash(s, 16);
    else if ((s->ndrvs + 1) * 4 > s->drvs_index_cap * 3) input_ref_drvs_index_rehash(s, s->drvs_index_cap * 2);
    uint64_t h = dcc_hash_str(drv_path, dp_len) ^ (dcc_hash_str(output_name, on_len) * 0x9e3779b97f4a7c15ULL);
    size_t slot = h & (s->drvs_index_cap - 1);
    for (;;) {
        size_t idx = s->drvs_index[slot];
        if (idx == DCC_EMPTY_SLOT) break;
        if (strcmp(s->drvs[idx].drv_path, drv_path) == 0 && strcmp(s->drvs[idx].output_name, output_name) == 0) return;
        slot = (slot + 1) & (s->drvs_index_cap - 1);
    }
    if (s->ndrvs == s->drvs_cap) {
        s->drvs_cap = s->drvs_cap ? s->drvs_cap * 2 : 8;
        s->drvs = realloc(s->drvs, s->drvs_cap * sizeof(*s->drvs));
    }
    s->drvs[s->ndrvs].drv_path = dup_cstr(drv_path, dp_len);
    s->drvs[s->ndrvs].output_name = dup_cstr(output_name, on_len);
    s->drvs_index[slot] = s->ndrvs;
    s->ndrvs++;
}

static void input_ref_srcs_index_rehash(InputRefSet *s, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < s->nsrcs; i++) {
        size_t slot = dcc_hash_str(s->srcs[i], strlen(s->srcs[i])) & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(s->srcs_index);
    s->srcs_index = new_table;
    s->srcs_index_cap = new_cap;
}

static void input_ref_add_src(InputRefSet *s, const char *path) {
    size_t len = strlen(path);
    if (s->srcs_index_cap == 0) input_ref_srcs_index_rehash(s, 16);
    else if ((s->nsrcs + 1) * 4 > s->srcs_index_cap * 3) input_ref_srcs_index_rehash(s, s->srcs_index_cap * 2);
    size_t slot = dcc_hash_str(path, len) & (s->srcs_index_cap - 1);
    for (;;) {
        size_t idx = s->srcs_index[slot];
        if (idx == DCC_EMPTY_SLOT) break;
        if (strcmp(s->srcs[idx], path) == 0) return;
        slot = (slot + 1) & (s->srcs_index_cap - 1);
    }
    if (s->nsrcs == s->srcs_cap) {
        s->srcs_cap = s->srcs_cap ? s->srcs_cap * 2 : 8;
        s->srcs = realloc(s->srcs, s->srcs_cap * sizeof(*s->srcs));
    }
    s->srcs[s->nsrcs] = dup_cstr(path, len);
    s->srcs_index[slot] = s->nsrcs;
    s->nsrcs++;
}

typedef struct {
    char *drv_path;
    char **output_names;
    size_t noutputs;
} DrvOutputsCacheEntry;
static DrvOutputsCacheEntry *g_drv_outputs_cache = NULL;
static size_t g_drv_outputs_cache_count = 0, g_drv_outputs_cache_cap = 0;
static size_t *g_drv_outputs_index = NULL;
static size_t g_drv_outputs_index_cap = 0;

static void drv_outputs_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < g_drv_outputs_cache_count; i++) {
        const char *dp = g_drv_outputs_cache[i].drv_path;
        size_t slot = dcc_hash_str(dp, strlen(dp)) & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_drv_outputs_index);
    g_drv_outputs_index = new_table;
    g_drv_outputs_index_cap = new_cap;
}

static const DrvOutputsCacheEntry *drv_outputs_cache_find(const char *drv_path) {
    if (g_drv_outputs_index_cap == 0) return NULL;
    size_t len = strlen(drv_path);
    size_t slot = dcc_hash_str(drv_path, len) & (g_drv_outputs_index_cap - 1);
    for (;;) {
        size_t idx = g_drv_outputs_index[slot];
        if (idx == DCC_EMPTY_SLOT) return NULL;
        if (strcmp(g_drv_outputs_cache[idx].drv_path, drv_path) == 0) return &g_drv_outputs_cache[idx];
        slot = (slot + 1) & (g_drv_outputs_index_cap - 1);
    }
}

static void drv_outputs_cache_insert(const char *drv_path, const char *const *output_names, size_t noutputs) {
    if (g_drv_outputs_cache_count == g_drv_outputs_cache_cap) {
        g_drv_outputs_cache_cap = g_drv_outputs_cache_cap ? g_drv_outputs_cache_cap * 2 : 64;
        g_drv_outputs_cache = realloc(g_drv_outputs_cache, g_drv_outputs_cache_cap * sizeof(*g_drv_outputs_cache));
    }
    if (g_drv_outputs_index_cap == 0) {
        drv_outputs_index_rehash(128);
    } else if ((g_drv_outputs_cache_count + 1) * 4 > g_drv_outputs_index_cap * 3) {
        drv_outputs_index_rehash(g_drv_outputs_index_cap * 2);
    }
    char **onames = malloc(noutputs * sizeof(char *));
    for (size_t i = 0; i < noutputs; i++) onames[i] = dup_cstr(output_names[i], strlen(output_names[i]));
    size_t idx = g_drv_outputs_cache_count;
    g_drv_outputs_cache[idx].drv_path = dup_cstr(drv_path, strlen(drv_path));
    g_drv_outputs_cache[idx].output_names = onames;
    g_drv_outputs_cache[idx].noutputs = noutputs;
    g_drv_outputs_cache_count++;
    size_t slot = dcc_hash_str(drv_path, strlen(drv_path)) & (g_drv_outputs_index_cap - 1);
    while (g_drv_outputs_index[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (g_drv_outputs_index_cap - 1);
    g_drv_outputs_index[slot] = idx;
}

typedef struct {
    char *drv_path;
    char **direct_drvs;
    size_t ndirect_drvs;
    char **direct_srcs;
    size_t ndirect_srcs;
    char **closure_drvs;
    size_t nclosure_drvs;
    char **closure_srcs;
    size_t nclosure_srcs;
    int materialized;
} DrvClosureCacheEntry;
static DrvClosureCacheEntry *g_drv_closure_cache = NULL;
static size_t g_drv_closure_cache_count = 0, g_drv_closure_cache_cap = 0;
static size_t *g_drv_closure_index = NULL;
static size_t g_drv_closure_index_cap = 0;

static void drv_closure_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < g_drv_closure_cache_count; i++) {
        const char *dp = g_drv_closure_cache[i].drv_path;
        size_t slot = dcc_hash_str(dp, strlen(dp)) & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_drv_closure_index);
    g_drv_closure_index = new_table;
    g_drv_closure_index_cap = new_cap;
}

static const DrvClosureCacheEntry *drv_closure_cache_find(const char *drv_path) {
    if (g_drv_closure_index_cap == 0) return NULL;
    size_t len = strlen(drv_path);
    size_t slot = dcc_hash_str(drv_path, len) & (g_drv_closure_index_cap - 1);
    for (;;) {
        size_t idx = g_drv_closure_index[slot];
        if (idx == DCC_EMPTY_SLOT) return NULL;
        if (strcmp(g_drv_closure_cache[idx].drv_path, drv_path) == 0) return &g_drv_closure_cache[idx];
        slot = (slot + 1) & (g_drv_closure_index_cap - 1);
    }
}

typedef struct {
    char **items;
    size_t n, cap;
    size_t *index;
    size_t index_cap;
} DrvClosureStrSet;

static void drv_closure_strset_rehash(DrvClosureStrSet *s, size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = DCC_EMPTY_SLOT;
    for (size_t i = 0; i < s->n; i++) {
        size_t slot = dcc_hash_str(s->items[i], strlen(s->items[i])) & (new_cap - 1);
        while (new_table[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(s->index);
    s->index = new_table;
    s->index_cap = new_cap;
}

static void drv_closure_strset_add(DrvClosureStrSet *s, const char *str) {
    size_t len = strlen(str);
    if (s->index_cap == 0) drv_closure_strset_rehash(s, 64);
    else if ((s->n + 1) * 4 > s->index_cap * 3) drv_closure_strset_rehash(s, s->index_cap * 2);
    size_t slot = dcc_hash_str(str, len) & (s->index_cap - 1);
    for (;;) {
        size_t idx = s->index[slot];
        if (idx == DCC_EMPTY_SLOT) break;
        if (strcmp(s->items[idx], str) == 0) return;
        slot = (slot + 1) & (s->index_cap - 1);
    }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->items = realloc(s->items, s->cap * sizeof(*s->items));
    }
    s->items[s->n] = dup_cstr(str, len);
    s->index[slot] = s->n;
    s->n++;
}

static void drv_closure_cache_insert(const char *drv_path, char **direct_drvs, size_t ndirect_drvs,
                                      char **direct_srcs, size_t ndirect_srcs) {
    if (g_drv_closure_cache_count == g_drv_closure_cache_cap) {
        g_drv_closure_cache_cap = g_drv_closure_cache_cap ? g_drv_closure_cache_cap * 2 : 64;
        g_drv_closure_cache = realloc(g_drv_closure_cache, g_drv_closure_cache_cap * sizeof(*g_drv_closure_cache));
    }
    if (g_drv_closure_index_cap == 0) {
        drv_closure_index_rehash(128);
    } else if ((g_drv_closure_cache_count + 1) * 4 > g_drv_closure_index_cap * 3) {
        drv_closure_index_rehash(g_drv_closure_index_cap * 2);
    }
    size_t idx = g_drv_closure_cache_count;
    g_drv_closure_cache[idx].drv_path = dup_cstr(drv_path, strlen(drv_path));
    g_drv_closure_cache[idx].direct_drvs = direct_drvs;
    g_drv_closure_cache[idx].ndirect_drvs = ndirect_drvs;
    g_drv_closure_cache[idx].direct_srcs = direct_srcs;
    g_drv_closure_cache[idx].ndirect_srcs = ndirect_srcs;
    g_drv_closure_cache[idx].closure_drvs = NULL;
    g_drv_closure_cache[idx].nclosure_drvs = 0;
    g_drv_closure_cache[idx].closure_srcs = NULL;
    g_drv_closure_cache[idx].nclosure_srcs = 0;
    g_drv_closure_cache[idx].materialized = 0;
    g_drv_closure_cache_count++;
    size_t slot = dcc_hash_str(drv_path, strlen(drv_path)) & (g_drv_closure_index_cap - 1);
    while (g_drv_closure_index[slot] != DCC_EMPTY_SLOT) slot = (slot + 1) & (g_drv_closure_index_cap - 1);
    g_drv_closure_index[slot] = idx;
}

static void drv_closure_materialize(DrvClosureCacheEntry *e) {
    if (e->materialized) return;
    e->materialized = 1;
    DrvClosureStrSet cdrvs;
    memset(&cdrvs, 0, sizeof(cdrvs));
    DrvClosureStrSet csrcs;
    memset(&csrcs, 0, sizeof(csrcs));
    for (size_t i = 0; i < e->ndirect_drvs; i++) {
        const char *dp = e->direct_drvs[i];
        drv_closure_strset_add(&cdrvs, dp);
        if (i == 0) continue;
        DrvClosureCacheEntry *sub = (DrvClosureCacheEntry *)drv_closure_cache_find(dp);
        if (sub) {
            drv_closure_materialize(sub);
            for (size_t j = 0; j < sub->nclosure_drvs; j++) drv_closure_strset_add(&cdrvs, sub->closure_drvs[j]);
            for (size_t j = 0; j < sub->nclosure_srcs; j++) drv_closure_strset_add(&csrcs, sub->closure_srcs[j]);
        }
    }
    for (size_t i = 0; i < e->ndirect_srcs; i++) drv_closure_strset_add(&csrcs, e->direct_srcs[i]);
    free(cdrvs.index);
    free(csrcs.index);
    e->closure_drvs = cdrvs.items;
    e->nclosure_drvs = cdrvs.n;
    e->closure_srcs = csrcs.items;
    e->nclosure_srcs = csrcs.n;
}

static DrvClosureCacheEntry *drv_closure_cache_find_materialized(const char *drv_path) {
    DrvClosureCacheEntry *e = (DrvClosureCacheEntry *)drv_closure_cache_find(drv_path);
    if (e) drv_closure_materialize(e);
    return e;
}

static void cache_own_drv_closure(const char *drv_path, const char *const *output_names, size_t noutputs,
                                   const InputRefSet *refs) {
    size_t ndirect_drvs = 1 + refs->ndrvs;
    char **direct_drvs = malloc(ndirect_drvs * sizeof(char *));
    direct_drvs[0] = dup_cstr(drv_path, strlen(drv_path));
    for (size_t i = 0; i < refs->ndrvs; i++) {
        const char *dp = refs->drvs[i].drv_path;
        direct_drvs[1 + i] = dup_cstr(dp, strlen(dp));
    }
    size_t ndirect_srcs = refs->nsrcs;
    char **direct_srcs = ndirect_srcs ? malloc(ndirect_srcs * sizeof(char *)) : NULL;
    for (size_t i = 0; i < ndirect_srcs; i++) {
        const char *sp = refs->srcs[i];
        direct_srcs[i] = dup_cstr(sp, strlen(sp));
    }
    drv_closure_cache_insert(drv_path, direct_drvs, ndirect_drvs, direct_srcs, ndirect_srcs);
    drv_outputs_cache_insert(drv_path, output_names, noutputs);
}

static void asnix_collect_input_refs(Value v, InputRefSet *out, int json_mode) {
    v = asnix_force_for_print(v);
    int tag = asnix_tag_of(v);
    if (tag == ASNIX_TAG_ATTRS) {
        v = asnix_bshared_attrs_resolve(v);
        const char *base = (const char *)(v.raw & ~0x7ULL);
        int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
        uint32_t sym_type = asnix_intern("type", 4);
        uint32_t sym_drvpath = asnix_intern("drvPath", 7);
        uint32_t sym_outname = asnix_intern("outputName", 10);
        int64_t idx_type = -1, idx_drvpath = -1, idx_outname = -1;
        for (int64_t i = 0; i < count; i++) {
            const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            if (sym == sym_type) idx_type = i;
            else if (sym == sym_drvpath) idx_drvpath = i;
            else if (sym == sym_outname) idx_outname = i;
        }
        if (idx_type < 0 || idx_drvpath < 0 || idx_outname < 0) {
            if (json_mode) {
                for (int64_t i = 0; i < count; i++) {
                    const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
                    asnix_collect_input_refs(*(const Value *)(entry + ASNIX_ATTR_OFF_VALUE), out, 1);
                }
                return;
            }
            uint32_t sym_tostring = asnix_intern("__toString", 10);
            int64_t idx_ts = asnix_bshared_attrs_find(v, sym_tostring);
            if (idx_ts >= 0) {
                Value fn = asnix_force_for_print(asnix_bshared_attrs_value(v, idx_ts));
                Value result = asnix_apply(fn, &v, 1);
                asnix_collect_input_refs(asnix_force_for_print(result), out, 0);
                return;
            }
            uint32_t sym_outpath = asnix_intern("outPath", 7);
            int64_t idx_op = asnix_bshared_attrs_find(v, sym_outpath);
            if (idx_op >= 0) {
                asnix_collect_input_refs(asnix_bshared_attrs_value(v, idx_op), out, 0);
                return;
            }
            return;
        }
        Value type_val = asnix_force_for_print(
            *(const Value *)(base + ASNIX_ATTRS_OFF_ENTRIES + idx_type * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE));
        if (asnix_tag_of(type_val) != ASNIX_TAG_PTR) return;
        int64_t tlen;
        const char *tbytes = asnix_ptr_bytes(type_val, &tlen);
        if (tlen != 10 || memcmp(tbytes, "derivation", 10) != 0) return;
        Value drvpath_val = asnix_force_for_print(
            *(const Value *)(base + ASNIX_ATTRS_OFF_ENTRIES + idx_drvpath * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE));
        Value outname_val = asnix_force_for_print(
            *(const Value *)(base + ASNIX_ATTRS_OFF_ENTRIES + idx_outname * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE));
        int64_t dlen, olen;
        const char *dbytes = asnix_ptr_bytes(drvpath_val, &dlen);
        const char *obytes = asnix_ptr_bytes(outname_val, &olen);
        char *dpath = dup_cstr(dbytes, (size_t)dlen);
        char *oname = dup_cstr(obytes, (size_t)olen);
        input_ref_add_built(out, dpath, oname);
        free(dpath);
        free(oname);
        return;
    }
    if (tag == ASNIX_TAG_LIST) {
        const char *base = (const char *)(v.raw & ~0x7ULL);
        int64_t count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
        const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
        for (int64_t i = 0; i < count; i++) asnix_collect_input_refs(elems[i], out, json_mode);
        return;
    }
    if (tag == ASNIX_TAG_PTR && !asnix_value_is_path(v) && !asnix_value_is_float(v)) {
        const AsnixStringContext *ctx = asnix_string_context_of(v);
        if (!ctx) return;
        for (size_t i = 0; i < ctx->ndrvs; i++) input_ref_add_built(out, ctx->drvs[i].drv_path, ctx->drvs[i].output_name);
        for (size_t i = 0; i < ctx->nsrcs; i++) input_ref_add_src(out, ctx->srcs[i]);
        for (size_t i = 0; i < ctx->ndeep; i++) {
            const char *dp = ctx->deep[i];
            const DrvClosureCacheEntry *closure = drv_closure_cache_find_materialized(dp);
            if (!closure) {
                input_ref_add_src(out, dp);
                continue;
            }
            for (size_t j = 0; j < closure->nclosure_drvs; j++) {
                const char *member = closure->closure_drvs[j];
                input_ref_add_src(out, member);
                const DrvOutputsCacheEntry *outs = drv_outputs_cache_find(member);
                if (outs) {
                    for (size_t k = 0; k < outs->noutputs; k++) input_ref_add_built(out, member, outs->output_names[k]);
                }
            }
            for (size_t j = 0; j < closure->nclosure_srcs; j++) input_ref_add_src(out, closure->closure_srcs[j]);
        }
        return;
    }
    if (tag == ASNIX_TAG_PTR && asnix_value_is_path(v)) {
        int64_t len;
        const char *bytes = asnix_ptr_bytes(v, &len);
        char *path = dup_cstr(bytes, (size_t)len);
        if (len > 11 && memcmp(bytes, "/nix/store/", 11) == 0) {
            input_ref_add_src(out, path);
        } else {
            char store_path[300];
            asnix_copy_path_to_store(path, store_path, sizeof(store_path));
            input_ref_add_src(out, store_path);
        }
        free(path);
    }
}

static char *normalize_json_attr(const char *raw_json) {
    Value parsed = asnix_json_parse_structured_attrs(raw_json, (int64_t)strlen(raw_json));
    char *normalized_tmp = asnix_toJSON_cstr(parsed);
    char *normalized = asnix_arena_alloc(strlen(normalized_tmp) + 1);
    memcpy(normalized, normalized_tmp, strlen(normalized_tmp) + 1);
    free(normalized_tmp);
    return normalized;
}

typedef struct {
    const char **group_paths;
    const char *(*group_outs)[ASNIX_DRV_MAX_OUTPUTS];
    size_t *group_nouts;
    size_t *group_order;
    AtermInputDrv *masked_input_drvs;
    AtermInputDrv *real_input_drvs;
    char (*masked_hash_hex)[65];
    const char **drv_refs;
    size_t drv_refs_cap;
} DrvScratchFrame;

static DrvScratchFrame *g_drv_scratch_frames = NULL;
static size_t g_drv_scratch_frames_cap = 0;
static size_t g_drv_scratch_depth = 0;

static DrvScratchFrame *drv_scratch_acquire(void) {
    size_t d = g_drv_scratch_depth++;
    if (d >= g_drv_scratch_frames_cap) {
        size_t new_cap = d + 1;
        g_drv_scratch_frames = realloc(g_drv_scratch_frames, new_cap * sizeof(*g_drv_scratch_frames));
        memset(g_drv_scratch_frames + g_drv_scratch_frames_cap, 0,
               (new_cap - g_drv_scratch_frames_cap) * sizeof(*g_drv_scratch_frames));
        g_drv_scratch_frames_cap = new_cap;
    }
    DrvScratchFrame *f = &g_drv_scratch_frames[d];
    if (!f->group_paths) {
        f->group_paths = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(const char *));
        f->group_outs = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(*f->group_outs));
        f->group_nouts = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(size_t));
        f->group_order = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(size_t));
        f->masked_input_drvs = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(AtermInputDrv));
        f->real_input_drvs = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(AtermInputDrv));
        f->masked_hash_hex = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(*f->masked_hash_hex));
    }
    return f;
}

static void drv_scratch_release(void) { g_drv_scratch_depth--; }

static const char **drv_scratch_refs(DrvScratchFrame *f, size_t need) {
    if (need > f->drv_refs_cap) {
        size_t new_cap = f->drv_refs_cap ? f->drv_refs_cap * 2 : 64;
        while (new_cap < need) new_cap *= 2;
        free(f->drv_refs);
        f->drv_refs = malloc(new_cap * sizeof(const char *));
        f->drv_refs_cap = new_cap;
    }
    return f->drv_refs;
}

Value asnix_prim_derivation(const Value *args, int64_t argc) {
    (void)argc;
    Value attrs_val = args[0];
    if (asnix_tag_of(attrs_val) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: derivation: argument must be an attribute set\n");
        exit(1);
    }
    attrs_val = asnix_bshared_attrs_resolve(attrs_val);
    const char *base = (const char *)(attrs_val.raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);

    const char *name = NULL;
    const char *system = NULL;
    const char *builder = NULL;
    const char *output_hash = NULL;
    const char *output_hash_algo = NULL;
    const char *output_hash_mode = NULL;
    const char *arg_strs[ASNIX_DRV_MAX_ARGS];
    size_t nargs = 0;
    AtermEnvVar env[ASNIX_DRV_MAX_ENV];
    size_t nenv = 0;
    const char *output_names[ASNIX_DRV_MAX_OUTPUTS];
    size_t noutputs = 0;
    int saw_outputs_attr = 0;

    int structured_attrs = 0;
    {
        uint32_t sa_sym = asnix_intern("__structuredAttrs", 17);
        for (int64_t i = 0; i < count; i++) {
            const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            if (sym != sa_sym) continue;
            Value raw_val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            Value val = asnix_force_for_print(raw_val);
            if (asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_TRUE) {
                structured_attrs = 1;
            } else if (!(asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_FALSE)) {
                fprintf(stderr, "asnix: derivation: '__structuredAttrs' must be a boolean\n");
                exit(1);
            }
            break;
        }
    }
    int ignore_nulls = 0;
    {
        uint32_t in_sym = asnix_intern("__ignoreNulls", 13);
        for (int64_t i = 0; i < count; i++) {
            const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            if (sym != in_sym) continue;
            Value raw_val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            Value val = asnix_force_for_print(raw_val);
            if (asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_TRUE) {
                ignore_nulls = 1;
            } else if (!(asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_FALSE)) {
                fprintf(stderr, "asnix: derivation: '__ignoreNulls' must be a boolean\n");
                exit(1);
            }
            break;
        }
    }
    uint32_t json_syms[ASNIX_DRV_MAX_ENV];
    Value json_vals[ASNIX_DRV_MAX_ENV];
    size_t njson = 0;

    InputRefSet refs;
    input_ref_set_init(&refs);

    for (int64_t i = 0; i < count; i++) {
        const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
        Value raw_val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
        Value val = asnix_force_for_print(raw_val);

        size_t keylen;
        const char *key = asnix_sym_name(sym, &keylen);

        if (keylen == 13 && memcmp(key, "__ignoreNulls", 13) == 0) continue;
        if (ignore_nulls && asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_NULL) continue;

        int is_special_early = (keylen == 4 && memcmp(key, "name", 4) == 0) ||
                               (keylen == 6 && memcmp(key, "system", 6) == 0) ||
                               (keylen == 7 && memcmp(key, "builder", 7) == 0) ||
                               (keylen == 10 && memcmp(key, "outputHash", 10) == 0) ||
                               (keylen == 14 && memcmp(key, "outputHashAlgo", 14) == 0) ||
                               (keylen == 14 && memcmp(key, "outputHashMode", 14) == 0);
        int is_args_key = (keylen == 4 && memcmp(key, "args", 4) == 0);
        int json_mode_for_attr = structured_attrs && !is_special_early && !is_args_key;
        asnix_collect_input_refs(val, &refs, json_mode_for_attr);

        if (keylen == 17 && memcmp(key, "__structuredAttrs", 17) == 0 && structured_attrs) continue;

        if (keylen == 4 && memcmp(key, "args", 4) == 0) {
            if (asnix_tag_of(val) != ASNIX_TAG_LIST) {
                fprintf(stderr, "asnix: derivation: 'args' must be a list\n");
                exit(1);
            }
            const char *lbase = (const char *)(val.raw & ~0x7ULL);
            int64_t lcount = *(const int64_t *)(lbase + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(lbase + ASNIX_LIST_OFF_ELEMS);
            for (int64_t j = 0; j < lcount; j++) {
                if (nargs >= ASNIX_DRV_MAX_ARGS) {
                    fprintf(stderr, "asnix: derivation: too many args (max %d)\n", ASNIX_DRV_MAX_ARGS);
                    exit(1);
                }
                Value ev = asnix_force_for_print(elems[j]);
                Value sv = asnix_coerce_to_string_for_derivation(ev);
                arg_strs[nargs++] = asnix_string_value_to_cstr(sv);
            }
            continue;
        }

        if (keylen == 7 && memcmp(key, "outputs", 7) == 0) {
            if (asnix_tag_of(val) != ASNIX_TAG_LIST) {
                fprintf(stderr, "asnix: derivation: 'outputs' must be a list of strings\n");
                exit(1);
            }
            const char *lbase = (const char *)(val.raw & ~0x7ULL);
            int64_t lcount = *(const int64_t *)(lbase + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(lbase + ASNIX_LIST_OFF_ELEMS);
            saw_outputs_attr = 1;
            noutputs = 0;
            for (int64_t j = 0; j < lcount; j++) {
                Value ev = asnix_force_for_print(elems[j]);
                Value osv = asnix_coerce_to_string_for_derivation(ev);
                const char *oname = asnix_string_value_to_cstr(osv);
                if (strcmp(oname, "drvPath") == 0) {
                    fprintf(stderr, "asnix: derivation: invalid derivation output name 'drvPath'\n");
                    exit(1);
                }
                for (size_t k = 0; k < noutputs; k++) {
                    if (strcmp(output_names[k], oname) == 0) {
                        fprintf(stderr, "asnix: derivation: duplicate derivation output '%s'\n", oname);
                        exit(1);
                    }
                }
                if (noutputs >= ASNIX_DRV_MAX_OUTPUTS) {
                    fprintf(stderr, "asnix: derivation: too many outputs (max %d)\n", ASNIX_DRV_MAX_OUTPUTS);
                    exit(1);
                }
                output_names[noutputs++] = oname;
            }
            if (noutputs == 0) {
                fprintf(stderr, "asnix: derivation: derivation cannot have an empty set of outputs\n");
                exit(1);
            }
        }

        if (structured_attrs) {
            if (njson >= ASNIX_DRV_MAX_ENV) {
                fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
                exit(1);
            }
            json_syms[njson] = sym;
            json_vals[njson] = val;
            njson++;
        }

        int is_special = (keylen == 4 && memcmp(key, "name", 4) == 0) ||
                         (keylen == 6 && memcmp(key, "system", 6) == 0) ||
                         (keylen == 7 && memcmp(key, "builder", 7) == 0) ||
                         (keylen == 10 && memcmp(key, "outputHash", 10) == 0) ||
                         (keylen == 14 && memcmp(key, "outputHashAlgo", 14) == 0) ||
                         (keylen == 14 && memcmp(key, "outputHashMode", 14) == 0);
        if (!structured_attrs || is_special) {
            Value sv = asnix_coerce_to_string_for_derivation(val);
            char *valcstr = asnix_string_value_to_cstr(sv);

            if (keylen == 4 && memcmp(key, "name", 4) == 0) name = valcstr;
            else if (keylen == 6 && memcmp(key, "system", 6) == 0) system = valcstr;
            else if (keylen == 7 && memcmp(key, "builder", 7) == 0) builder = valcstr;
            else if (keylen == 10 && memcmp(key, "outputHash", 10) == 0) output_hash = valcstr;
            else if (keylen == 14 && memcmp(key, "outputHashAlgo", 14) == 0) output_hash_algo = valcstr;
            else if (keylen == 14 && memcmp(key, "outputHashMode", 14) == 0) output_hash_mode = valcstr;

            if (!structured_attrs) {
                char *keycstr = asnix_arena_alloc(keylen + 1);
                memcpy(keycstr, key, keylen);
                keycstr[keylen] = '\0';

                if (keylen == 6 && memcmp(key, "__json", 6) == 0) {
                    valcstr = normalize_json_attr(valcstr);
                }

                if (nenv >= ASNIX_DRV_MAX_ENV) {
                    fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
                    exit(1);
                }
                env[nenv].key = keycstr;
                env[nenv].value = valcstr;
                nenv++;
            }
        }
    }

    if (structured_attrs) {
        Value json_attrs = asnix_bshared_make_attrs(json_syms, json_vals, (int64_t)njson);
        char *json_tmp = asnix_toJSON_cstr(json_attrs);
        size_t json_len = strlen(json_tmp);
        char *json_cstr = asnix_arena_alloc(json_len + 1);
        memcpy(json_cstr, json_tmp, json_len + 1);
        free(json_tmp);

        if (nenv >= ASNIX_DRV_MAX_ENV) {
            fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
            exit(1);
        }
        env[nenv].key = "__json";
        env[nenv].value = json_cstr;
        nenv++;
    }

    if (!name || !system || !builder) {
        fprintf(stderr, "asnix: derivation: 'name', 'system', and 'builder' are required attributes\n");
        exit(1);
    }
    asnix_check_store_path_name(name);
    int has_fixed_hash;
    uint8_t fixed_hash_raw[64];
    size_t fixed_hash_raw_len = 0;
    const char *fixed_hash_algo = NULL;
    size_t fixed_hash_algo_len = 0;
    decode_output_hash(output_hash, output_hash_algo, output_hash_mode, &has_fixed_hash, fixed_hash_raw,
                        &fixed_hash_raw_len, &fixed_hash_algo, &fixed_hash_algo_len);

    if (!saw_outputs_attr) {
        output_names[0] = "out";
        noutputs = 1;
    }
    if (output_hash && (noutputs != 1 || strcmp(output_names[0], "out") != 0)) {
        fprintf(stderr, "asnix: derivation: multiple outputs are not supported in fixed-output derivations\n");
        exit(1);
    }

    for (size_t o = 0; o < noutputs; o++) {
        size_t idx = ASNIX_DRV_MAX_ENV;
        for (size_t i = 0; i < nenv; i++)
            if (strcmp(env[i].key, output_names[o]) == 0) {
                idx = i;
                break;
            }
        if (idx == ASNIX_DRV_MAX_ENV) {
            if (nenv >= ASNIX_DRV_MAX_ENV) {
                fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
                exit(1);
            }
            idx = nenv++;
        }
        env[idx].key = output_names[o];
        env[idx].value = "";
    }

    qsort(env, nenv, sizeof(env[0]), envvar_cmp);

    DrvScratchFrame *scratch = drv_scratch_acquire();
    const char **group_paths = scratch->group_paths;
    const char *(*group_outs)[ASNIX_DRV_MAX_OUTPUTS] = scratch->group_outs;
    size_t *group_nouts = scratch->group_nouts;
    size_t ngroups = 0;
    for (size_t i = 0; i < refs.ndrvs; i++) {
        const char *dp = refs.drvs[i].drv_path;
        size_t g = ngroups;
        for (size_t k = 0; k < ngroups; k++) {
            if (strcmp(group_paths[k], dp) == 0) {
                g = k;
                break;
            }
        }
        if (g == ngroups) {
            if (ngroups >= ASNIX_DRV_MAX_INPUT_DRVS) {
                fprintf(stderr, "asnix: derivation: too many distinct input derivations (max %d)\n",
                        ASNIX_DRV_MAX_INPUT_DRVS);
                exit(1);
            }
            group_paths[ngroups] = dp;
            group_nouts[ngroups] = 0;
            ngroups++;
        }
        if (group_nouts[g] >= ASNIX_DRV_MAX_OUTPUTS) {
            fprintf(stderr, "asnix: derivation: too many referenced outputs of one input derivation (max %d)\n",
                    ASNIX_DRV_MAX_OUTPUTS);
            exit(1);
        }
        group_outs[g][group_nouts[g]++] = refs.drvs[i].output_name;
    }
    size_t *group_order = scratch->group_order;
    for (size_t g = 0; g < ngroups; g++) group_order[g] = g;
    for (size_t i = 1; i < ngroups; i++) {
        size_t j = i;
        while (j > 0 && strcmp(group_paths[group_order[j - 1]], group_paths[group_order[j]]) > 0) {
            size_t t = group_order[j - 1];
            group_order[j - 1] = group_order[j];
            group_order[j] = t;
            j--;
        }
    }
    for (size_t g = 0; g < ngroups; g++) {
        for (size_t i = 1; i < group_nouts[g]; i++) {
            size_t j = i;
            while (j > 0 && strcmp(group_outs[g][j - 1], group_outs[g][j]) > 0) {
                const char *t = group_outs[g][j - 1];
                group_outs[g][j - 1] = group_outs[g][j];
                group_outs[g][j] = t;
                j--;
            }
        }
    }

    AtermInputDrv *masked_input_drvs = scratch->masked_input_drvs;
    AtermInputDrv *real_input_drvs = scratch->real_input_drvs;
    char (*masked_hash_hex)[65] = scratch->masked_hash_hex;
    for (size_t gi = 0; gi < ngroups; gi++) {
        size_t g = group_order[gi];
        MaskedHashCacheEntry entry;
        if (!masked_hash_cache_find(group_paths[g], &entry)) {
            fprintf(stderr,
                    "asnix: derivation: internal error: no cached hash for input derivation '%s' "
                    "(referenced before it was ever evaluated?)\n",
                    group_paths[g]);
            exit(1);
        }
        static const char hexdigits[] = "0123456789abcdef";
        for (int b = 0; b < 32; b++) {
            masked_hash_hex[gi][2 * b] = hexdigits[entry.hash[b] >> 4];
            masked_hash_hex[gi][2 * b + 1] = hexdigits[entry.hash[b] & 0xf];
        }
        masked_hash_hex[gi][64] = '\0';

        masked_input_drvs[gi].drv_path = masked_hash_hex[gi];
        masked_input_drvs[gi].output_names = group_outs[g];
        masked_input_drvs[gi].noutput_names = group_nouts[g];

        real_input_drvs[gi].drv_path = group_paths[g];
        real_input_drvs[gi].output_names = group_outs[g];
        real_input_drvs[gi].noutput_names = group_nouts[g];
    }

    for (size_t i = 1; i < ngroups; i++) {
        size_t j = i;
        while (j > 0 && strcmp(masked_input_drvs[j - 1].drv_path, masked_input_drvs[j].drv_path) > 0) {
            AtermInputDrv t = masked_input_drvs[j - 1];
            masked_input_drvs[j - 1] = masked_input_drvs[j];
            masked_input_drvs[j] = t;
            j--;
        }
    }

    qsort(refs.srcs, refs.nsrcs, sizeof(char *), strptr_cmp);

    AtermOutput masked_outs[ASNIX_DRV_MAX_OUTPUTS];
    for (size_t o = 0; o < noutputs; o++) {
        masked_outs[o].name = output_names[o];
        masked_outs[o].path = "";
        masked_outs[o].hash_algo = NULL;
        masked_outs[o].hash = NULL;
    }
    qsort(masked_outs, noutputs, sizeof(masked_outs[0]), atermoutput_cmp);

    AtermEscapeCache *aterm_cache = asnix_aterm_escape_cache_new(2 * nenv + nargs + refs.nsrcs + 2 * ngroups + 8);

    char *masked_aterm = asnix_build_derivation_aterm_cached(masked_outs, noutputs, masked_input_drvs, ngroups,
                                                              (const char **)refs.srcs, refs.nsrcs, system, builder,
                                                              arg_strs, nargs, env, nenv, aterm_cache);
    uint8_t masked_digest[32];
    sha256_or_die(masked_aterm, strlen(masked_aterm), masked_digest);
    free(masked_aterm);

    char out_paths[ASNIX_DRV_MAX_OUTPUTS][300];
    for (size_t o = 0; o < noutputs; o++) {
        char type_str[32 + 7];
        snprintf(type_str, sizeof(type_str), "output:%s", output_names[o]);
        char path_name[256];
        output_path_name(name, output_names[o], path_name, sizeof(path_name));
        compute_store_path(type_str, masked_digest, path_name, out_paths[o], sizeof(out_paths[o]));

        for (size_t i = 0; i < nenv; i++)
            if (strcmp(env[i].key, output_names[o]) == 0) env[i].value = out_paths[o];
    }

    char fixed_hash_algo_cstr[16];
    char fixed_hash_algo_hex[128 + 1];
    if (has_fixed_hash) {
        asnix_compute_fixed_output_path(output_hash_mode, fixed_hash_raw, fixed_hash_raw_len, fixed_hash_algo,
                                   fixed_hash_algo_len, name, out_paths[0], sizeof(out_paths[0]));
        for (size_t i = 0; i < nenv; i++)
            if (strcmp(env[i].key, "out") == 0) env[i].value = out_paths[0];

        int is_recursive = output_hash_mode && strcmp(output_hash_mode, "recursive") == 0;
        snprintf(fixed_hash_algo_cstr, sizeof(fixed_hash_algo_cstr), "%s%.*s", is_recursive ? "r:" : "",
                 (int)fixed_hash_algo_len, fixed_hash_algo);

        static const char hexdigits[] = "0123456789abcdef";
        for (size_t i = 0; i < fixed_hash_raw_len; i++) {
            fixed_hash_algo_hex[2 * i] = hexdigits[fixed_hash_raw[i] >> 4];
            fixed_hash_algo_hex[2 * i + 1] = hexdigits[fixed_hash_raw[i] & 0xf];
        }
        fixed_hash_algo_hex[2 * fixed_hash_raw_len] = '\0';
    }

    AtermOutput real_outs[ASNIX_DRV_MAX_OUTPUTS];
    for (size_t o = 0; o < noutputs; o++) {
        real_outs[o].name = output_names[o];
        real_outs[o].path = out_paths[o];
        if (has_fixed_hash && strcmp(output_names[o], "out") == 0) {
            real_outs[o].hash_algo = fixed_hash_algo_cstr;
            real_outs[o].hash = fixed_hash_algo_hex;
        } else {
            real_outs[o].hash_algo = NULL;
            real_outs[o].hash = NULL;
        }
    }
    qsort(real_outs, noutputs, sizeof(real_outs[0]), atermoutput_cmp);

    char *real_aterm = asnix_build_derivation_aterm_cached(real_outs, noutputs, real_input_drvs, ngroups,
                                                            (const char **)refs.srcs, refs.nsrcs, system, builder,
                                                            arg_strs, nargs, env, nenv, aterm_cache);
    {
        const char *dbg = getenv("ASNIX_DEBUG_DRV");
        if (dbg && strcmp(dbg, name) == 0) fprintf(stderr, "ASNIX_DEBUG_DRV[%s] = %s\n", name, real_aterm);
    }
    uint8_t real_digest[32];
    sha256_or_die(real_aterm, strlen(real_aterm), real_digest);
    free(real_aterm);

    const char **drv_refs = drv_scratch_refs(scratch, ngroups + refs.nsrcs);
    size_t ndrv_refs = 0;
    for (size_t gi = 0; gi < ngroups; gi++) drv_refs[ndrv_refs++] = group_paths[group_order[gi]];
    for (size_t i = 0; i < refs.nsrcs; i++) drv_refs[ndrv_refs++] = refs.srcs[i];
    qsort(drv_refs, ndrv_refs, sizeof(const char *), strptr_cmp);
    {
        size_t w = 0;
        for (size_t r = 0; r < ndrv_refs; r++) {
            if (w == 0 || strcmp(drv_refs[w - 1], drv_refs[r]) != 0) drv_refs[w++] = drv_refs[r];
        }
        ndrv_refs = w;
    }

    size_t drv_type_cap = 5;
    for (size_t i = 0; i < ndrv_refs; i++) drv_type_cap += strlen(drv_refs[i]) + 1;
    char *drv_type = malloc(drv_type_cap);
    size_t drv_type_len = (size_t)snprintf(drv_type, drv_type_cap, "text");
    for (size_t i = 0; i < ndrv_refs; i++) {
        drv_type_len += (size_t)snprintf(drv_type + drv_type_len, drv_type_cap - drv_type_len, ":%s", drv_refs[i]);
    }
    char drv_name[256];
    snprintf(drv_name, sizeof(drv_name), "%s.drv", name);
    char drv_path[300];
    compute_store_path(drv_type, real_digest, drv_name, drv_path, sizeof(drv_path));
    free(drv_type);

    if (has_fixed_hash) {
        char fixed_hash_payload[512];
        int pn = snprintf(fixed_hash_payload, sizeof(fixed_hash_payload), "fixed:out:%s:%s:%s",
                           fixed_hash_algo_cstr, fixed_hash_algo_hex, out_paths[0]);
        uint8_t fixed_cache_hash[32];
        sha256_or_die(fixed_hash_payload, (size_t)pn, fixed_cache_hash);
        masked_hash_cache_insert(drv_path, 1, fixed_cache_hash);
    } else {
        char *cache_aterm = asnix_build_derivation_aterm_cached(real_outs, noutputs, masked_input_drvs, ngroups,
                                                                  (const char **)refs.srcs, refs.nsrcs, system,
                                                                  builder, arg_strs, nargs, env, nenv, aterm_cache);
        uint8_t cache_digest[32];
        sha256_or_die(cache_aterm, strlen(cache_aterm), cache_digest);
        free(cache_aterm);
        masked_hash_cache_insert(drv_path, 0, cache_digest);
    }
    asnix_aterm_escape_cache_free(aterm_cache);
    drv_scratch_release();

    cache_own_drv_closure(drv_path, output_names, noutputs, &refs);

    size_t nkeys = 1 + noutputs;
    uint32_t syms[1 + ASNIX_DRV_MAX_OUTPUTS];
    Value vals[1 + ASNIX_DRV_MAX_OUTPUTS];
    syms[0] = asnix_intern("drvPath", 7);
    vals[0] = asnix_make_string_value(drv_path, strlen(drv_path));
    AsnixStringContext drv_ctx;
    asnix_ctx_init(&drv_ctx);
    asnix_ctx_add_deep(&drv_ctx, drv_path);
    asnix_string_attach_context(vals[0], &drv_ctx);
    for (size_t o = 0; o < noutputs; o++) {
        syms[1 + o] = asnix_intern(output_names[o], strlen(output_names[o]));
        vals[1 + o] = asnix_make_string_value(out_paths[o], strlen(out_paths[o]));
        AsnixStringContext out_ctx;
        asnix_ctx_init(&out_ctx);
        asnix_ctx_add_built(&out_ctx, drv_path, output_names[o]);
        asnix_string_attach_context(vals[1 + o], &out_ctx);
    }
    for (size_t i = 1; i < nkeys; i++) {
        size_t j = i;
        while (j > 0 && syms[j - 1] > syms[j]) {
            uint32_t ts = syms[j - 1];
            syms[j - 1] = syms[j];
            syms[j] = ts;
            Value tv = vals[j - 1];
            vals[j - 1] = vals[j];
            vals[j] = tv;
            j--;
        }
    }

    void *mem = asnix_arena_alloc(8 + nkeys * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = (int64_t)nkeys;
    for (size_t i = 0; i < nkeys; i++) {
        char *e = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        *(uint32_t *)(e + ASNIX_ATTR_OFF_SYM) = syms[i];
        *(Value *)(e + ASNIX_ATTR_OFF_VALUE) = vals[i];
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

Value asnix_make_derivation_strict_primop(void) {
    return asnix_bshared_make_primop((void *)asnix_prim_derivation, 1);
}

static const char ASNIX_DERIVATION_NIX_SOURCE[] =
    "drvAttrs@{\n"
    "  outputs ? [ \"out\" ],\n"
    "  ...\n"
    "}:\n"
    "\n"
    "let\n"
    "\n"
    "  strict = derivationStrict drvAttrs;\n"
    "\n"
    "  commonAttrs =\n"
    "    drvAttrs\n"
    "    // (builtins.listToAttrs outputsList)\n"
    "    // {\n"
    "      all = map (x: x.value) outputsList;\n"
    "      inherit drvAttrs;\n"
    "    };\n"
    "\n"
    "  outputToAttrListElement = outputName: {\n"
    "    name = outputName;\n"
    "    value = commonAttrs // {\n"
    "      outPath = strict.${outputName};\n"
    "      drvPath = strict.drvPath;\n"
    "      type = \"derivation\";\n"
    "      inherit outputName;\n"
    "    };\n"
    "  };\n"
    "\n"
    "  outputsList = map outputToAttrListElement outputs;\n"
    "\n"
    "in\n"
    "(builtins.head outputsList).value\n";

#define ASNIX_DERIVATION_TEMP_PATH "/tmp/.asnix-corepkgs-derivation.nix"

static const char *write_derivation_nix(void) {
    FILE *f = fopen(ASNIX_DERIVATION_TEMP_PATH, "w");
    if (!f) {
        fprintf(stderr, "asnix: could not write '%s' for the `derivation` wrapper\n", ASNIX_DERIVATION_TEMP_PATH);
        exit(1);
    }
    fwrite(ASNIX_DERIVATION_NIX_SOURCE, 1, sizeof(ASNIX_DERIVATION_NIX_SOURCE) - 1, f);
    fclose(f);
    return ASNIX_DERIVATION_TEMP_PATH;
}

Value asnix_make_derivation_wrapper_value(void) {
    static int have_cached = 0;
    static Value cached;
    if (!have_cached) {
        cached = asnix_eval_file_for_derivation_bootstrap(write_derivation_nix());
        have_cached = 1;
    }
    return cached;
}

const AsnixBuiltinMiscDef ASNIX_DERIVATION_BUILTINS[] = {
    { "derivation", asnix_make_derivation_wrapper_value },
    { "derivationStrict", asnix_make_derivation_strict_primop },
};
const size_t ASNIX_DERIVATION_BUILTINS_COUNT =
    sizeof(ASNIX_DERIVATION_BUILTINS) / sizeof(ASNIX_DERIVATION_BUILTINS[0]);

