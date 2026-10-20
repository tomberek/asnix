#ifndef ASNIX_BUILTINS_H
#define ASNIX_BUILTINS_H

#include "asnix/value.h"

/* Builds the `builtins` attrset (sorted by symbol id, matching OP_SELECT's
 * expectations) containing every native primop this milestone ships. The
 * compiler binds this as the single root-level scope slot (see
 * compiler.c/main.c) so user programs reference it like any other
 * variable: `builtins.toString 5`. */
Value asnix_make_builtins_attrs(void);

#endif /* ASNIX_BUILTINS_H */
