#define _DEFAULT_SOURCE

#include "value_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stats.h"
#include "builtins_shared.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"

#define ASNIX_PTR_BASE_MASK 0x0000FFFFFFFFFFF8ULL

Value asnix_make_string_value(const char *s, size_t len) {
    void *mem = asnix_arena_alloc(8 + len);
    asnix_stats_string++;
    *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + ASNIX_STRING_OFF_BYTES, s, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

Value asnix_make_path_value(const char *abs_path, size_t len) {
    void *mem = asnix_arena_alloc(16 + len);
    asnix_stats_path++;
    *(int64_t *)((char *)mem + ASNIX_PATH_OFF_MARKER) = ASNIX_PATH_MARKER;
    *(int64_t *)((char *)mem + ASNIX_PATH_OFF_LEN) = (int64_t)len;
    memcpy((char *)mem + ASNIX_PATH_OFF_BYTES, abs_path, len);
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

Value asnix_make_float_value(double d) {
    void *mem = asnix_arena_alloc(16);
    *(int64_t *)((char *)mem + ASNIX_FLOAT_OFF_MARKER) = ASNIX_FLOAT_MARKER;
    *(double *)((char *)mem + ASNIX_FLOAT_OFF_VALUE) = d;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

double asnix_float_value(Value v) {
    const char *base = (const char *)(v.raw & ASNIX_PTR_BASE_MASK);
    return *(const double *)(base + ASNIX_FLOAT_OFF_VALUE);
}

Value asnix_make_cell_value(Value *target) {
    void *mem = asnix_arena_alloc(16);
    asnix_stats_cell++;
    *(int64_t *)((char *)mem + ASNIX_CELL_OFF_MARKER) = ASNIX_CELL_MARKER;
    *(Value **)((char *)mem + ASNIX_CELL_OFF_TARGET) = target;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

Value *asnix_cell_target(Value v) {
    const char *base = (const char *)(v.raw & ASNIX_PTR_BASE_MASK);
    return *(Value **)(base + ASNIX_CELL_OFF_TARGET);
}

Value asnix_resolve_cell_chain(Value v) {
    int64_t i = 0;
    while (asnix_value_is_cell(v)) {
        if (++i > ASNIX_CELL_CHASE_LIMIT) {
            static const char msg[] = "asnix: infinite recursion forcing a thunk (blackhole), aborting\n";
            fwrite(msg, 1, sizeof(msg) - 1, stderr);
            exit(1);
        }
        v = *asnix_cell_target(v);
    }
    return v;
}

Value asnix_make_lazyapp1(Value fn, Value arg1) {
    void *mem = asnix_arena_alloc(ASNIX_LAZYAPP_SIZE);
    asnix_stats_lazyapp++;
    *(int64_t *)((char *)mem + ASNIX_LAZYAPP_OFF_MARKER) = ASNIX_LAZYAPP_MARKER;
    *(Value *)((char *)mem + ASNIX_LAZYAPP_OFF_FN) = fn;
    *(Value *)((char *)mem + ASNIX_LAZYAPP_OFF_ARG1) = arg1;
    *(Value *)((char *)mem + ASNIX_LAZYAPP_OFF_ARG2) = (Value){ .raw = 0 };
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

Value asnix_make_lazyapp2(Value fn, Value arg1, Value arg2) {
    void *mem = asnix_arena_alloc(ASNIX_LAZYAPP_SIZE);
    asnix_stats_lazyapp++;
    *(int64_t *)((char *)mem + ASNIX_LAZYAPP_OFF_MARKER) = ASNIX_LAZYAPP_MARKER;
    *(Value *)((char *)mem + ASNIX_LAZYAPP_OFF_FN) = fn;
    *(Value *)((char *)mem + ASNIX_LAZYAPP_OFF_ARG1) = arg1;
    *(Value *)((char *)mem + ASNIX_LAZYAPP_OFF_ARG2) = arg2;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
}

int asnix_value_is_lazyapp(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return 0;
    const char *base = (const char *)(v.raw & ASNIX_PTR_BASE_MASK);
    int64_t marker = *(const int64_t *)(base + ASNIX_LAZYAPP_OFF_MARKER);
    return marker == ASNIX_LAZYAPP_MARKER || marker == ASNIX_LAZYAPP_FORCED_MARKER;
}

Value asnix_resolve_lazyapp(Value v) {
    char *base = (char *)(v.raw & ASNIX_PTR_BASE_MASK);
    int64_t marker = *(int64_t *)(base + ASNIX_LAZYAPP_OFF_MARKER);
    if (marker == ASNIX_LAZYAPP_FORCED_MARKER) {
        return *(Value *)(base + ASNIX_LAZYAPP_OFF_RESULT);
    }
    Value fn = *(Value *)(base + ASNIX_LAZYAPP_OFF_FN);
    Value arg1 = *(Value *)(base + ASNIX_LAZYAPP_OFF_ARG1);
    Value arg2 = *(Value *)(base + ASNIX_LAZYAPP_OFF_ARG2);
    Value result = (arg2.raw == 0) ? asnix_apply(fn, &arg1, 1) : asnix_bshared_apply2(fn, arg1, arg2);
    *(int64_t *)(base + ASNIX_LAZYAPP_OFF_MARKER) = ASNIX_LAZYAPP_FORCED_MARKER;
    *(Value *)(base + ASNIX_LAZYAPP_OFF_RESULT) = result;
    return result;
}

static void *thunkmap_desc_of(Value v) {
    return (void *)(uintptr_t)(v.raw & ASNIX_PTR_BASE_MASK);
}

static int64_t thunkmap_idx_of(Value v) {
    return (int64_t)(v.raw >> 48);
}

void *asnix_thunkmap_make_desc(Value f, const void *src_entries) {
    void *mem = asnix_arena_alloc(ASNIX_THUNKMAP_SIZE);
    asnix_stats_thunkmap_desc++;
    *(int64_t *)((char *)mem + ASNIX_THUNKMAP_OFF_MARKER) = ASNIX_THUNKMAP_MARKER;
    *(Value *)((char *)mem + ASNIX_THUNKMAP_OFF_F) = f;
    *(const void **)((char *)mem + ASNIX_THUNKMAP_OFF_SRC_ENTRIES) = src_entries;
    *(Value *)((char *)mem + ASNIX_THUNKMAP_OFF_OUT) = (Value){ .raw = 0 };
    return mem;
}

Value asnix_thunkmap_pack(void *desc, int64_t idx) {
    asnix_stats_thunkmap_elem++;
    uint64_t p = (uint64_t)(uintptr_t)desc;
    return (Value){ .raw = p | ((uint64_t)idx << 48) | ASNIX_TAG_PTR };
}

void asnix_thunkmap_set_out(void *desc, Value out) {
    *(Value *)((char *)desc + ASNIX_THUNKMAP_OFF_OUT) = out;
}

int asnix_value_is_thunkmap(Value v) {
    if (asnix_tag_of(v) != ASNIX_TAG_PTR) return 0;
    const void *base = thunkmap_desc_of(v);
    return *(const int64_t *)((const char *)base + ASNIX_THUNKMAP_OFF_MARKER) == ASNIX_THUNKMAP_MARKER;
}

Value *asnix_thunkmap_slot(Value v) {
    void *desc = thunkmap_desc_of(v);
    int64_t idx = thunkmap_idx_of(v);
    Value out = *(Value *)((char *)desc + ASNIX_THUNKMAP_OFF_OUT);
    char *entries = (char *)(uintptr_t)(out.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_ENTRIES;
    return (Value *)(entries + idx * ASNIX_ATTR_SIZE + ASNIX_ATTR_OFF_VALUE);
}

Value asnix_resolve_thunkmap(Value v) {
    Value *slot = asnix_thunkmap_slot(v);
    if (slot->raw != v.raw) {
        return *slot;
    }
    void *desc = thunkmap_desc_of(v);
    int64_t idx = thunkmap_idx_of(v);
    Value f = *(Value *)((char *)desc + ASNIX_THUNKMAP_OFF_F);
    const char *src_entries = *(const char **)((char *)desc + ASNIX_THUNKMAP_OFF_SRC_ENTRIES);
    const char *src_entry = src_entries + idx * ASNIX_ATTR_SIZE;
    uint32_t sym = *(const uint32_t *)(src_entry + ASNIX_ATTR_OFF_SYM);
    Value value = *(const Value *)(src_entry + ASNIX_ATTR_OFF_VALUE);
    size_t len;
    const char *name = asnix_sym_name(sym, &len);
    Value name_val = asnix_make_string_value(name, len);
    Value result = asnix_bshared_apply2(f, name_val, value);
    *slot = result;
    return result;
}

static int as_double(Value v, double *out) {
    if (asnix_value_is_float(v)) {
        *out = asnix_float_value(v);
        return 1;
    }
    if (asnix_tag_of(v) == ASNIX_TAG_INT) {
        *out = (double)asnix_int_value(v);
        return 1;
    }
    return 0;
}

const char *asnix_ptr_bytes(Value v, int64_t *out_len) {
    const char *base = (const char *)(v.raw & ~0x7ULL);
    if (asnix_value_is_path(v)) {
        *out_len = *(const int64_t *)(base + ASNIX_PATH_OFF_LEN);
        return base + ASNIX_PATH_OFF_BYTES;
    }
    *out_len = *(const int64_t *)(base + ASNIX_STRING_OFF_LEN);
    return base + ASNIX_STRING_OFF_BYTES;
}

Value asnix_coerce_to_string(Value v) {
    char buf[32];
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: {
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)asnix_int_value(v));
            return asnix_make_string_value(buf, (size_t)n);
        }
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: return asnix_make_string_value("1", 1);
                case ASNIX_IMM_FALSE: return asnix_make_string_value("", 0);
                case ASNIX_IMM_NULL: return asnix_make_string_value("", 0);
                default: break;
            }
            break;
        case ASNIX_TAG_PTR: {
            if (asnix_value_is_float(v)) {
                char buf[512];
                int n = snprintf(buf, sizeof(buf), "%f", asnix_float_value(v));
                return asnix_make_string_value(buf, (size_t)n);
            }
            if (!asnix_value_is_path(v)) return v;
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            return asnix_make_string_value(bytes, (size_t)len);
        }
        case ASNIX_TAG_ATTRS: {
            uint32_t sym_tostring = asnix_intern("__toString", 10);
            int64_t idx = asnix_bshared_attrs_find(v, sym_tostring);
            if (idx >= 0) {
                Value fn = asnix_force_for_print(asnix_bshared_attrs_value(v, idx));
                Value result = asnix_apply(fn, &v, 1);
                return asnix_coerce_to_string(asnix_force_for_print(result));
            }
            uint32_t sym_outpath = asnix_intern("outPath", 7);
            idx = asnix_bshared_attrs_find(v, sym_outpath);
            if (idx >= 0) {
                return asnix_coerce_to_string(asnix_force_for_print(asnix_bshared_attrs_value(v, idx)));
            }
            break;
        }
        case ASNIX_TAG_LIST: {
            int64_t count = asnix_bshared_list_count(v);
            const Value *elems = asnix_bshared_list_elems(v);
            char *result = NULL;
            size_t result_len = 0;
            AsnixStringContext ctx;
            asnix_ctx_init(&ctx);
            for (int64_t i = 0; i < count; i++) {
                Value piece = asnix_coerce_to_string(asnix_force_for_print(elems[i]));
                asnix_ctx_union_into(&ctx, asnix_string_context_of(piece));
                int64_t piece_len;
                const char *piece_bytes = asnix_ptr_bytes(piece, &piece_len);
                int has_sep = (i + 1 < count);
                result = realloc(result, result_len + (size_t)piece_len + (has_sep ? 1 : 0));
                memcpy(result + result_len, piece_bytes, (size_t)piece_len);
                result_len += (size_t)piece_len;
                if (has_sep) result[result_len++] = ' ';
            }
            Value out = asnix_make_string_value(result ? result : "", result_len);
            free(result);
            asnix_string_attach_context(out, &ctx);
            return out;
        }
        default: break;
    }
    fprintf(stderr, "asnix: cannot coerce this value to a string\n");
    exit(1);
}

Value asnix_coerce_to_string_for_derivation(Value v) {
    if (asnix_tag_of(v) == ASNIX_TAG_PTR && asnix_value_is_path(v)) {
        int64_t len;
        const char *bytes = asnix_ptr_bytes(v, &len);
        char *nul_terminated = malloc((size_t)len + 1);
        memcpy(nul_terminated, bytes, (size_t)len);
        nul_terminated[len] = '\0';
        if (len > 11 && memcmp(nul_terminated, "/nix/store/", 11) == 0) {
            Value out = asnix_make_string_value(nul_terminated, (size_t)len);
            AsnixStringContext ctx;
            asnix_ctx_init(&ctx);
            asnix_ctx_add_opaque(&ctx, nul_terminated);
            asnix_string_attach_context(out, &ctx);
            free(nul_terminated);
            return out;
        }
        const char *already = asnix_copy_path_to_store_cache_lookup(nul_terminated);
        char store_path[300];
        if (already) {
            snprintf(store_path, sizeof(store_path), "%s", already);
        } else {
            struct stat st;
            if (lstat(nul_terminated, &st) != 0) {
                fprintf(stderr, "asnix: path '%s' does not exist\n", nul_terminated);
                free(nul_terminated);
                exit(1);
            }
            asnix_copy_path_to_store(nul_terminated, store_path, sizeof(store_path));
        }
        free(nul_terminated);
        Value out = asnix_make_string_value(store_path, strlen(store_path));
        AsnixStringContext ctx;
        asnix_ctx_init(&ctx);
        asnix_ctx_add_opaque(&ctx, store_path);
        asnix_string_attach_context(out, &ctx);
        return out;
    }
    if (asnix_tag_of(v) == ASNIX_TAG_ATTRS) {
        uint32_t sym_tostring = asnix_intern("__toString", 10);
        int64_t idx = asnix_bshared_attrs_find(v, sym_tostring);
        if (idx >= 0) {
            Value fn = asnix_force_for_print(asnix_bshared_attrs_value(v, idx));
            Value result = asnix_apply(fn, &v, 1);
            return asnix_coerce_to_string_for_derivation(asnix_force_for_print(result));
        }
        uint32_t sym_outpath = asnix_intern("outPath", 7);
        idx = asnix_bshared_attrs_find(v, sym_outpath);
        if (idx >= 0) {
            return asnix_coerce_to_string_for_derivation(asnix_force_for_print(asnix_bshared_attrs_value(v, idx)));
        }
    }
    if (asnix_tag_of(v) == ASNIX_TAG_LIST) {
        int64_t count = asnix_bshared_list_count(v);
        const Value *elems = asnix_bshared_list_elems(v);
        char *result = NULL;
        size_t result_len = 0;
        AsnixStringContext ctx;
        asnix_ctx_init(&ctx);
        for (int64_t i = 0; i < count; i++) {
            Value piece = asnix_coerce_to_string_for_derivation(asnix_force_for_print(elems[i]));
            asnix_ctx_union_into(&ctx, asnix_string_context_of(piece));
            int64_t piece_len;
            const char *piece_bytes = asnix_ptr_bytes(piece, &piece_len);
            int has_sep = (i + 1 < count);
            result = realloc(result, result_len + (size_t)piece_len + (has_sep ? 1 : 0));
            memcpy(result + result_len, piece_bytes, (size_t)piece_len);
            result_len += (size_t)piece_len;
            if (has_sep) result[result_len++] = ' ';
        }
        Value out = asnix_make_string_value(result ? result : "", result_len);
        free(result);
        asnix_string_attach_context(out, &ctx);
        return out;
    }
    return asnix_coerce_to_string(v);
}

static Value coerce_to_string_for_interp(Value v, int check_path_exists, int copy_to_store) {
    if (asnix_tag_of(v) == ASNIX_TAG_PTR) {
        if (asnix_value_is_path(v)) {
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            char *nul_terminated = malloc((size_t)len + 1);
            memcpy(nul_terminated, bytes, (size_t)len);
            nul_terminated[len] = '\0';
            if (check_path_exists && !(copy_to_store && asnix_copy_path_to_store_cache_lookup(nul_terminated))) {
                struct stat st;
                if (lstat(nul_terminated, &st) != 0) {
                    fprintf(stderr, "asnix: path '%s' does not exist\n", nul_terminated);
                    free(nul_terminated);
                    exit(1);
                }
            }
            if (copy_to_store) {
                int in_store = len > 11 && memcmp(nul_terminated, "/nix/store/", 11) == 0;
                char store_path[300];
                const char *final_path = nul_terminated;
                if (!in_store) {
                    asnix_copy_path_to_store(nul_terminated, store_path, sizeof(store_path));
                    final_path = store_path;
                }
                Value out = asnix_make_string_value(final_path, strlen(final_path));
                AsnixStringContext ctx;
                asnix_ctx_init(&ctx);
                asnix_ctx_add_opaque(&ctx, final_path);
                asnix_string_attach_context(out, &ctx);
                free(nul_terminated);
                return out;
            }
            free(nul_terminated);
            return asnix_coerce_to_string(v);
        }
        if (asnix_value_is_float(v)) {
            fprintf(stderr, "asnix: cannot coerce a float to a string in a string interpolation "
                             "(use builtins.toString explicitly)\n");
            exit(1);
        }
        return v;
    }
    if (asnix_tag_of(v) == ASNIX_TAG_ATTRS) {
        uint32_t sym_tostring = asnix_intern("__toString", 10);
        uint32_t sym_outpath = asnix_intern("outPath", 7);
        if (asnix_bshared_attrs_find(v, sym_tostring) >= 0 || asnix_bshared_attrs_find(v, sym_outpath) >= 0) {
            return asnix_coerce_to_string(v);
        }
    }
    const char *kind = "this value";
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: kind = "an integer"; break;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE:
                case ASNIX_IMM_FALSE: kind = "a boolean"; break;
                case ASNIX_IMM_NULL: kind = "null"; break;
                default: break;
            }
            break;
        case ASNIX_TAG_LIST: kind = "a list"; break;
        case ASNIX_TAG_ATTRS: kind = "a set"; break;
        case ASNIX_TAG_CLOSURE:
        case ASNIX_TAG_PRIMOP: kind = "a function"; break;
        default: break;
    }
    fprintf(stderr, "asnix: cannot coerce %s to a string in a string interpolation "
                     "(use builtins.toString explicitly)\n", kind);
    exit(1);
}

Value asnix_coerce_to_string_for_concat_sep(Value v) {
    return coerce_to_string_for_interp(v, 1, 1);
}

static Value concat_strings_impl(const Value *parts, int64_t count, int check_path_exists, int copy_to_store) {
    if (count <= 0) return asnix_make_string_value("", 0);

    Value *coerced = malloc((size_t)count * sizeof(Value));
    int64_t total_len = 0;
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    for (int64_t i = 0; i < count; i++) {
        coerced[i] = coerce_to_string_for_interp(parts[i], check_path_exists, copy_to_store);
        asnix_ctx_union_into(&ctx, asnix_string_context_of(coerced[i]));
        const char *s = (const char *)(coerced[i].raw & ~0x7ULL);
        total_len += *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    }

    void *mem = asnix_arena_alloc(8 + (size_t)total_len);
    *(int64_t *)((char *)mem + ASNIX_STRING_OFF_LEN) = total_len;
    char *out = (char *)mem + ASNIX_STRING_OFF_BYTES;
    for (int64_t i = 0; i < count; i++) {
        const char *s = (const char *)(coerced[i].raw & ~0x7ULL);
        int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
        memcpy(out, s + ASNIX_STRING_OFF_BYTES, (size_t)len);
        out += len;
    }
    free(coerced);
    Value result = (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_PTR };
    asnix_string_attach_context(result, &ctx);
    return result;
}

static Value canon_wrap_as_path(Value string_result) {
    int64_t len;
    const char *bytes = asnix_ptr_bytes(string_result, &len);
    char *nul_terminated = malloc((size_t)len + 1);
    memcpy(nul_terminated, bytes, (size_t)len);
    nul_terminated[len] = '\0';
    char *canon = asnix_canon_path(nul_terminated);
    free(nul_terminated);
    Value out = asnix_make_path_value(canon, strlen(canon));
    free(canon);
    return out;
}

Value asnix_concat_path(const Value *parts, int64_t count) {
    Value result = concat_strings_impl(parts, count, 0, 0);
    return canon_wrap_as_path(result);
}

Value asnix_concat_strings(const Value *parts, int64_t count) {
    return concat_strings_impl(parts, count, 1, 1);
}

Value asnix_add_values(Value lhs, Value rhs) {
    int lt = asnix_tag_of(lhs), rt = asnix_tag_of(rhs);
    if (lt == ASNIX_TAG_INT && rt == ASNIX_TAG_INT) {
        return asnix_make_int(asnix_int_value(lhs) + asnix_int_value(rhs));
    }
    if (asnix_value_is_float(lhs) || asnix_value_is_float(rhs)) {
        double a, b;
        if (!as_double(lhs, &a)) {
            fprintf(stderr, "asnix: '+': cannot add a non-numeric value to a float\n");
            exit(1);
        }
        if (!as_double(rhs, &b)) {
            fprintf(stderr, "asnix: '+': cannot add a non-numeric value to a float\n");
            exit(1);
        }
        return asnix_make_float_value(a + b);
    }
    if (lt == ASNIX_TAG_INT || rt == ASNIX_TAG_INT) {
        fprintf(stderr, "asnix: '+': cannot add an integer and a non-integer\n");
        exit(1);
    }
    Value parts[2] = { lhs, rhs };
    Value result = concat_strings_impl(parts, 2, 0, !asnix_value_is_path(lhs));
    if (asnix_value_is_path(lhs)) {
        return canon_wrap_as_path(result);
    }
    return result;
}

Value asnix_sub_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return asnix_make_float_value(a - b);
    fprintf(stderr, "asnix: '-': operands must both be numbers (int or float), aborting\n");
    exit(1);
}

Value asnix_mul_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return asnix_make_float_value(a * b);
    fprintf(stderr, "asnix: '*': operands must both be numbers (int or float), aborting\n");
    exit(1);
}

Value asnix_div_values(Value lhs, Value rhs) {
    double a, b;
    if (!as_double(lhs, &a) || !as_double(rhs, &b)) {
        fprintf(stderr, "asnix: '/': operands must both be numbers (int or float), aborting\n");
        exit(1);
    }
    if (b == 0.0) {
        fprintf(stderr, "asnix: division by zero, aborting\n");
        exit(1);
    }
    return asnix_make_float_value(a / b);
}

static int is_plain_string(Value v) {
    return asnix_tag_of(v) == ASNIX_TAG_PTR && !asnix_value_is_path(v) && !asnix_value_is_float(v);
}

static int str_lt(Value a, Value b) {
    int64_t la, lb;
    const char *ba = asnix_ptr_bytes(a, &la);
    const char *bb = asnix_ptr_bytes(b, &lb);
    int64_t n = la < lb ? la : lb;
    int c = n > 0 ? memcmp(ba, bb, (size_t)n) : 0;
    if (c != 0) return c < 0;
    return la < lb;
}

Value asnix_cmp_lt_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a < b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return str_lt(lhs, rhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_cmp_gt_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a > b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return str_lt(rhs, lhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_cmp_le_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a <= b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return !str_lt(rhs, lhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_cmp_ge_values(Value lhs, Value rhs) {
    double a, b;
    if (as_double(lhs, &a) && as_double(rhs, &b)) return a >= b ? ASNIX_TRUE : ASNIX_FALSE;
    if (is_plain_string(lhs) && is_plain_string(rhs)) return !str_lt(lhs, rhs) ? ASNIX_TRUE : ASNIX_FALSE;
    fprintf(stderr, "asnix: comparison (< > <= >=) operands must both be numbers or both be strings, aborting\n");
    exit(1);
}

Value asnix_concat_lists(Value lhs, Value rhs) {
    if (asnix_tag_of(lhs) != ASNIX_TAG_LIST) {
        fprintf(stderr, "asnix: expected a list but found a non-list value (lhs of '++')\n");
        exit(1);
    }
    if (asnix_tag_of(rhs) != ASNIX_TAG_LIST) {
        fprintf(stderr, "asnix: expected a list but found a non-list value (rhs of '++')\n");
        exit(1);
    }
    const char *lbase = (const char *)(uintptr_t)(lhs.raw & ~(uint64_t)ASNIX_TAG_MASK);
    const char *rbase = (const char *)(uintptr_t)(rhs.raw & ~(uint64_t)ASNIX_TAG_MASK);
    int64_t lcount = *(const int64_t *)(lbase + ASNIX_LIST_OFF_COUNT);
    int64_t rcount = *(const int64_t *)(rbase + ASNIX_LIST_OFF_COUNT);
    int64_t total = lcount + rcount;

    void *mem = asnix_arena_alloc(8 + (uint64_t)total * sizeof(Value));
    *(int64_t *)((char *)mem + ASNIX_LIST_OFF_COUNT) = total;
    Value *out = (Value *)((char *)mem + ASNIX_LIST_OFF_ELEMS);
    if (lcount > 0) {
        memcpy(out, lbase + ASNIX_LIST_OFF_ELEMS, (size_t)lcount * sizeof(Value));
    }
    if (rcount > 0) {
        memcpy(out + lcount, rbase + ASNIX_LIST_OFF_ELEMS, (size_t)rcount * sizeof(Value));
    }
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_LIST };
}

char *asnix_canon_path(const char *in) {
    size_t len = strlen(in);
    char *out = malloc(len + 2);
    char **comps = malloc((len + 1) * sizeof(char *));
    size_t ncomps = 0;

    size_t i = 0;
    while (i < len) {
        while (i < len && in[i] == '/') i++;
        size_t start = i;
        while (i < len && in[i] != '/') i++;
        size_t clen = i - start;
        if (clen == 0) continue;
        if (clen == 1 && in[start] == '.') continue;
        if (clen == 2 && in[start] == '.' && in[start + 1] == '.') {
            if (ncomps > 0) ncomps--;
            continue;
        }
        char *c = malloc(clen + 1);
        memcpy(c, in + start, clen);
        c[clen] = '\0';
        comps[ncomps++] = c;
    }

    size_t pos = 0;
    for (size_t k = 0; k < ncomps; k++) {
        out[pos++] = '/';
        size_t clen = strlen(comps[k]);
        memcpy(out + pos, comps[k], clen);
        pos += clen;
        free(comps[k]);
    }
    free(comps);
    if (pos == 0) out[pos++] = '/';
    out[pos] = '\0';
    return out;
}

char *asnix_string_value_to_cstr(Value string_val) {
    const char *s = (const char *)(string_val.raw & ~0x7ULL);
    int64_t len = *(const int64_t *)(s + ASNIX_STRING_OFF_LEN);
    char *out = asnix_arena_alloc((size_t)len + 1);
    memcpy(out, s + ASNIX_STRING_OFF_BYTES, (size_t)len);
    out[len] = '\0';
    return out;
}

