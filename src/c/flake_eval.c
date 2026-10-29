
#include "flake_eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "asnix/ir.h"
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

static void check_no_declared_inputs(Value flake_attrs, const char *dir) {
    uint32_t inputs_sym = asnix_intern("inputs", 6);
    int64_t idx = asnix_bshared_attrs_find(flake_attrs, inputs_sym);
    if (idx < 0) return;
    Value inputs_val = asnix_force_for_print(asnix_bshared_attrs_value(flake_attrs, idx));
    if (asnix_tag_of(inputs_val) != ASNIX_TAG_ATTRS) return;
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

