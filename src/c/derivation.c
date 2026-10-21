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
#include "base64.h"
#include "builtins_compare.h" /* asnix_toJSON_cstr -- M69's __structuredAttrs support */
#include "builtins_json.h" /* asnix_json_parse_structured_attrs -- M80's literal `__json` normalization */
#include "builtins_shared.h" /* asnix_bshared_make_primop, asnix_bshared_make_attrs */
#include "eval_file.h"       /* asnix_eval_file -- M58's derivation.nix wrapper */
#include "hash_shim.h"
#include "nar.h" /* asnix_copy_path_to_store -- M75 */
#include "store_path.h"
#include "string_context.h" /* M76 */
#include "symtab.h"
#include "value_util.h"

#define ASNIX_DRV_MAX_ARGS    64
/* M72: bumped from 64 -- found via d.nix, a real stdenv.mkDerivation
 * call (structuredAttrs-era make-derivation.nix sets a couple dozen
 * attributes unconditionally -- enableParallelBuilding, strictDeps,
 * depsBuildBuild, depsHostHostPropagated, __structuredAttrs, env,
 * outputChecks, ... -- before the package itself adds any of its own)
 * exceeded this on its very first attribute-count check. These are
 * plain fixed-size stack arrays (`env[ASNIX_DRV_MAX_ENV]` etc. below),
 * not an arena allocation, so the cost of a larger bound is just more
 * stack space per `derivation {...}` call, negligible at this size. */
#define ASNIX_DRV_MAX_ENV     256
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

/* M74: sorts a plain `char *[]` (e.g. InputRefSet's `srcs`) ascending --
 * `qsort`'s comparator receives pointers to the array ELEMENTS, so for
 * an array of `char *` that's a `char **`. */
static int strptr_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
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

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hash_algo_digest_bytes(const char *s, size_t len) {
    if (len == 3) return 16;       /* md5 */
    if (len == 4) return 20;       /* sha1 */
    if (len == 6 && s[3] == '2') return 32; /* sha256 */
    return 64;                     /* sha512 */
}

/* M73: on top of validate_output_hash_fields's existing well-formedness
 * checks (unchanged below), this ALSO decodes a valid `outputHash` into
 * raw digest bytes and reports the resolved algorithm -- needed by
 * compute_fixed_output_path (below) to actually implement real Nix's
 * `makeFixedOutputPath` formula, which this project validated the
 * SHAPE of since M48 but never actually used for path computation
 * (confirmed via a direct `nix-instantiate` comparison: a fixed-output
 * `derivation {...}` call's `outPath` did NOT match real Nix at all --
 * `validate_output_hash_fields`'s own pre-M73 comment admitted this
 * outright, "it does NOT change how the derivation's store path is
 * actually computed"). `*out_has_hash` is 0 if `output_hash` is NULL
 * (not a fixed-output derivation at all) -- callers must check this
 * before using `out_raw`/`out_raw_len`/`out_algo`. */
static void decode_output_hash(const char *output_hash, const char *output_hash_algo,
                                const char *output_hash_mode, int *out_has_hash, uint8_t out_raw[64],
                                size_t *out_raw_len, const char **out_algo, size_t *out_algo_len) {
    *out_has_hash = 0;
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

    if (hash_len == hex_len) {
        for (size_t i = 0; i < n; i++) {
            int hi = hex_digit_value(hash_body[2 * i]);
            int lo = hex_digit_value(hash_body[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                fprintf(stderr, "asnix: hash '%s' is not valid hex\n", output_hash);
                exit(1);
            }
            out_raw[i] = (uint8_t)((hi << 4) | lo);
        }
    } else if (hash_len == b32_len) {
        if (asnix_base32_decode(hash_body, hash_len, out_raw, n) != 0) {
            fprintf(stderr, "asnix: hash '%s' is not valid base32\n", output_hash);
            exit(1);
        }
    } else {
        if (asnix_base64_decode(hash_body, hash_len, out_raw, 64) != (int64_t)n) {
            fprintf(stderr, "asnix: hash '%s' is not valid base64\n", output_hash);
            exit(1);
        }
    }
    *out_has_hash = 1;
    *out_raw_len = n;
    *out_algo = algo;
    *out_algo_len = algo_len;
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

    /* `type` is "text:" plus one ":"-joined store path per referenced
     * input derivation/source (M74) -- unbounded by any fixed limit
     * this project enforces elsewhere, unlike `name` (capped at 211
     * bytes, asnix_check_store_path_name) or the hash (always exactly
     * 71 bytes). A 512-byte fixed buffer overflowed for real stdenv
     * derivations with many direct inputs (found chasing d.nix past
     * M74's own small verification cases) -- size dynamically instead
     * of guessing a bigger constant. */
    size_t preimage_cap = strlen(type) + strlen(hashstr) + strlen(ASNIX_STORE_DIR) + strlen(name) + 8;
    char *preimage = malloc(preimage_cap);
    size_t n = asnix_store_path_preimage(type, hashstr, name, preimage, preimage_cap);
    if (n == 0) {
        fprintf(stderr, "asnix: derivation: preimage too long\n");
        exit(1);
    }

    uint8_t outer_digest[32];
    sha256_or_die(preimage, n, outer_digest);
    free(preimage);

    if (asnix_make_store_path_from_digest(outer_digest, name, out, out_cap) == 0) {
        fprintf(stderr, "asnix: derivation: computed store path too long\n");
        exit(1);
    }
}

/* M73: real Nix's `makeFixedOutputPath` (~/nix/src/libstore/
 * store-dir-config.cc) -- computes a fixed-output derivation's "out"
 * output path DIRECTLY from its content hash, NOT via the ordinary
 * masked-ATerm-digest scheme (compute_store_path's own caller below,
 * used by every OTHER derivation) -- a completely separate formula,
 * only reached when `outputHash` is given. Verified directly against
 * real Nix (a minimal fixed-output `derivation {...}` repro,
 * see this milestone's scope notes) -- before this fix,
 * `validate_output_hash_fields` (now `decode_output_hash`) only ever
 * checked outputHash's SHAPE, never used it for path computation at
 * all, so every fixed-output derivation's outPath was simply wrong.
 * Two cases, ported verbatim from the real formula:
 *  - sha256 + "recursive" (NAR) ingestion: the output path is `
 *    makeStorePath("source", <the content hash itself>, name)` --
 *    the raw hash feeds straight into `compute_store_path`'s own
 *    preimage-then-outer-hash pipeline as its "inner digest", no
 *    extra wrapping. (Real Nix's `type` additionally gets ":<ref>"
 *    segments appended per referenced store path -- out of scope here,
 *    matching this project's existing fixed-output `unsafeDiscardReferences`
 *    scope cut: fixed-output derivations aren't allowed to reference
 *    other store paths without it, and this project doesn't model that
 *    attribute at all, so there are never any references to append.)
 *  - every other combination (the common case in practice: "flat"
 *    mode, which is `<nix/fetchurl.nix>`'s own default, M46): hash a
 *    small "fixed:out:<modePrefix><algo>:<hex-hash>:" payload string
 *    with SHA-256, then feed THAT hash into
 *    `makeStorePath("output:out", ..., name)` the same way. */
static void compute_fixed_output_path(const char *output_hash_mode, const uint8_t raw[64], size_t raw_len,
                                       const char *algo, size_t algo_len, const char *name, char *out,
                                       size_t out_cap) {
    int is_recursive = output_hash_mode && strcmp(output_hash_mode, "recursive") == 0;
    if (is_recursive && algo_len == 6 && memcmp(algo, "sha256", 6) == 0) {
        uint8_t digest32[32];
        memcpy(digest32, raw, 32);
        compute_store_path("source", digest32, name, out, out_cap);
        return;
    }
    static const char hexdigits[] = "0123456789abcdef";
    char hexhash[128 + 1];
    for (size_t i = 0; i < raw_len; i++) {
        hexhash[2 * i] = hexdigits[raw[i] >> 4];
        hexhash[2 * i + 1] = hexdigits[raw[i] & 0xf];
    }
    hexhash[2 * raw_len] = '\0';
    char payload[256];
    int n = snprintf(payload, sizeof(payload), "fixed:out:%s%.*s:%s:", is_recursive ? "r:" : "", (int)algo_len,
                      algo, hexhash);
    uint8_t digest[32];
    sha256_or_die(payload, (size_t)n, digest);
    compute_store_path("output:out", digest, name, out, out_cap);
}

/* M74: a process-global, in-memory cache of every derivation's own
 * "masked hash" (real Nix's `hashDerivationModulo`, see the long
 * comment below), keyed by that derivation's `drvPath`. This is how a
 * LATER `derivation {...}` call that references an EARLIER one (as
 * `src`, `buildInputs`, etc.) resolves that earlier derivation's hash
 * -- without ever touching a real `/nix/store` (this project has none,
 * see docs/architecture.md's M45/M46 notes). Verified this is exactly
 * what real Nix itself does in `--read-only-mode` (confirmed by
 * reading `~/nix/src/libexpr/primops.cc` directly): plain evaluation
 * normally ALSO writes real `.drv` files to disk as a side effect, and
 * a later derivation's hash computation can fall back to reading one
 * of those files back if its own in-process cache
 * (`derivation::masked::hashes`) doesn't have it cached already -- but
 * the cache is populated eagerly, immediately after every derivation's
 * own hash is computed, specifically so the common case (an input
 * derivation was evaluated earlier in the SAME process, which Nix's
 * call-by-need evaluation guarantees for anything actually referenced)
 * never needs the disk fallback at all. This project only implements
 * the cache half -- correct for exactly that common, guaranteed-by-
 * laziness case, which is the only one `asnix_collect_input_refs`
 * below can ever actually produce a reference for in the first place
 * (it can only find a reference to a derivation Value that was already
 * forced, which only happens after that derivation was already
 * evaluated). A linear-scan array, same "simple, small N" judgment as
 * M65's import-value cache (`eval_file.c`'s `g_import_cache`) -- the
 * number of distinct derivations one process constructs is nowhere
 * near large enough for this to matter. */
typedef struct {
    char *drv_path;
    int is_fixed_output;
    uint8_t hash[32];
} MaskedHashCacheEntry;
static MaskedHashCacheEntry *g_masked_hash_cache = NULL;
static size_t g_masked_hash_cache_count = 0, g_masked_hash_cache_cap = 0;

static int masked_hash_cache_find(const char *drv_path, MaskedHashCacheEntry *out) {
    for (size_t i = 0; i < g_masked_hash_cache_count; i++) {
        if (strcmp(g_masked_hash_cache[i].drv_path, drv_path) == 0) {
            *out = g_masked_hash_cache[i];
            return 1;
        }
    }
    return 0;
}

static void masked_hash_cache_insert(const char *drv_path, int is_fixed_output, const uint8_t hash[32]) {
    if (g_masked_hash_cache_count == g_masked_hash_cache_cap) {
        g_masked_hash_cache_cap = g_masked_hash_cache_cap ? g_masked_hash_cache_cap * 2 : 64;
        g_masked_hash_cache = realloc(g_masked_hash_cache, g_masked_hash_cache_cap * sizeof(*g_masked_hash_cache));
    }
    size_t len = strlen(drv_path);
    char *copy = malloc(len + 1);
    memcpy(copy, drv_path, len + 1);
    g_masked_hash_cache[g_masked_hash_cache_count].drv_path = copy;
    g_masked_hash_cache[g_masked_hash_cache_count].is_fixed_output = is_fixed_output;
    memcpy(g_masked_hash_cache[g_masked_hash_cache_count].hash, hash, 32);
    g_masked_hash_cache_count++;
}

/* M74: a narrow, CALL-SCOPED collector of "which attribute values of
 * THIS ONE `derivation {...}` call reference another derivation's
 * output (or a plain already-in-the-store path)" -- NOT a general,
 * persistent string-context feature (that was investigated and
 * explicitly declined as disproportionate, see docs/architecture.md's
 * M61 scope notes: a new Value-level heap kind propagated correctly
 * through the ~101 call sites of every string-producing builtin). This
 * only needs to answer one question, once, right here: real Nix's
 * `derivationStrict` builds its `inputDrvs`/`inputSrcs` from the UNION
 * of context collected while coercing every attribute to a string --
 * this reconstructs just enough of that one specific union, by walking
 * each attribute's ALREADY-FORCED value looking only for the shapes
 * that can carry a reference in the first place, rather than adding a
 * new tagged Value kind every OTHER builtin would also have to reason
 * about. Scope cuts (matching M61's own precedent of naming what's
 * deferred, not silently skipping it):
 *  - `DrvDeep` context (referencing a derivation's `.drvPath` itself,
 *    which in real Nix pulls in that derivation's ENTIRE transitive
 *    closure) is not handled -- not hit by any attribute shape found
 *    chasing `d.nix` so far (an attribute referencing `.drvPath`
 *    directly, rather than an output, is rare).
 *  - A plain (non-store, e.g. a local project file) Path attribute
 *    value is not copied into a content-addressed store and tracked
 *    as an `inputSrcs` entry -- this project has no such
 *    infrastructure at all (M45's `toFile` explicitly declined the
 *    equivalent: it computes what path content WOULD get, never
 *    writes it). Only a Path value that's ALREADY a `/nix/store/...`
 *    path (typically itself a previously-computed derivation output or
 *    `toFile`/`<nix/fetchurl.nix>` path, reached as a bare Path rather
 *    than through a derivation attrset) is tracked, verbatim, as an
 *    `inputSrcs` entry. */
typedef struct {
    char *drv_path;
    char *output_name;
} BuiltRef;

typedef struct {
    BuiltRef *drvs;
    size_t ndrvs, drvs_cap;
    char **srcs;
    size_t nsrcs, srcs_cap;
} InputRefSet;

static void input_ref_set_init(InputRefSet *s) {
    memset(s, 0, sizeof(*s));
}

static char *dup_cstr(const char *s, size_t len) {
    char *out = malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void input_ref_add_built(InputRefSet *s, const char *drv_path, const char *output_name) {
    for (size_t i = 0; i < s->ndrvs; i++) {
        if (strcmp(s->drvs[i].drv_path, drv_path) == 0 && strcmp(s->drvs[i].output_name, output_name) == 0) return;
    }
    if (s->ndrvs == s->drvs_cap) {
        s->drvs_cap = s->drvs_cap ? s->drvs_cap * 2 : 8;
        s->drvs = realloc(s->drvs, s->drvs_cap * sizeof(*s->drvs));
    }
    s->drvs[s->ndrvs].drv_path = dup_cstr(drv_path, strlen(drv_path));
    s->drvs[s->ndrvs].output_name = dup_cstr(output_name, strlen(output_name));
    s->ndrvs++;
}

static void input_ref_add_src(InputRefSet *s, const char *path) {
    for (size_t i = 0; i < s->nsrcs; i++) {
        if (strcmp(s->srcs[i], path) == 0) return;
    }
    if (s->nsrcs == s->srcs_cap) {
        s->srcs_cap = s->srcs_cap ? s->srcs_cap * 2 : 8;
        s->srcs = realloc(s->srcs, s->srcs_cap * sizeof(*s->srcs));
    }
    s->srcs[s->nsrcs++] = dup_cstr(path, strlen(path));
}

static void asnix_collect_input_refs(Value v, InputRefSet *out) {
    v = asnix_force_for_print(v);
    int tag = asnix_tag_of(v);
    if (tag == ASNIX_TAG_ATTRS) {
        const char *base = (const char *)(v.raw & ~0x7ULL);
        int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
        uint32_t sym_type = asnix_intern("type", 4);
        uint32_t sym_drvpath = asnix_intern("drvPath", 7);
        uint32_t sym_outname = asnix_intern("outputName", 10);
        int64_t idx_type = -1, idx_drvpath = -1, idx_outname = -1;
        for (int64_t i = 0; i < count; i++) {
            const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            if (sym == sym_type) idx_type = i;
            else if (sym == sym_drvpath) idx_drvpath = i;
            else if (sym == sym_outname) idx_outname = i;
        }
        /* Every derivation Value reachable from user code has these
         * three fields (M58's wrapper, ASNIX_DERIVATION_NIX_SOURCE) --
         * an attrset without all three just isn't a derivation output
         * ITSELF, but (M76) may still have one buried inside its own
         * attribute values (e.g. a `__structuredAttrs`-mode attribute,
         * which bypasses the normal string-coercion path below entirely
         * and only ever reaches this function as a raw, uncoerced
         * Value -- confirmed needed: real `hello.drv` itself uses
         * `__structuredAttrs`) -- recurse into every nested value in
         * that case, matching real Nix's own JSON serializer
         * (`printValueAsJSON`), which does the same walk to collect
         * context from a structured-attrs blob. */
        if (idx_type < 0 || idx_drvpath < 0 || idx_outname < 0) {
            for (int64_t i = 0; i < count; i++) {
                const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
                asnix_collect_input_refs(*(const Value *)(entry + ASNIX_ATTR_OFF_VALUE), out);
            }
            return;
        }
        Value type_val = asnix_force_for_print(
            *(const Value *)(base + ASNIX_ATTRS_OFF_ENTRIES + idx_type * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE));
        if (asnix_tag_of(type_val) != ASNIX_TAG_PTR) return;
        int64_t tlen;
        const char *tbytes = asnix_ptr_bytes(type_val, &tlen);
        if (tlen != 10 || memcmp(tbytes, "derivation", 10) != 0) return;
        Value drvpath_val = asnix_force_for_print(
            *(const Value *)(base + ASNIX_ATTRS_OFF_ENTRIES + idx_drvpath * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE));
        Value outname_val = asnix_force_for_print(
            *(const Value *)(base + ASNIX_ATTRS_OFF_ENTRIES + idx_outname * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE));
        int64_t dlen, olen;
        const char *dbytes = asnix_ptr_bytes(drvpath_val, &dlen);
        const char *obytes = asnix_ptr_bytes(outname_val, &olen);
        char *dpath = dup_cstr(dbytes, (size_t)dlen);
        char *oname = dup_cstr(obytes, (size_t)olen);
        input_ref_add_built(out, dpath, oname);
        free(dpath);
        free(oname);
        return;
    }
    if (tag == ASNIX_TAG_LIST) {
        const char *base = (const char *)(v.raw & ~0x7ULL);
        int64_t count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
        const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
        for (int64_t i = 0; i < count; i++) asnix_collect_input_refs(elems[i], out);
        return;
    }
    /* M76: a plain string (ASNIX_TAG_PTR, neither Path nor Float) may
     * already carry attached string context -- e.g. the result of an
     * earlier `"${pkg}/bin/x"` interpolation, evaluated long before
     * THIS `derivation {...}` call ever runs, with no structure left
     * to walk by the time it gets here. This is the actual fix for the
     * gap M74's own scope notes flagged: `builder = stdenvShell;`
     * (nixpkgs) traces back to a string interpolation of a derivation
     * reference, which the structural checks above alone can never
     * see. See string_context.h's own header comment. */
    if (tag == ASNIX_TAG_PTR && !asnix_value_is_path(v) && !asnix_value_is_float(v)) {
        const AsnixStringContext *ctx = asnix_string_context_of(v);
        if (!ctx) return;
        for (size_t i = 0; i < ctx->ndrvs; i++) input_ref_add_built(out, ctx->drvs[i].drv_path, ctx->drvs[i].output_name);
        for (size_t i = 0; i < ctx->nsrcs; i++) input_ref_add_src(out, ctx->srcs[i]);
        return;
    }
    if (tag == ASNIX_TAG_PTR && asnix_value_is_path(v)) {
        int64_t len;
        const char *bytes = asnix_ptr_bytes(v, &len);
        char *path = dup_cstr(bytes, (size_t)len);
        if (len > 11 && memcmp(bytes, "/nix/store/", 11) == 0) {
            input_ref_add_src(out, path);
        } else {
            /* M75: a Path NOT already under `/nix/store/` still becomes
             * an `inputSrcs` entry -- just the COPIED store path, not
             * the literal filesystem path -- matching
             * asnix_coerce_to_string_for_derivation's own handling of
             * the SAME attribute value (value_util.c), which this
             * collector must agree with exactly: the `.drv`'s `env`
             * entry and its `inputSrcs` entry have to name the same
             * path. asnix_copy_path_to_store's own cache (nar.c) means
             * computing it here and again during coercion doesn't
             * re-walk/re-hash the file twice. */
            char store_path[300];
            asnix_copy_path_to_store(path, store_path, sizeof(store_path));
            input_ref_add_src(out, store_path);
        }
        free(path);
    }
}

/* M80: a literal `__json = "...";` attribute -- real Nix's deprecated-
 * but-still-supported alternative to `__structuredAttrs = true;` for
 * setting structured attributes (confirmed directly: `nix-instantiate`
 * warns "setting structured attributes via '__json' is deprecated... Set
 * '__structuredAttrs = true' instead" but still accepts it) -- is NOT
 * stored verbatim in the resulting `.drv`: real Nix's own
 * `StructuredAttrs::parse(s)` (~/nix/src/libstore/parsed-derivations.cc)
 * parses it with `nlohmann::json::parse` and keeps the ORIGINAL text as
 * `verbatim`, but `derivationStrict`'s caller throws that `verbatim` away
 * before this derivation is ever serialized (confirmed empirically, not
 * just by reading `unparse()`'s verbatim-preserving fast path in
 * isolation): two JSON texts that only differ in key order / number
 * spelling / duplicate-key repetition (`{"b":1,"a":[1,2.5,null]}` vs
 * `{"a":[1,2.5,null],"b":1}`, or `{"a":1,"a":2}` vs `{"a":2}`) produce
 * BYTE-IDENTICAL `.drv` files against real `nix-instantiate`, which is
 * only possible if the stored `__json` env value is the RE-DUMPED
 * (sorted keys, shortest-round-trip numbers, last-wins duplicates) form,
 * not the original bytes. Reusing `asnix_json_parse_structured_attrs`
 * (parse) + `asnix_toJSON_cstr` (re-dump, same encoder `__structuredAttrs
 * = true;` mode already uses below) gets exactly that for free. Also
 * doubles as this attribute's OWN validation: a non-JSON `__json` value
 * must fail the whole evaluation (eval-fail-json-attr-not-json.nix),
 * which falls out of `asnix_json_parse_structured_attrs`'s ordinary
 * fatal-on-malformed-input behavior with no extra code here. */
static char *normalize_json_attr(const char *raw_json) {
    Value parsed = asnix_json_parse_structured_attrs(raw_json, (int64_t)strlen(raw_json));
    char *normalized_tmp = asnix_toJSON_cstr(parsed);
    char *normalized = asnix_arena_alloc(strlen(normalized_tmp) + 1);
    memcpy(normalized, normalized_tmp, strlen(normalized_tmp) + 1);
    free(normalized_tmp);
    return normalized;
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

    /* M69: `__structuredAttrs = true;` switches derivationStrict's whole
     * attribute-passing mode (~/nix/src/libexpr/primops.cc's
     * derivationStrictInternal, verified directly against real Nix --
     * see this milestone's scope notes in docs/architecture.md). Found
     * the attribute's own value with a dedicated pre-scan, exactly like
     * real Nix's own `attrs->get(state.s.structuredAttrs)` lookup BEFORE
     * its per-attribute loop even starts -- the attribute can appear
     * anywhere in the input attrset, not necessarily before the
     * attributes whose handling it changes. */
    int structured_attrs = 0;
    {
        uint32_t sa_sym = asnix_intern("__structuredAttrs", 17);
        for (int64_t i = 0; i < count; i++) {
            const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            if (sym != sa_sym) continue;
            Value raw_val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            Value val = asnix_force_for_print(raw_val);
            if (asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_TRUE) {
                structured_attrs = 1;
            } else if (!(asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_FALSE)) {
                fprintf(stderr, "asnix: derivation: '__structuredAttrs' must be a boolean\n");
                exit(1);
            }
            break;
        }
    }
    /* M76: `__ignoreNulls = true;` -- found chasing d.nix past this
     * milestone's own core string-context mechanism: nixpkgs's own
     * `make-derivation.nix` sets this unconditionally, and real Nix's
     * `derivationStrictInternal` (~/nix/src/libexpr/primops.cc) skips
     * the `__ignoreNulls` attribute itself UNCONDITIONALLY (regardless
     * of its own value or `__structuredAttrs` mode -- a plain `if
     * (i->name == state.s.ignoreNulls) continue;` before the main
     * per-attribute switch, NOT gated like `__structuredAttrs`'s own
     * conditional skip below), and additionally skips EVERY OTHER
     * attribute whose forced value is `null` when `ignoreNulls` is
     * true -- confirmed directly: real `hello.drv`'s own `__json` blob
     * has neither `__ignoreNulls` nor `userHook` (a null-valued
     * attribute nixpkgs sets) at all, where asnix previously emitted
     * both verbatim. */
    int ignore_nulls = 0;
    {
        uint32_t in_sym = asnix_intern("__ignoreNulls", 13);
        for (int64_t i = 0; i < count; i++) {
            const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
            uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
            if (sym != in_sym) continue;
            Value raw_val = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            Value val = asnix_force_for_print(raw_val);
            if (asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_TRUE) {
                ignore_nulls = 1;
            } else if (!(asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_FALSE)) {
                fprintf(stderr, "asnix: derivation: '__ignoreNulls' must be a boolean\n");
                exit(1);
            }
            break;
        }
    }
    /* In structured-attrs mode, every attribute except `args` (handled
     * identically either way, see below) and `__structuredAttrs` itself
     * gets JSON-serialized into one `__json` env var instead of each
     * becoming its own stringified env var -- this is the whole point:
     * an attribute whose value isn't coercible to a plain string (e.g.
     * `env = { foo = "bar"; }`) must flow into the JSON blob rather than
     * being rejected by `asnix_coerce_to_string`. Collected here as raw
     * (sym, forced-value) pairs and turned into one JSON string with
     * `asnix_toJSON_cstr` (the exact `builtins.toJSON` encoder, reused
     * rather than writing a second JSON serializer) after the loop. */
    uint32_t json_syms[ASNIX_DRV_MAX_ENV];
    Value json_vals[ASNIX_DRV_MAX_ENV];
    size_t njson = 0;

    /* M74: collected from EVERY attribute's value below, unconditionally
     * -- including `args`/`outputs`, matching real Nix's own
     * context-collecting `coerceToString` calls for those two
     * attributes specifically (confirmed in `~/nix/src/libexpr/
     * primops.cc`), not just the generic per-attribute path. */
    InputRefSet refs;
    input_ref_set_init(&refs);

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

        /* M76: `__ignoreNulls` itself is always excluded (see this
         * function's own pre-scan comment above); every OTHER
         * null-valued attribute is excluded too, but only when the
         * flag is actually set -- both unconditional-on-the-key /
         * conditional-on-every-other-key splits matching real Nix
         * exactly. Must come before `asnix_collect_input_refs` too: a
         * skipped attribute contributes nothing to `inputDrvs`/
         * `inputSrcs` either, matching real Nix's own early `continue`
         * (which skips its context-collecting `coerceToString` calls
         * entirely for a dropped attribute). */
        if (keylen == 13 && memcmp(key, "__ignoreNulls", 13) == 0) continue;
        if (ignore_nulls && asnix_tag_of(val) == ASNIX_TAG_IMM && asnix_imm_subtag(val) == ASNIX_IMM_NULL) continue;

        asnix_collect_input_refs(val, &refs);

        /* When the mode is actually active, `__structuredAttrs` itself is
         * never pushed into `env` OR the JSON blob -- matches real Nix's
         * own `if (i->name == state.s.structuredAttrs) continue;`, which
         * only fires inside its `jsonObject` branch. Confirmed directly
         * against real Nix that an explicit `__structuredAttrs = false;`
         * is NOT special-cased this way -- it falls through to the
         * ordinary per-attribute path like any other key, ending up as a
         * literal (empty-string, since `coerceToString false == ""`)
         * `__structuredAttrs` env var in the .drv -- so this `continue`
         * must be conditional on `structured_attrs`, not unconditional,
         * to keep that (admittedly obscure) pre-existing behavior. */
        if (keylen == 17 && memcmp(key, "__structuredAttrs", 17) == 0 && structured_attrs) continue;

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
                Value sv = asnix_coerce_to_string_for_derivation(ev);
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
                Value osv = asnix_coerce_to_string_for_derivation(ev);
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

        /* M69: in structured-attrs mode, EVERY key reaching this point
         * (ordinary attrs, `outputs`, and the handful of scalar fields
         * below) gets collected for the single `__json` env var -- the
         * raw, already-forced Value, not a string coercion, which is
         * exactly what lets a non-coercible value like a nested attrset
         * flow through instead of hitting `asnix_coerce_to_string`'s
         * fatal path. Matches real Nix's own unconditional
         * `jsonObject->structuredAttrs.emplace(key, printValueAsJSON(...))`
         * inside its own per-attribute default case (primops.cc) -- every
         * key gets JSON-serialized, `args` being the only generic-path
         * key excluded (handled above, via `continue`, in both modes). */
        if (structured_attrs) {
            if (njson >= ASNIX_DRV_MAX_ENV) {
                fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
                exit(1);
            }
            json_syms[njson] = sym;
            json_vals[njson] = val;
            njson++;
        }

        /* `name`/`system`/`builder`/`outputHash*` must always be
         * recovered as plain C strings for this function's own internal
         * use (store-path/ATerm computation) regardless of
         * `__structuredAttrs` -- real Nix does the same: its own nested
         * switch inside the `jsonObject` branch still calls
         * `forceString`/`forceStringNoCtx` on exactly these keys. Every
         * OTHER key is only coerced (and only ever pushed into `env`) in
         * non-structured mode -- skipping that coercion in structured
         * mode for a non-special key is the actual fix for the "cannot
         * coerce this value to a string" crash this milestone exists to
         * fix (e.g. `env = { foo = "bar"; };`, a nested attrset). */
        int is_special = (keylen == 4 && memcmp(key, "name", 4) == 0) ||
                         (keylen == 6 && memcmp(key, "system", 6) == 0) ||
                         (keylen == 7 && memcmp(key, "builder", 7) == 0) ||
                         (keylen == 10 && memcmp(key, "outputHash", 10) == 0) ||
                         (keylen == 14 && memcmp(key, "outputHashAlgo", 14) == 0) ||
                         (keylen == 14 && memcmp(key, "outputHashMode", 14) == 0);
        if (!structured_attrs || is_special) {
            Value sv = asnix_coerce_to_string_for_derivation(val);
            char *valcstr = asnix_string_value_to_cstr(sv);

            if (keylen == 4 && memcmp(key, "name", 4) == 0) name = valcstr;
            else if (keylen == 6 && memcmp(key, "system", 6) == 0) system = valcstr;
            else if (keylen == 7 && memcmp(key, "builder", 7) == 0) builder = valcstr;
            else if (keylen == 10 && memcmp(key, "outputHash", 10) == 0) output_hash = valcstr;
            else if (keylen == 14 && memcmp(key, "outputHashAlgo", 14) == 0) output_hash_algo = valcstr;
            else if (keylen == 14 && memcmp(key, "outputHashMode", 14) == 0) output_hash_mode = valcstr;

            if (!structured_attrs) {
                char *keycstr = asnix_arena_alloc(keylen + 1);
                memcpy(keycstr, key, keylen);
                keycstr[keylen] = '\0';

                /* M80: see normalize_json_attr's own comment -- only
                 * reachable here (not in `__structuredAttrs = true;`
                 * mode, where a literal `__json` key -- an unusual
                 * combination real Nix doesn't special-case either --
                 * would already have been folded into the `json_vals`
                 * blob above like any other ordinary key instead). */
                if (keylen == 6 && memcmp(key, "__json", 6) == 0) {
                    valcstr = normalize_json_attr(valcstr);
                }

                if (nenv >= ASNIX_DRV_MAX_ENV) {
                    fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
                    exit(1);
                }
                env[nenv].key = keycstr;
                env[nenv].value = valcstr;
                nenv++;
            }
        }
    }

    /* Structured-attrs mode: fold every collected (sym, value) pair into
     * one attrset and hand it to the exact `builtins.toJSON` encoder
     * (`asnix_toJSON_cstr`, builtins_compare.c) to get the single `__json`
     * env var's value -- this is the ONE place real Nix's own JSON blob
     * (`drv.structuredAttrs`) and this project's ordinary per-output `env`
     * array meet: `StructuredAttrs::unparse()`'s `("__json", <dump>)` pair
     * is merged into `drv.env` right before ATerm serialization
     * (~/nix/src/libstore/derivation/aterm.cc's `unparse`), which is
     * exactly what happens here too, just eagerly rather than at
     * serialization time (no behavioral difference for this project,
     * since nothing else reads `env` in between). */
    if (structured_attrs) {
        Value json_attrs = asnix_bshared_make_attrs(json_syms, json_vals, (int64_t)njson);
        /* asnix_toJSON_cstr mallocs its result (it's a generic, reusable
         * helper also used by `builtins.toJSON`, which returns an
         * ordinary heap-allocated Value string) -- copied into the arena
         * here and freed immediately after, matching every other env
         * value in this function (`asnix_string_value_to_cstr`, used for
         * every OTHER attribute's value, is arena-allocated too). */
        char *json_tmp = asnix_toJSON_cstr(json_attrs);
        size_t json_len = strlen(json_tmp);
        char *json_cstr = asnix_arena_alloc(json_len + 1);
        memcpy(json_cstr, json_tmp, json_len + 1);
        free(json_tmp);

        if (nenv >= ASNIX_DRV_MAX_ENV) {
            fprintf(stderr, "asnix: derivation: too many attributes (max %d)\n", ASNIX_DRV_MAX_ENV);
            exit(1);
        }
        env[nenv].key = "__json";
        env[nenv].value = json_cstr;
        nenv++;
    }

    if (!name || !system || !builder) {
        fprintf(stderr, "asnix: derivation: 'name', 'system', and 'builder' are required attributes\n");
        exit(1);
    }
    asnix_check_store_path_name(name);
    int has_fixed_hash;
    uint8_t fixed_hash_raw[64];
    size_t fixed_hash_raw_len = 0;
    const char *fixed_hash_algo = NULL;
    size_t fixed_hash_algo_len = 0;
    decode_output_hash(output_hash, output_hash_algo, output_hash_mode, &has_fixed_hash, fixed_hash_raw,
                        &fixed_hash_raw_len, &fixed_hash_algo, &fixed_hash_algo_len);

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

    /* M74: group the collected `Built` refs by `drv_path` into
     * `(drv_path, [output_names...])` entries -- real Nix's own
     * `DerivedPathMap<StringSet>` does the same grouping (one entry
     * per distinct input derivation, with the union of every output
     * name actually referenced anywhere in this derivation's
     * attributes). `output_names` for a group is the exact list that
     * ends up in BOTH the masked and real passes' `inputDrvs` section
     * (`asnix_build_derivation_aterm`'s own convention, see aterm.h) --
     * only the group's `drv_path` STRING differs between the two
     * passes (literal for "real", the referenced derivation's own
     * cached masked hash -- hex, no algorithm prefix, exactly real
     * Nix's `drvHash.to_string(Base16, false)` -- for "masked", the
     * actual `hashDerivationModulo` substitution). A small, bounded,
     * linear-scan grouping, same convention as every other small array
     * in this function (`ASNIX_DRV_MAX_ARGS`/`ENV`/`OUTPUTS` above). 64
     * was too small for real nixpkgs: `stdenv-linux` alone has well
     * over 64 direct input derivations once every setup-hook/wrapper
     * dependency is counted (found chasing d.nix past M74's own small
     * verification cases) -- 1024 accommodates that, but unlike every
     * OTHER fixed-size array in this function, this one is heap- not
     * stack-allocated: resolving one derivation's input refs can
     * recursively force (hence recursively call this very function
     * for) a dependency several levels deep in nixpkgs's own
     * derivation graph, and 1024 entries' worth of these arrays on the
     * stack per nested call overflowed the real stack chasing d.nix
     * past this exact point. */
#define ASNIX_DRV_MAX_INPUT_DRVS 1024
    const char **group_paths = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(const char *));
    const char *(*group_outs)[ASNIX_DRV_MAX_OUTPUTS] = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(*group_outs));
    size_t *group_nouts = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(size_t));
    size_t ngroups = 0;
    for (size_t i = 0; i < refs.ndrvs; i++) {
        const char *dp = refs.drvs[i].drv_path;
        size_t g = ngroups;
        for (size_t k = 0; k < ngroups; k++) {
            if (strcmp(group_paths[k], dp) == 0) {
                g = k;
                break;
            }
        }
        if (g == ngroups) {
            if (ngroups >= ASNIX_DRV_MAX_INPUT_DRVS) {
                fprintf(stderr, "asnix: derivation: too many distinct input derivations (max %d)\n",
                        ASNIX_DRV_MAX_INPUT_DRVS);
                exit(1);
            }
            group_paths[ngroups] = dp;
            group_nouts[ngroups] = 0;
            ngroups++;
        }
        if (group_nouts[g] >= ASNIX_DRV_MAX_OUTPUTS) {
            fprintf(stderr, "asnix: derivation: too many referenced outputs of one input derivation (max %d)\n",
                    ASNIX_DRV_MAX_OUTPUTS);
            exit(1);
        }
        group_outs[g][group_nouts[g]++] = refs.drvs[i].output_name;
    }
    /* Groups sorted by `drv_path` (an index array, since `group_paths`/
     * `group_outs`/`group_nouts` are three separate parallel arrays);
     * each group's own output-name list sorted too -- both required by
     * `asnix_build_derivation_aterm`'s own sorting contract. */
    size_t *group_order = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(size_t));
    for (size_t g = 0; g < ngroups; g++) group_order[g] = g;
    for (size_t i = 1; i < ngroups; i++) {
        size_t j = i;
        while (j > 0 && strcmp(group_paths[group_order[j - 1]], group_paths[group_order[j]]) > 0) {
            size_t t = group_order[j - 1];
            group_order[j - 1] = group_order[j];
            group_order[j] = t;
            j--;
        }
    }
    for (size_t g = 0; g < ngroups; g++) {
        for (size_t i = 1; i < group_nouts[g]; i++) {
            size_t j = i;
            while (j > 0 && strcmp(group_outs[g][j - 1], group_outs[g][j]) > 0) {
                const char *t = group_outs[g][j - 1];
                group_outs[g][j - 1] = group_outs[g][j];
                group_outs[g][j] = t;
                j--;
            }
        }
    }

    AtermInputDrv *masked_input_drvs = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(AtermInputDrv));
    AtermInputDrv *real_input_drvs = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(AtermInputDrv));
    char (*masked_hash_hex)[65] = malloc(ASNIX_DRV_MAX_INPUT_DRVS * sizeof(*masked_hash_hex));
    for (size_t gi = 0; gi < ngroups; gi++) {
        size_t g = group_order[gi];
        MaskedHashCacheEntry entry;
        /* In a correctly-evaluated program this is always a hit: Nix's
         * call-by-need evaluation always forces (hence already fully
         * evaluated and cached, see masked_hash_cache_insert below) a
         * derivation before any attribute referencing it can be read
         * here. A miss means a real invariant was violated -- fatal,
         * matching this project's standing convention, not a silent
         * fallback. */
        if (!masked_hash_cache_find(group_paths[g], &entry)) {
            fprintf(stderr,
                    "asnix: derivation: internal error: no cached hash for input derivation '%s' "
                    "(referenced before it was ever evaluated?)\n",
                    group_paths[g]);
            exit(1);
        }
        static const char hexdigits[] = "0123456789abcdef";
        for (int b = 0; b < 32; b++) {
            masked_hash_hex[gi][2 * b] = hexdigits[entry.hash[b] >> 4];
            masked_hash_hex[gi][2 * b + 1] = hexdigits[entry.hash[b] & 0xf];
        }
        masked_hash_hex[gi][64] = '\0';

        masked_input_drvs[gi].drv_path = masked_hash_hex[gi];
        masked_input_drvs[gi].output_names = group_outs[g];
        masked_input_drvs[gi].noutput_names = group_nouts[g];

        real_input_drvs[gi].drv_path = group_paths[g];
        real_input_drvs[gi].output_names = group_outs[g];
        real_input_drvs[gi].noutput_names = group_nouts[g];
    }

    /* Real Nix's `hashDerivationModulo` substitution doesn't just swap
     * each `inputDrvs` entry's path for its resolved hash in place -- it
     * rebuilds `inputDrvs` as a NEW `std::map<string, StringSet>` keyed
     * by that hash string (`~/nix/src/libstore/derivations.cc`), and a
     * `std::map`'s iteration (which `unparse` walks) is sorted by ITS
     * OWN key. So the masked pass's `InputDrvs` section must be sorted
     * by the substituted hash hex string, NOT by the literal drv_path
     * order `group_order` already gives `real_input_drvs` -- a
     * same-input-set derivation can have its two input drvs' hashes
     * sort in the opposite order from their literal paths (confirmed:
     * this milestone's own list-of-two-inputs verification case hits
     * exactly this, `b2`'s hash sorts before `a`'s despite `a.drv`
     * sorting before `b2.drv`). */
    for (size_t i = 1; i < ngroups; i++) {
        size_t j = i;
        while (j > 0 && strcmp(masked_input_drvs[j - 1].drv_path, masked_input_drvs[j].drv_path) > 0) {
            AtermInputDrv t = masked_input_drvs[j - 1];
            masked_input_drvs[j - 1] = masked_input_drvs[j];
            masked_input_drvs[j] = t;
            j--;
        }
    }

    /* `inputSrcs`: just the collected, deduped, already-in-the-store
     * paths, sorted -- no substitution needed for either pass (unlike
     * `inputDrvs`, a plain store path has no further hash to resolve). */
    qsort(refs.srcs, refs.nsrcs, sizeof(char *), strptr_cmp);

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
        masked_outs[o].hash_algo = NULL;
        masked_outs[o].hash = NULL;
    }
    qsort(masked_outs, noutputs, sizeof(masked_outs[0]), atermoutput_cmp);

    char *masked_aterm = asnix_build_derivation_aterm(masked_outs, noutputs, masked_input_drvs, ngroups,
                                                       (const char **)refs.srcs, refs.nsrcs, system, builder,
                                                       arg_strs, nargs, env, nenv);
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

    /* M73: a fixed-output derivation's "out" path is NOT derived from
     * `masked_digest` at all -- override it with the real formula
     * (compute_fixed_output_path, above). Safe to index [0]/output_names[0]
     * directly: `output_hash` (hence `has_fixed_hash`) is only ever
     * accepted together with exactly one output named "out" (the
     * `multiple outputs are not supported in fixed-output derivations`
     * check above). The corresponding `env["out"]` entry (already set
     * to the wrong, masked-digest-derived path by the loop just above)
     * is corrected too, so the real ATerm pass below -- and `__json`,
     * already serialized earlier for structured-attrs mode -- both see
     * the right value where they read `env`/`out_paths` afterward. */
    char fixed_hash_algo_cstr[16]; /* longest: "r:" + "sha256"/"sha512" + NUL */
    char fixed_hash_algo_hex[128 + 1];
    if (has_fixed_hash) {
        compute_fixed_output_path(output_hash_mode, fixed_hash_raw, fixed_hash_raw_len, fixed_hash_algo,
                                   fixed_hash_algo_len, name, out_paths[0], sizeof(out_paths[0]));
        for (size_t i = 0; i < nenv; i++)
            if (strcmp(env[i].key, "out") == 0) env[i].value = out_paths[0];

        /* `fixed_hash_algo` may point into the MIDDLE of `output_hash`'s
         * own buffer (e.g. the SRI-prefix-stripping case in
         * decode_output_hash, "sha256-base64body" -- `algo` points at
         * the whole string, bounded by `algo_len`, NOT NUL-terminated
         * at that boundary) -- copy it into its own small, properly
         * NUL-terminated buffer before handing it to AtermOutput, which
         * (like every other field asnix_build_derivation_aterm reads)
         * expects a plain C string, not a (pointer,length) pair.
         * "recursive" mode additionally prepends "r:" to the ATerm's
         * hashAlgo field (confirmed directly against a real .drv:
         * `("out",path,"r:sha256",hash)`, not `("out",path,"sha256",
         * hash)`) -- `makeFileIngestionPrefix`'s own "r:"/""  split,
         * used here ONLY for this ATerm-serialization field, NOT for
         * compute_fixed_output_path's path-computation formula above
         * (whose two branches already each handle this distinction
         * their own way: the recursive+sha256 branch bypasses the
         * "fixed:out:..." payload scheme entirely, every other
         * combination bakes its own "r:"/"" prefix into that payload
         * string directly). */
        int is_recursive = output_hash_mode && strcmp(output_hash_mode, "recursive") == 0;
        snprintf(fixed_hash_algo_cstr, sizeof(fixed_hash_algo_cstr), "%s%.*s", is_recursive ? "r:" : "",
                 (int)fixed_hash_algo_len, fixed_hash_algo);

        static const char hexdigits[] = "0123456789abcdef";
        for (size_t i = 0; i < fixed_hash_raw_len; i++) {
            fixed_hash_algo_hex[2 * i] = hexdigits[fixed_hash_raw[i] >> 4];
            fixed_hash_algo_hex[2 * i + 1] = hexdigits[fixed_hash_raw[i] & 0xf];
        }
        fixed_hash_algo_hex[2 * fixed_hash_raw_len] = '\0';
    }

    AtermOutput real_outs[ASNIX_DRV_MAX_OUTPUTS];
    for (size_t o = 0; o < noutputs; o++) {
        real_outs[o].name = output_names[o];
        real_outs[o].path = out_paths[o];
        /* M73: real Nix's ATerm `Outputs` section carries the real
         * hashAlgo/hash fields for a fixed-output derivation (confirmed
         * directly against a real .drv: `("out",path,"sha256","0d5f60...")`,
         * NOT `("out",path,"","")`) -- needed so this derivation's OWN
         * `drvPath` (hashed from this REAL, unmasked ATerm) matches real
         * Nix. Every other derivation keeps "","" exactly as before. */
        if (has_fixed_hash && strcmp(output_names[o], "out") == 0) {
            real_outs[o].hash_algo = fixed_hash_algo_cstr;
            real_outs[o].hash = fixed_hash_algo_hex;
        } else {
            real_outs[o].hash_algo = NULL;
            real_outs[o].hash = NULL;
        }
    }
    qsort(real_outs, noutputs, sizeof(real_outs[0]), atermoutput_cmp);

    char *real_aterm = asnix_build_derivation_aterm(real_outs, noutputs, real_input_drvs, ngroups,
                                                     (const char **)refs.srcs, refs.nsrcs, system, builder, arg_strs,
                                                     nargs, env, nenv);
    /* Debugging aid for the standing d.nix/nixpkgs-hash-parity bug-chase
     * (M74-M77+): set ASNIX_DEBUG_DRV=<name> to dump this derivation's
     * own real ATerm text to stderr when its `name` matches, instead of
     * repeatedly adding/removing a one-off fprintf here each session. */
    {
        const char *dbg = getenv("ASNIX_DEBUG_DRV");
        if (dbg && strcmp(dbg, name) == 0) fprintf(stderr, "ASNIX_DEBUG_DRV[%s] = %s\n", name, real_aterm);
    }
    uint8_t real_digest[32];
    sha256_or_die(real_aterm, strlen(real_aterm), real_digest);
    free(real_aterm);

    /* M74: `drvPath` itself is a content-addressed "text" path whose
     * TYPE string is NOT the bare `"text"` this project previously
     * (incorrectly, but unnoticed until this milestone's own 2-level
     * verification case, which has an actual reference to catch it --
     * every PRIOR fixture had zero references) used -- real Nix's
     * `infoForDerivation` (~/nix/src/libstore/derivations.cc) builds
     * `references = drv.inputSrcs ∪ {every drv.inputDrvs path}` and
     * feeds `TextInfo{hash; references}` through
     * `makeFixedOutputPathFromCA`, whose "text" branch is exactly
     * `makeType(store, "text", references)` -- `"text"`, then `":" +
     * path` for each reference IN SORTED ORDER (confirmed: a `.drv`
     * genuinely "contains" the store paths of everything it refers to,
     * by definition of what a derivation's `inputSrcs`/`inputDrvs`
     * are, so this is a real reference-tracking requirement, not an
     * arbitrary detail). `group_paths[]` (already sorted via
     * `group_order`, the literal -- not hash-substituted -- input
     * derivation paths) and `refs.srcs[]` (already sorted) are
     * disjoint by construction (a path is either a derivation or a
     * plain source, never both), so concatenating then re-sorting the
     * union is simplest. */
    const char **drv_refs = malloc((ngroups + refs.nsrcs) * sizeof(const char *));
    size_t ndrv_refs = 0;
    for (size_t gi = 0; gi < ngroups; gi++) drv_refs[ndrv_refs++] = group_paths[group_order[gi]];
    for (size_t i = 0; i < refs.nsrcs; i++) drv_refs[ndrv_refs++] = refs.srcs[i];
    qsort(drv_refs, ndrv_refs, sizeof(const char *), strptr_cmp);
    free(group_paths);
    free(group_order);
    free(group_nouts);

    /* A fixed-size buffer here overflowed for real stdenv derivations
     * with many direct inputs (found chasing d.nix past M74's own small
     * verification cases, same class of bug as compute_store_path's
     * preimage buffer above) -- size exactly, from the known reference
     * strings, instead of guessing a constant. */
    size_t drv_type_cap = 5; /* "text" + NUL */
    for (size_t i = 0; i < ndrv_refs; i++) drv_type_cap += strlen(drv_refs[i]) + 1; /* ":" + path */
    char *drv_type = malloc(drv_type_cap);
    size_t drv_type_len = (size_t)snprintf(drv_type, drv_type_cap, "text");
    for (size_t i = 0; i < ndrv_refs; i++) {
        drv_type_len += (size_t)snprintf(drv_type + drv_type_len, drv_type_cap - drv_type_len, ":%s", drv_refs[i]);
    }
    free(drv_refs);

    char drv_name[256];
    snprintf(drv_name, sizeof(drv_name), "%s.drv", name);
    char drv_path[300];
    compute_store_path(drv_type, real_digest, drv_name, drv_path, sizeof(drv_path));
    free(drv_type);

    /* M74: cache THIS derivation's own `hashDerivationModulo`-equivalent
     * now, eagerly, right after computing it -- so a LATER derivation
     * that references this one (via `drv_path`) can resolve it purely
     * in-memory (see masked_hash_cache_find's own caller above). Two
     * cases, matching real Nix's `DrvHashModulo` variant exactly:
     *  - fixed-output: real Nix's own formula is `sha256("fixed:out:"
     *    + <algo, "r:"-prefixed if recursive> + ":" + <bare hex, no
     *    prefix> + ":" + <this output's own already-computed store
     *    path>)`, verified directly against
     *    `~/nix/src/libstore/derivations.cc`'s `hashDerivationModulo`
     *    (the SAME real-Nix function this whole milestone ports, read
     *    in full before writing this). Reuses `fixed_hash_algo_cstr`/
     *    `fixed_hash_algo_hex` (already built above for the ATerm's
     *    Outputs-section fields, M73) and `out_paths[0]` (this
     *    derivation's own "out" path).
     *  - every other (regular) derivation: `hashDerivationModulo(drv,
     *    maskOutputs=false)` -- NOT `masked_digest` (that's
     *    `maskOutputs=TRUE`, used only for THIS derivation's OWN
     *    output-path computation above, M8/M62) and NOT `real_digest`
     *    either (that hashes the LITERAL, as-written-to-disk ATerm --
     *    literal `inputDrvs` paths, used only for this derivation's
     *    OWN `drvPath` just above) -- a THIRD variant: this
     *    derivation's REAL (filled-in, unmasked) outputs, same as
     *    `real_outs`, combined with the SAME hash-SUBSTITUTED
     *    `inputDrvs` the masked pass already uses (`masked_input_drvs`)
     *    -- `hashDerivationModulo`'s own source confirms `maskOutputs`
     *    only ever controls whether OUTPUT paths are blanked, NOT
     *    whether `inputDrvs` get hash-substituted (that substitution
     *    always happens, independent of `maskOutputs`). Found via this
     *    milestone's own 2-level verification case: caching
     *    `masked_digest` instead gave a plausible-looking but WRONG
     *    substituted hash for a depending derivation, confirmed by
     *    computing both candidate hashes by hand and checking which
     *    one reproduces real Nix's actual output path. */
    if (has_fixed_hash) {
        char fixed_hash_payload[512];
        int pn = snprintf(fixed_hash_payload, sizeof(fixed_hash_payload), "fixed:out:%s:%s:%s",
                           fixed_hash_algo_cstr, fixed_hash_algo_hex, out_paths[0]);
        uint8_t fixed_cache_hash[32];
        sha256_or_die(fixed_hash_payload, (size_t)pn, fixed_cache_hash);
        masked_hash_cache_insert(drv_path, 1, fixed_cache_hash);
    } else {
        char *cache_aterm = asnix_build_derivation_aterm(real_outs, noutputs, masked_input_drvs, ngroups,
                                                           (const char **)refs.srcs, refs.nsrcs, system, builder,
                                                           arg_strs, nargs, env, nenv);
        uint8_t cache_digest[32];
        sha256_or_die(cache_aterm, strlen(cache_aterm), cache_digest);
        free(cache_aterm);
        masked_hash_cache_insert(drv_path, 0, cache_digest);
    }
    free(group_outs);
    free(masked_input_drvs);
    free(real_input_drvs);
    free(masked_hash_hex);

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
        /* M76: this is THE origin point for real string context --
         * `pkg.out`/`pkg.${name}` (hence `"${pkg}/bin/x"`-style
         * interpolation, and `asnix_coerce_to_string[_for_derivation]`'s
         * own `outPath` lookup) ultimately resolve to exactly this
         * string Value. Attaching `Built(drv_path, output_names[o])`
         * here is what lets a LATER derivation's attribute coercion
         * (string interpolation, not just a bare attrset/Path) recover
         * this dependency -- see string_context.h's header comment for
         * why this matters (hello.drv's own `builder` attribute reaches
         * `bash` only this way). `vals[0]` (`drvPath` itself) stays
         * context-free -- that's real Nix's separate `DrvDeep` context
         * (referencing the WHOLE closure), explicitly out of scope,
         * same as M74's own structural collector never handled it. */
        AsnixStringContext out_ctx;
        asnix_ctx_init(&out_ctx);
        asnix_ctx_add_built(&out_ctx, drv_path, output_names[o]);
        asnix_string_attach_context(vals[1 + o], &out_ctx);
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
