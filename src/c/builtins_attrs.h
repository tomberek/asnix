#ifndef ASNIX_BUILTINS_ATTRS_H
#define ASNIX_BUILTINS_ATTRS_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"
#include "builtins_list.h" /* shared AsnixPrimFn/AsnixBuiltinDef typedefs */

/*
 * builtins-attrs task: attrNames/attrValues/hasAttr/getAttr/mapAttrs/
 * removeAttrs, in their own translation unit (src/c/builtins_attrs.c) to
 * avoid merge conflicts with sibling agents also adding builtins -- see
 * src/c/builtins.c's BUILTINS[]/BuiltinDef for the shape this mirrors.
 *
 * Integration step: concatenate ASNIX_ATTRS_BUILTINS into the table
 * asnix_make_builtins_attrs() (builtins.c) walks, the same way it already
 * walks its own BUILTINS[]. AsnixPrimFn/AsnixBuiltinDef themselves come
 * from builtins_list.h (two sibling agents independently defined the
 * identical shape; integration kept one copy rather than two).
 */

extern const AsnixBuiltinDef ASNIX_ATTRS_BUILTINS[];
extern const size_t ASNIX_ATTRS_BUILTINS_COUNT;

#endif /* ASNIX_BUILTINS_ATTRS_H */
