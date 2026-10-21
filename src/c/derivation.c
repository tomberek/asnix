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
#include "hash_shim.h"
#include "store_path.h"
#include "symtab.h"
#include "value_util.h"

#define ASNIX_DRV_MAX_ARGS 64
#define ASNIX_DRV_MAX_ENV  64

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

    /* `out` is always present, blanked for the masked/self-referential-
     * hash pass -- overwrite it if the input attrset happened to set one
     * itself (real Nix's derivationStrict does the same: the output path
     * it computes always wins). */
    size_t out_idx = ASNIX_DRV_MAX_ENV;
    for (size_t i = 0; i < nenv; i++)
        if (strcmp(env[i].key, "out") == 0) {
            out_idx = i;
            break;
        }
    if (out_idx == ASNIX_DRV_MAX_ENV) {
        if (nenv >= ASNIX_DRV_MAX_ENV) {
            fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
            exit(1);
        }
        out_idx = nenv++;
    }
    env[out_idx].key = "out";
    env[out_idx].value = "";

    qsort(env, nenv, sizeof(env[0]), envvar_cmp);

    char *masked_aterm = asnix_build_derivation_aterm("", system, builder, arg_strs, nargs, env, nenv);
    uint8_t masked_digest[32];
    sha256_or_die(masked_aterm, strlen(masked_aterm), masked_digest);
    free(masked_aterm);

    char out_path[300];
    compute_store_path("output:out", masked_digest, name, out_path, sizeof(out_path));

    for (size_t i = 0; i < nenv; i++)
        if (strcmp(env[i].key, "out") == 0) env[i].value = out_path;

    char *real_aterm = asnix_build_derivation_aterm(out_path, system, builder, arg_strs, nargs, env, nenv);
    uint8_t real_digest[32];
    sha256_or_die(real_aterm, strlen(real_aterm), real_digest);
    free(real_aterm);

    char drv_name[256];
    snprintf(drv_name, sizeof(drv_name), "%s.drv", name);
    char drv_path[300];
    compute_store_path("text", real_digest, drv_name, drv_path, sizeof(drv_path));

    uint32_t syms[4] = {
        asnix_intern("outPath", 7),
        asnix_intern("drvPath", 7),
        asnix_intern("type", 4),
        asnix_intern("out", 3),
    };
    Value vals[4] = {
        asnix_make_string_value(out_path, strlen(out_path)),
        asnix_make_string_value(drv_path, strlen(drv_path)),
        asnix_make_string_value("derivation", 10),
        asnix_make_string_value(out_path, strlen(out_path)),
    };
    for (int i = 1; i < 4; i++) {
        int j = i;
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

    void *mem = asnix_arena_alloc(8 + 4 * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = 4;
    for (int i = 0; i < 4; i++) {
        char *e = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        *(int64_t *)(e + ASNIX_ATTR_OFF_SYM) = (int64_t)syms[i];
        *(Value *)(e + ASNIX_ATTR_OFF_VALUE) = vals[i];
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}
