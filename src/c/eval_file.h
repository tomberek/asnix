#ifndef ASNIX_EVAL_FILE_H
#define ASNIX_EVAL_FILE_H

#include "asnix/value.h"

/* Runs the full lex -> parse -> compile -> eval pipeline on the .nix file
 * at `path` and returns the resulting Value. Used by both main() (the
 * top-level program) and the `import` builtin (src/c/builtins.c) so the
 * pipeline exists in exactly one place.
 *
 * Each call gets its OWN fresh root Env/`builtins` attrset (matching real
 * Nix: an imported file does not implicitly see the importing file's
 * local scope, only the global `builtins`). Relative path literals
 * (including the target of a nested `import`) written in `path` resolve
 * against `path`'s OWN directory, not the process's cwd or whichever
 * file imported it -- verified against `nix-instantiate --eval` (see
 * docs/architecture.md's import/path scope notes); implemented by
 * resolving `path` to an absolute path with `realpath()` and passing its
 * directory to `asnix_compile()` as the base_dir every AST_PATH node in
 * this file resolves against (see compiler.c's AST_PATH case).
 *
 * Aborts the process (message to stderr, exit 1) if `path` can't be
 * opened or resolved to an absolute path. */
Value asnix_eval_file(const char *path);

/* M57i: runs the same lex -> parse -> compile -> eval pipeline over a
 * source STRING (not a file) -- used by main.c's `--arg name EXPR` CLI
 * flag, whose EXPR is an arbitrary Nix expression, not a plain value.
 * Relative path literals inside `src` resolve against the process's
 * current working directory (matching real Nix: a `--arg`/`--argstr`
 * value has no "containing file" of its own to resolve against). */
Value asnix_eval_expr_source(const char *src);

#endif /* ASNIX_EVAL_FILE_H */
