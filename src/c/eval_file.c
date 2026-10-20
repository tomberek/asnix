/* realpath() is POSIX, not C11 -- needs an explicit feature-test macro
 * under -std=c11 (see Makefile's CFLAGS). */
#define _DEFAULT_SOURCE

#include "eval_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/env.h"
#include "asnix/ir.h"
#include "builtins.h"
#include "builtins_list.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "asnix: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
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

Value asnix_eval_file(const char *path) {
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        fprintf(stderr, "asnix: cannot resolve path '%s'\n", path);
        exit(1);
    }
    char *base_dir = dir_of(resolved);

    char *src = read_file(resolved);
    size_t ntoks;
    Token *toks = asnix_lex(src, &ntoks);
    Ast *ast = asnix_parse(toks, ntoks);
    IrProgram *prog = asnix_compile(ast, base_dir);

    /* Fresh root env/builtins every call -- an imported file does not
     * inherit the importing file's local scope (see eval_file.h). Slots
     * 1-4 (`import`, `map`, `abort`, `toString`) are four of the small
     * set of names real Nix exposes as bare globals in addition to
     * `builtins.X` -- see builtins.h's asnix_make_import_primop comment
     * for the full list checked; compiler.c's root ScopeFrame must list
     * the same names in the same order. */
    Env *root_env = asnix_env_alloc(NULL, 5);
    root_env->values[0] = asnix_make_builtins_attrs();
    root_env->values[1] = asnix_make_import_primop();
    root_env->values[2] = asnix_make_map_primop();
    root_env->values[3] = asnix_make_abort_primop();
    root_env->values[4] = asnix_make_toString_primop();

    Value result = asnix_eval(prog, root_env);

    free(resolved);
    free(base_dir);
    return result;
}
