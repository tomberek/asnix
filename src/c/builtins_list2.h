#ifndef ASNIX_BUILTINS_LIST2_H
#define ASNIX_BUILTINS_LIST2_H

#include <stddef.h>

#include "builtins_misc.h" /* shared AsnixBuiltinMiscDef/-Builder {name, build()} shape */

/*
 * Second round of list builtins, added alongside genericClosure
 * (builtins_closure.c): elem, any, all, concatMap, sort, genList,
 * partition, foldl, foldr, catAttrs. Kept separate from M17's
 * builtins_list.c (map/filter/foldl'/elemAt/concatLists/tail/head) rather
 * than appended to it, matching this project's existing one-feature-batch-
 * per-file convention for builtins modules.
 *
 * All of these are true arity>=2 builtins, so this module uses the
 * {name, build()} shape (builtins_misc.h) rather than plain
 * {name, fn, arity} -- see builtins_shared.h's make_curried_native, the
 * mechanism every entry's `build()` uses.
 */

extern const AsnixBuiltinMiscDef ASNIX_LIST2_BUILTINS[];
extern const size_t ASNIX_LIST2_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_LIST2_H */
