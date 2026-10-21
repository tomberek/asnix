#ifndef ASNIX_VALUE_UTIL_H
#define ASNIX_VALUE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "asnix/value.h"

/* Small, shared helpers used by multiple builtins (builtins.c, derivation.c)
 * -- factored out once a second user appeared, not speculatively. */

/* Arena-allocates a String{len;bytes} heap object and returns it tagged. */
Value asnix_make_string_value(const char *s, size_t len);

/* Arena-allocates a Path heap object (see heap.h) and returns it tagged
 * ASNIX_TAG_PTR. `abs_path` must already be an absolute, canonicalized
 * path (see compiler.c's AST_PATH case) -- this helper does no resolution
 * of its own. */
Value asnix_make_path_value(const char *abs_path, size_t len);

/* True iff `v` is an ASNIX_TAG_PTR object storing a Path rather than a
 * String (see heap.h's ASNIX_PATH_MARKER note). False for any other tag. */
int asnix_value_is_path(Value v);

/* Returns a pointer to the raw bytes (NOT NUL-terminated either way) and
 * length of an ASNIX_TAG_PTR value, whichever of the two kinds (String or
 * Path) it actually is -- for callers (printing, hashing, ...) that just
 * want "the text" regardless of which. Aborts if `v` isn't ASNIX_TAG_PTR. */
const char *asnix_ptr_bytes(Value v, int64_t *out_len);

/* Coerces INT/BOOL/NULL/string/path to a string Value, matching real
 * Nix's `toString` (this *is* `toString`'s logic -- `prim_toString` in
 * builtins.c just calls this directly). A Path argument is converted to a
 * plain String of its (already-absolute) text -- matching real Nix, where
 * `toString somePath` changes the value's type from "path" to "string"
 * (verified against `nix-instantiate --eval`; this project does not model
 * string "context", so unlike real Nix this never copies anything to the
 * store -- see docs/architecture.md's import/path scope notes). Aborts
 * the process on an uncoercible type (list/attrset/closure/etc), same as
 * real Nix. */
Value asnix_coerce_to_string(Value v);

/* Concatenates `count` values into one freshly arena-allocated String --
 * the runtime half of string interpolation ("foo ${expr} bar"): the
 * compiler pushes each literal chunk (already a string constant) and each
 * interpolated expression's raw result, then OP_STR_CONCAT (src/asm/ops/
 * op_str_concat.S) calls this with a pointer straight into VALSTACK, same
 * pop-N-build-one shape as OP_MK_LIST/OP_MK_ATTRS.
 *
 * Each non-string part is rejected with a fatal error, NOT coerced the way
 * `asnix_coerce_to_string`/`builtins.toString` would -- this matches real
 * Nix exactly (verified with `nix-instantiate --eval`: `"${7}"` and
 * `"${true}"` and `"${null}"` all fail with "cannot coerce ... to a
 * string", even though `toString 7`/`true`/`null` succeed; real Nix's
 * `coerceToString` has a `coerceMore` flag that `toString` sets and plain
 * "${...}" interpolation doesn't). An interpolated non-string value needs
 * an explicit `builtins.toString` in the source, same as real Nix. */
Value asnix_concat_strings(const Value *parts, int64_t count);

/* The `+` operator's full dispatch, not just integer arithmetic -- real
 * Nix's `+` does int addition for two ints, but string/path concatenation
 * (same coercion rules `asnix_concat_strings` already implements for
 * `"${a}${b}"` interpolation) for anything else, and is a hard type error
 * if exactly one side is an int (verified against `nix-instantiate
 * --eval`: `1 + "a"` -> "cannot add a string to an integer", `"a" + 1` ->
 * "cannot coerce an integer to a string" -- this project matches the
 * *class* of error, not real Nix's exact wording, same convention as every
 * other fatal message in this codebase). Result stays a Path (rather than
 * becoming a plain String) when the LHS specifically was a Path -- matches
 * the common real-world case (`somePath + "/subpath"`); mixed string+path
 * or path+path beyond that single rule is a documented, narrower scope cut
 * (real Nix additionally runs a path-existence check and copies-to-store
 * in some of these mixed cases -- not replicated here, see
 * docs/architecture.md). Used by `asnix_op_add` (src/asm/ops/op_arith.S)
 * whenever both operands aren't already known to be ints. */
Value asnix_add_values(Value lhs, Value rhs);

/* `++` (M23): both operands must already be verified TAG_LIST by the
 * caller (op_concat_lists.S does the type check, same split OP_UPDATE
 * uses for asnix_attrs_update) -- pushes a fresh List holding `lhs`'s
 * elements followed by `rhs`'s, in order. */
Value asnix_concat_lists(Value lhs, Value rhs);

/* Arena-allocates a NUL-terminated copy of an asnix String's bytes.
 * asnix strings are never NUL-terminated themselves (see heap.h), but
 * code boundaries that expect plain C strings (aterm.c's API) need one;
 * the arena is used (never freed) rather than malloc so nothing needs
 * explicit cleanup, consistent with this project's memory model. */
char *asnix_string_value_to_cstr(Value string_val);

#endif /* ASNIX_VALUE_UTIL_H */
