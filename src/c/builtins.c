#include "builtins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "hash_shim.h"
#include "symtab.h"

static Value make_string(const char *s, size_t len) {
    void *mem = asnix_arena_alloc(8 + len);
    *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + ASNIX_STRING_OFF_BYTES, s, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

static Value prim_toString(const Value *args, int64_t argc) {
    (void)argc;
    Value v = args[0];
    char buf[32];
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: {
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)asnix_int_value(v));
            return make_string(buf, (size_t)n);
        }
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: return make_string("true", 4);
                case ASNIX_IMM_FALSE: return make_string("false", 5);
                case ASNIX_IMM_NULL: return make_string("", 0); /* matches real Nix */
                default: break;
            }
            break;
        case ASNIX_TAG_PTR:
            return v; /* already a string */
        default: break;
    }
    fprintf(stderr, "asnix: toString: unsupported value type\n");
    exit(1);
}

static Value prim_typeOf(const Value *args, int64_t argc) {
    (void)argc;
    switch (asnix_tag_of(args[0])) {
        case ASNIX_TAG_INT: return make_string("int", 3);
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(args[0])) {
                case ASNIX_IMM_TRUE:
                case ASNIX_IMM_FALSE: return make_string("bool", 4);
                case ASNIX_IMM_NULL: return make_string("null", 4);
                default: return make_string("imm", 3);
            }
        case ASNIX_TAG_PTR: return make_string("string", 6);
        case ASNIX_TAG_LIST: return make_string("list", 4);
        case ASNIX_TAG_ATTRS: return make_string("set", 3);
        case ASNIX_TAG_CLOSURE:
        case ASNIX_TAG_PRIMOP: return make_string("lambda", 6);
        default: return make_string("unknown", 7);
    }
}

static Value prim_length(const Value *args, int64_t argc) {
    (void)argc;
    if (asnix_tag_of(args[0]) != ASNIX_TAG_LIST) {
        fprintf(stderr, "asnix: length: argument is not a list\n");
        exit(1);
    }
    const char *base = (const char *)(args[0].raw & ~0x7ULL);
    int64_t count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
    return asnix_make_int(count);
}

static Value make_primop_value(void *fn, int64_t arity) {
    void *mem = asnix_arena_alloc(ASNIX_PRIMOP_SIZE);
    *(void **)((char *)mem + ASNIX_PRIMOP_OFF_FN) = fn;
    *(int64_t *)((char *)mem + ASNIX_PRIMOP_OFF_ARITY) = arity;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PRIMOP };
}

/*
 * builtins.hashString algo s -- M8. Arity in real Nix is 2, but that can't
 * be registered directly as a single arity-2 BuiltinDef here: parse_app
 * (parser.c) always desugars multi-argument application into nested
 * single-argument AST_APP nodes (`f x y` => App(App(f,x),y), exactly like
 * real Nix's own curried-application grammar), and compiler.c's AST_APP
 * case always emits `ib_call(b, 1)` -- so `builtins.hashString "sha256"
 * "hello"` reaches OP_CALL as *two* separate argc=1 calls, not one argc=2
 * call. OP_CALL's primop branch (op_call.S) requires an exact arity match
 * per call and hands the native fn nothing but `(args, argc)` -- no
 * self/closure pointer, so there is no channel to carry data (like which
 * algorithm was picked) from a first call into a second one (see
 * docs/abi.md's "Primop calls (M7)" section: "a plain C function, nothing
 * asnix-specific about its own convention").
 *
 * The fix mirrors how curried *user* lambdas already get multi-arg calls
 * to work in this codebase: `a: b: body` is two nested arity-1 closures,
 * where calling the outer one returns a new callable (the inner closure)
 * that already has `a` baked into its captured env. `prim_hashString`
 * does the native-code equivalent: it's itself arity 1, takes just the
 * algorithm name, and -- since there are only 4 supported algorithms,
 * known statically -- returns one of 4 dedicated arity-1 PrimOps
 * (`prim_hashString_apply_{md5,sha1,sha256,sha512}`), each with the
 * chosen algorithm already baked into *which function pointer it is*
 * rather than any captured runtime data. No mutable global, no
 * extra-data-after-the-PrimOp-struct trick, no asm changes needed.
 */
static Value hash_apply(const Value *args, int64_t (*hash_fn)(const void *, size_t, unsigned char *), size_t raw_len) {
    Value in = args[0];
    if (asnix_tag_of(in) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: hashString: second argument must be a string\n");
        exit(1);
    }
    const char *s = (const char *)(in.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    const char *bytes = s + ASNIX_STRING_OFF_BYTES;

    unsigned char raw[ASNIX_HASH_SHA512_LEN]; /* largest digest; raw_len is always <= this */
    int64_t rc = hash_fn(bytes, (size_t)len, raw);
    if (rc != 0) {
        fprintf(stderr, "asnix: hashString: failed to load libcrypto (rc=%lld) -- "
                         "enter the flake devShell (needs ASNIX_LIBCRYPTO_LIB / openssl)\n",
                (long long)rc);
        exit(1);
    }

    char hex[2 * ASNIX_HASH_SHA512_LEN];
    asnix_hash_to_hex(raw, raw_len, hex);
    return make_string(hex, 2 * raw_len);
}

static Value prim_hashString_apply_md5(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, asnix_hash_md5, ASNIX_HASH_MD5_LEN);
}
static Value prim_hashString_apply_sha1(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, asnix_hash_sha1, ASNIX_HASH_SHA1_LEN);
}
static Value prim_hashString_apply_sha256(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, asnix_hash_sha256, ASNIX_HASH_SHA256_LEN);
}
static Value prim_hashString_apply_sha512(const Value *args, int64_t argc) {
    (void)argc;
    return hash_apply(args, asnix_hash_sha512, ASNIX_HASH_SHA512_LEN);
}

static Value prim_hashString(const Value *args, int64_t argc) {
    (void)argc;
    Value algo = args[0];
    if (asnix_tag_of(algo) != ASNIX_TAG_PTR) {
        fprintf(stderr, "asnix: hashString: first argument (algorithm name) must be a string\n");
        exit(1);
    }
    const char *s = (const char *)(algo.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    const char *name = s + ASNIX_STRING_OFF_BYTES;

    if (len == 3 && memcmp(name, "md5", 3) == 0) return make_primop_value((void *)prim_hashString_apply_md5, 1);
    if (len == 4 && memcmp(name, "sha1", 4) == 0) return make_primop_value((void *)prim_hashString_apply_sha1, 1);
    if (len == 6 && memcmp(name, "sha256", 6) == 0) return make_primop_value((void *)prim_hashString_apply_sha256, 1);
    if (len == 6 && memcmp(name, "sha512", 6) == 0) return make_primop_value((void *)prim_hashString_apply_sha512, 1);

    fprintf(stderr, "asnix: hashString: unsupported algorithm \"%.*s\" (expected md5/sha1/sha256/sha512)\n",
            (int)len, name);
    exit(1);
}

typedef Value (*PrimFn)(const Value *, int64_t);

typedef struct {
    const char *name;
    PrimFn fn;
    int64_t arity;
} BuiltinDef;

static const BuiltinDef BUILTINS[] = {
    { "toString", prim_toString, 1 },
    { "typeOf", prim_typeOf, 1 },
    { "length", prim_length, 1 },
    { "hashString", prim_hashString, 1 }, /* true arity 2, curried -- see hash_apply's comment above */
};

Value asnix_make_builtins_attrs(void) {
    size_t n = sizeof(BUILTINS) / sizeof(BUILTINS[0]);
    uint32_t syms[sizeof(BUILTINS) / sizeof(BUILTINS[0])];
    size_t order[sizeof(BUILTINS) / sizeof(BUILTINS[0])];
    for (size_t i = 0; i < n; i++) {
        syms[i] = asnix_intern(BUILTINS[i].name, strlen(BUILTINS[i].name));
        order[i] = i;
    }
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        while (j > 0 && syms[order[j - 1]] > syms[order[j]]) {
            size_t tmp = order[j - 1];
            order[j - 1] = order[j];
            order[j] = tmp;
            j--;
        }
    }

    void *mem = asnix_arena_alloc(8 + n * ASNIX_ATTR_SIZE);
    *(int64_t *)((char *)mem + ASNIX_ATTRS_OFF_COUNT) = (int64_t)n;
    for (size_t i = 0; i < n; i++) {
        char *entry = (char *)mem + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
        *(int64_t *)(entry + ASNIX_ATTR_OFF_SYM) = (int64_t)syms[order[i]]; /* zero-extends; see heap.h */
        *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) =
            make_primop_value((void *)BUILTINS[order[i]].fn, BUILTINS[order[i]].arity);
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_ATTRS };
}
