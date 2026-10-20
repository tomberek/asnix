#ifndef ASNIX_BUILTINS_LIST_H
#define ASNIX_BUILTINS_LIST_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"

/*
 * builtins-list stage: list-processing builtins (map, filter, foldl',
 * elemAt, concatLists, tail, head) implemented in builtins_list.c, kept in
 * their own file/table (not builtins.c's BUILTINS[] directly) so this
 * agent and the sibling agents also adding builtins in parallel don't
 * textually collide on the same array. A later integration step merges
 * asnix_builtins_list[] into the shared BUILTINS[] table in builtins.c
 * (same {name, fn, arity} shape as that file's local BuiltinDef, so the
 * merge is a straight concatenation, not a rewrite).
 *
 * genericClosure is NOT included here -- see builtins_list.c's header
 * comment for why it was cut rather than rushed.
 */

typedef Value (*AsnixPrimFn)(const Value *args, int64_t argc);

typedef struct {
    const char *name;
    AsnixPrimFn fn;
    int64_t arity;
} AsnixBuiltinDef;

extern const AsnixBuiltinDef asnix_builtins_list[];
extern const size_t asnix_builtins_list_count;

#endif /* ASNIX_BUILTINS_LIST_H */
