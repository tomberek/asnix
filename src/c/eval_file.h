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

/* M54: `builtins.scopedImport scope path` -- same pipeline as
 * asnix_eval_file above, except `scope` (an already-forced Attrs Value)
 * supplies an extra, innermost lexical scope level for the imported
 * file: each of `scope`'s attribute names becomes directly resolvable as
 * a bare identifier inside `path`'s own source (compiler.c's
 * asnix_compile_scoped), taking priority over (shadowing) the ordinary
 * bare globals/`builtins.X` of the same name -- exactly matching real
 * Nix's own `scopedImport` (see compiler.c's asnix_compile_scoped for
 * the full rationale). `scope`'s field VALUES are not forced here (same
 * laziness as any other attrset field, M6) -- only its NAMES are needed
 * up front, to build the compile-time ScopeFrame. */
Value asnix_eval_file_scoped(const char *path, Value scope);

#endif /* ASNIX_EVAL_FILE_H */
