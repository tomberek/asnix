/* realpath() is POSIX, not C11 -- needs an explicit feature-test macro
 * under -std=c11 (see Makefile's CFLAGS). */
#define _DEFAULT_SOURCE

#include "eval_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "asnix/env.h"
#include "asnix/ir.h"
#include "builtins.h"
#include "builtins_attrs.h"
#include "builtins_list.h"
#include "builtins_m54.h"
#include "builtins_misc.h"
#include "builtins_shared.h"
#include "builtins_toml.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "symtab.h"

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

/* Resolves `path` to its final absolute form (following the `import
 * somedir` => `somedir/default.nix` rule) and reads+lexes+parses it,
 * writing the resolved path and its containing directory out through
 * `*resolved_out`/`*base_dir_out` (both caller-owned, malloc'd -- the
 * caller frees them once done with `base_dir_out`, which asnix_compile/
 * asnix_compile_scoped only reads during the one compile call). Shared by
 * asnix_eval_file and asnix_eval_file_scoped (M54) below, which differ
 * only in what root Env/ScopeFrame they build around the parsed Ast. */
static Ast *parse_file(const char *path, char **resolved_out, char **base_dir_out) {
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        fprintf(stderr, "asnix: cannot resolve path '%s'\n", path);
        exit(1);
    }
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
    *resolved_out = resolved;
    *base_dir_out = dir_of(resolved);

    char *src = read_file(resolved);
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    return asnix_parse(toks, ntoks);
}

/* Fresh root env/builtins every call -- an imported file does not inherit
 * the importing file's local scope (see eval_file.h). Slots 1-13
 * (`import`, `map`, `abort`, `toString`, `removeAttrs`, `isNull`, `throw`,
 * `derivation`, `baseNameOf`, `dirOf`, `fromTOML`, `placeholder`,
 * `scopedImport`) are the names real Nix exposes as bare globals in
 * addition to `builtins.X` -- every primop cppnix registers WITHOUT a
 * `__` prefix gets this treatment automatically (`EvalState::addPrimOp`,
 * `eval.cc`); see builtins.h's asnix_make_import_primop comment and
 * builtins_toml.c's asnix_make_fromTOML_primop comment for the fuller
 * picture (including real ones not yet implemented at all:
 * `fetchTarball`/`fetchGit`/`fetchMercurial`/`fetchTree`/
 * `derivationStrict`/`break`). compiler.c's root ScopeFrame
 * (init_builtins_frame) must list the same names in the same order. `up`
 * is the caller's choice (NULL for the ordinary case, or scopedImport's
 * own extra scope-attrs frame). */
static Env *make_root_env(Env *up) {
    Env *root_env = asnix_env_alloc(up, 14);
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
    root_env->values[12] = asnix_make_placeholder_primop();
    root_env->values[13] = asnix_make_scopedImport_primop();
    return root_env;
}

Value asnix_eval_file(const char *path) {
    char *resolved, *base_dir;
    Ast *ast = parse_file(path, &resolved, &base_dir);
    IrProgram *prog = asnix_compile(ast, base_dir);
    Env *root_env = make_root_env(NULL);
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
 * order its names were listed) wrapped around the ordinary 13-slot root
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

    IrProgram *prog = asnix_compile_scoped(ast, base_dir, names, lens, (size_t)n);

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
