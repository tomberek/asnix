/* M126: `./asnix --flake DIR[#attr.path]` -- see flake_eval.h's own
 * header comment for the full picture and docs/architecture.md's M126
 * notes for the research this is scoped from (iets's call-flake.nix-based
 * approach vs. asnix's existing fetch infra).
 *
 * The actual `callFlake`-equivalent logic (applying `outputs` to `{ self
 * = result; }` and layering `sourceInfo`/`outPath`/`_type` on top, exactly
 * matching cppnix's own `call-flake.nix`, verified directly against
 * `nix-instantiate --eval` for the zero-input case this milestone
 * handles -- see docs/architecture.md) is written as a small embedded Nix
 * expression (CALL_FLAKE_MIN below) and run through the SAME lex->parse->
 * compile->eval pipeline as any other Nix source (`asnix_eval_expr_source`,
 * eval_file.h) -- the identical "embed a verbatim/near-verbatim .nix
 * snippet rather than hand-rolling its recursive-attrset logic in C"
 * trick M46 already used for `<nix/fetchurl.nix>`, and the same approach
 * iets itself takes (it embeds cppnix's own call-flake.nix byte-for-byte).
 * The *mutual recursion* between `result` and the `self` it hands back to
 * `outputs` is ordinary Nix `let`-recursion -- asnix's bytecode VM already
 * supports that (it's exactly how plain `let`/`rec {}` works), so nothing
 * new is needed on the evaluator side at all; only `sourceInfo` (the
 * fetched tree's `outPath`/`narHash`) is built in C, reusing M75/M80's
 * NAR-hash store-copy machinery the exact same way M82's `fetchGit` does. */

#include "flake_eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "asnix/ir.h" /* asnix_apply, asnix_force_for_print */
#include "base64.h"
#include "builtins_shared.h"
#include "eval_file.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

static _Noreturn void flake_fatal(const char *msg) {
    fprintf(stderr, "asnix: --flake: %s\n", msg);
    exit(1);
}

/* cppnix's call-flake.nix, reduced to the one case this milestone
 * actually handles: no declared inputs at all, so the attrset `outputs`
 * is applied to is always just `{ self = result; }` (real call-flake.nix's
 * `inputs // { self = result; }` with `inputs = {}` -- see
 * flake_eval.h). Curried two levels deep (`sourceInfo: outputsFn: ...`)
 * so main.c can apply it one `asnix_apply` call at a time, same
 * convention as every other multi-arg call in this codebase (main.c's own
 * header comment on `fn a b` via two successive single-arg applies). */
static const char *const CALL_FLAKE_MIN =
    "sourceInfo: outputsFn:\n"
    "let\n"
    "  outputs = outputsFn { self = result; };\n"
    "  result = outputs // sourceInfo // {\n"
    "    outPath = sourceInfo.outPath;\n"
    "    inputs = { };\n"
    "    inherit outputs sourceInfo;\n"
    "    _type = \"flake\";\n"
    "  };\n"
    "in result\n";

/* Builds `{ outPath; narHash; }` for the tree at `dir`, exactly the way
 * `builtins.fetchTree { type = "path"; path = dir; }` (and, on the C
 * side, `builtins.fetchGit`/M82) already does: NAR-hash the directory,
 * then store-copy it under the name "source" (libfetchers' own default
 * name for an unnamed tree -- confirmed directly against `nix-instantiate
 * --eval --extra-experimental-features 'flakes fetch-tree' -E
 * 'builtins.fetchTree { type = "path"; path = ...; }'`, which returns
 * exactly these two fields and no others for a plain path input: no
 * `lastModified`, unlike a git-tracked tree). */
static Value make_source_info(const char *dir) {
    uint8_t digest[32];
    if (asnix_hash_path_nar_sha256(dir, NULL, NULL, digest) != 0) {
        flake_fatal("failed to load libcrypto -- enter the flake devShell");
    }
    size_t b64len = asnix_base64_encoded_length(32);
    char narhash_b64[64];
    asnix_base64_encode(digest, 32, narhash_b64);
    char narhash_str[80];
    snprintf(narhash_str, sizeof(narhash_str), "sha256-%.*s", (int)b64len, narhash_b64);

    char out_path[300];
    asnix_copy_path_to_store_ext(dir, "source", 1, NULL, NULL, NULL, out_path, sizeof(out_path));

    Value out_path_str = asnix_make_string_value(out_path, strlen(out_path));
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    asnix_ctx_add_opaque(&ctx, out_path);
    asnix_string_attach_context(out_path_str, &ctx);

    uint32_t syms[2];
    Value vals[2];
    syms[0] = asnix_intern("outPath", 7);
    vals[0] = out_path_str;
    syms[1] = asnix_intern("narHash", 7);
    vals[1] = asnix_make_string_value(narhash_str, strlen(narhash_str));
    return asnix_bshared_make_attrs(syms, vals, 2);
}

/* Fatal unless `flake.nix`'s own `inputs` attribute (if present at all)
 * forces to an empty attrset -- the only case this milestone's
 * zero-argument `CALL_FLAKE_MIN` driver above is correct for. A flake
 * that declares inputs needs real input resolution/locking/fetching
 * (flake.lock parsing, `fetchTree` for non-local sources, `follows`,
 * ...) -- out of scope for this milestone, see flake_eval.h and
 * docs/architecture.md's M126 notes for exactly why this is a real,
 * deliberate scope cut rather than a silently-wrong approximation: a
 * flake WITH declared inputs would need its `outputs` function called
 * with those inputs actually resolved, which this driver cannot do
 * (it hands `outputsFn` only `{ self = result; }`, no other names), so
 * refusing loudly here is strictly better than calling `outputs` with a
 * subtly wrong argument and risking a confusing downstream
 * "undefined variable"-style failure instead of a clear one. */
static void check_no_declared_inputs(Value flake_attrs, const char *dir) {
    uint32_t inputs_sym = asnix_intern("inputs", 6);
    int64_t idx = asnix_bshared_attrs_find(flake_attrs, inputs_sym);
    if (idx < 0) return;
    Value inputs_val = asnix_force_for_print(asnix_bshared_attrs_value(flake_attrs, idx));
    if (asnix_tag_of(inputs_val) != ASNIX_TAG_ATTRS) return; /* malformed flake.nix -- not this gate's job to report */
    int64_t n = asnix_bshared_attrs_count(inputs_val);
    if (n == 0) return;
    size_t namelen;
    const char *name = asnix_sym_name(asnix_bshared_attrs_sym(inputs_val, 0), &namelen);
    char msg[400];
    snprintf(msg, sizeof(msg),
             "flake '%s' declares input '%.*s' (and %lld more); flake inputs are not supported yet "
             "(only a flake with no declared inputs can be evaluated)",
             dir, (int)namelen, name, (long long)(n - 1));
    flake_fatal(msg);
}

Value asnix_eval_flake_outputs(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char msg[400];
        snprintf(msg, sizeof(msg), "path '%s' is not a flake (because it's not a directory)", dir);
        flake_fatal(msg);
    }

    char flake_nix[300];
    snprintf(flake_nix, sizeof(flake_nix), "%s/flake.nix", dir);
    if (stat(flake_nix, &st) != 0 || !S_ISREG(st.st_mode)) {
        char msg[400];
        snprintf(msg, sizeof(msg), "path '%s' is not a flake (because it doesn't contain a 'flake.nix' file)", dir);
        flake_fatal(msg);
    }

    Value flake_attrs = asnix_force_for_print(asnix_eval_file(flake_nix));
    if (asnix_tag_of(flake_attrs) != ASNIX_TAG_ATTRS) {
        char msg[400];
        snprintf(msg, sizeof(msg), "flake '%s': 'flake.nix' did not evaluate to an attribute set", dir);
        flake_fatal(msg);
    }
    check_no_declared_inputs(flake_attrs, dir);

    uint32_t outputs_sym = asnix_intern("outputs", 7);
    int64_t outputs_idx = asnix_bshared_attrs_find(flake_attrs, outputs_sym);
    if (outputs_idx < 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "flake '%s' has no attribute 'outputs'", dir);
        flake_fatal(msg);
    }
    Value outputs_fn = asnix_force_for_print(asnix_bshared_attrs_value(flake_attrs, outputs_idx));

    Value source_info = make_source_info(dir);

    Value driver = asnix_force_for_print(asnix_eval_expr_source(CALL_FLAKE_MIN));
    Value step1 = asnix_force_for_print(asnix_apply(driver, &source_info, 1));
    Value result = asnix_force_for_print(asnix_apply(step1, &outputs_fn, 1));
    return result;
}
