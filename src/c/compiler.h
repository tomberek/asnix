#ifndef ASNIX_COMPILER_H
#define ASNIX_COMPILER_H

#include "ast.h"
#include "asnix/ir.h"

/* Compiles an AST produced by asnix_parse() into a linked (handler
 * addresses already resolved) IrProgram ready for asnix_eval(). Name
 * resolution happens inline during this walk (no separate annotation
 * pass) -- see src/c/compiler.c for why that's sufficient at M4's scope.
 *
 * `base_dir` is the absolute directory of the .nix file being compiled
 * (no trailing slash) -- relative path literals (AST_PATH, e.g.
 * `./foo.nix`) resolve against it, matching real Nix's rule that a path
 * literal resolves relative to the directory of the file it's *written
 * in*, not the process's cwd or the importing file's directory (see
 * src/c/eval_file.c, which computes this once per file and is the only
 * other caller of asnix_compile besides this file's own tests). */
IrProgram *asnix_compile(Ast *root, const char *base_dir);

#endif /* ASNIX_COMPILER_H */
