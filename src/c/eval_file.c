/* getcwd() is POSIX, not C11 -- needs an explicit feature-test macro
 * under -std=c11 (see Makefile's CFLAGS). */
#define _DEFAULT_SOURCE

#include "eval_file.h"

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
#include "value_util.h" /* asnix_canon_path, M57h symlink fix */

/* `out_len` (M92: bytecode-cache key hashing needs the exact byte count,
 * not a strlen() guess that would silently misbehave on a file
 * containing an embedded NUL) may be NULL for callers that don't. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "asnix: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    /* ftell returns -1 on a stream that doesn't support seeking (e.g. a
     * directory opened via fopen, which "succeeds" on Linux but isn't a
     * regular file) -- (size_t)(-1)+1 wraps to 0, and the subsequent
     * `buf[size] = '\0'` with size still -1 writes one byte *before* a
     * ~0-byte allocation, corrupting the heap instead of erroring (found
     * importing a directory path with no default.nix handling -- see
     * asnix_eval_file's own fix for the real cause; this check is defense
     * in depth so a bad path can never reach this corruption again, even
     * from a future call site that isn't as careful). */
    if (size < 0) {
        fprintf(stderr, "asnix: cannot read '%s' (not a regular file?)\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)size;
    return buf;
}

/* Directory part of an absolute path (as produced by resolve_import_path,
 * so always starts with '/' and has no trailing slash itself). "/foo.nix"
 * -> "/" (the one case where the result legitimately keeps its slash). */
static char *dir_of(const char *abs_path) {
    const char *slash = strrchr(abs_path, '/');
    size_t dirlen = (size_t)(slash - abs_path);
    if (dirlen == 0) dirlen = 1;
    char *dir = malloc(dirlen + 1);
    memcpy(dir, abs_path, dirlen);
    dir[dirlen] = '\0';
    return dir;
}

/* M57h: makes `path` absolute (prepending the cwd if it's relative) and
 * lexically canonicalizes it (`.`/`..`/doubled slashes -- asnix_canon_path,
 * value_util.c), WITHOUT resolving symlinks on disk the way `realpath()`
 * does. This matters for relative imports reached *through* a symlinked
 * directory: `eval-okay-symlink-resolution.nix`'s fixture tree has
 * `symlink-resolution/foo/overlays -> ../overlays` (a symlink), and
 * `symlink-resolution/foo/overlays/overlay.nix` (the real file, reached
 * via that symlink) does `import ../lib`. Real Nix resolves that `..`
 * against the path AS WRITTEN/imported (`.../foo/overlays/overlay.nix`'s
 * own directory, `.../foo/overlays`) landing on `.../foo/lib` (which
 * exists) -- confirmed directly against `nix-instantiate --eval` on this
 * exact fixture tree. The previous `realpath()`-based approach instead
 * fully resolved the `foo/overlays` symlink FIRST, landing base_dir on
 * the symlink's physical TARGET directory (`symlink-resolution/overlays`,
 * since `overlays -> ../overlays`), so the same `../lib` instead resolved
 * to `symlink-resolution/lib`, which doesn't exist -- "asnix: cannot
 * resolve path" (actually a `is_directory`/`read_file` failure one level
 * down, since nothing here needs `realpath` to still successfully open
 * the file: `fopen`/`stat` already follow symlinks transparently at the
 * OS level regardless of whether *we* fully resolved the path ourselves). */
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

/* M82: real Nix's `resolveExprPath` (eval.cc) -- NOT to be confused with
 * M57h's `resolve_import_path` just above, which deliberately does NOT
 * resolve symlinks (needed so a relative import reached *through* a
 * symlinked DIRECTORY component resolves its own `../foo` against the
 * path as written, not the symlink's physical target -- see that
 * function's own comment). This is a narrower, different rule: real Nix
 * additionally follows a symlink chain on the exact literal path given to
 * `import` itself (the "last component", i.e. the whole path string
 * resolveExprPath was handed, as opposed to any symlinked directory
 * earlier in that path) -- `eval-okay-import-follows-symlink.nix`'s own
 * comment names the cppnix function directly. Confirmed against cppnix's
 * `resolveExprPath`: `while (lstat(path) is a symlink) path =
 * absPath(readlink(path), dirOf(path))`, i.e. the loop re-examines
 * whatever the symlink's target resolves to (itself possibly another
 * symlink, possibly reached via a *different* directory than the
 * original), which is exactly what a `link-imp.nix -> dir/imp.nix`
 * single-hop, or `link2.nix -> link-imp.nix -> dir/imp.nix` double-hop,
 * needs. Crucially, lstat()/readlink() only ever inspect the FINAL path
 * component (the OS itself transparently resolves any symlinked
 * directories earlier in the path when lstat() walks it) -- so a regular
 * file reached through a symlinked parent directory (M57h's own
 * `eval-okay-symlink-resolution.nix` fixture: `foo/overlays -> ../overlays`
 * is the symlink, but `overlay.nix` itself is a plain file) is NOT a
 * symlink itself, this loop's first lstat() sees S_ISLNK false
 * immediately, and `path` comes back completely unchanged -- so this is
 * additive, not a conflict with M57h's fix. `*is_dir_out` reports whether
 * the FINAL (post-symlink-chain) path is a directory, since the caller
 * (resolve_file_path) needs that anyway and re-stat'ing would be
 * redundant. */
static char *follow_symlink_chain(char *canon, int *is_dir_out) {
    struct stat st;
    for (int hops = 0; hops < 1024; hops++) {
        if (lstat(canon, &st) != 0) {
            /* Doesn't exist (or unreadable) -- let the ordinary
             * read_file()/is_directory() error paths downstream report
             * this the same way they always have; nothing more to
             * resolve. */
            *is_dir_out = 0;
            return canon;
        }
        if (!S_ISLNK(st.st_mode)) {
            *is_dir_out = S_ISDIR(st.st_mode);
            return canon;
        }
        char target[4096];
        ssize_t n = readlink(canon, target, sizeof(target) - 1);
        if (n < 0) {
            *is_dir_out = 0;
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

/* Resolves `path` to its final absolute form -- following the `import
 * somedir` => `somedir/default.nix` rule (verified against
 * `nix-instantiate --eval` -- found evaluating real nixpkgs,
 * `pkgs/top-level/impure.nix` does `import ./.`, i.e. imports its own
 * containing directory; previously unhandled: this directory path
 * reached read_file() directly, which "succeeded" opening it via fopen
 * but then corrupted the heap computing its size, see read_file's own
 * hardening above -- a real, serious memory-safety bug, not a clean
 * error) and M57h's symlink-preserving resolution above. Split out from
 * parse_file below (M65) so asnix_eval_file can check the import cache
 * BEFORE paying for a read+lex+parse+compile+eval it might not need. */
static char *resolve_file_path(const char *path) {
    char *resolved = resolve_import_path(path);
    int is_dir;
    resolved = follow_symlink_chain(resolved, &is_dir);
    if (is_dir) {
        size_t len = strlen(resolved);
        char *with_default = malloc(len + strlen("/default.nix") + 1);
        memcpy(with_default, resolved, len);
        memcpy(with_default + len, "/default.nix", strlen("/default.nix") + 1);
        free(resolved);
        resolved = with_default;
    }
    return resolved;
}

/* Reads+lexes+parses an ALREADY-RESOLVED path, writing its containing
 * directory out through `*base_dir_out` (caller-owned, malloc'd -- freed
 * once done with it, which asnix_compile/asnix_compile_scoped only read
 * during the one compile call). */
static Ast *parse_resolved_file(const char *resolved, char **base_dir_out) {
    *base_dir_out = dir_of(resolved);
    char *src = read_file(resolved, NULL);
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks, src);
    free(toks); /* M86 perf: asnix_parse copies everything it needs out of
                  * toks (names/strings point into src, not into this
                  * array) -- nothing retains it after this call, but it
                  * was never freed, leaking every file's whole token
                  * array for the life of the process. */
    return ast;
}

/* Shared by asnix_eval_file_scoped (M54) and the pre-M65 asnix_eval_file
 * shape -- resolves and parses in one step, also handing the resolved
 * path back to the caller. asnix_eval_file itself (below) no longer uses
 * this directly (it needs the resolved path BEFORE deciding whether to
 * parse at all, for the import cache), but asnix_eval_file_scoped still
 * does, since scopedImport results are intentionally never cached (each
 * call's `scope` argument can differ, so the same file path can
 * legitimately produce a different result per call -- caching by path
 * alone would be wrong there). */
static Ast *parse_file(const char *path, char **resolved_out, char **base_dir_out) {
    char *resolved = resolve_file_path(path);
    *resolved_out = resolved;
    return parse_resolved_file(resolved, base_dir_out);
}

/* M65: real Nix memoizes `import <path>` by the resolved path -- a
 * second `import` of the same file returns the exact same (possibly
 * still-lazy) Value, not a fresh independent evaluation, so the file's
 * own side effects (`builtins.trace`) and work happen at most once no
 * matter how many places import it. asnix had no such cache at all,
 * confirmed directly: `import X; import X;` (X containing `builtins.trace
 * "evaluated!" 1`) printed the trace line TWICE here vs. real Nix's ONCE.
 * Found chasing d.nix's own severe slowdown: nixpkgs' bootstrap chain
 * re-imports the same handful of files (`<nix/fetchurl.nix>`,
 * `lib/default.nix`, individual `pkgs/by-name` package.nix files
 * reached through more than one alias/overlay path) over and over,
 * each time paying a full independent read+lex+parse+compile+eval --
 * confirmed via `strace -e trace=newfstatat`, which showed the exact
 * same handful of paths being stat'd tens of thousands of times (NOT an
 * infinite loop -- the process still terminates, in ~1.5s without
 * strace's own massive per-syscall overhead -- just a severe, self-
 * inflicted amount of genuinely redundant work).
 *
 * A plain linear-scan array, not a hash table: the number of distinct
 * files a single process imports is small relative to the cost of each
 * entry (a full parse+compile+eval), so a linear strcmp scan's cost is
 * negligible next to what it's saving -- same "simple first" judgment
 * call as symtab.c's own choice to use a real hash table only once
 * *that* linear scan's cost (comparing every interned string) started
 * to matter at a much higher call frequency. Grows by doubling, same
 * convention as every other dynamic array in this codebase. */
typedef struct {
    char *path;
    Value value;
} ImportCacheEntry;
static ImportCacheEntry *g_import_cache = NULL;
static size_t g_import_cache_count = 0, g_import_cache_cap = 0;

static int import_cache_find(const char *resolved, Value *out) {
    for (size_t i = 0; i < g_import_cache_count; i++) {
        if (strcmp(g_import_cache[i].path, resolved) == 0) {
            *out = g_import_cache[i].value;
            return 1;
        }
    }
    return 0;
}

static void import_cache_insert(const char *resolved, Value v) {
    if (g_import_cache_count == g_import_cache_cap) {
        g_import_cache_cap = g_import_cache_cap ? g_import_cache_cap * 2 : 64;
        g_import_cache = realloc(g_import_cache, g_import_cache_cap * sizeof(ImportCacheEntry));
    }
    size_t len = strlen(resolved);
    char *copy = malloc(len + 1);
    memcpy(copy, resolved, len + 1);
    g_import_cache[g_import_cache_count].path = copy;
    g_import_cache[g_import_cache_count].value = v;
    g_import_cache_count++;
}

/* Fresh root env/builtins every call -- an imported file does not inherit
 * the importing file's local scope (see eval_file.h). Slots 1-15
 * (`import`, `map`, `abort`, `toString`, `removeAttrs`, `isNull`, `throw`,
 * `derivation`, `baseNameOf`, `dirOf`, `fromTOML`, `placeholder`,
 * `scopedImport`, `derivationStrict`, `fetchGit`) are the names real Nix
 * exposes as bare globals in addition to `builtins.X` -- every primop
 * cppnix registers WITHOUT a `__` prefix gets this treatment
 * automatically (`EvalState::addPrimOp`, `eval.cc`); see builtins.h's
 * asnix_make_import_primop comment, builtins_toml.c's
 * asnix_make_fromTOML_primop comment, and builtins_fetchgit.c's own
 * header comment (M82) for the fuller picture (including real ones
 * still not implemented at all: `fetchTarball`/`fetchMercurial`/
 * `fetchTree`/`break`). compiler.c's root ScopeFrame
 * (init_builtins_frame) must list the same names in the same order. `up`
 * is the caller's choice (NULL for the ordinary case, or scopedImport's
 * own extra scope-attrs frame). */
/* `skip_derivation_wrapper`: slot 8 (`derivation`) is normally
 * asnix_make_derivation_wrapper_value() (M58), which itself lexes,
 * parses, and evaluates a small embedded .nix file -- and THAT
 * evaluation calls back into this very function to build ITS OWN root
 * env, which would try to construct slot 8 again, forever. Passing 1
 * breaks that cycle by using the plain `derivationStrict` primop in
 * slot 8 instead for that one bootstrap evaluation; harmless, since the
 * wrapper source never references bare `derivation` itself, only
 * `derivationStrict` (slot 14). The ordinary path (every other caller)
 * passes 0 and gets the real wrapper, same as before this existed. */
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
    root_env->values[14] = asnix_make_derivation_strict_primop(); /* M58: derivationStrict, newly bare-global */
    root_env->values[15] = asnix_make_fetchGit_primop(); /* M82: fetchGit, newly bare-global */
    return root_env;
}

static Env *make_root_env(Env *up) { return make_root_env_ex(up, 0); }

/* M59: see eval_file.h's own comment on asnix_eval_top_level_file. Set
 * once (guarded), from a strdup'd copy -- NOT the `resolved` pointer
 * asnix_eval_file itself frees before returning -- so it stays valid for
 * the rest of the process, including during main.c's post-return
 * print_value(), which can still force thunks (and therefore still run
 * `unsafeGetAttrPos`) after asnix_eval_file's own `resolved`/`base_dir`
 * have already been freed. */
static char *g_top_level_file = NULL;

const char *asnix_eval_top_level_file(void) {
    return g_top_level_file;
}

Value asnix_eval_file(const char *path) {
    char *resolved = resolve_file_path(path);

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

    /* M92: give an in-flight background prefetch producer (see
     * bytecode_cache.h) a chance to finish before falling back to a
     * synchronous compile -- no-op if prefetching is disabled or no
     * producer was started for this exact path. */
    asnix_bytecode_prefetch_wait(resolved);

    char *base_dir = dir_of(resolved);
    size_t src_len;
    char *src = read_file(resolved, &src_len);

    IrProgram *prog = asnix_bytecode_cache_lookup(resolved, src, src_len);
    if (!prog) {
        size_t ntoks;
        Token *toks = asnix_lex(src, &ntoks);
        Ast *ast = asnix_parse(toks, ntoks, src);
        free(toks); /* M86 perf: see parse_resolved_file's identical free() comment */
        prog = asnix_compile(ast, base_dir, resolved);
        asnix_bytecode_cache_store(resolved, src, src_len, prog);
        asnix_bytecode_prefetch_imports(ast, base_dir);
    }
    Env *root_env = make_root_env(NULL);
    Value result = asnix_eval(prog, root_env);
    import_cache_insert(resolved, result);
    free(resolved);
    free(base_dir);
    return result;
}

/* M92: the fork()'d bytecode-prefetch producer's entire job (see
 * bytecode_cache.h's own comment on asnix_bytecode_prefetch_imports for
 * why this runs in a separate PROCESS, not a thread, and why that makes
 * this function safe to just call the normal, unmodified resolve/read/
 * lex/parse/compile pipeline with zero synchronization). Deliberately
 * does NOT touch g_import_cache/g_top_level_file or call asnix_eval --
 * this is cache-WARMING only, never evaluation; the only observable
 * effect on anything outside this one forked process is whatever
 * asnix_bytecode_cache_store writes to disk, read back later by the
 * parent's own ordinary asnix_bytecode_cache_lookup call. Any failure
 * (file doesn't exist, parse/compile error, cache store refused) just
 * means this child exits having cached nothing -- identical, from the
 * parent's point of view, to never having been forked at all. */
void asnix_prefetch_compile_and_cache(const char *path) {
    char *resolved = resolve_file_path(path);
    char *base_dir = dir_of(resolved);
    size_t src_len;
    char *src = read_file(resolved, &src_len);
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks, src);
    free(toks);
    IrProgram *prog = asnix_compile(ast, base_dir, resolved);
    asnix_bytecode_cache_store(resolved, src, src_len, prog);
}

/* M58: the one caller that needs make_root_env_ex's cycle-breaking
 * `skip_derivation_wrapper=1` path -- see that function's comment. */
Value asnix_eval_file_for_derivation_bootstrap(const char *path) {
    char *resolved, *base_dir;
    Ast *ast = parse_file(path, &resolved, &base_dir);
    IrProgram *prog = asnix_compile(ast, base_dir, resolved);
    Env *root_env = make_root_env_ex(NULL, 1);
    Value result = asnix_eval(prog, root_env);
    free(resolved);
    free(base_dir);
    return result;
}

/* M54: see eval_file.h/compiler.c's asnix_compile_scoped for the full
 * rationale. `scope`'s attribute NAMES (not values -- those stay lazy,
 * same as any other attrset field) become an extra, innermost lexical
 * ScopeFrame, resolved at compile time; at runtime that frame is one more
 * Env hop (`scope_env`, holding `scope`'s own field values in the same
 * order its names were listed) wrapped around the ordinary 14-slot root
 * env (`up = scope_env`'s own `up`). */
Value asnix_eval_file_scoped(const char *path, Value scope) {
    char *resolved, *base_dir;
    Ast *ast = parse_file(path, &resolved, &base_dir);

    int64_t n = asnix_bshared_attrs_count(scope);
    const char **names = malloc((size_t)(n > 0 ? n : 1) * sizeof(const char *));
    size_t *lens = malloc((size_t)(n > 0 ? n : 1) * sizeof(size_t));
    for (int64_t i = 0; i < n; i++) {
        uint32_t sym = asnix_bshared_attrs_sym(scope, i);
        names[i] = asnix_sym_name(sym, &lens[i]);
    }

    IrProgram *prog = asnix_compile_scoped(ast, base_dir, resolved, names, lens, (size_t)n);

    Env *root_env = make_root_env(NULL);
    Env *scope_env = asnix_env_alloc(root_env, n);
    for (int64_t i = 0; i < n; i++) scope_env->values[i] = asnix_bshared_attrs_value(scope, i);

    Value result = asnix_eval(prog, scope_env);

    free(names);
    free(lens);
    free(resolved);
    free(base_dir);
    return result;
}

/* M57i: CLI `--arg`/`--argstr` expression evaluation (main.c) -- the same
 * lex/parse/compile/fresh-root-env/eval pipeline `asnix_eval_file` runs
 * over a file's contents, but over a caller-supplied source STRING with
 * no file of its own; relative path literals in `src` resolve against the
 * process's cwd (confirmed: real Nix's own `--arg x ./foo.nix` resolves
 * `./foo.nix` against the invoking shell's cwd, not any particular
 * file's directory). */
Value asnix_eval_expr_source(const char *src) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "asnix: cannot determine the current directory\n");
        exit(1);
    }
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks, src);
    free(toks); /* M86 perf: see parse_resolved_file's identical free() comment */
    IrProgram *prog = asnix_compile(ast, cwd, NULL);
    Env *root_env = make_root_env(NULL);
    return asnix_eval(prog, root_env);
}
