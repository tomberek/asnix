#ifndef ASNIX_ATTRS_UPDATE_H
#define ASNIX_ATTRS_UPDATE_H

#include "asnix/value.h"

/* `a // b`: right-biased merge of two Attrs heap objects (both already
 * type-checked ASNIX_TAG_ATTRS by op_update.S's caller). Entries are
 * sorted-by-symbol-id arrays (see heap.h), so this is a plain two-pointer
 * sorted merge, O(|a|+|b|); on a duplicate key, `b`'s entry (value and
 * all) is kept verbatim, matching real Nix's right-wins `//` semantics.
 * Neither input is mutated or forced -- values (including any still-
 * unforced Thunk) are copied through unchanged. */
Value asnix_attrs_update(Value lhs, Value rhs);

#endif /* ASNIX_ATTRS_UPDATE_H */
