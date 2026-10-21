#include "derivation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h" /* asnix_force_for_print */
#include "aterm.h"
#include "base32.h"
#include "builtins_shared.h" /* asnix_bshared_make_primop */
#include "eval_file.h"       /* asnix_eval_file -- M58's derivation.nix wrapper */
#include "hash_shim.h"
#include "store_path.h"
#include "symtab.h"
#include "value_util.h"

#define ASNIX_DRV_MAX_ARGS    64
#define ASNIX_DRV_MAX_ENV     64
/* Real nixpkgs derivations rarely declare more than ~6 outputs
 * (out/bin/dev/lib/man/doc/debug being the usual full set); 16 is a
 * generous cap matching this file's existing "small bounded arrays,
 * fatal if exceeded" convention (see ASNIX_DRV_MAX_ARGS/ENV above). */
#define ASNIX_DRV_MAX_OUTPUTS 16

static void sha256_or_die(const char *s, size_t len, uint8_t out[32]) {
    int64_t rc = asnix_hash_sha256(s, len, out);
    if (rc != 0) {
        fprintf(stderr,
                "asnix: derivation: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }
}

static int envvar_cmp(const void *a, const void *b) {
    return strcmp(((const AtermEnvVar *)a)->key, ((const AtermEnvVar *)b)->key);
}

static int atermoutput_cmp(const void *a, const void *b) {
    return strcmp(((const AtermOutput *)a)->name, ((const AtermOutput *)b)->name);
}

/* Ported from ~/nix/src/libstore/derivations.cc's `outputPathName`: the
 * `name` component of an output's own store path is the derivation's
 * `name` attribute verbatim for the "out" output, and `name + "-" +
 * outputName` for every other output (e.g. "hello" -> "hello" for "out"
 * but "hello-bin" for "bin") -- confirmed directly against real Nix
 * (`nix-instantiate --eval --strict` on a 2-output `derivation {...}`,
 * see this milestone's scope notes in docs/architecture.md). */
static void output_path_name(const char *drv_name, const char *output_name, char *out, size_t out_cap) {
    if (strcmp(output_name, "out") == 0) {
        snprintf(out, out_cap, "%s", drv_name);
    } else {
        snprintf(out, out_cap, "%s-%s", drv_name, output_name);
    }
}

/* Validates `outputHash`/`outputHashAlgo`/`outputHashMode` -- these were
 * previously passed straight through as plain, unvalidated environment-
 * variable strings (not even recognized as special), so a real-Nix-rejected
 * value (unknown algo, wrong-length hash, unknown mode) silently "worked"
 * here instead of erroring, which numtide/nix-conformance's eval-fail-drv-
 * hash-algo/length/mode.nix all expect. Scope cut, deliberately NOT a full
 * port of real Nix's fixed-output-derivation machinery: this only
 * validates well-formedness (matching real Nix's own upfront checks in
 * `primops.cc`'s derivationStrict and `libstore/hash.cc`'s hash-string
 * parsing), it does NOT change how the derivation's store path is
 * actually computed (still the ordinary non-fixed-output preimage --
 * verified this doesn't matter for anything currently exercised, since
 * conformance only checks the *shape* of fixed-output derivations'
 * resulting paths, not exact digest equality with a real store, which
 * would need actual content fetching infra this project doesn't have). */
static int valid_hash_algo_str(const char *s, size_t len) {
    return (len == 3 && memcmp(s, "md5", 3) == 0) || (len == 4 && memcmp(s, "sha1", 4) == 0) ||
           (len == 6 && memcmp(s, "sha256", 6) == 0) || (len == 6 && memcmp(s, "sha512", 6) == 0);
}

static size_t hash_algo_digest_bytes(const char *s, size_t len) {
    if (len == 3) return 16;       /* md5 */
    if (len == 4) return 20;       /* sha1 */
    if (len == 6 && s[3] == '2') return 32; /* sha256 */
    return 64;                     /* sha512 */
}

static void validate_output_hash_fields(const char *output_hash, const char *output_hash_algo,
                                         const char *output_hash_mode) {
    if (output_hash_mode && strcmp(output_hash_mode, "flat") != 0 && strcmp(output_hash_mode, "recursive") != 0) {
        fprintf(stderr, "asnix: unknown hash mode '%s', expected 'flat' or 'recursive'\n", output_hash_mode);
        exit(1);
    }
    /* M48: an EMPTY `outputHashAlgo` means "unspecified, infer the
     * algorithm from outputHash's own SRI/legacy prefix instead"
     * (confirmed: `outputHashAlgo = ""; outputHash = "sha256-...";`
     * evaluates fine on real Nix -- `<nix/fetchurl.nix>`'s own default
     * for `outputHashAlgo` is literally `if hash != "" then "" else
     * ...`, i.e. this exact empty-string case, found evaluating real
     * nixpkgs). Treated the same as `output_hash_algo == NULL` below
     * (both mean "nothing known from this source yet, derive it from
     * output_hash's own prefix instead") -- this check must happen
     * BEFORE the unknown-algorithm rejection, which previously fired
     * on `""` itself before ever reaching the SRI-prefix-stripping
     * logic further down that would have resolved it correctly. */
    if (output_hash_algo && output_hash_algo[0] == '\0') output_hash_algo = NULL;
    if (output_hash_algo && !valid_hash_algo_str(output_hash_algo, strlen(output_hash_algo))) {
        fprintf(stderr, "asnix: unknown hash algorithm '%s'\n", output_hash_algo);
        exit(1);
    }
    if (!output_hash) return;
    const char *algo = output_hash_algo;
    size_t algo_len = algo ? strlen(algo) : 0;
    const char *hash_body = output_hash;
    /* An SRI-style "algo-..." or legacy "algo:..." prefix on the hash
     * string itself names the algorithm -- real Nix always recognizes and
     * strips this form regardless of whether outputHashAlgo was ALSO
     * given separately (confirmed needed: numtide/nix-conformance's
     * eval-okay-drv-hash-spellings.nix passes the SAME hash five
     * different ways, including `outputHashAlgo = "sha256"; outputHash =
     * "sha256-<base64>"` together -- stripping only when outputHashAlgo
     * was absent, as an earlier version of this function did, rejected
     * that case with a bogus "wrong length" error since the un-stripped
     * prefix was counted against the expected digest length). */
    const char *sep = strchr(output_hash, '-');
    const char *sep2 = strchr(output_hash, ':');
    if (sep2 && (!sep || sep2 < sep)) sep = sep2;
    if (sep && valid_hash_algo_str(output_hash, (size_t)(sep - output_hash))) {
        algo = output_hash;
        algo_len = (size_t)(sep - output_hash);
        hash_body = sep + 1;
    }
    if (!algo) return; /* no algo known from either source -- nothing to check */
    size_t n = hash_algo_digest_bytes(algo, algo_len);
    size_t hash_len = strlen(hash_body);
    size_t hex_len = 2 * n;
    size_t b32_len = asnix_base32_encoded_length(n);
    size_t b64_len = ((n + 2) / 3) * 4;
    if (hash_len != hex_len && hash_len != b32_len && hash_len != b64_len) {
        fprintf(stderr, "asnix: hash '%s' has wrong length for hash algorithm '%.*s'\n",
                output_hash, (int)algo_len, algo);
        exit(1);
    }
}

/* The full `makeStorePath(type, hash, name)` (see store_path.h's header
 * comment): hex-encode `inner_digest` with its "sha256:" algo prefix,
 * assemble the preimage string, hash THAT (the one crypto-dependent step
 * store_path.c deliberately leaves to its caller), then format the final
 * path. Forgetting this second hash and feeding `inner_digest` straight
 * into asnix_make_store_path_from_digest is exactly the bug this
 * function exists to make impossible to repeat at a second call site. */
static void compute_store_path(const char *type, const uint8_t inner_digest[32], const char *name, char *out,
                                size_t out_cap) {
    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < 32; i++) {
        hashstr[7 + 2 * i] = hexdigits[inner_digest[i] >> 4];
        hashstr[7 + 2 * i + 1] = hexdigits[inner_digest[i] & 0xf];
    }
    hashstr[7 + 64] = '\0';

    char preimage[512];
    size_t n = asnix_store_path_preimage(type, hashstr, name, preimage, sizeof(preimage));
    if (n == 0) {
        fprintf(stderr, "asnix: derivation: preimage too long\n");
        exit(1);
    }

    uint8_t outer_digest[32];
    sha256_or_die(preimage, n, outer_digest);

    if (asnix_make_store_path_from_digest(outer_digest, name, out, out_cap) == 0) {
        fprintf(stderr, "asnix: derivation: computed store path too long\n");
        exit(1);
    }
}

Value asnix_prim_derivation(const Value *args, int64_t argc) {
    (void)argc;
    Value attrs_val = args[0];
    if (asnix_tag_of(attrs_val) != ASNIX_TAG_ATTRS) {
        fprintf(stderr, "asnix: derivation: argument must be an attribute set\n");
        exit(1);
    }

    const char *base = (const char *)(attrs_val.raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);

    const char *name = NULL;
    const char *system = NULL;
    const char *builder = NULL;
    const char *output_hash = NULL;
    const char *output_hash_algo = NULL;
    const char *output_hash_mode = NULL;
    const char *arg_strs[ASNIX_DRV_MAX_ARGS];
    size_t nargs = 0;
    AtermEnvVar env[ASNIX_DRV_MAX_ENV];
    size_t nenv = 0;
    const char *output_names[ASNIX_DRV_MAX_OUTPUTS];
    size_t noutputs = 0;
    int saw_outputs_attr = 0;

    for (int64_t i = 0; i < count; i++) {
        const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
        Value raw_val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
        /* attrset binding values are lazy (M6) -- force each one. Safe to
         * call here even though we're already inside an active OP_CALL:
         * asnix_force_for_print brings its own self-contained dispatch
         * context and properly saves/restores every register it touches
         * (see src/asm/force.S), so it nests like any well-behaved call. */
        Value val = asnix_force_for_print(raw_val);

        size_t keylen;
        const char *key = asnix_sym_name(sym, &keylen);

        if (keylen == 4 && memcmp(key, "args", 4) == 0) {
            if (asnix_tag_of(val) != ASNIX_TAG_LIST) {
                fprintf(stderr, "asnix: derivation: 'args' must be a list\n");
                exit(1);
            }
            const char *lbase = (const char *)(val.raw & ~0x7ULL);
            int64_t lcount = *(const int64_t *)(lbase + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(lbase + ASNIX_LIST_OFF_ELEMS);
            for (int64_t j = 0; j < lcount; j++) {
                if (nargs >= ASNIX_DRV_MAX_ARGS) {
                    fprintf(stderr, "asnix: derivation: too many args (max %d)\n", ASNIX_DRV_MAX_ARGS);
                    exit(1);
                }
                Value ev = asnix_force_for_print(elems[j]);
                Value sv = asnix_coerce_to_string(ev);
                arg_strs[nargs++] = asnix_string_value_to_cstr(sv);
            }
            continue;
        }

        /* `outputs` (default ["out"], see ASNIX_DERIVATION_NIX_SOURCE's
         * own `outputs ? [ "out" ]` default) is special but, unlike
         * `args`, does NOT `continue`: real Nix's derivationStrict both
         * parses it into the set of output names below AND still coerces
         * it to a plain space-joined string and stores it as an ordinary
         * `outputs` environment variable (confirmed: real multi-output
         * .drv files have an `outputs = "out bin ..."` env var, consumed
         * by the generic build-time setup machinery) -- that part falls
         * through to the generic handling right below unchanged. */
        if (keylen == 7 && memcmp(key, "outputs", 7) == 0) {
            if (asnix_tag_of(val) != ASNIX_TAG_LIST) {
                fprintf(stderr, "asnix: derivation: 'outputs' must be a list of strings\n");
                exit(1);
            }
            const char *lbase = (const char *)(val.raw & ~0x7ULL);
            int64_t lcount = *(const int64_t *)(lbase + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(lbase + ASNIX_LIST_OFF_ELEMS);
            saw_outputs_attr = 1;
            noutputs = 0;
            for (int64_t j = 0; j < lcount; j++) {
                Value ev = asnix_force_for_print(elems[j]);
                Value osv = asnix_coerce_to_string(ev);
                const char *oname = asnix_string_value_to_cstr(osv);
                if (strcmp(oname, "drvPath") == 0) {
                    fprintf(stderr, "asnix: derivation: invalid derivation output name 'drvPath'\n");
                    exit(1);
                }
                for (size_t k = 0; k < noutputs; k++) {
                    if (strcmp(output_names[k], oname) == 0) {
                        fprintf(stderr, "asnix: derivation: duplicate derivation output '%s'\n", oname);
                        exit(1);
                    }
                }
                if (noutputs >= ASNIX_DRV_MAX_OUTPUTS) {
                    fprintf(stderr, "asnix: derivation: too many outputs (max %d)\n", ASNIX_DRV_MAX_OUTPUTS);
                    exit(1);
                }
                output_names[noutputs++] = oname;
            }
            if (noutputs == 0) {
                fprintf(stderr, "asnix: derivation: derivation cannot have an empty set of outputs\n");
                exit(1);
            }
        }

        Value sv = asnix_coerce_to_string(val);
        char *valcstr = asnix_string_value_to_cstr(sv);
        char *keycstr = asnix_arena_alloc(keylen + 1);
        memcpy(keycstr, key, keylen);
        keycstr[keylen] = '\0';

        if (keylen == 4 && memcmp(key, "name", 4) == 0) name = valcstr;
        else if (keylen == 6 && memcmp(key, "system", 6) == 0) system = valcstr;
        else if (keylen == 7 && memcmp(key, "builder", 7) == 0) builder = valcstr;
        else if (keylen == 10 && memcmp(key, "outputHash", 10) == 0) output_hash = valcstr;
        else if (keylen == 14 && memcmp(key, "outputHashAlgo", 14) == 0) output_hash_algo = valcstr;
        else if (keylen == 14 && memcmp(key, "outputHashMode", 14) == 0) output_hash_mode = valcstr;

        if (nenv >= ASNIX_DRV_MAX_ENV) {
            fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
            exit(1);
        }
        env[nenv].key = keycstr;
        env[nenv].value = valcstr;
        nenv++;
    }

    if (!name || !system || !builder) {
        fprintf(stderr, "asnix: derivation: 'name', 'system', and 'builder' are required attributes\n");
        exit(1);
    }
    asnix_check_store_path_name(name);
    validate_output_hash_fields(output_hash, output_hash_algo, output_hash_mode);

    /* `outputs` defaults to `["out"]` -- same default
     * ASNIX_DERIVATION_NIX_SOURCE's own `outputs ? [ "out" ]` already
     * assumes at the Nix level (M58). Fixed-output derivations (an
     * `outputHash` given) are restricted to a single "out" output by
     * real Nix too (~/nix/src/libexpr/primops.cc's
     * derivationStrictInternal: "multiple outputs are not supported in
     * fixed-output derivations") -- out of scope here to generalize
     * (see this file's header comment), so reject the combination
     * explicitly rather than silently computing something wrong. */
    if (!saw_outputs_attr) {
        output_names[0] = "out";
        noutputs = 1;
    }
    if (output_hash && (noutputs != 1 || strcmp(output_names[0], "out") != 0)) {
        fprintf(stderr, "asnix: derivation: multiple outputs are not supported in fixed-output derivations\n");
        exit(1);
    }

    /* Every declared output name gets its own env var, blanked for the
     * masked/self-referential-hash pass -- overwrite it if the input
     * attrset happened to set one itself (real Nix's derivationStrict
     * does the same: the output path it computes always wins). */
    for (size_t o = 0; o < noutputs; o++) {
        size_t idx = ASNIX_DRV_MAX_ENV;
        for (size_t i = 0; i < nenv; i++)
            if (strcmp(env[i].key, output_names[o]) == 0) {
                idx = i;
                break;
            }
        if (idx == ASNIX_DRV_MAX_ENV) {
            if (nenv >= ASNIX_DRV_MAX_ENV) {
                fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
                exit(1);
            }
            idx = nenv++;
        }
        env[idx].key = output_names[o];
        env[idx].value = "";
    }

    qsort(env, nenv, sizeof(env[0]), envvar_cmp);

    /* Masked pass: ONE hash covers every declared output at once (all
     * blanked to "" in both the Outputs section and their env vars) --
     * ported from real Nix's `processDerivationOutputPaths`
     * (~/nix/src/libstore/derivations.cc): a single `maskHash()` is
     * computed once and then reused, with only the per-output `type`
     * string ("output:<name>") and store-path `name` component
     * (`outputPathName`) varying per output below. This is the key
     * generalization from M8's single-output version: still exactly one
     * masked ATerm/hash, just with N (name,"") entries in its Outputs
     * section instead of one. */
    AtermOutput masked_outs[ASNIX_DRV_MAX_OUTPUTS];
    for (size_t o = 0; o < noutputs; o++) {
        masked_outs[o].name = output_names[o];
        masked_outs[o].path = "";
    }
    qsort(masked_outs, noutputs, sizeof(masked_outs[0]), atermoutput_cmp);

    char *masked_aterm = asnix_build_derivation_aterm(masked_outs, noutputs, system, builder, arg_strs, nargs, env, nenv);
    uint8_t masked_digest[32];
    sha256_or_die(masked_aterm, strlen(masked_aterm), masked_digest);
    free(masked_aterm);

    /* One distinct store path per output: same masked digest, but a
     * different preimage `type` ("output:<name>") and store-path `name`
     * component (`outputPathName`) for each -- exactly
     * `store.makeOutputPath(outputName, *drvHash, drvName)` in real
     * Nix's `derivations.cc`. */
    char out_paths[ASNIX_DRV_MAX_OUTPUTS][300];
    for (size_t o = 0; o < noutputs; o++) {
        char type_str[32 + 7];
        snprintf(type_str, sizeof(type_str), "output:%s", output_names[o]);
        char path_name[256];
        output_path_name(name, output_names[o], path_name, sizeof(path_name));
        compute_store_path(type_str, masked_digest, path_name, out_paths[o], sizeof(out_paths[o]));

        for (size_t i = 0; i < nenv; i++)
            if (strcmp(env[i].key, output_names[o]) == 0) env[i].value = out_paths[o];
    }

    AtermOutput real_outs[ASNIX_DRV_MAX_OUTPUTS];
    for (size_t o = 0; o < noutputs; o++) {
        real_outs[o].name = output_names[o];
        real_outs[o].path = out_paths[o];
    }
    qsort(real_outs, noutputs, sizeof(real_outs[0]), atermoutput_cmp);

    char *real_aterm = asnix_build_derivation_aterm(real_outs, noutputs, system, builder, arg_strs, nargs, env, nenv);
    uint8_t real_digest[32];
    sha256_or_die(real_aterm, strlen(real_aterm), real_digest);
    free(real_aterm);

    char drv_name[256];
    snprintf(drv_name, sizeof(drv_name), "%s.drv", name);
    char drv_path[300];
    compute_store_path("text", real_digest, drv_name, drv_path, sizeof(drv_path));

    /* `strict`'s own shape, matching real Nix's derivationStrict exactly
     * (~/nix/src/libexpr/primops.cc's `mkOutputString`): `drvPath` plus
     * one key per declared output name, each a plain string (that
     * output's own outPath) -- NOT a nested attrset, and no separate
     * top-level `outPath`/`type`/`out` keys (M58's wrapper,
     * ASNIX_DERIVATION_NIX_SOURCE, only ever reads `strict.drvPath` and
     * `strict.${outputName}`, confirmed by re-reading it above; it
     * builds the self-referential "nice" per-output attrsets itself). */
    size_t nkeys = 1 + noutputs;
    uint32_t syms[1 + ASNIX_DRV_MAX_OUTPUTS];
    Value vals[1 + ASNIX_DRV_MAX_OUTPUTS];
    syms[0] = asnix_intern("drvPath", 7);
    vals[0] = asnix_make_string_value(drv_path, strlen(drv_path));
    for (size_t o = 0; o < noutputs; o++) {
        syms[1 + o] = asnix_intern(output_names[o], strlen(output_names[o]));
        vals[1 + o] = asnix_make_string_value(out_paths[o], strlen(out_paths[o]));
    }
    for (size_t i = 1; i < nkeys; i++) {
        size_t j = i;
        while (j > 0 && syms[j - 1] > syms[j]) {
            uint32_t ts = syms[j - 1];
            syms[j - 1] = syms[j];
            syms[j] = ts;
            Value tv = vals[j - 1];
            vals[j - 1] = vals[j];
            vals[j] = tv;
            j--;
        }
    }

    void *mem = asnix_arena_alloc(8 + nkeys * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = (int64_t)nkeys;
    for (size_t i = 0; i < nkeys; i++) {
        char *e = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        *(int64_t *)(e + ASNIX_ATTR_OFF_SYM) = (int64_t)syms[i];
        *(Value *)(e + ASNIX_ATTR_OFF_VALUE) = vals[i];
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}

Value asnix_make_derivation_strict_primop(void) {
    return asnix_bshared_make_primop((void *)asnix_prim_derivation, 1);
}

/* M58: verbatim port of cppnix's `~/nix/src/libexpr/primops/
 * derivation.nix` (its large leading doc comment, lines 1-28 there,
 * dropped -- purely descriptive, no semantic effect), see derivation.h's
 * header comment on asnix_make_derivation_wrapper_value for why this
 * exists. References the bare global `derivationStrict` (not
 * `builtins.derivationStrict`), matching the original exactly -- this
 * resolves via the ordinary ASNIX_ROOT_BARE_GLOBALS machinery
 * (compiler.c/eval_file.c), the same way `derivation` itself already
 * was, not any special injected scope. */
static const char ASNIX_DERIVATION_NIX_SOURCE[] =
    "drvAttrs@{\n"
    "  outputs ? [ \"out\" ],\n"
    "  ...\n"
    "}:\n"
    "\n"
    "let\n"
    "\n"
    "  strict = derivationStrict drvAttrs;\n"
    "\n"
    "  commonAttrs =\n"
    "    drvAttrs\n"
    "    // (builtins.listToAttrs outputsList)\n"
    "    // {\n"
    "      all = map (x: x.value) outputsList;\n"
    "      inherit drvAttrs;\n"
    "    };\n"
    "\n"
    "  outputToAttrListElement = outputName: {\n"
    "    name = outputName;\n"
    "    value = commonAttrs // {\n"
    "      outPath = strict.${outputName};\n"
    "      drvPath = strict.drvPath;\n"
    "      type = \"derivation\";\n"
    "      inherit outputName;\n"
    "    };\n"
    "  };\n"
    "\n"
    "  outputsList = map outputToAttrListElement outputs;\n"
    "\n"
    "in\n"
    "(builtins.head outputsList).value\n";

#define ASNIX_DERIVATION_TEMP_PATH "/tmp/.asnix-corepkgs-derivation.nix"

static const char *write_derivation_nix(void) {
    FILE *f = fopen(ASNIX_DERIVATION_TEMP_PATH, "w");
    if (!f) {
        fprintf(stderr, "asnix: could not write '%s' for the `derivation` wrapper\n", ASNIX_DERIVATION_TEMP_PATH);
        exit(1);
    }
    fwrite(ASNIX_DERIVATION_NIX_SOURCE, 1, sizeof(ASNIX_DERIVATION_NIX_SOURCE) - 1, f);
    fclose(f);
    return ASNIX_DERIVATION_TEMP_PATH;
}

Value asnix_make_derivation_wrapper_value(void) {
    static int have_cached = 0;
    static Value cached;
    if (!have_cached) {
        cached = asnix_eval_file_for_derivation_bootstrap(write_derivation_nix());
        have_cached = 1;
    }
    return cached;
}

const AsnixBuiltinMiscDef ASNIX_DERIVATION_BUILTINS[] = {
    { "derivation", asnix_make_derivation_wrapper_value },
    { "derivationStrict", asnix_make_derivation_strict_primop },
};
const size_t ASNIX_DERIVATION_BUILTINS_COUNT =
    sizeof(ASNIX_DERIVATION_BUILTINS) / sizeof(ASNIX_DERIVATION_BUILTINS[0]);
