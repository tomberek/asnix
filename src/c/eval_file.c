#define _DEFAULT_SOURCE

#include "eval_file.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "asnix/env.h"
#include "asnix/ir.h"
#include "builtins.h"
#include "builtins_attrs.h"
#include "builtins_fetchgit.h"
#include "builtins_list.h"
#include "builtins_m54.h"
#include "builtins_misc.h"
#include "builtins_shared.h"
#include "builtins_toml.h"
#include "bytecode_cache.h"
#include "compiler.h"
#include "derivation.h"
#include "lexer.h"
#include "parser.h"
#include "symtab.h"
#include "value_util.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "asnix: cannot open '%s'\n", path);
        exit(1);
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "asnix: cannot read '%s' (not a regular file?)\n", path);
        exit(1);
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    size_t n = fread(buf, 1, size, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

static char *dir_of(const char *abs_path) {
    const char *slash = strrchr(abs_path, '/');
    size_t dirlen = (size_t)(slash - abs_path);
    if (dirlen == 0) dirlen = 1;
    char *dir = malloc(dirlen + 1);
    memcpy(dir, abs_path, dirlen);
    dir[dirlen] = '\0';
    return dir;
}

static char *resolve_import_path(const char *path) {
    char *abs;
    if (path[0] == '/') {
        abs = malloc(strlen(path) + 1);
        memcpy(abs, path, strlen(path) + 1);
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) {
            fprintf(stderr, "asnix: cannot determine the current directory\n");
            exit(1);
        }
        size_t cwd_len = strlen(cwd), path_len = strlen(path);
        abs = malloc(cwd_len + 1 + path_len + 1);
        memcpy(abs, cwd, cwd_len);
        abs[cwd_len] = '/';
        memcpy(abs + cwd_len + 1, path, path_len + 1);
    }
    char *canon = asnix_canon_path(abs);
    free(abs);
    return canon;
}

static char *follow_symlink_chain(char *canon, struct stat *out_st) {
    for (int hops = 0; hops < 1024; hops++) {
        if (lstat(canon, out_st) != 0) {
            memset(out_st, 0, sizeof *out_st);
            return canon;
        }
        if (!S_ISLNK(out_st->st_mode)) {
            return canon;
        }
        char target[4096];
        ssize_t n = readlink(canon, target, sizeof(target) - 1);
        if (n < 0) {
            memset(out_st, 0, sizeof *out_st);
            return canon;
        }
        target[n] = '\0';
        char *dir = dir_of(canon);
        char *joined;
        if (target[0] == '/') {
            joined = malloc((size_t)n + 1);
            memcpy(joined, target, (size_t)n + 1);
        } else {
            size_t dir_len = strlen(dir);
            joined = malloc(dir_len + 1 + (size_t)n + 1);
            memcpy(joined, dir, dir_len);
            joined[dir_len] = '/';
            memcpy(joined + dir_len + 1, target, (size_t)n + 1);
        }
        free(dir);
        free(canon);
        canon = asnix_canon_path(joined);
        free(joined);
    }
    fprintf(stderr, "asnix: too many levels of symbolic links in path '%s'\n", canon);
    exit(1);
}

static char *resolve_file_path(const char *path, struct stat *out_st) {
    char *resolved = resolve_import_path(path);
    resolved = follow_symlink_chain(resolved, out_st);
    if (S_ISDIR(out_st->st_mode)) {
        size_t len = strlen(resolved);
        char *with_default = malloc(len + strlen("/default.nix") + 1);
        memcpy(with_default, resolved, len);
        memcpy(with_default + len, "/default.nix", strlen("/default.nix") + 1);
        free(resolved);
        resolved = with_default;
        if (asnix_bytecode_cache_enabled()) {
            if (stat(resolved, out_st) != 0) memset(out_st, 0, sizeof *out_st);
        }
    }
    return resolved;
}

static Ast *parse_resolved_file(const char *resolved, char **base_dir_out, char **src_out) {
    *base_dir_out = dir_of(resolved);
    char *src = read_file(resolved, NULL);
    *src_out = src;
    Ast *ast = asnix_parse(src);
    return ast;
}

static Ast *parse_file(const char *path, char **resolved_out, char **base_dir_out, char **src_out) {
    struct stat st;
    char *resolved = resolve_file_path(path, &st);
    *resolved_out = resolved;
    return parse_resolved_file(resolved, base_dir_out, src_out);
}

typedef struct {
    char *path;
    Value value;
} ImportCacheEntry;
static ImportCacheEntry *g_import_cache = NULL;
static size_t g_import_cache_count = 0, g_import_cache_cap = 0;
static size_t *g_import_cache_index = NULL;
static size_t g_import_cache_index_cap = 0;
#define IMPORT_CACHE_EMPTY_SLOT ((size_t)-1)

static uint64_t import_cache_hash(const char *s, size_t len) {
    uint64_t h = 0;
    for (size_t i = 0; i < len; i++) {
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

static void import_cache_index_rehash(size_t new_cap) {
    size_t *new_table = malloc(new_cap * sizeof(size_t));
    for (size_t i = 0; i < new_cap; i++) new_table[i] = IMPORT_CACHE_EMPTY_SLOT;
    for (size_t i = 0; i < g_import_cache_count; i++) {
        size_t len = strlen(g_import_cache[i].path);
        size_t slot = import_cache_hash(g_import_cache[i].path, len) & (new_cap - 1);
        while (new_table[slot] != IMPORT_CACHE_EMPTY_SLOT) slot = (slot + 1) & (new_cap - 1);
        new_table[slot] = i;
    }
    free(g_import_cache_index);
    g_import_cache_index = new_table;
    g_import_cache_index_cap = new_cap;
}

static int import_cache_find(const char *resolved, Value *out) {
    if (g_import_cache_index_cap == 0) return 0;
    size_t len = strlen(resolved);
    size_t slot = import_cache_hash(resolved, len) & (g_import_cache_index_cap - 1);
    for (;;) {
        size_t idx = g_import_cache_index[slot];
        if (idx == IMPORT_CACHE_EMPTY_SLOT) return 0;
        if (strcmp(g_import_cache[idx].path, resolved) == 0) {
            *out = g_import_cache[idx].value;
            return 1;
        }
        slot = (slot + 1) & (g_import_cache_index_cap - 1);
    }
}

static void import_cache_insert(const char *resolved, Value v) {
    if (g_import_cache_count == g_import_cache_cap) {
        g_import_cache_cap = g_import_cache_cap ? g_import_cache_cap * 2 : 64;
        g_import_cache = realloc(g_import_cache, g_import_cache_cap * sizeof(ImportCacheEntry));
    }
    if (g_import_cache_index_cap == 0) import_cache_index_rehash(128);
    else if ((g_import_cache_count + 1) * 4 > g_import_cache_index_cap * 3) import_cache_index_rehash(g_import_cache_index_cap * 2);
    size_t len = strlen(resolved);
    char *copy = malloc(len + 1);
    memcpy(copy, resolved, len + 1);
    g_import_cache[g_import_cache_count].path = copy;
    g_import_cache[g_import_cache_count].value = v;
    size_t slot = import_cache_hash(copy, len) & (g_import_cache_index_cap - 1);
    while (g_import_cache_index[slot] != IMPORT_CACHE_EMPTY_SLOT) slot = (slot + 1) & (g_import_cache_index_cap - 1);
    g_import_cache_index[slot] = g_import_cache_count;
    g_import_cache_count++;
}

static Env *make_root_env_ex(Env *up, int skip_derivation_wrapper) {
    Env *root_env = asnix_env_alloc(up, 16);
    root_env->values[0] = skip_derivation_wrapper ? asnix_make_builtins_attrs_for_derivation_bootstrap()
                                                   : asnix_make_builtins_attrs();
    root_env->values[1] = asnix_make_import_primop();
    root_env->values[2] = asnix_make_map_primop();
    root_env->values[3] = asnix_make_abort_primop();
    root_env->values[4] = asnix_make_toString_primop();
    root_env->values[5] = asnix_make_removeAttrs_primop();
    root_env->values[6] = asnix_make_isNull_primop();
    root_env->values[7] = asnix_make_throw_primop();
    root_env->values[8] = skip_derivation_wrapper ? asnix_make_derivation_strict_primop()
                                                   : asnix_make_derivation_wrapper_value();
    root_env->values[9] = asnix_make_baseNameOf_primop();
    root_env->values[10] = asnix_make_dirOf_primop();
    root_env->values[11] = asnix_make_fromTOML_primop();
    root_env->values[12] = asnix_make_placeholder_primop();
    root_env->values[13] = asnix_make_scopedImport_primop();
    root_env->values[14] = asnix_make_derivation_strict_primop();
    root_env->values[15] = asnix_make_fetchGit_primop();
    return root_env;
}

static Env *make_root_env(Env *up) { return make_root_env_ex(up, 0); }

static char *g_top_level_file = NULL;

const char *asnix_eval_top_level_file(void) {
    return g_top_level_file;
}

Value asnix_eval_file(const char *path) {
    struct stat st;
    char *resolved = resolve_file_path(path, &st);

    if (!g_top_level_file) {
        size_t len = strlen(resolved);
        g_top_level_file = malloc(len + 1);
        memcpy(g_top_level_file, resolved, len + 1);
    }

    Value cached;
    if (import_cache_find(resolved, &cached)) {
        free(resolved);
        return cached;
    }

    asnix_bytecode_prefetch_wait(resolved);

    int64_t mtime_sec = (int64_t)st.st_mtim.tv_sec;
    int64_t mtime_nsec = (int64_t)st.st_mtim.tv_nsec;
    int64_t fsize = (int64_t)st.st_size;
    int entry_exists = 0;
    IrProgram *prog = asnix_bytecode_cache_lookup_fast(resolved, mtime_sec, mtime_nsec, fsize, &entry_exists);

    char *base_dir = NULL;
    if (!prog) {
        base_dir = dir_of(resolved);
        size_t src_len;
        char *src = read_file(resolved, &src_len);

        if (entry_exists) {
            prog = asnix_bytecode_cache_lookup_verify(resolved, src, src_len, mtime_sec, mtime_nsec, fsize);
        }
        if (!prog) {
            Ast *ast = asnix_parse(src);
            prog = asnix_compile(ast, base_dir, resolved);
            asnix_bytecode_cache_store(resolved, src, src_len, mtime_sec, mtime_nsec, fsize, prog);
            asnix_bytecode_prefetch_imports(ast, base_dir);
            asnix_ast_arena_reset();
        }
        if (!asnix_lazy_thunk_enabled) free(src);
    }
    Env *root_env = make_root_env(NULL);
    Value result = asnix_eval(prog, root_env);
    import_cache_insert(resolved, result);
    if (!asnix_lazy_thunk_enabled) free(resolved);
    if (!asnix_lazy_thunk_enabled) free(base_dir);
    return result;
}

static void prefetch_compile_and_store(char *resolved, char *base_dir, const struct stat *st, const char *src,
                                        size_t src_len) {
    Ast *ast = asnix_parse(src);
    IrProgram *prog = asnix_compile(ast, base_dir, resolved);
    asnix_bytecode_cache_store(resolved, src, src_len, (int64_t)st->st_mtim.tv_sec, (int64_t)st->st_mtim.tv_nsec,
                                (int64_t)st->st_size, prog);
    asnix_bytecode_prefetch_imports(ast, base_dir);
    asnix_ast_arena_reset();
}

void asnix_prefetch_compile_and_cache(const char *path) {
    struct stat st;
    char *resolved = resolve_file_path(path, &st);
    char *base_dir = dir_of(resolved);
    size_t src_len;
    char *src = read_file(resolved, &src_len);
    prefetch_compile_and_store(resolved, base_dir, &st, src, src_len);
    if (!asnix_lazy_thunk_enabled) free(src);
    if (!asnix_lazy_thunk_enabled) free(base_dir);
    if (!asnix_lazy_thunk_enabled) free(resolved);
}

void asnix_prefetch_compile_and_cache_buf(const char *path, const char *src, size_t src_len) {
    struct stat st;
    char *resolved = resolve_file_path(path, &st);
    char *base_dir = dir_of(resolved);
    prefetch_compile_and_store(resolved, base_dir, &st, src, src_len);
    if (!asnix_lazy_thunk_enabled) free(base_dir);
    if (!asnix_lazy_thunk_enabled) free(resolved);
}

Value asnix_eval_file_for_derivation_bootstrap(const char *path) {
    char *resolved, *base_dir, *src;
    Ast *ast = parse_file(path, &resolved, &base_dir, &src);
    IrProgram *prog = asnix_compile(ast, base_dir, resolved);
    asnix_ast_arena_reset();
    if (!asnix_lazy_thunk_enabled) free(src);
    Env *root_env = make_root_env_ex(NULL, 1);
    Value result = asnix_eval(prog, root_env);
    if (!asnix_lazy_thunk_enabled) free(resolved);
    if (!asnix_lazy_thunk_enabled) free(base_dir);
    return result;
}

Value asnix_eval_file_scoped(const char *path, Value scope) {
    char *resolved, *base_dir, *src;
    Ast *ast = parse_file(path, &resolved, &base_dir, &src);

    int64_t n = asnix_bshared_attrs_count(scope);
    const char **names = malloc((size_t)(n > 0 ? n : 1) * sizeof(const char *));
    size_t *lens = malloc((size_t)(n > 0 ? n : 1) * sizeof(size_t));
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = asnix_bshared_attrs_sym(scope, i);
        names[i] = asnix_sym_name(sym, &lens[i]);
    }

    IrProgram *prog = asnix_compile_scoped(ast, base_dir, resolved, names, lens, (size_t)n);
    asnix_ast_arena_reset();
    if (!asnix_lazy_thunk_enabled) free(src);

    Env *root_env = make_root_env(NULL);
    Env *scope_env = asnix_env_alloc(root_env, n);
    for (int64_t i = 0; i < n; i++) scope_env->values[i] = asnix_bshared_attrs_value(scope, i);

    Value result = asnix_eval(prog, scope_env);

    free(names);
    free(lens);
    if (!asnix_lazy_thunk_enabled) free(resolved);
    if (!asnix_lazy_thunk_enabled) free(base_dir);
    return result;
}

Value asnix_eval_expr_source(const char *src) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "asnix: cannot determine the current directory\n");
        exit(1);
    }
    Ast *ast = asnix_parse(src);
    IrProgram *prog = asnix_compile(ast, cwd, NULL);
    asnix_ast_arena_reset();
    Env *root_env = make_root_env(NULL);
    return asnix_eval(prog, root_env);
}

