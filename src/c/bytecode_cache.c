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

/* M102 (Finding 1): the on-disk FILENAME key is now sha256(resolved_path)
 * alone, hex-encoded -- NOT (path, content) the way it was pre-M102. See
 * bytecode_cache.h's own header comment for why this is still safe
 * (two different paths never collide; freshness against whatever
 * content currently lives at this path is validated separately, by the
 * entry's own header -- see CacheHeader below -- not by the filename).
 * Making the filename path-only, rather than content-addressed, is what
 * lets asnix_bytecode_cache_lookup_fast locate and open the right file
 * WITHOUT first reading the source to compute a content hash -- the
 * entire point of this fix. A side benefit: a file that's edited
 * repeatedly now reuses (overwrites, via the same atomic rename every
 * store already does) the same single on-disk entry forever, instead of
 * leaving the old content-addressed blob orphaned on disk every time. */
static void path_hash_hex(const char *resolved_path, char out_hex[64]) {
    unsigned char digest[ASNIX_HASH_SHA256_LEN];
    int64_t rc = asnix_hash_sha256(resolved_path, strlen(resolved_path), digest);
    if (rc != 0) {
        memset(out_hex, '0', 64);
        return;
    }
    asnix_hash_to_hex(digest, ASNIX_HASH_SHA256_LEN, out_hex);
}

static char *cache_file_path(const char *resolved_path) {
    char hex[64];
    path_hash_hex(resolved_path, hex);
    char *dir = cache_dir();
    size_t n = strlen(dir) + 1 + 2 + 1 + 64 + 4 + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/%.2s/%.64s.abc", dir, hex, hex);
    free(dir);
    return p;
}

/* ---------------------------------------------------------------------
 * M102 (Finding 1): cache-entry header -- mtime+size (the cheap
 * freshness check) and a content hash (the fallback check), stored
 * ahead of the entry's bytecode.c-serialized body.
 *
 * Fixed-width, hand-rolled (NOT bytecode.c's own WBuf/varint machinery
 * -- this is the cache LAYER wrapping that format, not part of the
 * format itself, and at a constant 72 bytes/entry the fixed-width cost
 * is negligible next to bytecode.c's own M102/Finding-2 varint win on
 * the much larger, highly repetitive body that follows).
 *
 * `CACHE_FORMAT_MAGIC`/`CACHE_FORMAT_VERSION` are THIS layer's own
 * magic/version, independent of bytecode.c's own "ASNXBC02" magic
 * inside the body -- bumping either one on a future format change is
 * enough to make every old-format entry fail `read_header` cleanly and
 * degrade to a plain miss (M94's original design contract, re-verified
 * here: a pre-M102 entry has no header at all, so its first 8 bytes are
 * literally bytecode.c's own "ASNXBC01"/"ASNXBC02" magic, never
 * "ANXCACH1" -- read_header's memcmp rejects it immediately). */
#define CACHE_FORMAT_MAGIC "ANXCACH1"
#define CACHE_FORMAT_VERSION ((uint8_t)2) /* bumped alongside bytecode.c's ASNXBC01->02 (M102 Finding 2) */

typedef struct {
    int64_t mtime_sec, mtime_nsec, size;
    unsigned char content_hash[ASNIX_HASH_SHA256_LEN];
} CacheHeader;

static int write_header(FILE *f, const CacheHeader *h) {
    uint8_t pad[7] = { 0 };
    if (fwrite(CACHE_FORMAT_MAGIC, 1, 8, f) != 8) return -1;
    uint8_t ver = CACHE_FORMAT_VERSION;
    if (fwrite(&ver, 1, 1, f) != 1) return -1;
    if (fwrite(pad, 1, 7, f) != 7) return -1;
    if (fwrite(&h->mtime_sec, sizeof h->mtime_sec, 1, f) != 1) return -1;
    if (fwrite(&h->mtime_nsec, sizeof h->mtime_nsec, 1, f) != 1) return -1;
    if (fwrite(&h->size, sizeof h->size, 1, f) != 1) return -1;
    if (fwrite(h->content_hash, 1, ASNIX_HASH_SHA256_LEN, f) != ASNIX_HASH_SHA256_LEN) return -1;
    return 0;
}

static int read_header(FILE *f, CacheHeader *h) {
    char magic[8];
    uint8_t ver, pad[7];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, CACHE_FORMAT_MAGIC, 8) != 0) return -1;
    if (fread(&ver, 1, 1, f) != 1 || ver != CACHE_FORMAT_VERSION) return -1;
    if (fread(pad, 1, 7, f) != 7) return -1;
    if (fread(&h->mtime_sec, sizeof h->mtime_sec, 1, f) != 1) return -1;
    if (fread(&h->mtime_nsec, sizeof h->mtime_nsec, 1, f) != 1) return -1;
    if (fread(&h->size, sizeof h->size, 1, f) != 1) return -1;
    if (fread(h->content_hash, 1, ASNIX_HASH_SHA256_LEN, f) != ASNIX_HASH_SHA256_LEN) return -1;
    return 0;
}

/* Visibility: ASNIX_BYTECODE_CACHE_STATS=1 (on top of ASNIX_BYTECODE_CACHE=1)
 * prints a one-line hit/miss/store summary to stderr at process exit --
 * the cache is otherwise silent by design (a stale/corrupt entry
 * degrades to an ordinary miss with no visible symptom at all, per this
 * file's own header comment), so without this there was no way to tell
 * whether a given run actually benefited from the cache or was quietly
 * missing every time. Counters are plain process-lifetime globals (this
 * file already has several, e.g. g_outstanding) -- no synchronization
 * needed since the fork-based prefetch children never share these
 * counters with the parent (each process gets its own copy via fork(),
 * and a child's own counts die with it when it `_exit(0)`s, which is
 * fine: a prefetch child's store is still reflected in the PARENT's
 * stats the next time the parent itself looks that path up and hits). */
static int64_t g_stat_hits = 0, g_stat_fast_hits = 0, g_stat_verify_hits = 0, g_stat_misses = 0, g_stat_stores = 0;
static int g_stats_atexit_registered = 0;

static int stats_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("ASNIX_BYTECODE_CACHE_STATS");
        cached = (v && strcmp(v, "1") == 0) ? 1 : 0;
    }
    return cached;
}

static void print_stats_atexit(void) {
    int64_t total = g_stat_hits + g_stat_misses;
    double rate = total > 0 ? (100.0 * (double)g_stat_hits / (double)total) : 0.0;
    fprintf(stderr,
            "asnix: bytecode cache: %lld hits (%lld fast-path, %lld content-verified), %lld misses, %lld stores "
            "(%.1f%% hit rate)\n",
            (long long)g_stat_hits, (long long)g_stat_fast_hits, (long long)g_stat_verify_hits,
            (long long)g_stat_misses, (long long)g_stat_stores, rate);
}

static void ensure_stats_atexit(void) {
    if (!stats_enabled() || g_stats_atexit_registered) return;
    g_stats_atexit_registered = 1;
    atexit(print_stats_atexit);
}

/* Writes a complete entry (header + body) atomically via the same
 * write-temp-then-rename dance this cache has always used -- shared by
 * asnix_bytecode_cache_store itself and by
 * asnix_bytecode_cache_lookup_verify's own "self-heal" (re-stamp an
 * entry with fresh mtime/size once its content is confirmed unchanged,
 * so the FAST path starts working again next time instead of paying
 * the content-hash fallback on every single run). */
static void write_entry_atomic(const char *path, const CacheHeader *h, const uint8_t *body, size_t body_len) {
    char *dir_copy = strdup(path);
    char *slash = strrchr(dir_copy, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir_copy);
    }
    free(dir_copy);
    size_t tmp_len = strlen(path) + 32;
    char *tmp = malloc(tmp_len);
    snprintf(tmp, tmp_len, "%s.tmp%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = write_header(f, h) == 0;
        size_t wrote = ok ? fwrite(body, 1, body_len, f) : 0;
        fclose(f);
        if (ok && wrote == body_len) {
            rename(tmp, path);
            g_stat_stores++;
        } else {
            unlink(tmp);
        }
    }
    free(tmp);
}

/* M102 (Finding 1), step 1 -- see bytecode_cache.h's own doc comment on
 * this function for the full contract. No source-file I/O at all: just
 * one open+read of the cache entry itself (or, on a plain miss, not
 * even that beyond the failed open). */
IrProgram *asnix_bytecode_cache_lookup_fast(const char *resolved_path, int64_t mtime_sec, int64_t mtime_nsec,
                                             int64_t size, int *out_entry_exists) {
    *out_entry_exists = 0;
    if (!asnix_bytecode_cache_enabled()) return NULL;
    ensure_stats_atexit();
    char *path = cache_file_path(resolved_path);
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) {
        g_stat_misses++;
        return NULL;
    }
    CacheHeader hdr;
    if (read_header(f, &hdr) != 0) {
        fclose(f);
        g_stat_misses++;
        return NULL;
    }
    *out_entry_exists = 1;
    if (hdr.mtime_sec != mtime_sec || hdr.mtime_nsec != mtime_nsec || hdr.size != size) {
        /* Stat drifted -- ambiguous, not necessarily stale (could be a
         * touch(1) with unchanged content). Caller should try the
         * content-hash fallback (asnix_bytecode_cache_lookup_verify)
         * rather than treating this as an outright miss yet. */
        fclose(f);
        return NULL;
    }
    long body_start = ftell(f);
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        g_stat_misses++;
        return NULL;
    }
    long end = ftell(f);
    if (body_start < 0 || end < body_start) {
        fclose(f);
        g_stat_misses++;
        return NULL;
    }
    fseek(f, body_start, SEEK_SET);
    size_t body_len = (size_t)(end - body_start);
    uint8_t *buf = malloc(body_len > 0 ? body_len : 1);
    size_t got = fread(buf, 1, body_len, f);
    fclose(f);
    if (got != body_len) {
        free(buf);
        g_stat_misses++;
        return NULL;
    }
    IrProgram *prog = asnix_bytecode_deserialize(buf, body_len);
    free(buf);
    if (prog) {
        g_stat_hits++;
        g_stat_fast_hits++;
    } else {
        g_stat_misses++;
    }
    return prog; /* NULL on any anomaly -- asnix_bytecode_deserialize's own contract */
}

/* M102 (Finding 1), step 2 -- only called by eval_file.c when step 1
 * above found an entry whose mtime/size didn't match. Needs the actual
 * source bytes (the caller reads these anyway on the way to an ordinary
 * cache-miss recompile, so this never adds a source-file open that
 * wouldn't already have happened) to compute a real content hash and
 * compare it against the one recorded in the SAME entry step 1 already
 * found. See bytecode_cache.h's own doc comment for the full contract
 * and the mtime+size-as-heuristic tradeoff this accepts. */
IrProgram *asnix_bytecode_cache_lookup_verify(const char *resolved_path, const char *source, size_t source_len,
                                               int64_t mtime_sec, int64_t mtime_nsec, int64_t size) {
    if (!asnix_bytecode_cache_enabled()) return NULL;
    ensure_stats_atexit();
    char *path = cache_file_path(resolved_path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        free(path);
        g_stat_misses++;
        return NULL;
    }
    CacheHeader hdr;
    if (read_header(f, &hdr) != 0) {
        fclose(f);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    unsigned char digest[ASNIX_HASH_SHA256_LEN];
    if (asnix_hash_sha256(source, source_len, digest) != 0 || memcmp(digest, hdr.content_hash, ASNIX_HASH_SHA256_LEN) != 0) {
        /* Either couldn't hash, or content genuinely differs -- a real
         * miss; the caller will recompile and asnix_bytecode_cache_store
         * will overwrite this stale entry. */
        fclose(f);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    long body_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    if (body_start < 0 || end < body_start) {
        fclose(f);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    fseek(f, body_start, SEEK_SET);
    size_t body_len = (size_t)(end - body_start);
    uint8_t *buf = malloc(body_len > 0 ? body_len : 1);
    size_t got = fread(buf, 1, body_len, f);
    fclose(f);
    if (got != body_len) {
        free(buf);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    IrProgram *prog = asnix_bytecode_deserialize(buf, body_len);
    if (prog) {
        g_stat_hits++;
        g_stat_verify_hits++;
        /* Self-heal: content is confirmed unchanged, so re-stamp this
         * entry with the FRESH mtime/size the caller just stat'd --
         * restores the fast path for this exact path on the next
         * lookup, at the one-time cost of rewriting the entry (same
         * atomic write-temp-then-rename every store already pays). */
        CacheHeader fresh = hdr;
        fresh.mtime_sec = mtime_sec;
        fresh.mtime_nsec = mtime_nsec;
        fresh.size = size;
        write_entry_atomic(path, &fresh, buf, body_len);
    } else {
        g_stat_misses++;
    }
    free(buf);
    free(path);
    return prog;
}

void asnix_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, int64_t mtime_sec,
                                 int64_t mtime_nsec, int64_t size, IrProgram *prog) {
    if (!asnix_bytecode_cache_enabled()) return;
    ensure_stats_atexit();
    uint8_t *buf;
    size_t len;
    if (asnix_bytecode_serialize(prog, &buf, &len) != 0) return; /* refused -- some
                                                                    * construct this
                                                                    * format doesn't
                                                                    * support; just
                                                                    * skip caching it */
    CacheHeader h;
    h.mtime_sec = mtime_sec;
    h.mtime_nsec = mtime_nsec;
    h.size = size;
    if (asnix_hash_sha256(source, source_len, h.content_hash) != 0) {
        free(buf);
        return; /* couldn't hash -- skip caching this one rather than
                  * store an entry with an undefined content hash */
    }
    char *path = cache_file_path(resolved_path);
    write_entry_atomic(path, &h, buf, len);
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
