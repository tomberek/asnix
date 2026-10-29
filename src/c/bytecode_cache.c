#define _DEFAULT_SOURCE
#include "bytecode_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <liburing.h>

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

static char *cache_dir(void) {
    static char *cached = NULL;
    if (cached) return cached;
    const char *override = getenv("ASNIX_BYTECODE_CACHE_DIR");
    if (override) {
        cached = strdup(override);
        return cached;
    }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    size_t n = strlen(home) + strlen("/.cache/asnix/bytecode") + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/.cache/asnix/bytecode", home);
    cached = p;
    return cached;
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
            mkdir(copy, 0755);
            copy[i] = '/';
        }
    }
    int rc = mkdir(copy, 0755);
    free(copy);
    return (rc == 0 || errno_is_exist()) ? 0 : -1;
}

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
    return p;
}

#define CACHE_FORMAT_MAGIC "ANXCACH1"
#define CACHE_FORMAT_VERSION ((uint8_t)2)

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

#define CACHE_HEADER_LEN 72

static int parse_header_from_buf(const uint8_t *buf, size_t len, CacheHeader *h) {
    if (len < CACHE_HEADER_LEN) return -1;
    if (memcmp(buf, CACHE_FORMAT_MAGIC, 8) != 0) return -1;
    if (buf[8] != CACHE_FORMAT_VERSION) return -1;
    memcpy(&h->mtime_sec, buf + 16, sizeof h->mtime_sec);
    memcpy(&h->mtime_nsec, buf + 24, sizeof h->mtime_nsec);
    memcpy(&h->size, buf + 32, sizeof h->size);
    memcpy(h->content_hash, buf + 40, ASNIX_HASH_SHA256_LEN);
    return 0;
}

static const uint8_t *mmap_whole_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *out_len = (size_t)st.st_size;
    return (const uint8_t *)p;
}

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

IrProgram *asnix_bytecode_cache_lookup_fast(const char *resolved_path, int64_t mtime_sec, int64_t mtime_nsec,
                                             int64_t size, int *out_entry_exists) {
    *out_entry_exists = 0;
    if (!asnix_bytecode_cache_enabled()) return NULL;
    ensure_stats_atexit();
    char *path = cache_file_path(resolved_path);
    size_t total_len;
    const uint8_t *map = mmap_whole_file(path, &total_len);
    free(path);
    if (!map) {
        g_stat_misses++;
        return NULL;
    }
    CacheHeader hdr;
    if (parse_header_from_buf(map, total_len, &hdr) != 0) {
        munmap((void *)map, total_len);
        g_stat_misses++;
        return NULL;
    }
    *out_entry_exists = 1;
    if (hdr.mtime_sec != mtime_sec || hdr.mtime_nsec != mtime_nsec || hdr.size != size) {
        munmap((void *)map, total_len);
        return NULL;
    }
    const uint8_t *body = map + CACHE_HEADER_LEN;
    size_t body_len = total_len - CACHE_HEADER_LEN;
    IrProgram *prog = asnix_bytecode_deserialize(body, body_len);
    munmap((void *)map, total_len);
    if (prog) {
        g_stat_hits++;
        g_stat_fast_hits++;
    } else {
        g_stat_misses++;
    }
    return prog;
}

IrProgram *asnix_bytecode_cache_lookup_verify(const char *resolved_path, const char *source, size_t source_len,
                                               int64_t mtime_sec, int64_t mtime_nsec, int64_t size) {
    if (!asnix_bytecode_cache_enabled()) return NULL;
    ensure_stats_atexit();
    char *path = cache_file_path(resolved_path);
    size_t total_len;
    const uint8_t *map = mmap_whole_file(path, &total_len);
    if (!map) {
        free(path);
        g_stat_misses++;
        return NULL;
    }
    CacheHeader hdr;
    if (parse_header_from_buf(map, total_len, &hdr) != 0) {
        munmap((void *)map, total_len);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    unsigned char digest[ASNIX_HASH_SHA256_LEN];
    if (asnix_hash_sha256(source, source_len, digest) != 0 || memcmp(digest, hdr.content_hash, ASNIX_HASH_SHA256_LEN) != 0) {
        munmap((void *)map, total_len);
        free(path);
        g_stat_misses++;
        return NULL;
    }
    const uint8_t *body = map + CACHE_HEADER_LEN;
    size_t body_len = total_len - CACHE_HEADER_LEN;
    IrProgram *prog = asnix_bytecode_deserialize(body, body_len);
    if (prog) {
        g_stat_hits++;
        g_stat_verify_hits++;
        CacheHeader fresh = hdr;
        fresh.mtime_sec = mtime_sec;
        fresh.mtime_nsec = mtime_nsec;
        fresh.size = size;
        write_entry_atomic(path, &fresh, body, body_len);
    } else {
        g_stat_misses++;
    }
    munmap((void *)map, total_len);
    free(path);
    return prog;
}

void asnix_bytecode_cache_store(const char *resolved_path, const char *source, size_t source_len, int64_t mtime_sec,
                                 int64_t mtime_nsec, int64_t size, IrProgram *prog) {
    if (!asnix_bytecode_cache_enabled()) return;
    ensure_stats_atexit();
    uint8_t *buf;
    size_t len;
    if (asnix_bytecode_serialize(prog, &buf, &len) != 0) return;
    CacheHeader h;
    h.mtime_sec = mtime_sec;
    h.mtime_nsec = mtime_nsec;
    h.size = size;
    if (asnix_hash_sha256(source, source_len, h.content_hash) != 0) {
        free(buf);
        return;
    }
    char *path = cache_file_path(resolved_path);
    write_entry_atomic(path, &h, buf, len);
    free(path);
    free(buf);
}

#define MAX_OUTSTANDING 256
#define ASNIX_PREFETCH_WORKERS 3
#define ASNIX_PREFETCH_MSG_MAX 4000
#define ASNIX_PREFETCH_INFLIGHT 4
#define ASNIX_PREFETCH_READ_BUF (256 * 1024)
#define ASNIX_PREFETCH_RING_DEPTH 8

static char *g_outstanding[MAX_OUTSTANDING];
static int g_outstanding_count = 0;

static int g_is_worker = 0;
static int g_worker_done_wfd = -1;

void asnix_prefetch_compile_and_cache(const char *path);
void asnix_prefetch_compile_and_cache_buf(const char *path, const char *src, size_t src_len);

static int prefetch_read_exact(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (r == 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

static int prefetch_read_message(int fd, char *out, size_t cap) {
    uint32_t len;
    if (!prefetch_read_exact(fd, &len, sizeof len)) return 0;
    if (len >= cap) return 0;
    if (!prefetch_read_exact(fd, out, len)) return 0;
    out[len] = '\0';
    return 1;
}

static int prefetch_write_message(int fd, const char *path) {
    size_t len = strlen(path);
    if (len >= ASNIX_PREFETCH_MSG_MAX - sizeof(uint32_t)) return 0;
    char buf[ASNIX_PREFETCH_MSG_MAX];
    uint32_t len32 = (uint32_t)len;
    memcpy(buf, &len32, sizeof len32);
    memcpy(buf + sizeof len32, path, len);
    ssize_t w = write(fd, buf, sizeof(len32) + len);
    return w == (ssize_t)(sizeof(len32) + len);
}

static int prefetch_write_tagged(int fd, char tag, const char *path) {
    size_t len = strlen(path);
    if (len >= ASNIX_PREFETCH_MSG_MAX - sizeof(uint32_t) - 1) return 0;
    char buf[ASNIX_PREFETCH_MSG_MAX];
    buf[0] = tag;
    uint32_t len32 = (uint32_t)len;
    memcpy(buf + 1, &len32, sizeof len32);
    memcpy(buf + 1 + sizeof len32, path, len);
    size_t total = 1 + sizeof(len32) + len;
    ssize_t w = write(fd, buf, total);
    return w == (ssize_t)total;
}

static int prefetch_read_tagged(int fd, char *out_tag, char *out_path, size_t cap) {
    char tag;
    if (!prefetch_read_exact(fd, &tag, 1)) return 0;
    uint32_t len;
    if (!prefetch_read_exact(fd, &len, sizeof len)) return 0;
    if (len >= cap) return 0;
    if (!prefetch_read_exact(fd, out_path, len)) return 0;
    out_path[len] = '\0';
    *out_tag = tag;
    return 1;
}

static void worker_main(int job_rfd, int done_wfd) {
    g_is_worker = 1;
    g_worker_done_wfd = done_wfd;

    struct io_uring ring;
    if (io_uring_queue_init(ASNIX_PREFETCH_RING_DEPTH, &ring, 0) != 0) {
        for (;;) {
            char path[ASNIX_PREFETCH_MSG_MAX];
            if (!prefetch_read_message(job_rfd, path, sizeof path)) break;
            asnix_prefetch_compile_and_cache(path);
            prefetch_write_tagged(done_wfd, 'D', path);
        }
        return;
    }

    char *bufs[ASNIX_PREFETCH_INFLIGHT];
    for (int i = 0; i < ASNIX_PREFETCH_INFLIGHT; i++) bufs[i] = malloc(ASNIX_PREFETCH_READ_BUF);
    struct {
        char path[ASNIX_PREFETCH_MSG_MAX];
        int fd;
        int active;
    } slots[ASNIX_PREFETCH_INFLIGHT];
    for (int i = 0; i < ASNIX_PREFETCH_INFLIGHT; i++) slots[i].active = 0;
    int eof_seen = 0;

    for (;;) {
        int any_active = 0;
        for (int i = 0; i < ASNIX_PREFETCH_INFLIGHT; i++) if (slots[i].active) any_active = 1;

        if (!eof_seen) {
            for (int i = 0; i < ASNIX_PREFETCH_INFLIGHT; i++) {
                if (slots[i].active) continue;
                int must_block = !any_active;
                if (!must_block) {
                    struct pollfd pfd;
                    pfd.fd = job_rfd;
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
                }
                if (!prefetch_read_message(job_rfd, slots[i].path, sizeof slots[i].path)) {
                    eof_seen = 1;
                    break;
                }
                int fd = open(slots[i].path, O_RDONLY);
                if (fd < 0) continue;
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                if (!sqe) {
                    close(fd);
                    asnix_prefetch_compile_and_cache(slots[i].path);
                    prefetch_write_tagged(done_wfd, 'D', slots[i].path);
                    continue;
                }
                io_uring_prep_read(sqe, fd, bufs[i], ASNIX_PREFETCH_READ_BUF, 0);
                sqe->user_data = (uint64_t)i;
                slots[i].fd = fd;
                slots[i].active = 1;
                any_active = 1;
            }
        }

        if (!any_active) {
            if (eof_seen) break;
            continue;
        }

        io_uring_submit(&ring);
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) != 0) break;
        do {
            int i = (int)cqe->user_data;
            long long res = cqe->res;
            io_uring_cqe_seen(&ring, cqe);
            close(slots[i].fd);
            if (res < 0 || (size_t)res >= ASNIX_PREFETCH_READ_BUF) {
                asnix_prefetch_compile_and_cache(slots[i].path);
            } else {
                bufs[i][res] = '\0';
                asnix_prefetch_compile_and_cache_buf(slots[i].path, bufs[i], (size_t)res);
            }
            prefetch_write_tagged(done_wfd, 'D', slots[i].path);
            slots[i].active = 0;
        } while (io_uring_peek_cqe(&ring, &cqe) == 0);
    }

    for (int i = 0; i < ASNIX_PREFETCH_INFLIGHT; i++) free(bufs[i]);
    io_uring_queue_exit(&ring);
}

static int g_pool_state = 0;
static pid_t g_worker_pid[ASNIX_PREFETCH_WORKERS];
static int g_job_wfd[ASNIX_PREFETCH_WORKERS];
static int g_done_rfd = -1;
static int g_rr = 0;

static void prefetch_pool_shutdown(void) {
    for (int i = 0; i < ASNIX_PREFETCH_WORKERS; i++) {
        if (g_job_wfd[i] >= 0) {
            close(g_job_wfd[i]);
            g_job_wfd[i] = -1;
        }
    }
    for (int i = 0; i < ASNIX_PREFETCH_WORKERS; i++) {
        if (g_worker_pid[i] > 0) {
            int status;
            waitpid(g_worker_pid[i], &status, 0);
            g_worker_pid[i] = -1;
        }
    }
    if (g_done_rfd >= 0) {
        close(g_done_rfd);
        g_done_rfd = -1;
    }
}

static void ensure_pool(void) {
    if (g_pool_state != 0) return;
    g_pool_state = -1;
    int done_pipe[2];
    if (pipe(done_pipe) != 0) return;
    int job_pipe[ASNIX_PREFETCH_WORKERS][2];
    for (int i = 0; i < ASNIX_PREFETCH_WORKERS; i++) {
        if (pipe(job_pipe[i]) != 0) {
            job_pipe[i][0] = -1;
            job_pipe[i][1] = -1;
            continue;
        }
        fcntl(job_pipe[i][1], F_SETFL, O_NONBLOCK);
    }
    for (int i = 0; i < ASNIX_PREFETCH_WORKERS; i++) {
        if (job_pipe[i][0] < 0) {
            g_worker_pid[i] = -1;
            g_job_wfd[i] = -1;
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            for (int k = 0; k < ASNIX_PREFETCH_WORKERS; k++) {
                if (k == i) {
                    if (job_pipe[k][1] >= 0) close(job_pipe[k][1]);
                } else {
                    if (job_pipe[k][0] >= 0) close(job_pipe[k][0]);
                    if (job_pipe[k][1] >= 0) close(job_pipe[k][1]);
                }
            }
            close(done_pipe[0]);
            worker_main(job_pipe[i][0], done_pipe[1]);
            _exit(0);
        } else if (pid > 0) {
            close(job_pipe[i][0]);
            g_job_wfd[i] = job_pipe[i][1];
            g_worker_pid[i] = pid;
        } else {
            close(job_pipe[i][0]);
            close(job_pipe[i][1]);
            g_job_wfd[i] = -1;
            g_worker_pid[i] = -1;
        }
    }
    close(done_pipe[1]);
    g_done_rfd = done_pipe[0];
    int any_worker = 0;
    for (int i = 0; i < ASNIX_PREFETCH_WORKERS; i++) if (g_job_wfd[i] >= 0) any_worker = 1;
    if (!any_worker) {
        close(g_done_rfd);
        g_done_rfd = -1;
        return;
    }
    atexit(prefetch_pool_shutdown);
    g_pool_state = 1;
}

static int prefetch_outstanding_contains(const char *path) {
    for (int i = 0; i < g_outstanding_count; i++) {
        if (strcmp(g_outstanding[i], path) == 0) return 1;
    }
    return 0;
}

static void prefetch_outstanding_remove(const char *path) {
    for (int i = 0; i < g_outstanding_count; i++) {
        if (strcmp(g_outstanding[i], path) == 0) {
            free(g_outstanding[i]);
            g_outstanding[i] = g_outstanding[--g_outstanding_count];
            return;
        }
    }
}

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
    int n = snprintf(out, out_cap, "%s/%.*s", base_dir, (int)arg->as.path.len, arg->as.path.text);
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

static void prefetch_dispatch(const char *target) {
    if (g_pool_state != 1) return;
    if (g_outstanding_count >= MAX_OUTSTANDING) return;
    if (prefetch_outstanding_contains(target)) return;
    for (int tries = 0; tries < ASNIX_PREFETCH_WORKERS; tries++) {
        int wi = (g_rr + tries) % ASNIX_PREFETCH_WORKERS;
        if (g_job_wfd[wi] >= 0 && prefetch_write_message(g_job_wfd[wi], target)) {
            g_rr = (wi + 1) % ASNIX_PREFETCH_WORKERS;
            g_outstanding[g_outstanding_count++] = strdup(target);
            return;
        }
    }
}

static int prefetch_target_already_fresh(const char *target) {
    struct stat st;
    if (stat(target, &st) != 0) return 0;
    char *path = cache_file_path(target);
    size_t total_len;
    const uint8_t *map = mmap_whole_file(path, &total_len);
    free(path);
    if (!map) return 0;
    CacheHeader hdr;
    int fresh = 0;
    if (parse_header_from_buf(map, total_len, &hdr) == 0) {
        fresh = (hdr.mtime_sec == (int64_t)st.st_mtim.tv_sec && hdr.mtime_nsec == (int64_t)st.st_mtim.tv_nsec &&
                 hdr.size == (int64_t)st.st_size);
    }
    munmap((void *)map, total_len);
    return fresh;
}

static void try_prefetch_target(Ast *arg, const char *base_dir) {
    char target[4096];
    if (resolve_literal_target(arg, base_dir, target, sizeof target) != 0) return;
    if (prefetch_target_already_fresh(target)) return;
    if (g_is_worker) {
        prefetch_write_tagged(g_worker_done_wfd, 'F', target);
    } else {
        prefetch_dispatch(target);
    }
}

static void walk_for_imports(Ast *node, const char *base_dir) {
    if (!node) return;
    if (node->kind == AST_APP) {
        if (node->as.app.fn->kind == AST_VAR && node->as.app.fn->as.var.name_len == 6 &&
            memcmp(node->as.app.fn->as.var.name, "import", 6) == 0) {
            try_prefetch_target(node->as.app.arg, base_dir);
        } else if (node->as.app.fn->kind == AST_APP) {
            Ast *inner_fn = node->as.app.fn->as.app.fn;
            Ast *inner_arg = node->as.app.fn->as.app.arg;
            int is_call_package =
                (inner_fn->kind == AST_VAR && inner_fn->as.var.name_len == 11 &&
                 memcmp(inner_fn->as.var.name, "callPackage", 11) == 0) ||
                (inner_fn->kind == AST_SELECT && inner_fn->as.select.attr_expr == NULL &&
                 inner_fn->as.select.attr_len == 11 && memcmp(inner_fn->as.select.attr, "callPackage", 11) == 0);
            if (is_call_package) try_prefetch_target(inner_arg, base_dir);
        }
    }
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
            for (size_t i = 0; i < node->as.list.count; i++) {
                Ast *elem = node->as.list.elems[i];
                if (elem->kind == AST_PATH) try_prefetch_target(elem, base_dir);
                walk_for_imports(elem, base_dir);
            }
            return;
        default:
            return;
    }
}

void asnix_bytecode_prefetch_imports(Ast *root, const char *base_dir) {
    if (!prefetch_enabled()) return;
    if (!g_is_worker) {
        ensure_pool();
        if (g_pool_state != 1) return;
    }
    walk_for_imports(root, base_dir);
}

static void prefetch_drain_nonblocking(const char *want, int *found) {
    for (;;) {
        struct pollfd pfd;
        pfd.fd = g_done_rfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return;
        char tag, path[ASNIX_PREFETCH_MSG_MAX];
        if (!prefetch_read_tagged(g_done_rfd, &tag, path, sizeof path)) return;
        if (tag == 'D') {
            prefetch_outstanding_remove(path);
            if (found && want && strcmp(path, want) == 0) *found = 1;
        } else if (tag == 'F') {
            prefetch_dispatch(path);
        }
    }
}

void asnix_bytecode_prefetch_wait(const char *resolved_path) {
    if (!prefetch_enabled() || g_pool_state != 1) return;
    if (!prefetch_outstanding_contains(resolved_path)) {
        prefetch_drain_nonblocking(NULL, NULL);
        return;
    }
    int found = 0;
    prefetch_drain_nonblocking(resolved_path, &found);
    if (found) return;
    for (int tries = 0; tries < 10 && !found; tries++) {
        struct pollfd pfd;
        pfd.fd = g_done_rfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 5) <= 0) continue;
        char tag, path[ASNIX_PREFETCH_MSG_MAX];
        if (!prefetch_read_tagged(g_done_rfd, &tag, path, sizeof path)) break;
        if (tag == 'D') {
            prefetch_outstanding_remove(path);
            if (strcmp(path, resolved_path) == 0) found = 1;
        } else if (tag == 'F') {
            prefetch_dispatch(path);
        }
    }
}

