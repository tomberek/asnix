#ifndef ASNIX_COMPILER_H
#define ASNIX_COMPILER_H

#include "ast.h"
#include "asnix/ir.h"

/* Compiles an AST produced by asnix_parse() into a linked (handler
 * addresses already resolved) IrProgram ready for asnix_eval(). Name
 * resolution happens inline during this walk (no separate annotation
 * pass) -- see src/c/compiler.c for why that's sufficient at M4's scope. */
IrProgram *asnix_compile(Ast *root);

#endif /* ASNIX_COMPILER_H */
