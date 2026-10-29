#ifndef ASNIX_CODEGEN_H
#define ASNIX_CODEGEN_H

#include "ast.h"
#include "asnix/value.h"

typedef Value (*AsnixNativeFn)(void);

AsnixNativeFn asnix_codegen(Ast *root);

#endif

