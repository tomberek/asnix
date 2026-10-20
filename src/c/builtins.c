#include "builtins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
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
