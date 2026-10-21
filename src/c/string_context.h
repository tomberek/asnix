#ifndef ASNIX_STRING_CONTEXT_H
#define ASNIX_STRING_CONTEXT_H

#include <stddef.h>

#include "asnix/value.h"

/*
 * M76: real Nix string context -- an invisible dependency-provenance set
 * attached to a string *value*, surviving concatenation/coercion. Found
 * load-bearing (not just a nice-to-have introspection feature) chasing
 * d.nix past M74/M75: nixpkgs sets `builder = stdenvShell;`, which
 * traces back to `shell = "${stage0.bash}/bin/bash";` -- a STRING
 * INTERPOLATION of a derivation reference, concatenated with a literal
 * suffix, evaluated to a plain opaque string long before `derivation
 * {...}`'s primop ever sees it. M74's structural collector
 * (asnix_collect_input_refs, derivation.c) walks an already-forced
 * Value looking for a bare derivation attrset/list/Path -- it is blind
 * to this by construction, since once a reference becomes a STRING via
 * interpolation, there's no structure left to walk. This is the same
 * feature M61 (and implicitly M45/M54/M60) already declined once, for
 * being disproportionate to its own user-facing payoff
 * (getContext/hasContext/addDrvOutputDependencies) -- revealed here to
 * also be load-bearing for hash parity, not just introspection.
 *
 * Key simplification vs. that earlier, declined scope: that estimate
 * (~101 call sites, a new tagged Value kind touching the asm layer)
 * assumed context would be baked into the STRING HEAP LAYOUT itself
 * (like Path/Float's marker-byte trick, include/asnix/heap.h). Instead,
 * this attaches context via a SIDE TABLE keyed by the string Value's
 * own arena pointer (a linear-scan array, same convention as
 * eval_file.c's import cache / derivation.c's masked-hash cache) --
 * ordinary string reads (every asm op: equality, printing, hashing,
 * OP_STR_CONCAT's own byte-copying) stay COMPLETELY untouched, since
 * they never need to know a context exists. Only the specific C-level
 * functions that *construct* a new string from existing string/attrs/
 * list inputs need to query-and-propagate.
 *
 * Reuses exactly M74's own BuiltRef/InputRefSet vocabulary (dedup-on-
 * insert growable arrays) -- this type replaces that derivation.c-local
 * struct, moved here since context needs to be readable from
 * value_util.c, builtins_m54.c, AND derivation.c.
 */

typedef struct {
    char *drv_path;
    char *output_name;
} AsnixCtxBuiltRef;

typedef struct {
    AsnixCtxBuiltRef *drvs;
    size_t ndrvs, drvs_cap;
    char **srcs;
    size_t nsrcs, srcs_cap;
} AsnixStringContext;

void asnix_ctx_init(AsnixStringContext *ctx);
void asnix_ctx_add_built(AsnixStringContext *ctx, const char *drv_path, const char *output_name);
void asnix_ctx_add_opaque(AsnixStringContext *ctx, const char *path);
int asnix_ctx_is_empty(const AsnixStringContext *ctx);
/* dst |= src (deduped) -- a no-op if src is NULL or empty. */
void asnix_ctx_union_into(AsnixStringContext *dst, const AsnixStringContext *src);

/* Attaches a deep copy of `ctx` to `v` (a String Value, i.e.
 * ASNIX_TAG_PTR, not a Path/Float) in the process-lifetime side table --
 * a no-op if `ctx` is NULL or empty (the overwhelmingly common case:
 * every plain-literal/arithmetic/non-derivation string never gets an
 * entry at all, so lookups for them stay a fast "not found"). */
void asnix_string_attach_context(Value v, const AsnixStringContext *ctx);

/* NULL if `v` has no attached context. The returned pointer is owned by
 * the side table (process lifetime, same memory model as the rest of
 * this codebase) -- never free it. */
const AsnixStringContext *asnix_string_context_of(Value v);

#endif /* ASNIX_STRING_CONTEXT_H */
