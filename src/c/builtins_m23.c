#define _DEFAULT_SOURCE
#include "builtins_m23.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "hash_shim.h"
#include "symtab.h"
#include "value_util.h"

typedef struct {
    const void **items;
    size_t len, cap;
} PtrSet;

static int ptrset_contains_or_add(PtrSet *s, const void *p) {
    for (size_t i = 0; i < s->len; i++) {
        if (s->items[i] == p) return 1;
    }
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->items = realloc(s->items, s->cap * sizeof(const void *));
    }
    s->items[s->len++] = p;
    return 0;
}

static void deep_force_rec(Value v, PtrSet *seen) {
    v = asnix_force_for_print(v);
    int tag = asnix_tag_of(v);
    if (tag != ASNIX_TAG_ATTRS && tag != ASNIX_TAG_LIST) return;
    const void *ptr = (const void *)(uintptr_t)(v.raw & ~(uint64_t)ASNIX_TAG_MASK);
    if (ptrset_contains_or_add(seen, ptr)) return;
    if (tag == ASNIX_TAG_ATTRS) {
        int64_t n = asnix_bshared_attrs_count(v);
        for (int64_t i = 0; i < n; i++) deep_force_rec(asnix_bshared_attrs_value(v, i), seen);
    } else {
        int64_t n = asnix_bshared_list_count(v);
        const Value *elems = asnix_bshared_list_elems(v);
        for (int64_t i = 0; i < n; i++) deep_force_rec(elems[i], seen);
    }
}

static Value seq2_impl(const Value *args, int64_t argc) {
    (void)argc;
    asnix_force_for_print(args[0]);
    return asnix_force_for_print(args[1]);
}
static Value build_seq(void) { return asnix_bshared_make_curried_native((void *)seq2_impl, 2); }

static Value deepSeq2_impl(const Value *args, int64_t argc) {
    (void)argc;
    PtrSet seen = { 0 };
    deep_force_rec(args[0], &seen);
    free(seen.items);
    return asnix_force_for_print(args[1]);
}
static Value build_deepSeq(void) { return asnix_bshared_make_curried_native((void *)deepSeq2_impl, 2); }

static char *resolve_fs_path(Value v, const char *who) {
    Value coerced = asnix_coerce_to_string(v);
    int64_t len;
    const char *bytes = asnix_ptr_bytes(coerced, &len);
    if (len == 0 || bytes[0] != '/') {
        fprintf(stderr, "asnix: %s: string '%.*s' doesn't represent an absolute path\n", who, (int)len, bytes);
        exit(1);
    }
    char *path = asnix_arena_alloc((size_t)len + 1);
    memcpy(path, bytes, (size_t)len);
    path[len] = '\0';
    return path;
}

static Value prim_pathExists(const Value *args, int64_t argc) {
    (void)argc;
    Value v = asnix_force_for_print(args[0]);
    int must_be_dir = 0;
    if (asnix_tag_of(v) == ASNIX_TAG_PTR && !asnix_value_is_path(v) && !asnix_value_is_float(v)) {
        int64_t slen;
        const char *s = asnix_ptr_bytes(v, &slen);
        if (slen >= 1 && s[slen - 1] == '/') must_be_dir = 1;
        else if (slen >= 2 && s[slen - 2] == '/' && s[slen - 1] == '.') must_be_dir = 1;
    }
    char *path = resolve_fs_path(v, "pathExists");
    struct stat st;
    int exists;
    if (must_be_dir) {
        exists = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    } else {
        exists = (lstat(path, &st) == 0);
    }
    return asnix_make_imm(exists ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}

static char *read_whole_file(const char *path, const char *who, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "asnix: %s: cannot open '%s'\n", who, path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "asnix: %s: cannot read '%s' (not a regular file?)\n", who, path);
        exit(1);
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size);
    size_t n = size > 0 ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    *out_len = n;
    return buf;
}

static Value prim_readFile(const Value *args, int64_t argc) {
    (void)argc;
    char *path = resolve_fs_path(args[0], "readFile");
    size_t len;
    char *buf = read_whole_file(path, "readFile", &len);
    Value result = asnix_make_string_value(buf, len);
    free(buf);
    return result;
}

static const char *stat_type_str(const struct stat *st) {
    if (S_ISDIR(st->st_mode)) return "directory";
    if (S_ISREG(st->st_mode)) return "regular";
    if (S_ISLNK(st->st_mode)) return "symlink";
    return "unknown";
}

static Value prim_readFileType(const Value *args, int64_t argc) {
    (void)argc;
    char *path = resolve_fs_path(args[0], "readFileType");
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "asnix: readFileType: cannot stat '%s'\n", path);
        exit(1);
    }
    const char *kind = stat_type_str(&st);
    return asnix_make_string_value(kind, strlen(kind));
}

static Value prim_readDir(const Value *args, int64_t argc) {
    (void)argc;
    char *path = resolve_fs_path(args[0], "readDir");
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "asnix: readDir: cannot open directory '%s'\n", path);
        exit(1);
    }
    uint32_t *syms = NULL;
    Value *vals = NULL;
    size_t n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        size_t namelen = strlen(ent->d_name);
        const char *kind;
        switch (ent->d_type) {
            case DT_DIR: kind = "directory"; break;
            case DT_REG: kind = "regular"; break;
            case DT_LNK: kind = "symlink"; break;
            case DT_UNKNOWN: {
                char *entpath = malloc(strlen(path) + 1 + namelen + 1);
                snprintf(entpath, strlen(path) + 1 + namelen + 1, "%s/%s", path, ent->d_name);
                struct stat st;
                kind = (lstat(entpath, &st) == 0) ? stat_type_str(&st) : "unknown";
                free(entpath);
                break;
            }
            default: kind = "unknown"; break;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            syms = realloc(syms, cap * sizeof(uint32_t));
            vals = realloc(vals, cap * sizeof(Value));
        }
        syms[n] = asnix_intern(ent->d_name, namelen);
        vals[n] = asnix_make_string_value(kind, strlen(kind));
        n++;
    }
    closedir(dir);
    Value result = asnix_bshared_make_attrs(syms, vals, (int64_t)n);
    free(syms);
    free(vals);
    return result;
}

static Value prim_toPath(const Value *args, int64_t argc) {
    (void)argc;
    Value coerced = asnix_coerce_to_string(args[0]);
    int64_t len;
    const char *bytes = asnix_ptr_bytes(coerced, &len);
    if (len == 0 || bytes[0] != '/') {
        fprintf(stderr, "asnix: string '%.*s' doesn't represent an absolute path\n", (int)len, bytes);
        exit(1);
    }
    return asnix_make_string_value(bytes, (size_t)len);
}

static Value prim_getEnv(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "getEnv");
    int64_t len;
    const char *bytes = asnix_ptr_bytes(args[0], &len);
    char *name = asnix_arena_alloc((size_t)len + 1);
    memcpy(name, bytes, (size_t)len);
    name[len] = '\0';
    const char *val = getenv(name);
    if (!val) return asnix_make_string_value("", 0);
    return asnix_make_string_value(val, strlen(val));
}

static Value hashFile_apply(const char *path, int64_t (*hash_fn)(const void *, size_t, unsigned char *),
                             size_t raw_len) {
    size_t len;
    char *buf = read_whole_file(path, "hashFile", &len);
    unsigned char raw[ASNIX_HASH_SHA512_LEN];
    int64_t rc = hash_fn(buf, len, raw);
    free(buf);
    if (rc != 0) {
        fprintf(stderr, "asnix: hashFile: failed to load libcrypto (rc=%lld) -- "
                         "enter the flake devShell (needs ASNIX_LIBCRYPTO_LIB / openssl)\n",
                (long long)rc);
        exit(1);
    }
    char hex[2 * ASNIX_HASH_SHA512_LEN];
    asnix_hash_to_hex(raw, raw_len, hex);
    return asnix_make_string_value(hex, 2 * raw_len);
}

static Value hashFile2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "hashFile");
    int64_t len;
    const char *name = asnix_ptr_bytes(args[0], &len);
    char *path = resolve_fs_path(args[1], "hashFile");

    if (len == 3 && memcmp(name, "md5", 3) == 0) return hashFile_apply(path, asnix_hash_md5, ASNIX_HASH_MD5_LEN);
    if (len == 4 && memcmp(name, "sha1", 4) == 0) return hashFile_apply(path, asnix_hash_sha1, ASNIX_HASH_SHA1_LEN);
    if (len == 6 && memcmp(name, "sha256", 6) == 0)
        return hashFile_apply(path, asnix_hash_sha256, ASNIX_HASH_SHA256_LEN);
    if (len == 6 && memcmp(name, "sha512", 6) == 0)
        return hashFile_apply(path, asnix_hash_sha512, ASNIX_HASH_SHA512_LEN);
    fprintf(stderr, "asnix: hashFile: unsupported algorithm \"%.*s\" (expected md5/sha1/sha256/sha512)\n", (int)len,
            name);
    exit(1);
}
static Value build_hashFile(void) { return asnix_bshared_make_curried_native((void *)hashFile2_impl, 2); }

Value asnix_prim_intersectAttrs2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value e1 = args[0], e2 = args[1];
    ASNIX_REQUIRE_ATTRS(e1, "intersectAttrs");
    ASNIX_REQUIRE_ATTRS(e2, "intersectAttrs");
    int64_t n1 = asnix_bshared_attrs_count(e1);
    int64_t n2 = asnix_bshared_attrs_count(e2);
    int64_t cap = n1 < n2 ? n1 : n2;
    uint32_t *syms = cap > 0 ? malloc((size_t)cap * sizeof(uint32_t)) : NULL;
    Value *vals = cap > 0 ? malloc((size_t)cap * sizeof(Value)) : NULL;
    int64_t out_n = 0;
    if (n1 <= n2) {
        for (int64_t i = 0; i < n1; i++) {
            uint32_t sym = asnix_bshared_attrs_sym(e1, i);
            int64_t j = asnix_bshared_attrs_find(e2, sym);
            if (j >= 0) {
                syms[out_n] = sym;
                vals[out_n] = asnix_bshared_attrs_value(e2, j);
                out_n++;
            }
        }
    } else {
        for (int64_t i = 0; i < n2; i++) {
            uint32_t sym = asnix_bshared_attrs_sym(e2, i);
            if (asnix_bshared_attrs_find(e1, sym) >= 0) {
                syms[out_n] = sym;
                vals[out_n] = asnix_bshared_attrs_value(e2, i);
                out_n++;
            }
        }
    }
    Value result = asnix_bshared_make_attrs(syms, vals, out_n);
    free(syms);
    free(vals);
    return result;
}
static Value build_intersectAttrs(void) {
    return asnix_bshared_make_curried_native((void *)asnix_prim_intersectAttrs2_impl, 2);
}

#define ASNIX_GROUPBY_HASH_MIN 32

typedef struct {
    uint32_t sym;
    Value *items;
    size_t len, cap;
} GroupBucket;

static Value groupBy2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0], list = args[1];
    ASNIX_REQUIRE_LIST(list, "groupBy");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);
    GroupBucket *buckets = NULL;
    size_t nbuckets = 0, bcap = 0;

    int use_hash = n > ASNIX_GROUPBY_HASH_MIN;
    size_t hcap = 0;
    uint32_t *hsym = NULL;
    size_t *hbucket = NULL;
    uint8_t *hused = NULL;
    if (use_hash) {
        hcap = 16;
        while (hcap < (size_t)n * 2) hcap <<= 1;
        hsym = malloc(hcap * sizeof(uint32_t));
        hbucket = malloc(hcap * sizeof(size_t));
        hused = calloc(hcap, 1);
    }

    for (int64_t i = 0; i < n; i++) {
        Value key = asnix_apply(f, &elems[i], 1);
        key = asnix_force_for_print(key);
        if (asnix_tag_of(key) != ASNIX_TAG_PTR || asnix_value_is_path(key) || asnix_value_is_float(key)) {
            fprintf(stderr, "asnix: groupBy: key function must return a string\n");
            exit(1);
        }
        int64_t klen;
        const char *kbytes = asnix_ptr_bytes(key, &klen);
        uint32_t sym = asnix_intern(kbytes, (size_t)klen);
        GroupBucket *b = NULL;
        size_t hslot = 0;
        if (use_hash) {
            size_t mask = hcap - 1;
            hslot = ((size_t)sym * 2654435761u) & mask;
            while (hused[hslot]) {
                if (hsym[hslot] == sym) {
                    b = &buckets[hbucket[hslot]];
                    break;
                }
                hslot = (hslot + 1) & mask;
            }
        } else {
            for (size_t j = 0; j < nbuckets; j++) {
                if (buckets[j].sym == sym) {
                    b = &buckets[j];
                    break;
                }
            }
        }
        if (!b) {
            if (nbuckets == bcap) {
                bcap = bcap ? bcap * 2 : 8;
                buckets = realloc(buckets, bcap * sizeof(GroupBucket));
            }
            size_t new_idx = nbuckets;
            b = &buckets[nbuckets++];
            b->sym = sym;
            b->items = NULL;
            b->len = 0;
            b->cap = 0;
            if (use_hash) {
                hused[hslot] = 1;
                hsym[hslot] = sym;
                hbucket[hslot] = new_idx;
            }
        }
        if (b->len == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 8;
            b->items = realloc(b->items, b->cap * sizeof(Value));
        }
        b->items[b->len++] = elems[i];
    }
    if (use_hash) {
        free(hsym);
        free(hbucket);
        free(hused);
    }
    uint32_t *syms = nbuckets > 0 ? malloc(nbuckets * sizeof(uint32_t)) : NULL;
    Value *vals = nbuckets > 0 ? malloc(nbuckets * sizeof(Value)) : NULL;
    for (size_t j = 0; j < nbuckets; j++) {
        syms[j] = buckets[j].sym;
        vals[j] = asnix_bshared_make_list(buckets[j].items, (int64_t)buckets[j].len);
        free(buckets[j].items);
    }
    free(buckets);
    Value result = asnix_bshared_make_attrs(syms, vals, (int64_t)nbuckets);
    free(syms);
    free(vals);
    return result;
}
static Value build_groupBy(void) { return asnix_bshared_make_curried_native((void *)groupBy2_impl, 2); }

static Value prim_parseDrvName(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "parseDrvName");
    int64_t len;
    const char *s = asnix_ptr_bytes(args[0], &len);
    int64_t split = -1;
    for (int64_t i = 0; i < len - 1; i++) {
        if (s[i] == '-' && !isalpha((unsigned char)s[i + 1])) {
            split = i;
            break;
        }
    }
    Value name_val, version_val;
    if (split < 0) {
        name_val = asnix_make_string_value(s, (size_t)len);
        version_val = asnix_make_string_value("", 0);
    } else {
        name_val = asnix_make_string_value(s, (size_t)split);
        version_val = asnix_make_string_value(s + split + 1, (size_t)(len - split - 1));
    }
    uint32_t syms[2] = { asnix_intern("name", 4), asnix_intern("version", 7) };
    Value vals[2] = { name_val, version_val };
    return asnix_bshared_make_attrs(syms, vals, 2);
}

static Value add2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_add_values(args[0], args[1]);
}
static Value sub2_impl(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) == ASNIX_TAG_INT && asnix_tag_of(args[1]) == ASNIX_TAG_INT) {
        return asnix_make_int(asnix_int_value(args[0]) - asnix_int_value(args[1]));
    }
    return asnix_sub_values(args[0], args[1]);
}
static Value mul2_impl(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) == ASNIX_TAG_INT && asnix_tag_of(args[1]) == ASNIX_TAG_INT) {
        int64_t a = asnix_int_value(args[0]), b = asnix_int_value(args[1]), r;
        if (__builtin_mul_overflow(a, b, &r)) {
            fprintf(stderr, "asnix: integer overflow in multiplying %lld * %lld\n", (long long)a, (long long)b);
            exit(1);
        }
        return asnix_make_int(r);
    }
    return asnix_mul_values(args[0], args[1]);
}
static Value div2_impl(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) == ASNIX_TAG_INT && asnix_tag_of(args[1]) == ASNIX_TAG_INT) {
        int64_t rhs = asnix_int_value(args[1]);
        if (rhs == 0) {
            fprintf(stderr, "asnix: division by zero, aborting\n");
            exit(1);
        }
        return asnix_make_int(asnix_int_value(args[0]) / rhs);
    }
    return asnix_div_values(args[0], args[1]);
}
static Value build_add(void) { return asnix_bshared_make_curried_native((void *)add2_impl, 2); }
static Value build_sub(void) { return asnix_bshared_make_curried_native((void *)sub2_impl, 2); }
static Value build_mul(void) { return asnix_bshared_make_curried_native((void *)mul2_impl, 2); }
static Value build_div(void) { return asnix_bshared_make_curried_native((void *)div2_impl, 2); }

const AsnixBuiltinMiscDef ASNIX_M23_BUILTINS[] = {
    { "seq", build_seq },
    { "deepSeq", build_deepSeq },
    { "intersectAttrs", build_intersectAttrs },
    { "groupBy", build_groupBy },
    { "add", build_add },
    { "sub", build_sub },
    { "mul", build_mul },
    { "div", build_div },
    { "hashFile", build_hashFile },
};

const size_t ASNIX_M23_BUILTINS_COUNT = sizeof(ASNIX_M23_BUILTINS) / sizeof(ASNIX_M23_BUILTINS[0]);

Value asnix_prim_pathExists(const Value *args, int64_t argc) { return prim_pathExists(args, argc); }
Value asnix_prim_readFile(const Value *args, int64_t argc) { return prim_readFile(args, argc); }
Value asnix_prim_readFileType(const Value *args, int64_t argc) { return prim_readFileType(args, argc); }
Value asnix_prim_readDir(const Value *args, int64_t argc) { return prim_readDir(args, argc); }
Value asnix_prim_toPath(const Value *args, int64_t argc) { return prim_toPath(args, argc); }
Value asnix_prim_getEnv(const Value *args, int64_t argc) { return prim_getEnv(args, argc); }
Value asnix_prim_parseDrvName(const Value *args, int64_t argc) { return prim_parseDrvName(args, argc); }

