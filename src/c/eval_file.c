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
#include "builtins_list.h"
#include "builtins_misc.h"
#include "builtins_toml.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "value_util.h" /* asnix_canon_path, M57h symlink fix */

static char *read_file(const char *path) {
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
    return buf;
}

/* True iff `path` names a directory (not a regular file). */
static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Directory part of an absolute path (as produced by realpath(), so
 * always starts with '/' and has no trailing slash itself). "/foo.nix"
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

/* M57i: factored out of asnix_eval_file so `--arg name EXPR` (main.c)
 * can run the exact same lex/parse/compile/fresh-root-env/eval pipeline
 * over a CLI-supplied expression string, not just a file's contents --
 * every root global (`import`, `builtins`, ...) an ordinary top-level
 * file sees must be visible to a `--arg` expression too (real Nix's own
 * `--arg`/`--argstr` values are parsed as ordinary top-level
 * expressions). `base_dir` is where THIS source's own relative path
 * literals resolve against -- the containing file's directory for
 * asnix_eval_file's call below, or the process's cwd for a `--arg`
 * expression (matching real Nix: confirmed `--arg x ./foo.nix` resolves
 * `./foo.nix` against the invoking shell's cwd, not any particular
 * file's directory, since a CLI expression has no file of its own). */
static Value eval_source(const char *src, const char *base_dir) {
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks);
    IrProgram *prog = asnix_compile(ast, base_dir);

    /* Fresh root env/builtins every call -- an imported file does not
     * inherit the importing file's local scope (see eval_file.h). Slots
     * 1-11 (`import`, `map`, `abort`, `toString`, `removeAttrs`, `isNull`,
     * `throw`, `derivation`, `baseNameOf`, `dirOf`, `fromTOML`) are the
     * names real Nix exposes as bare globals in addition to `builtins.X`
     * -- every primop cppnix registers WITHOUT a `__` prefix gets this
     * treatment automatically (`EvalState::addPrimOp`, `eval.cc`); see
     * builtins.h's asnix_make_import_primop comment and
     * builtins_toml.c's asnix_make_fromTOML_primop comment for the fuller
     * picture (including real ones not yet implemented at all:
     * `fetchTarball`/`fetchGit`/`fetchMercurial`/`fetchTree`/
     * `scopedImport`/`placeholder`/`derivationStrict`/`break`).
     * compiler.c's root ScopeFrame must list the same names in the same
     * order. */
    Env *root_env = asnix_env_alloc(NULL, 12);
    root_env->values[0] = asnix_make_builtins_attrs();
    root_env->values[1] = asnix_make_import_primop();
    root_env->values[2] = asnix_make_map_primop();
    root_env->values[3] = asnix_make_abort_primop();
    root_env->values[4] = asnix_make_toString_primop();
    root_env->values[5] = asnix_make_removeAttrs_primop();
    root_env->values[6] = asnix_make_isNull_primop();
    root_env->values[7] = asnix_make_throw_primop();
    root_env->values[8] = asnix_make_derivation_primop();
    root_env->values[9] = asnix_make_baseNameOf_primop();
    root_env->values[10] = asnix_make_dirOf_primop();
    root_env->values[11] = asnix_make_fromTOML_primop();

    return asnix_eval(prog, root_env);
}

Value asnix_eval_file(const char *path) {
    char *resolved = resolve_import_path(path);
    /* `import somedir` means `import somedir/default.nix` in real Nix
     * (verified against `nix-instantiate --eval`) -- found evaluating
     * real nixpkgs (`pkgs/top-level/impure.nix` does `import ./.`, i.e.
     * imports its own containing directory). Previously unhandled: this
     * directory path reached read_file() directly, which "succeeded"
     * opening it via fopen but then corrupted the heap computing its
     * size (see read_file's own hardening above) -- a real, serious
     * memory-safety bug, not a clean error. */
    if (is_directory(resolved)) {
        size_t len = strlen(resolved);
        char *with_default = malloc(len + strlen("/default.nix") + 1);
        memcpy(with_default, resolved, len);
        memcpy(with_default + len, "/default.nix", strlen("/default.nix") + 1);
        free(resolved);
        resolved = with_default;
    }
    char *base_dir = dir_of(resolved);
    char *src = read_file(resolved);

    Value result = eval_source(src, base_dir);

    free(resolved);
    free(base_dir);
    return result;
}

Value asnix_eval_expr_source(const char *src) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "asnix: cannot determine the current directory\n");
        exit(1);
    }
    return eval_source(src, cwd);
}
