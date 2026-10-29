#define _POSIX_C_SOURCE 200809L

#include "builtins_m54.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/art_sym.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "base32.h"
#include "base64.h"
#include "builtins_shared.h"
#include "eval_file.h"
#include "hash_shim.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

static void fatal(const char *who, const char *msg) {
    fprintf(stderr, "asnix: %s: %s\n", who, msg);
    exit(1);
}

#define ASNIX_ZIP_ART_THRESHOLD 64

static Value bitAnd2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_INT(args[0], "bitAnd");
    ASNIX_REQUIRE_INT(args[1], "bitAnd");
    return asnix_make_int(asnix_int_value(args[0]) & asnix_int_value(args[1]));
}
static Value bitOr2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_INT(args[0], "bitOr");
    ASNIX_REQUIRE_INT(args[1], "bitOr");
    return asnix_make_int(asnix_int_value(args[0]) | asnix_int_value(args[1]));
}
static Value bitXor2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_INT(args[0], "bitXor");
    ASNIX_REQUIRE_INT(args[1], "bitXor");
    return asnix_make_int(asnix_int_value(args[0]) ^ asnix_int_value(args[1]));
}
static Value build_bitAnd(void) { return asnix_bshared_make_curried_native((void *)bitAnd2_impl, 2); }
static Value build_bitOr(void) { return asnix_bshared_make_curried_native((void *)bitOr2_impl, 2); }
static Value build_bitXor(void) { return asnix_bshared_make_curried_native((void *)bitXor2_impl, 2); }

static void trace_print_string(FILE *out, const char *bytes, int64_t len) {
    fputc('"', out);
    for (int64_t i = 0; i < len; i++) {
        char c = bytes[i];
        switch (c) {
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            default: fputc(c, out); break;
        }
    }
    fputc('"', out);
}

static void trace_print_value(FILE *out, Value v) {
    v = asnix_force_for_print(v);
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT:
            fprintf(out, "%lld", (long long)asnix_int_value(v));
            return;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: fputs("true", out); return;
                case ASNIX_IMM_FALSE: fputs("false", out); return;
                case ASNIX_IMM_NULL: fputs("null", out); return;
                default: fputs("<imm>", out); return;
            }
        case ASNIX_TAG_PTR: {
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            if (asnix_value_is_path(v)) fwrite(bytes, 1, (size_t)len, out);
            else trace_print_string(out, bytes, len);
            return;
        }
        case ASNIX_TAG_LIST: {
            int64_t n = asnix_bshared_list_count(v);
            const Value *elems = asnix_bshared_list_elems(v);
            fputs("[ ", out);
            for (int64_t i = 0; i < n; i++) {
                trace_print_value(out, elems[i]);
                fputc(' ', out);
            }
            fputc(']', out);
            return;
        }
        case ASNIX_TAG_ATTRS: {
            int64_t n = asnix_bshared_attrs_count(v);
            fputs("{ ", out);
            for (int64_t i = 0; i < n; i++) {
                size_t len;
                const char *name = asnix_sym_name(asnix_bshared_attrs_sym(v, i), &len);
                fwrite(name, 1, len, out);
                fputs(" = ", out);
                trace_print_value(out, asnix_bshared_attrs_value(v, i));
                fputs("; ", out);
            }
            fputc('}', out);
            return;
        }
        case ASNIX_TAG_CLOSURE:
        case ASNIX_TAG_PRIMOP:
            fputs("<LAMBDA>", out);
            return;
        default:
            fputs("<unknown>", out);
            return;
    }
}

static Value trace2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value msg = asnix_force_for_print(args[0]);
    if (asnix_tag_of(msg) == ASNIX_TAG_PTR && !asnix_value_is_path(msg)) {
        int64_t len;
        const char *bytes = asnix_ptr_bytes(msg, &len);
        fprintf(stderr, "trace: %.*s\n", (int)len, bytes);
    } else {
        fputs("trace: ", stderr);
        trace_print_value(stderr, msg);
        fputc('\n', stderr);
    }
    return args[1];
}

static Value make_curried_native_noforce(void *native_fn, int64_t arity) {
    Value native_primop = asnix_bshared_make_primop_noforce(native_fn, arity);
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

static Value build_trace(void) { return make_curried_native_noforce((void *)trace2_impl, 2); }

static Value zipAttrsWith2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value f = args[0];
    Value list = args[1];
    ASNIX_REQUIRE_LIST(list, "zipAttrsWith");
    int64_t n = asnix_bshared_list_count(list);
    const Value *elems = asnix_bshared_list_elems(list);

    Value *sets = n > 0 ? asnix_arena_alloc((uint64_t)n * sizeof(Value)) : NULL;
    int64_t total_attrs = 0;
    for (int64_t i = 0; i < n; i++) {
        Value set = asnix_force_for_print(elems[i]);
        ASNIX_REQUIRE_ATTRS(set, "zipAttrsWith");
        sets[i] = set;
        total_attrs += asnix_bshared_attrs_count(set);
    }

    uint32_t *distinct;
    int64_t distinct_n;
    if (total_attrs <= ASNIX_ZIP_ART_THRESHOLD) {
        uint32_t *all_syms = total_attrs > 0 ? asnix_arena_alloc((uint64_t)total_attrs * sizeof(uint32_t)) : NULL;
        int64_t total_n = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t cnt = asnix_bshared_attrs_count(sets[i]);
            for (int64_t j = 0; j < cnt; j++) all_syms[total_n++] = asnix_bshared_attrs_sym(sets[i], j);
        }
        for (int64_t i = 1; i < total_n; i++) {
            int64_t j = i;
            uint32_t s = all_syms[i];
            while (j > 0 && all_syms[j - 1] > s) {
                all_syms[j] = all_syms[j - 1];
                j--;
            }
            all_syms[j] = s;
        }
        distinct = total_n > 0 ? asnix_arena_alloc((uint64_t)total_n * sizeof(uint32_t)) : NULL;
        distinct_n = 0;
        for (int64_t i = 0; i < total_n; i++) {
            if (distinct_n == 0 || distinct[distinct_n - 1] != all_syms[i]) distinct[distinct_n++] = all_syms[i];
        }
    } else {
        AsnixArtSymSet sym_set;
        asnix_art_sym_init(&sym_set);
        for (int64_t i = 0; i < n; i++) {
            int64_t cnt = asnix_bshared_attrs_count(sets[i]);
            for (int64_t j = 0; j < cnt; j++) asnix_art_sym_insert(&sym_set, asnix_bshared_attrs_sym(sets[i], j), (Value){0});
        }
        distinct = asnix_arena_alloc((uint64_t)total_attrs * sizeof(uint32_t));
        distinct_n = asnix_art_sym_collect_sorted(&sym_set, distinct, NULL);
    }

    Value *out_vals = distinct_n > 0 ? asnix_arena_alloc((uint64_t)distinct_n * sizeof(Value)) : NULL;
    Value *value_lists = distinct_n > 0 ? asnix_arena_alloc((uint64_t)distinct_n * sizeof(Value)) : NULL;
    for (int64_t k = 0; k < distinct_n; k++) {
        uint32_t sym = distinct[k];
        Value *per_key = n > 0 ? asnix_arena_alloc((uint64_t)n * sizeof(Value)) : NULL;
        int64_t cnt = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t idx = asnix_bshared_attrs_find(sets[i], sym);
            if (idx >= 0) per_key[cnt++] = asnix_bshared_attrs_value(sets[i], idx);
        }
        value_lists[k] = asnix_bshared_make_list(per_key, cnt);
    }

    if (distinct_n > 0 && distinct_n <= ASNIX_THUNKMAP_MAX_IDX + 1) {
        char *src_entries = asnix_arena_alloc((uint64_t)distinct_n * ASNIX_ATTR_SIZE);
        for (int64_t k = 0; k < distinct_n; k++) {
            char *entry = src_entries + k * ASNIX_ATTR_SIZE;
            *(uint32_t *)(entry + ASNIX_ATTR_OFF_SYM) = distinct[k];
            *(Value *)(entry + ASNIX_ATTR_OFF_VALUE) = value_lists[k];
        }
        void *desc = asnix_thunkmap_make_desc(f, src_entries);
        for (int64_t k = 0; k < distinct_n; k++) out_vals[k] = asnix_thunkmap_pack(desc, k);
        Value out = asnix_bshared_make_attrs(distinct, out_vals, distinct_n);
        asnix_thunkmap_set_out(desc, out);
        return out;
    }

    for (int64_t k = 0; k < distinct_n; k++) {
        size_t namelen;
        const char *name = asnix_sym_name(distinct[k], &namelen);
        Value name_val = asnix_make_string_value(name, namelen);
        out_vals[k] = asnix_make_lazyapp2(f, name_val, value_lists[k]);
    }
    return asnix_bshared_make_attrs(distinct, out_vals, distinct_n);
}

static Value build_zipAttrsWith(void) { return asnix_bshared_make_curried_native((void *)zipAttrsWith2_impl, 2); }

typedef struct {
    FILE *out;
    int depth;
} XmlCtx;

static void xml_indent(XmlCtx *ctx) {
    for (int i = 0; i < ctx->depth; i++) fputs("  ", ctx->out);
}

static void xml_escape_attr(FILE *out, const char *s, int64_t len) {
    for (int64_t i = 0; i < len; i++) {
        char c = s[i];
        switch (c) {
            case '"': fputs("&quot;", out); break;
            case '<': fputs("&lt;", out); break;
            case '>': fputs("&gt;", out); break;
            case '&': fputs("&amp;", out); break;
            case '\n': fputs("&#xA;", out); break;
            default: fputc(c, out); break;
        }
    }
}

static void xml_open(XmlCtx *ctx, const char *name) {
    xml_indent(ctx);
    fprintf(ctx->out, "<%s>\n", name);
    ctx->depth++;
}
static void xml_close(XmlCtx *ctx, const char *name) {
    ctx->depth--;
    xml_indent(ctx);
    fprintf(ctx->out, "</%s>\n", name);
}
static void xml_open_attr(XmlCtx *ctx, const char *elem, const char *attrname, const char *val, int64_t vallen) {
    xml_indent(ctx);
    fprintf(ctx->out, "<%s %s=\"", elem, attrname);
    xml_escape_attr(ctx->out, val, vallen);
    fputs("\">\n", ctx->out);
    ctx->depth++;
}
static void xml_empty(XmlCtx *ctx, const char *name) {
    xml_indent(ctx);
    fprintf(ctx->out, "<%s />\n", name);
}
static void xml_empty_attr(XmlCtx *ctx, const char *elem, const char *attrname, const char *val, int64_t vallen) {
    xml_indent(ctx);
    fprintf(ctx->out, "<%s %s=\"", elem, attrname);
    xml_escape_attr(ctx->out, val, vallen);
    fputs("\" />\n", ctx->out);
}

static void xml_attrs_sorted_order(Value attrs, int64_t n, int64_t *order) {
    for (int64_t i = 0; i < n; i++) order[i] = i;
    for (int64_t i = 1; i < n; i++) {
        int64_t j = i;
        while (j > 0) {
            size_t al, bl;
            const char *an = asnix_sym_name(asnix_bshared_attrs_sym(attrs, order[j - 1]), &al);
            const char *bn = asnix_sym_name(asnix_bshared_attrs_sym(attrs, order[j]), &bl);
            size_t minlen = al < bl ? al : bl;
            int cmp = minlen > 0 ? memcmp(an, bn, minlen) : 0;
            if (cmp == 0) cmp = (int)al - (int)bl;
            if (cmp <= 0) break;
            int64_t tmp = order[j - 1];
            order[j - 1] = order[j];
            order[j] = tmp;
            j--;
        }
    }
}

static void xml_print_value(XmlCtx *ctx, Value v) {
    v = asnix_force_for_print(v);
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%lld", (long long)asnix_int_value(v));
            xml_empty_attr(ctx, "int", "value", buf, len);
            return;
        }
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: xml_empty_attr(ctx, "bool", "value", "true", 4); return;
                case ASNIX_IMM_FALSE: xml_empty_attr(ctx, "bool", "value", "false", 5); return;
                case ASNIX_IMM_NULL: xml_empty(ctx, "null"); return;
                default: xml_empty(ctx, "unevaluated"); return;
            }
        case ASNIX_TAG_PTR: {
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            if (asnix_value_is_path(v)) xml_empty_attr(ctx, "path", "value", bytes, len);
            else xml_empty_attr(ctx, "string", "value", bytes, len);
            return;
        }
        case ASNIX_TAG_LIST: {
            int64_t n = asnix_bshared_list_count(v);
            const Value *elems = asnix_bshared_list_elems(v);
            xml_open(ctx, "list");
            for (int64_t i = 0; i < n; i++) xml_print_value(ctx, elems[i]);
            xml_close(ctx, "list");
            return;
        }
        case ASNIX_TAG_ATTRS: {
            int64_t n = asnix_bshared_attrs_count(v);
            int64_t *order = n > 0 ? asnix_arena_alloc((uint64_t)n * sizeof(int64_t)) : NULL;
            xml_attrs_sorted_order(v, n, order);
            xml_open(ctx, "attrs");
            for (int64_t i = 0; i < n; i++) {
                size_t namelen;
                const char *name = asnix_sym_name(asnix_bshared_attrs_sym(v, order[i]), &namelen);
                xml_open_attr(ctx, "attr", "name", name, (int64_t)namelen);
                xml_print_value(ctx, asnix_bshared_attrs_value(v, order[i]));
                xml_close(ctx, "attr");
            }
            xml_close(ctx, "attrs");
            return;
        }
        case ASNIX_TAG_CLOSURE:
            xml_open(ctx, "function");
            xml_empty_attr(ctx, "varpat", "name", "", 0);
            xml_close(ctx, "function");
            return;
        case ASNIX_TAG_PRIMOP:
        default:
            xml_empty(ctx, "unevaluated");
            return;
    }
}

static Value prim_toXML(const Value *args, int64_t argc) {
    (void)argc;
    char *buf = NULL;
    size_t sz = 0;
    FILE *out = open_memstream(&buf, &sz);
    fputs("<?xml version='1.0' encoding='utf-8'?>\n", out);
    XmlCtx ctx = { out, 0 };
    xml_open(&ctx, "expr");
    xml_print_value(&ctx, args[0]);
    xml_close(&ctx, "expr");
    fclose(out);
    Value result = asnix_make_string_value(buf, sz);
    free(buf);
    return result;
}
static Value build_toXML(void) { return asnix_bshared_make_primop((void *)prim_toXML, 1); }

static int64_t algo_name_to_size(const char *name, int64_t len) {
    if (len == 3 && memcmp(name, "md5", 3) == 0) return ASNIX_HASH_MD5_LEN;
    if (len == 4 && memcmp(name, "sha1", 4) == 0) return ASNIX_HASH_SHA1_LEN;
    if (len == 6 && memcmp(name, "sha256", 6) == 0) return ASNIX_HASH_SHA256_LEN;
    if (len == 6 && memcmp(name, "sha512", 6) == 0) return ASNIX_HASH_SHA512_LEN;
    return -1;
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int str_eq_lit(const char *s, int64_t len, const char *lit) {
    size_t litlen = strlen(lit);
    return (size_t)len == litlen && memcmp(s, lit, litlen) == 0;
}

static void parse_any_hash(const char *s, int64_t slen, const char *given_algo, int64_t given_algo_len,
                            unsigned char *out_raw, int64_t *out_size, const char **out_name, int64_t *out_name_len) {
    int64_t colon = -1, dash = -1;
    for (int64_t i = 0; i < slen; i++) {
        if (s[i] == ':') { colon = i; break; }
    }
    int is_sri = 0;
    const char *algo_part = NULL;
    int64_t algo_part_len = 0;
    const char *rest = s;
    int64_t rest_len = slen;
    if (colon >= 0) {
        algo_part = s;
        algo_part_len = colon;
        rest = s + colon + 1;
        rest_len = slen - colon - 1;
    } else {
        for (int64_t i = 0; i < slen; i++) {
            if (s[i] == '-') { dash = i; break; }
        }
        if (dash >= 0) {
            is_sri = 1;
            algo_part = s;
            algo_part_len = dash;
            rest = s + dash + 1;
            rest_len = slen - dash - 1;
        }
    }

    int64_t parsed_size = algo_part ? algo_name_to_size(algo_part, algo_part_len) : -1;
    if (algo_part && parsed_size < 0) fatal("convertHash", "hash has an unknown algorithm prefix");
    int64_t given_size = given_algo ? algo_name_to_size(given_algo, given_algo_len) : -1;
    if (given_algo && given_size < 0) fatal("convertHash", "unknown hashAlgo");

    const char *name;
    int64_t name_len, size;
    if (parsed_size >= 0 && given_size >= 0) {
        if (parsed_size != given_size) fatal("convertHash", "hash's own algorithm prefix disagrees with hashAlgo");
        name = given_algo;
        name_len = given_algo_len;
        size = given_size;
    } else if (parsed_size >= 0) {
        name = algo_part;
        name_len = algo_part_len;
        size = parsed_size;
    } else if (given_size >= 0) {
        name = given_algo;
        name_len = given_algo_len;
        size = given_size;
    } else {
        fatal("convertHash", "hash does not include a type, nor is hashAlgo given");
        return;
    }

    if (is_sri) {
        int64_t decoded = asnix_base64_decode(rest, (size_t)rest_len, out_raw, 64);
        if (decoded != size) fatal("convertHash", "invalid SRI hash length");
    } else {
        int64_t hexlen = size * 2;
        int64_t b32len = (int64_t)asnix_base32_encoded_length((size_t)size);
        int64_t b64len = (int64_t)asnix_base64_encoded_length((size_t)size);
        if (rest_len == hexlen) {
            for (int64_t i = 0; i < size; i++) {
                int hi = hex_digit_value(rest[2 * i]);
                int lo = hex_digit_value(rest[2 * i + 1]);
                if (hi < 0 || lo < 0) fatal("convertHash", "invalid base16 hash");
                out_raw[i] = (unsigned char)((hi << 4) | lo);
            }
        } else if (rest_len == b32len) {
            if (asnix_base32_decode(rest, (size_t)rest_len, out_raw, (size_t)size) != 0)
                fatal("convertHash", "invalid nix32 hash");
        } else if (rest_len == b64len) {
            if (asnix_base64_decode(rest, (size_t)rest_len, out_raw, 64) != size)
                fatal("convertHash", "invalid base64 hash");
        } else {
            fatal("convertHash", "hash has the wrong length for its hash algorithm");
        }
    }

    *out_size = size;
    *out_name = name;
    *out_name_len = name_len;
}

static Value prim_convertHash(const Value *args, int64_t argc) {
    (void)argc;
    Value in = args[0];
    ASNIX_REQUIRE_ATTRS(in, "convertHash");

    int64_t idx_hash = asnix_bshared_attrs_find(in, asnix_intern("hash", 4));
    if (idx_hash < 0) fatal("convertHash", "missing attribute 'hash'");
    Value hash_v = asnix_force_for_print(asnix_bshared_attrs_value(in, idx_hash));
    ASNIX_REQUIRE_STRING(hash_v, "convertHash");
    int64_t hlen;
    const char *hbytes = asnix_ptr_bytes(hash_v, &hlen);

    const char *algo_bytes = NULL;
    int64_t algo_len = 0;
    int64_t idx_algo = asnix_bshared_attrs_find(in, asnix_intern("hashAlgo", 8));
    if (idx_algo >= 0) {
        Value algo_v = asnix_force_for_print(asnix_bshared_attrs_value(in, idx_algo));
        ASNIX_REQUIRE_STRING(algo_v, "convertHash");
        algo_bytes = asnix_ptr_bytes(algo_v, &algo_len);
    }

    int64_t idx_fmt = asnix_bshared_attrs_find(in, asnix_intern("toHashFormat", 12));
    if (idx_fmt < 0) fatal("convertHash", "missing attribute 'toHashFormat'");
    Value fmt_v = asnix_force_for_print(asnix_bshared_attrs_value(in, idx_fmt));
    ASNIX_REQUIRE_STRING(fmt_v, "convertHash");
    int64_t flen;
    const char *fbytes = asnix_ptr_bytes(fmt_v, &flen);

    unsigned char raw[64];
    int64_t size, name_len;
    const char *name;
    parse_any_hash(hbytes, hlen, algo_bytes, algo_len, raw, &size, &name, &name_len);

    if (str_eq_lit(fbytes, flen, "base16")) {
        char hex[128];
        asnix_hash_to_hex(raw, (size_t)size, hex);
        return asnix_make_string_value(hex, (size_t)(2 * size));
    }
    if (str_eq_lit(fbytes, flen, "nix32") || str_eq_lit(fbytes, flen, "base32")) {
        size_t enc_len = asnix_base32_encoded_length((size_t)size);
        char *enc = asnix_arena_alloc(enc_len);
        asnix_base32_encode(raw, (size_t)size, enc);
        return asnix_make_string_value(enc, enc_len);
    }
    if (str_eq_lit(fbytes, flen, "base64")) {
        size_t enc_len = asnix_base64_encoded_length((size_t)size);
        char *enc = asnix_arena_alloc(enc_len);
        asnix_base64_encode(raw, (size_t)size, enc);
        return asnix_make_string_value(enc, enc_len);
    }
    if (str_eq_lit(fbytes, flen, "sri")) {
        size_t enc_len = asnix_base64_encoded_length((size_t)size);
        char *enc = asnix_arena_alloc((size_t)name_len + 1 + enc_len);
        memcpy(enc, name, (size_t)name_len);
        enc[name_len] = '-';
        asnix_base64_encode(raw, (size_t)size, enc + name_len + 1);
        return asnix_make_string_value(enc, (size_t)name_len + 1 + enc_len);
    }
    fatal("convertHash", "unknown toHashFormat (expected base16/nix32/base32/base64/sri)");
    return ASNIX_NULL;
}
static Value build_convertHash(void) { return asnix_bshared_make_primop((void *)prim_convertHash, 1); }

static Value prim_getContext(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "getContext");
    const AsnixStringContext *ctx = asnix_string_context_of(args[0]);
    if (asnix_ctx_is_empty(ctx)) return asnix_bshared_make_attrs(NULL, NULL, 0);

    typedef struct {
        const char *path;
        int is_path;
        int all_outputs;
        const char **outputs;
        size_t noutputs, outputs_cap;
    } GetCtxEntry;
    size_t cap = ctx->ndrvs + ctx->nsrcs + ctx->ndeep;
    GetCtxEntry *entries = calloc(cap ? cap : 1, sizeof(GetCtxEntry));
    size_t nentries = 0;
#define GETCTX_FIND_OR_CREATE(pathArg)                                                                               \
    ({                                                                                                               \
        GetCtxEntry *found = NULL;                                                                                   \
        for (size_t _i = 0; _i < nentries; _i++) {                                                                   \
            if (strcmp(entries[_i].path, (pathArg)) == 0) {                                                         \
                found = &entries[_i];                                                                                \
                break;                                                                                               \
            }                                                                                                        \
        }                                                                                                            \
        if (!found) {                                                                                                \
            found = &entries[nentries++];                                                                           \
            found->path = (pathArg);                                                                                 \
        }                                                                                                            \
        found;                                                                                                       \
    })
    for (size_t i = 0; i < ctx->ndrvs; i++) {
        GetCtxEntry *e = GETCTX_FIND_OR_CREATE(ctx->drvs[i].drv_path);
        if (e->noutputs == e->outputs_cap) {
            e->outputs_cap = e->outputs_cap ? e->outputs_cap * 2 : 4;
            e->outputs = realloc(e->outputs, e->outputs_cap * sizeof(*e->outputs));
        }
        e->outputs[e->noutputs++] = ctx->drvs[i].output_name;
    }
    for (size_t i = 0; i < ctx->nsrcs; i++) {
        GetCtxEntry *e = GETCTX_FIND_OR_CREATE(ctx->srcs[i]);
        e->is_path = 1;
    }
    for (size_t i = 0; i < ctx->ndeep; i++) {
        GetCtxEntry *e = GETCTX_FIND_OR_CREATE(ctx->deep[i]);
        e->all_outputs = 1;
    }
#undef GETCTX_FIND_OR_CREATE

    uint32_t sym_path = asnix_intern("path", 4);
    uint32_t sym_alloutputs = asnix_intern("allOutputs", 10);
    uint32_t sym_outputs = asnix_intern("outputs", 7);
    uint32_t *top_syms = malloc(nentries * sizeof(uint32_t));
    Value *top_vals = malloc(nentries * sizeof(Value));
    for (size_t i = 0; i < nentries; i++) {
        GetCtxEntry *e = &entries[i];
        uint32_t nsyms[3];
        Value nvals[3];
        size_t nk = 0;
        if (e->is_path) {
            nsyms[nk] = sym_path;
            nvals[nk] = asnix_make_imm(ASNIX_IMM_TRUE);
            nk++;
        }
        if (e->all_outputs) {
            nsyms[nk] = sym_alloutputs;
            nvals[nk] = asnix_make_imm(ASNIX_IMM_TRUE);
            nk++;
        }
        if (e->noutputs > 0) {
            for (size_t j = 1; j < e->noutputs; j++) {
                size_t k = j;
                while (k > 0 && strcmp(e->outputs[k - 1], e->outputs[k]) > 0) {
                    const char *tmp = e->outputs[k - 1];
                    e->outputs[k - 1] = e->outputs[k];
                    e->outputs[k] = tmp;
                    k--;
                }
            }
            Value *olist = malloc(e->noutputs * sizeof(Value));
            for (size_t j = 0; j < e->noutputs; j++) olist[j] = asnix_make_string_value(e->outputs[j], strlen(e->outputs[j]));
            nsyms[nk] = sym_outputs;
            nvals[nk] = asnix_bshared_make_list(olist, (int64_t)e->noutputs);
            free(olist);
            nk++;
        }
        top_syms[i] = asnix_intern(e->path, strlen(e->path));
        top_vals[i] = asnix_bshared_make_attrs(nsyms, nvals, (int64_t)nk);
        free(e->outputs);
    }
    free(entries);
    Value result = asnix_bshared_make_attrs(top_syms, top_vals, (int64_t)nentries);
    free(top_syms);
    free(top_vals);
    return result;
}
static Value build_getContext(void) { return asnix_bshared_make_primop((void *)prim_getContext, 1); }

static Value prim_hasContext(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "hasContext");
    const AsnixStringContext *ctx = asnix_string_context_of(args[0]);
    return asnix_make_imm(asnix_ctx_is_empty(ctx) ? ASNIX_IMM_FALSE : ASNIX_IMM_TRUE);
}
static Value build_hasContext(void) { return asnix_bshared_make_primop((void *)prim_hasContext, 1); }

static int ends_with_dot_drv(const char *s, size_t len) {
    return len > 4 && memcmp(s + len - 4, ".drv", 4) == 0;
}
static Value appendContext2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "appendContext");
    ASNIX_REQUIRE_ATTRS(args[1], "appendContext");

    int64_t orig_len;
    const char *orig_bytes;
    asnix_bshared_string_bytes(args[0], &orig_bytes, &orig_len, "appendContext");

    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    asnix_ctx_union_into(&ctx, asnix_string_context_of(args[0]));

    uint32_t sym_path = asnix_intern("path", 4);
    uint32_t sym_alloutputs = asnix_intern("allOutputs", 10);
    uint32_t sym_outputs = asnix_intern("outputs", 7);

    int64_t nattrs = asnix_bshared_attrs_count(args[1]);
    for (int64_t i = 0; i < nattrs; i++) {
        uint32_t key_sym = asnix_bshared_attrs_sym(args[1], i);
        size_t key_len;
        const char *key_name = asnix_sym_name(key_sym, &key_len);
        if (!(key_len > 11 && memcmp(key_name, "/nix/store/", 11) == 0)) {
            fprintf(stderr, "asnix: appendContext: context key '%.*s' is not a store path\n", (int)key_len,
                    key_name);
            exit(1);
        }
        char *path = strndup(key_name, key_len);
        Value entry = asnix_force_for_print(asnix_bshared_attrs_value(args[1], i));
        ASNIX_REQUIRE_ATTRS(entry, "appendContext");

        int64_t idx = asnix_bshared_attrs_find(entry, sym_path);
        if (idx >= 0) {
            Value b = asnix_force_for_print(asnix_bshared_attrs_value(entry, idx));
            if (asnix_tag_of(b) == ASNIX_TAG_IMM && asnix_imm_subtag(b) == ASNIX_IMM_TRUE) asnix_ctx_add_opaque(&ctx, path);
        }
        idx = asnix_bshared_attrs_find(entry, sym_alloutputs);
        if (idx >= 0) {
            Value b = asnix_force_for_print(asnix_bshared_attrs_value(entry, idx));
            if (asnix_tag_of(b) == ASNIX_TAG_IMM && asnix_imm_subtag(b) == ASNIX_IMM_TRUE) {
                if (!ends_with_dot_drv(path, key_len)) {
                    fprintf(stderr,
                            "asnix: appendContext: tried to add all-outputs context of %s, which is not a "
                            "derivation, to a string\n",
                            path);
                    exit(1);
                }
                asnix_ctx_add_deep(&ctx, path);
            }
        }
        idx = asnix_bshared_attrs_find(entry, sym_outputs);
        if (idx >= 0) {
            Value lst = asnix_force_for_print(asnix_bshared_attrs_value(entry, idx));
            ASNIX_REQUIRE_LIST(lst, "appendContext");
            int64_t ocount = asnix_bshared_list_count(lst);
            const Value *oelems = asnix_bshared_list_elems(lst);
            if (ocount > 0 && !ends_with_dot_drv(path, key_len)) {
                fprintf(stderr,
                        "asnix: appendContext: tried to add derivation output context of %s, which is not a "
                        "derivation, to a string\n",
                        path);
                exit(1);
            }
            for (int64_t j = 0; j < ocount; j++) {
                Value onm = asnix_force_for_print(oelems[j]);
                ASNIX_REQUIRE_STRING(onm, "appendContext");
                int64_t onlen;
                const char *onbytes;
                asnix_bshared_string_bytes(onm, &onbytes, &onlen, "appendContext");
                char *oname = strndup(onbytes, (size_t)onlen);
                asnix_ctx_add_built(&ctx, path, oname);
                free(oname);
            }
        }
        free(path);
    }

    Value out = asnix_make_string_value(orig_bytes, (size_t)orig_len);
    asnix_string_attach_context(out, &ctx);
    return out;
}
static Value build_appendContext(void) { return asnix_bshared_make_curried_native((void *)appendContext2_impl, 2); }

static Value prim_unsafeDiscardOutputDependency(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "unsafeDiscardOutputDependency");
    int64_t len;
    const char *bytes;
    asnix_bshared_string_bytes(args[0], &bytes, &len, "unsafeDiscardOutputDependency");
    const AsnixStringContext *src = asnix_string_context_of(args[0]);
    if (asnix_ctx_is_empty(src)) return args[0];
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    for (size_t i = 0; i < src->ndrvs; i++) asnix_ctx_add_built(&ctx, src->drvs[i].drv_path, src->drvs[i].output_name);
    for (size_t i = 0; i < src->nsrcs; i++) asnix_ctx_add_opaque(&ctx, src->srcs[i]);
    for (size_t i = 0; i < src->ndeep; i++) asnix_ctx_add_opaque(&ctx, src->deep[i]);
    Value out = asnix_make_string_value(bytes, (size_t)len);
    asnix_string_attach_context(out, &ctx);
    return out;
}
static Value build_unsafeDiscardOutputDependency(void) {
    return asnix_bshared_make_primop((void *)prim_unsafeDiscardOutputDependency, 1);
}

static Value prim_addDrvOutputDependencies(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "addDrvOutputDependencies");
    int64_t len;
    const char *bytes;
    asnix_bshared_string_bytes(args[0], &bytes, &len, "addDrvOutputDependencies");
    const AsnixStringContext *src = asnix_string_context_of(args[0]);
    size_t total = src ? src->ndrvs + src->nsrcs + src->ndeep : 0;
    if (total != 1) {
        fprintf(stderr, "asnix: addDrvOutputDependencies: context must have exactly one element, but has %zu\n",
                total);
        exit(1);
    }
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    if (src->ndeep == 1) {
        asnix_ctx_add_deep(&ctx, src->deep[0]);
    } else if (src->nsrcs == 1) {
        if (!ends_with_dot_drv(src->srcs[0], strlen(src->srcs[0]))) {
            fprintf(stderr, "asnix: addDrvOutputDependencies: path '%s' is not a derivation\n", src->srcs[0]);
            exit(1);
        }
        asnix_ctx_add_deep(&ctx, src->srcs[0]);
    } else {
        fprintf(stderr,
                "asnix: addDrvOutputDependencies: can only act on derivations, not on a derivation output "
                "such as '%s'\n",
                src->drvs[0].output_name);
        exit(1);
    }
    Value out = asnix_make_string_value(bytes, (size_t)len);
    asnix_string_attach_context(out, &ctx);
    return out;
}
static Value build_addDrvOutputDependencies(void) {
    return asnix_bshared_make_primop((void *)prim_addDrvOutputDependencies, 1);
}

static Value prim_unsafeDiscardStringContext(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "unsafeDiscardStringContext");
    int64_t len;
    const char *bytes;
    asnix_bshared_string_bytes(args[0], &bytes, &len, "unsafeDiscardStringContext");
    return asnix_make_string_value(bytes, (size_t)len);
}
static Value build_unsafeDiscardStringContext(void) {
    return asnix_bshared_make_primop((void *)prim_unsafeDiscardStringContext, 1);
}

static Value scopedImport2_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value scope = args[0];
    Value target = args[1];
    ASNIX_REQUIRE_ATTRS(scope, "scopedImport");

    int64_t len;
    const char *bytes;
    if (asnix_value_is_path(target)) {
        bytes = asnix_ptr_bytes(target, &len);
    } else if (asnix_tag_of(target) == ASNIX_TAG_PTR) {
        bytes = asnix_ptr_bytes(target, &len);
        if (len == 0 || bytes[0] != '/') fatal("scopedImport", "path string doesn't represent an absolute path");
    } else {
        fatal("scopedImport", "second argument must be a path or an absolute-path string");
        return ASNIX_NULL;
    }
    char *path = asnix_arena_alloc((size_t)len + 1);
    memcpy(path, bytes, (size_t)len);
    path[len] = '\0';
    return asnix_eval_file_scoped(path, scope);
}
static Value build_scopedImport(void) { return asnix_bshared_make_curried_native((void *)scopedImport2_impl, 2); }

Value asnix_make_scopedImport_primop(void) { return build_scopedImport(); }

static Value unsafeGetAttrPos2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "unsafeGetAttrPos");
    ASNIX_REQUIRE_ATTRS(args[1], "unsafeGetAttrPos");

    const char *name_bytes;
    int64_t name_len;
    asnix_bshared_string_bytes(args[0], &name_bytes, &name_len, "unsafeGetAttrPos");
    uint32_t name_sym = asnix_intern(name_bytes, (size_t)name_len);

    int64_t idx = asnix_bshared_attrs_find(args[1], name_sym);
    if (idx < 0) return ASNIX_NULL;

    Value attrs_flat = asnix_bshared_attrs_resolve(args[1]);
    const char *entry = (const char *)(attrs_flat.raw & ~0x7ULL) + ASNIX_ATTRS_OFF_ENTRIES + idx * ASNIX_ATTR_SIZE;
    Value attr_val = asnix_force_for_print(*(const Value *)(entry + ASNIX_ATTR_OFF_VALUE));
    if (asnix_tag_of(attr_val) == ASNIX_TAG_PRIMOP) return ASNIX_NULL;

    const char *file = asnix_eval_top_level_file();
    if (!file) file = "";

    uint32_t syms[3];
    Value vals[3];
    syms[0] = asnix_intern("file", 4);
    vals[0] = asnix_make_string_value(file, strlen(file));
    syms[1] = asnix_intern("line", 4);
    vals[1] = asnix_make_int(1);
    syms[2] = asnix_intern("column", 6);
    vals[2] = asnix_make_int(1);
    return asnix_bshared_make_attrs(syms, vals, 3);
}
static Value build_unsafeGetAttrPos(void) { return asnix_bshared_make_curried_native((void *)unsafeGetAttrPos2_impl, 2); }

const AsnixBuiltinM54Def ASNIX_M54_BUILTINS[] = {
    { "bitAnd", build_bitAnd },
    { "bitOr", build_bitOr },
    { "bitXor", build_bitXor },
    { "trace", build_trace },
    { "zipAttrsWith", build_zipAttrsWith },
    { "toXML", build_toXML },
    { "convertHash", build_convertHash },
    { "getContext", build_getContext },
    { "hasContext", build_hasContext },
    { "appendContext", build_appendContext },
    { "unsafeDiscardStringContext", build_unsafeDiscardStringContext },
    { "unsafeDiscardOutputDependency", build_unsafeDiscardOutputDependency },
    { "addDrvOutputDependencies", build_addDrvOutputDependencies },
    { "scopedImport", build_scopedImport },
    { "unsafeGetAttrPos", build_unsafeGetAttrPos },
};

const size_t ASNIX_M54_BUILTINS_COUNT = sizeof(ASNIX_M54_BUILTINS) / sizeof(ASNIX_M54_BUILTINS[0]);

