#ifndef ASNIX_VALUE_UTIL_H
#define ASNIX_VALUE_UTIL_H

#include <stddef.h>

#include "asnix/value.h"

/* Small, shared helpers used by multiple builtins (builtins.c, derivation.c)
 * -- factored out once a second user appeared, not speculatively. */

/* Arena-allocates a String{len;bytes} heap object and returns it tagged. */
Value asnix_make_string_value(const char *s, size_t len);

/* Coerces INT/BOOL/NULL/already-a-string to a string Value, matching real
 * Nix's `toString` (this *is* `toString`'s logic -- `prim_toString` in
 * builtins.c just calls this directly). Aborts the process on an
 * uncoercible type (list/attrset/closure/etc), same as real Nix. */
Value asnix_coerce_to_string(Value v);

/* Arena-allocates a NUL-terminated copy of an asnix String's bytes.
 * asnix strings are never NUL-terminated themselves (see heap.h), but
 * code boundaries that expect plain C strings (aterm.c's API) need one;
 * the arena is used (never freed) rather than malloc so nothing needs
 * explicit cleanup, consistent with this project's memory model. */
char *asnix_string_value_to_cstr(Value string_val);

#endif /* ASNIX_VALUE_UTIL_H */
