#include "derivation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h" /* asnix_force_for_print */
#include "aterm.h"
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
