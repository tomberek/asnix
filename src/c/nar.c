#define _DEFAULT_SOURCE /* lstat()/strdup()/readlink() -- same feature-test macro value_util.c uses */

#include "nar.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hash_shim.h"
#include "store_path.h"

typedef struct {
    char *buf;
    size_t len, cap;
} NarBuf;

static void nb_reserve(NarBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    b->cap = b->cap ? b->cap * 2 : 4096;
    while (b->len + extra > b->cap) b->cap *= 2;
    b->buf = realloc(b->buf, b->cap);
}

static void nb_bytes(NarBuf *b, const void *p, size_t n) {
    nb_reserve(b, n);
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

/* Little-endian 8-byte length, matching real Nix's own
 * `operator<<(Sink&, uint64_t)` (~/nix/src/libutil/include/nix/util/
 * serialise.hh) exactly. */
static void nb_u64(NarBuf *b, uint64_t n) {
    unsigned char buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (unsigned char)(n >> (8 * i));
    nb_bytes(b, buf, 8);
}

/* NAR string field: 8-byte length, raw bytes, zero-padded to the next
 * 8-byte boundary (~/nix/src/libutil/serialise.cc's `writeString`). */
static void nb_str(NarBuf *b, const char *s, size_t n) {
    nb_u64(b, (uint64_t)n);
    nb_bytes(b, s, n);
    size_t pad = (8 - (n % 8)) % 8;
    if (pad) {
        static const char zero[8] = { 0 };
        nb_bytes(b, zero, pad);
    }
}

static void nb_cstr(NarBuf *b, const char *s) {
    nb_str(b, s, strlen(s));
}

#define ASNIX_NAR_MAX_DEPTH 256

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void nar_dump(NarBuf *b, const char *path, int depth) {
    if (depth >= ASNIX_NAR_MAX_DEPTH) {
        fprintf(stderr, "asnix: path '%s' exceeds maximum NAR directory depth of %d\n", path, ASNIX_NAR_MAX_DEPTH);
        exit(1);
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "asnix: cannot stat '%s' while copying it to the store\n", path);
        exit(1);
    }

    nb_cstr(b, "(");

    if (S_ISREG(st.st_mode)) {
        nb_cstr(b, "type");
        nb_cstr(b, "regular");
        if (st.st_mode & S_IXUSR) {
            nb_cstr(b, "executable");
            nb_cstr(b, "");
        }
        nb_cstr(b, "contents");
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "asnix: cannot open '%s' while copying it to the store\n", path);
            exit(1);
        }
        uint64_t size = (uint64_t)st.st_size;
        nb_u64(b, size);
        char chunk[65536];
        uint64_t remaining = size;
        while (remaining > 0) {
            size_t want = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
            size_t got = fread(chunk, 1, want, f);
            if (got == 0) break;
            nb_bytes(b, chunk, got);
            remaining -= got;
        }
        fclose(f);
        size_t pad = (8 - (size % 8)) % 8;
        if (pad) {
            static const char zero[8] = { 0 };
            nb_bytes(b, zero, pad);
        }
    } else if (S_ISDIR(st.st_mode)) {
        nb_cstr(b, "type");
        nb_cstr(b, "directory");

        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "asnix: cannot read directory '%s' while copying it to the store\n", path);
            exit(1);
        }
        char **names = NULL;
        size_t nnames = 0, cap = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (nnames == cap) {
                cap = cap ? cap * 2 : 16;
                names = realloc(names, cap * sizeof(char *));
            }
            names[nnames++] = strdup(de->d_name);
        }
        closedir(d);
        qsort(names, nnames, sizeof(char *), name_cmp);

        for (size_t i = 0; i < nnames; i++) {
            nb_cstr(b, "entry");
            nb_cstr(b, "(");
            nb_cstr(b, "name");
            nb_cstr(b, names[i]);
            nb_cstr(b, "node");
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            nar_dump(b, child, depth + 1);
            nb_cstr(b, ")");
            free(names[i]);
        }
        free(names);
    } else if (S_ISLNK(st.st_mode)) {
        nb_cstr(b, "type");
        nb_cstr(b, "symlink");
        nb_cstr(b, "target");
        char target[4096];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n < 0) {
            fprintf(stderr, "asnix: cannot read symlink '%s' while copying it to the store\n", path);
            exit(1);
        }
        target[n] = '\0';
        nb_cstr(b, target);
    } else {
        fprintf(stderr, "asnix: file '%s' has an unsupported type for copying to the store\n", path);
        exit(1);
    }

    nb_cstr(b, ")");
}

typedef struct {
    char *path;
    char *result;
} CopyCacheEntry;
static CopyCacheEntry *g_copy_cache = NULL;
static size_t g_copy_cache_count = 0, g_copy_cache_cap = 0;

static const char *copy_cache_find(const char *path) {
    for (size_t i = 0; i < g_copy_cache_count; i++)
        if (strcmp(g_copy_cache[i].path, path) == 0) return g_copy_cache[i].result;
    return NULL;
}

static void copy_cache_insert(const char *path, const char *result) {
    if (g_copy_cache_count == g_copy_cache_cap) {
        g_copy_cache_cap = g_copy_cache_cap ? g_copy_cache_cap * 2 : 16;
        g_copy_cache = realloc(g_copy_cache, g_copy_cache_cap * sizeof(*g_copy_cache));
    }
    g_copy_cache[g_copy_cache_count].path = strdup(path);
    g_copy_cache[g_copy_cache_count].result = strdup(result);
    g_copy_cache_count++;
}

void asnix_copy_path_to_store(const char *path, char *out, size_t out_cap) {
    const char *cached = copy_cache_find(path);
    if (cached) {
        snprintf(out, out_cap, "%s", cached);
        return;
    }

    NarBuf b = { 0 };
    nb_cstr(&b, "nix-archive-1");
    nar_dump(&b, path, 0);

    uint8_t nar_hash[32];
    if (asnix_hash_sha256(b.buf, b.len, nar_hash) != 0) {
        fprintf(stderr, "asnix: failed to load libcrypto -- enter the flake devShell\n");
        exit(1);
    }
    free(b.buf);

    /* Real Nix's `baseNameOf` -- the last '/'-separated component,
     * same as every other `name` this project derives from a path
     * (e.g. M46's fetchurl-shaped fixed-output derivations use the
     * URL's own last segment). A trailing slash isn't expected here:
     * every path reaching this function was already canonicalized by
     * the parser/OP_PATH_CONCAT (M64) before being coerced. */
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;

    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < 32; i++) {
        hashstr[7 + 2 * i] = hexdigits[nar_hash[i] >> 4];
        hashstr[7 + 2 * i + 1] = hexdigits[nar_hash[i] & 0xf];
    }
    hashstr[7 + 64] = '\0';

    size_t preimage_cap = strlen("source") + strlen(hashstr) + strlen(ASNIX_STORE_DIR) + strlen(name) + 8;
    char *preimage = malloc(preimage_cap);
    size_t n = asnix_store_path_preimage("source", hashstr, name, preimage, preimage_cap);
    if (n == 0) {
        fprintf(stderr, "asnix: preimage too long copying '%s' to the store\n", path);
        exit(1);
    }
    uint8_t outer_digest[32];
    if (asnix_hash_sha256(preimage, n, outer_digest) != 0) {
        fprintf(stderr, "asnix: failed to load libcrypto -- enter the flake devShell\n");
        exit(1);
    }
    free(preimage);

    if (asnix_make_store_path_from_digest(outer_digest, name, out, out_cap) == 0) {
        fprintf(stderr, "asnix: computed store path too long copying '%s' to the store\n", path);
        exit(1);
    }

    copy_cache_insert(path, out);
}
