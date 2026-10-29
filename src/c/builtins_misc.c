#include "builtins_misc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "base32.h"
#include "builtins_shared.h"
#include "hash_shim.h"
#include "store_path.h"
#include "string_context.h"
#include "value_util.h"

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

static void fatal(const char *msg) {
    fprintf(stderr, "asnix: %s\n", msg);
    exit(1);
}

static void as_string(Value v, const char **bytes_out, int64_t *len_out, const char *who) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: %s: expected a string argument\n", who);
        exit(1);
    }
    const char *s = (const char *)(v.raw & ~0x7ULL);
    *len_out = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    *bytes_out = s + ASNIX_STRING_OFF_BYTES;
}

static Value make_curried_native(void *native_fn, int64_t arity) {
    Value native_primop = make_primop_value(native_fn, arity);
    Value empty_formals = asnix_bshared_native_formals_marker();

    IrBuilder *b = ib_new();
    ib_push_const(b, (int64_t)native_primop.raw);
    for (int64_t depth = arity - 1; depth >= 0; depth--) ib_var_ref(b, depth, 0);
    ib_call(b, arity);
    ib_ret(b);
    IrProgram *prog = ib_finish(b);

    for (int64_t i = arity - 1; i >= 1; i--) {
        IrBuilder *wrap_b = ib_new();
        ib_lambda(wrap_b, 1, prog, (int64_t)empty_formals.raw);
        ib_ret(wrap_b);
        prog = ib_finish(wrap_b);
    }

    void *mem = asnix_arena_alloc(ASNIX_CLOSURE_SIZE);
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_UP) = NULL;
    *(IrProgram **)((char *)mem + ASNIX_CLOSURE_OFF_PROG) = prog;
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

static Value substring3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value start_v = args[0], len_v = args[1], str_v = args[2];
    if (asnix_tag_of(start_v) != ASNIX_TAG_INT || asnix_tag_of(len_v) != ASNIX_TAG_INT)
        fatal("substring: start and len must be integers");
    int64_t start = asnix_int_value(start_v);
    int64_t len = asnix_int_value(len_v);
    if (start < 0) fatal("negative start position in 'substring'");

    const char *bytes;
    int64_t slen;
    as_string(str_v, &bytes, &slen, "substring");

    int64_t start_n = start > slen ? slen : start;
    int64_t actual_len = len < 0 ? slen - start_n : (start_n + len > slen ? slen - start_n : len);
    Value out = asnix_make_string_value(bytes + start_n, (size_t)actual_len);
    const AsnixStringContext *ctx = asnix_string_context_of(str_v);
    if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
    return out;
}

static Value build_substring(void) {
    return make_curried_native((void *)substring3_impl, 3);
}

static Value prim_stringLength(const Value *args, int64_t argc) {
    (void)argc;
    const char *bytes;
    int64_t len;
    as_string(args[0], &bytes, &len, "stringLength");
    return asnix_make_int(len);
}

static Value build_stringLength(void) {
    return make_primop_value((void *)prim_stringLength, 1);
}

static Value concatStringsSep2_impl(const Value *args, int64_t argc) {
    (void)argc;
    const char *sep_bytes;
    int64_t sep_len;
    as_string(args[0], &sep_bytes, &sep_len, "concatStringsSep");

    Value list_v = args[1];
    if (asnix_tag_of(list_v) != ASNIX_TAG_LIST) fatal("concatStringsSep: second argument must be a list");
    const char *list_base = (const char *)(list_v.raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(list_base + ASNIX_LIST_OFF_COUNT);
    const Value *elems = (const Value *)(list_base + ASNIX_LIST_OFF_ELEMS);

    int64_t total = 0;
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    Value *forced = malloc((size_t)(count > 0 ? count : 1) * sizeof(Value));
    for (int64_t i = 0; i < count; i++) {
        const char *b;
        int64_t l;
        forced[i] = asnix_coerce_to_string_for_concat_sep(asnix_force_for_print(elems[i]));
        as_string(forced[i], &b, &l, "concatStringsSep");
        asnix_ctx_union_into(&ctx, asnix_string_context_of(forced[i]));
        total += l;
        if (i + 1 < count) total += sep_len;
    }

    char *out = asnix_arena_alloc((uint64_t)total);
    char *cur = out;
    for (int64_t i = 0; i < count; i++) {
        const char *b;
        int64_t l;
        as_string(forced[i], &b, &l, "concatStringsSep");
        memcpy(cur, b, (size_t)l);
        cur += l;
        if (i + 1 < count) {
            memcpy(cur, sep_bytes, (size_t)sep_len);
            cur += sep_len;
        }
    }
    free(forced);
    Value result = asnix_make_string_value(out, (size_t)total);
    asnix_string_attach_context(result, &ctx);
    return result;
}

static Value build_concatStringsSep(void) {
    return make_curried_native((void *)concatStringsSep2_impl, 2);
}

static Value addErrorContext2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return args[1];
}
static Value build_addErrorContext(void) {
    return make_curried_native((void *)addErrorContext2_impl, 2);
}

static Value prim_isNull(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_IMM && asnix_imm_subtag(args[0]) == ASNIX_IMM_NULL
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isString(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_PTR && !asnix_value_is_path(args[0]) &&
                                   !asnix_value_is_float(args[0])
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isPath(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_PTR && asnix_value_is_path(args[0])
                               ? ASNIX_IMM_TRUE
                               : ASNIX_IMM_FALSE);
}
static Value prim_isFloat(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_value_is_float(args[0]) ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isList(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_LIST ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isAttrs(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_ATTRS ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isFunction(const Value *args, int64_t argc) {
    (void)argc;
    int tag = asnix_tag_of(args[0]);
    return asnix_make_imm(tag == ASNIX_TAG_CLOSURE || tag == ASNIX_TAG_PRIMOP ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}

static Value prim_functionArgs(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) != ASNIX_TAG_CLOSURE) return asnix_bshared_make_attrs(NULL, NULL, 0);
    const char *base = (const char *)(args[0].raw & ~0x7ULL);
    return *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
}
static Value prim_isInt(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_make_imm(asnix_tag_of(args[0]) == ASNIX_TAG_INT ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}
static Value prim_isBool(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) != ASNIX_TAG_IMM) return asnix_make_imm(ASNIX_IMM_FALSE);
    int sub = asnix_imm_subtag(args[0]);
    return asnix_make_imm(sub == ASNIX_IMM_TRUE || sub == ASNIX_IMM_FALSE ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
}

static Value prim_floor(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    if (asnix_tag_of(v) == ASNIX_TAG_INT) return v;
    if (!asnix_value_is_float(v)) fatal("floor: argument is not a number");
    double d = asnix_float_value(v);
    int64_t t = (int64_t)d;
    if (d < 0 && (double)t != d) t -= 1;
    return asnix_make_int(t);
}
static Value prim_ceil(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    if (asnix_tag_of(v) == ASNIX_TAG_INT) return v;
    if (!asnix_value_is_float(v)) fatal("ceil: argument is not a number");
    double d = asnix_float_value(v);
    int64_t t = (int64_t)d;
    if (d > 0 && (double)t != d) t += 1;
    return asnix_make_int(t);
}

static Value prim_baseNameOf(const Value *args, int64_t argc) {
    (void)argc;
    int64_t len;
    const char *bytes = asnix_ptr_bytes(args[0], &len);
    if (len > 0 && bytes[len - 1] == '/') len--;
    int64_t start = len;
    while (start > 0 && bytes[start - 1] != '/') start--;
    Value out = asnix_make_string_value(bytes + start, (size_t)(len - start));
    const AsnixStringContext *ctx = asnix_string_context_of(args[0]);
    if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
    return out;
}

static Value prim_dirOf(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    int64_t len;
    const char *bytes = asnix_ptr_bytes(v, &len);
    int is_path = asnix_value_is_path(v);
    if (is_path) {
        if (len == 1 && bytes[0] == '/') return asnix_make_path_value("/", 1);
        int64_t pos = len;
        while (pos > 0 && bytes[pos - 1] != '/') pos--;
        pos--;
        if (pos <= 0) return asnix_make_path_value("/", 1);
        return asnix_make_path_value(bytes, (size_t)pos);
    }
    int64_t pos = len;
    while (pos > 0 && bytes[pos - 1] != '/') pos--;
    const AsnixStringContext *ctx = asnix_string_context_of(v);
    if (pos == 0) {
        Value out = asnix_make_string_value(".", 1);
        if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
        return out;
    }
    pos--;
    if (pos == 0) {
        Value out = asnix_make_string_value("/", 1);
        if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
        return out;
    }
    Value out = asnix_make_string_value(bytes, (size_t)pos);
    if (!asnix_ctx_is_empty(ctx)) asnix_string_attach_context(out, ctx);
    return out;
}

static Value build_baseNameOf(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
static Value build_dirOf(void) { return make_primop_value((void *)prim_dirOf, 1); }
Value asnix_make_baseNameOf_primop(void) { return make_primop_value((void *)prim_baseNameOf, 1); }
Value asnix_make_dirOf_primop(void) { return make_primop_value((void *)prim_dirOf, 1); }

static Value prim_placeholder(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "placeholder");
    int64_t name_len;
    const char *name_bytes;
    asnix_bshared_string_bytes(args[0], &name_bytes, &name_len, "placeholder");

    size_t preimage_len = 11 + (size_t)name_len;
    char *preimage = malloc(preimage_len);
    memcpy(preimage, "nix-output:", 11);
    memcpy(preimage + 11, name_bytes, (size_t)name_len);

    uint8_t digest[ASNIX_HASH_SHA256_LEN];
    int64_t rc = asnix_hash_sha256(preimage, preimage_len, digest);
    free(preimage);
    if (rc != 0) {
        fprintf(stderr, "asnix: placeholder: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    size_t b32_len = asnix_base32_encoded_length(ASNIX_HASH_SHA256_LEN);
    char *out = malloc(1 + b32_len);
    out[0] = '/';
    asnix_base32_encode(digest, ASNIX_HASH_SHA256_LEN, out + 1);
    Value result = asnix_make_string_value(out, 1 + b32_len);
    free(out);
    return result;
}
static Value build_placeholder(void) { return make_primop_value((void *)prim_placeholder, 1); }
Value asnix_make_placeholder_primop(void) { return make_primop_value((void *)prim_placeholder, 1); }

static Value prim_toFile(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "toFile");
    ASNIX_REQUIRE_STRING(args[1], "toFile");

    char *name = asnix_string_value_to_cstr(args[0]);
    asnix_check_store_path_name(name);

    int64_t contents_len;
    const char *contents_bytes;
    asnix_bshared_string_bytes(args[1], &contents_bytes, &contents_len, "toFile");

    const AsnixStringContext *contents_ctx = asnix_string_context_of(args[1]);
    if (contents_ctx && (contents_ctx->ndrvs > 0 || contents_ctx->ndeep > 0)) {
        fprintf(stderr,
                "asnix: toFile: files created by builtins.toFile may not reference derivations, but '%s' does\n",
                name);
        exit(1);
    }
    size_t nrefs = contents_ctx ? contents_ctx->nsrcs : 0;
    char **refs = NULL;
    if (nrefs > 0) {
        refs = malloc(nrefs * sizeof(char *));
        memcpy(refs, contents_ctx->srcs, nrefs * sizeof(char *));
        for (size_t i = 1; i < nrefs; i++) {
            size_t j = i;
            while (j > 0 && strcmp(refs[j - 1], refs[j]) > 0) {
                char *tmp = refs[j - 1];
                refs[j - 1] = refs[j];
                refs[j] = tmp;
                j--;
            }
        }
    }

    uint8_t inner_digest[ASNIX_HASH_SHA256_LEN];
    int64_t rc = asnix_hash_sha256(contents_bytes, (size_t)contents_len, inner_digest);
    if (rc != 0) {
        fprintf(stderr, "asnix: toFile: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < ASNIX_HASH_SHA256_LEN; i++) {
        hashstr[7 + 2 * i] = hexdigits[inner_digest[i] >> 4];
        hashstr[7 + 2 * i + 1] = hexdigits[inner_digest[i] & 0xf];
    }
    hashstr[7 + 64] = '\0';

    char *type_str = malloc(4 + nrefs * 300 + 1);
    size_t type_len = 4;
    memcpy(type_str, "text", 4);
    for (size_t i = 0; i < nrefs; i++) {
        type_str[type_len++] = ':';
        size_t rl = strlen(refs[i]);
        memcpy(type_str + type_len, refs[i], rl);
        type_len += rl;
    }
    type_str[type_len] = '\0';

    char preimage[4096];
    size_t n = asnix_store_path_preimage(type_str, hashstr, name, preimage, sizeof(preimage));
    free(type_str);
    free(refs);
    if (n == 0) {
        fprintf(stderr, "asnix: toFile: preimage too long\n");
        exit(1);
    }

    uint8_t outer_digest[ASNIX_HASH_SHA256_LEN];
    rc = asnix_hash_sha256(preimage, n, outer_digest);
    if (rc != 0) {
        fprintf(stderr, "asnix: toFile: failed to load libcrypto (rc=%lld) -- enter the flake devShell\n",
                (long long)rc);
        exit(1);
    }

    char out_path[300];
    if (asnix_make_store_path_from_digest(outer_digest, name, out_path, sizeof(out_path)) == 0) {
        fprintf(stderr, "asnix: toFile: computed store path too long\n");
        exit(1);
    }
    Value out = asnix_make_string_value(out_path, strlen(out_path));
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    asnix_ctx_add_opaque(&ctx, out_path);
    asnix_string_attach_context(out, &ctx);
    return out;
}
static Value build_toFile(void) { return make_curried_native((void *)prim_toFile, 2); }

static Value build_isNull(void) { return make_primop_value((void *)prim_isNull, 1); }

Value asnix_make_isNull_primop(void) { return make_primop_value((void *)prim_isNull, 1); }
static Value build_isString(void) { return make_primop_value((void *)prim_isString, 1); }
static Value build_isPath(void) { return make_primop_value((void *)prim_isPath, 1); }
static Value build_functionArgs(void) { return make_primop_value((void *)prim_functionArgs, 1); }
static Value build_isList(void) { return make_primop_value((void *)prim_isList, 1); }
static Value build_isAttrs(void) { return make_primop_value((void *)prim_isAttrs, 1); }
static Value build_isFunction(void) { return make_primop_value((void *)prim_isFunction, 1); }
static Value build_isInt(void) { return make_primop_value((void *)prim_isInt, 1); }
static Value build_isBool(void) { return make_primop_value((void *)prim_isBool, 1); }
static Value build_isFloat(void) { return make_primop_value((void *)prim_isFloat, 1); }
static Value build_floor(void) { return make_primop_value((void *)prim_floor, 1); }
static Value build_ceil(void) { return make_primop_value((void *)prim_ceil, 1); }

static Value build_nixVersion(void) { return asnix_make_string_value("2.35.2", 6); }

static Value build_currentSystem(void) { return asnix_make_string_value("x86_64-linux", 12); }

static Value build_storeDir(void) { return asnix_make_string_value("/nix/store", 10); }

static Value build_langVersion(void) { return asnix_make_int(6); }

const AsnixBuiltinMiscDef ASNIX_BUILTINS_MISC[] = {
    { "substring", build_substring },
    { "stringLength", build_stringLength },
    { "concatStringsSep", build_concatStringsSep },
    { "isNull", build_isNull },
    { "isString", build_isString },
    { "isPath", build_isPath },
    { "functionArgs", build_functionArgs },
    { "addErrorContext", build_addErrorContext },
    { "isList", build_isList },
    { "isAttrs", build_isAttrs },
    { "isFunction", build_isFunction },
    { "isInt", build_isInt },
    { "isBool", build_isBool },
    { "isFloat", build_isFloat },
    { "floor", build_floor },
    { "ceil", build_ceil },
    { "nixVersion", build_nixVersion },
    { "currentSystem", build_currentSystem },
    { "storeDir", build_storeDir },
    { "langVersion", build_langVersion },
    { "baseNameOf", build_baseNameOf },
    { "dirOf", build_dirOf },
    { "placeholder", build_placeholder },
    { "toFile", build_toFile },
};

const size_t ASNIX_BUILTINS_MISC_COUNT = sizeof(ASNIX_BUILTINS_MISC) / sizeof(ASNIX_BUILTINS_MISC[0]);

