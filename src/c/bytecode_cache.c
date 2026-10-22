#define _DEFAULT_SOURCE /* mkdir()/fork()/waitpid() -- same convention eval_file.c's own
                          * feature-test macro already establishes for this codebase */
#include "bytecode_cache.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ast.h"
#include "bytecode.h"
#include "hash_shim.h"

int asnix_bytecode_cache_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("ASNIX_BYTECODE_CACHE");
        cached = (v && strcmp(v, "1") == 0) ? 1 : 0;
    }
    return cached;
}

static int prefetch_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("ASNIX_BYTECODE_PRODUCER");
        cached = (v && strcmp(v, "1") == 0 && asnix_bytecode_cache_enabled()) ? 1 : 0;
    }
    return cached;
}

/* Arena-free: this file only ever runs small, one-shot, process-lifetime
 * allocations (cache directory path, hex key) -- plain malloc, freed
 * locally, same convention nar.c's own store-copy helpers use for
 * their own scratch buffers. */
static char *cache_dir(void) {
    const char *override = getenv("ASNIX_BYTECODE_CACHE_DIR");
    if (override) return strdup(override);
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    size_t n = strlen(home) + strlen("/.cache/asnix/bytecode") + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/.cache/asnix/bytecode", home);
    return p;
}

static int errno_is_exist(void) {
    return errno == EEXIST;
}

static int mkdir_p(const char *path) {
    char *copy = strdup(path);
    size_t len = strlen(copy);
    for (size_t i = 1; i < len; i++) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            mkdir(copy, 0755); /* ignore EEXIST and any other failure here --
                                 * the final mkdir below is the one whose
                                 * result actually matters */
            copy[i] = '/';
        }
    }
    int rc = mkdir(copy, 0755);
    free(copy);
    return (rc == 0 || errno_is_exist()) ? 0 : -1;
}

/* Key = sha256(resolved_path, '\0', source bytes), hex-encoded -- see
 * bytecode_cache.h's header comment for why the path must be part of
 * the key, not just content. */
static void cache_key_hex(const char *resolved_path, const char *source, size_t source_len, char out_hex[64]) {
    size_t path_len = strlen(resolved_path);
    size_t buf_len = path_len + 1 + source_len;
    char *buf = malloc(buf_len);
    memcpy(buf, resolved_path, path_len);
    buf[path_len] = '\0';
    memcpy(buf + path_len + 1, source, source_len);
    unsigned char digest[ASNIX_HASH_SHA256_LEN];
    int64_t rc = asnix_hash_sha256(buf, buf_len, digest);
    free(buf);
    if (rc != 0) {
        memset(out_hex, '0', 64);
        return;
    }
    asnix_hash_to_hex(digest, ASNIX_HASH_SHA256_LEN, out_hex);
}

static char *cache_file_path(const char *resolved_path, const char *source, size_t source_len) {
    char hex[64];
    cache_key_hex(resolved_path, source, source_len, hex);
    char *dir = cache_dir();
    size_t n = strlen(dir) + 1 + 2 + 1 + 64 + 4 + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/%.2s/%.64s.abc", dir, hex, hex);
    free(dir);
    return p;
}

IrProgram *asnix_bytecode_cache_lookup(const char *resolved_path, const char *source, size_t source_len) {
    if (!asnix_bytecode_cache_enabled()) return NULL;
    char *path = cache_file_path(resolved_path, source, source_len);
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    IrProgram *prog = asnix_bytecode_deserialize(buf, (size_t)sz);
    free(buf);
    return prog; /* NULL on any anomaly -- asnix_bytecode_deserialize's own contract */
}

void asnix_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, IrProgram *prog) {
    if (!asnix_bytecode_cache_enabled()) return;
    uint8_t *buf;
    size_t len;
    if (asnix_bytecode_serialize(prog, &buf, &len) != 0) return; /* refused -- some
                                                                    * construct this
                                                                    * format doesn't
                                                                    * support; just
                                                                    * skip caching it */
    char *path = cache_file_path(resolved_path, source, source_len);
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(path);
        *slash = '/';
    }
    /* Write-to-temp-then-rename: atomic from any concurrent reader's
     * point of view (including the fork-based prefetch producer and the
     * main process racing on the same path) -- a reader either sees the
     * old file (ENOENT before the first write ever happens) or the
     * fully-written new one, never a half-written one. */
    size_t tmp_len = strlen(path) + 32;
    char *tmp = malloc(tmp_len);
    snprintf(tmp, tmp_len, "%s.tmp%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (f) {
        size_t wrote = fwrite(buf, 1, len, f);
        fclose(f);
        if (wrote == len) rename(tmp, path);
        else unlink(tmp);
    }
    free(tmp);
    free(path);
    free(buf);
}

/* ---------------------------------------------------------------------
 * M92: fork-based prefetch producer -- see bytecode_cache.h's own
 * header comment for why this is a process, not a thread.
 * --------------------------------------------------------------------- */
#define MAX_OUTSTANDING 64
typedef struct {
    char *path;
    pid_t pid;
} Outstanding;
static Outstanding g_outstanding[MAX_OUTSTANDING];
static int g_outstanding_count = 0;

/* Exported by eval_file.c -- resolves, reads, lexes, parses, and
 * compiles `path` exactly like asnix_eval_file would, then stores the
 * result via asnix_bytecode_cache_store, all self-contained there since
 * eval_file.c already has every static helper this needs in scope. Does
 * nothing (silently) on any failure -- a prefetch child that can't
 * compile its target just exits having cached nothing, identical to
 * never having been forked at all from the consumer's point of view. */
void asnix_prefetch_compile_and_cache(const char *path);

static int resolve_literal_target(Ast *arg, const char *base_dir, char *out, size_t out_cap) {
    if (arg->kind != AST_PATH) return -1;
    if (arg->as.path.is_search || arg->as.path.interp_count > 0 || arg->as.path.is_home) return -1;
    if (arg->as.path.len == 0) return -1;
    if (arg->as.path.text[0] == '/') {
        if (arg->as.path.len >= out_cap) return -1;
        memcpy(out, arg->as.path.text, arg->as.path.len);
        out[arg->as.path.len] = '\0';
        return 0;
    }
    /* Relative -- an approximate join against base_dir (no ./.. collapsing,
     * no symlink following) is fine here: this path is used ONLY to
     * decide what to speculatively prefetch, never as the authoritative
     * cache key (eval_file.c's own resolve_file_path remains the sole
     * source of truth for that) -- see bytecode_cache.h's own comment on
     * why an inexact prefetch target is harmless, just a wasted guess. */
    int n = snprintf(out, out_cap, "%s/%.*s", base_dir, (int)arg->as.path.len, arg->as.path.text);
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

static void walk_for_imports(Ast *node, const char *base_dir) {
    if (!node || g_outstanding_count >= MAX_OUTSTANDING) return;
    if (node->kind == AST_APP && node->as.app.fn->kind == AST_VAR && node->as.app.fn->as.var.name_len == 6 &&
        memcmp(node->as.app.fn->as.var.name, "import", 6) == 0) {
        char target[4096];
        if (resolve_literal_target(node->as.app.arg, base_dir, target, sizeof target) == 0) {
            int already = 0;
            for (int i = 0; i < g_outstanding_count; i++) {
                if (strcmp(g_outstanding[i].path, target) == 0) already = 1;
            }
            if (!already) {
                pid_t pid = fork();
                if (pid == 0) {
                    asnix_prefetch_compile_and_cache(target);
                    _exit(0);
                } else if (pid > 0) {
                    g_outstanding[g_outstanding_count].path = strdup(target);
                    g_outstanding[g_outstanding_count].pid = pid;
                    g_outstanding_count++;
                }
                /* fork() < 0 (resource limits): silently skip prefetching
                 * this one target -- the consumer's own ordinary
                 * synchronous fallback covers it exactly as if
                 * prefetching had never been attempted. */
            }
        }
    }
    /* Recurse into every Ast* field that can itself contain a nested
     * `import` call -- deliberately conservative/shallow (does not
     * walk into lambda/thunk bodies that would need full scope
     * tracking to do perfectly); missing a deeply-nested static import
     * only means a missed prefetch opportunity, never a correctness
     * issue (see this file's header comment). */
    switch (node->kind) {
        case AST_APP:
            walk_for_imports(node->as.app.fn, base_dir);
            walk_for_imports(node->as.app.arg, base_dir);
            return;
        case AST_LET:
            for (size_t i = 0; i < node->as.let1.count; i++) walk_for_imports(node->as.let1.bindings[i].value, base_dir);
            walk_for_imports(node->as.let1.body, base_dir);
            return;
        case AST_IF:
            walk_for_imports(node->as.if1.cond, base_dir);
            walk_for_imports(node->as.if1.then_, base_dir);
            walk_for_imports(node->as.if1.else_, base_dir);
            return;
        case AST_ATTRSET:
            for (size_t i = 0; i < node->as.attrset.count; i++) walk_for_imports(node->as.attrset.bindings[i].value, base_dir);
            return;
        case AST_LIST:
            for (size_t i = 0; i < node->as.list.count; i++) walk_for_imports(node->as.list.elems[i], base_dir);
            return;
        default:
            return; /* AST_VAR/AST_INT/.../AST_LAMBDA body etc: not followed --
                      * conservative on purpose, see comment above */
    }
}

void asnix_bytecode_prefetch_imports(Ast *root, const char *base_dir) {
    if (!prefetch_enabled()) return;
    walk_for_imports(root, base_dir);
}

void asnix_bytecode_prefetch_wait(const char *resolved_path) {
    if (!prefetch_enabled()) return;
    for (int i = 0; i < g_outstanding_count; i++) {
        if (strcmp(g_outstanding[i].path, resolved_path) == 0) {
            int status;
            waitpid(g_outstanding[i].pid, &status, 0);
            free(g_outstanding[i].path);
            g_outstanding[i] = g_outstanding[--g_outstanding_count];
            return;
        }
    }
    /* Opportunistically reap anything else that already finished, so a
     * long-running process (unusual for this CLI, but cheap to handle)
     * doesn't accumulate zombies while waiting on unrelated paths. */
    for (int i = 0; i < g_outstanding_count;) {
        int status;
        pid_t r = waitpid(g_outstanding[i].pid, &status, WNOHANG);
        if (r > 0) {
            free(g_outstanding[i].path);
            g_outstanding[i] = g_outstanding[--g_outstanding_count];
        } else {
            i++;
        }
    }
}
