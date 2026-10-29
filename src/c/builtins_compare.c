#include "builtins_compare.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "nar.h"
#include "string_context.h"
#include "symtab.h"
#include "value_util.h"

static Value lessThan2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_bshared_value_lt(args[0], args[1], "lessThan") ? ASNIX_TRUE : ASNIX_FALSE;
}
static Value build_lessThan(void) { return asnix_bshared_make_curried_native((void *)lessThan2_impl, 2); }

static void next_component(const char *s, int64_t len, int64_t *pos, const char **out_ptr, int64_t *out_len) {
    int64_t p = *pos;
    while (p < len && (s[p] == '.' || s[p] == '-')) p++;
    if (p >= len) {
        *out_ptr = s + p;
        *out_len = 0;
        *pos = p;
        return;
    }
    int64_t start = p;
    if (s[p] >= '0' && s[p] <= '9') {
        while (p < len && s[p] >= '0' && s[p] <= '9') p++;
    } else {
        while (p < len && !(s[p] >= '0' && s[p] <= '9') && s[p] != '.' && s[p] != '-') p++;
    }
    *out_ptr = s + start;
    *out_len = p - start;
    *pos = p;
}

static int component_is_int(const char *s, int64_t len, int64_t *out) {
    if (len == 0) return 0;
    int64_t v = 0;
    for (int64_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > 2147483647LL) return 0;
    }
    *out = v;
    return 1;
}

static int components_lt(const char *c1, int64_t l1, const char *c2, int64_t l2) {
    int64_t n1, n2;
    int is1 = component_is_int(c1, l1, &n1);
    int is2 = component_is_int(c2, l2, &n2);
    if (is1 && is2) return n1 < n2;
    if (l1 == 0 && is2) return 1;
    int c1_is_pre = (l1 == 3 && memcmp(c1, "pre", 3) == 0);
    int c2_is_pre = (l2 == 3 && memcmp(c2, "pre", 3) == 0);
    if (c1_is_pre && !c2_is_pre) return 1;
    if (c2_is_pre) return 0;
    if (is2) return 1;
    if (is1) return 0;
    int64_t n = l1 < l2 ? l1 : l2;
    int c = n > 0 ? memcmp(c1, c2, (size_t)n) : 0;
    if (c != 0) return c < 0;
    return l1 < l2;
}

static int compare_versions(const char *v1, int64_t len1, const char *v2, int64_t len2) {
    int64_t p1 = 0, p2 = 0;
    while (p1 < len1 || p2 < len2) {
        const char *c1p, *c2p;
        int64_t c1l, c2l;
        next_component(v1, len1, &p1, &c1p, &c1l);
        next_component(v2, len2, &p2, &c2p, &c2l);
        if (components_lt(c1p, c1l, c2p, c2l)) return -1;
        if (components_lt(c2p, c2l, c1p, c1l)) return 1;
    }
    return 0;
}

static Value prim_splitVersion(const Value *args, int64_t argc) {
    (void)argc;
    int64_t slen;
    const char *sbytes;
    asnix_bshared_string_bytes(args[0], &sbytes, &slen, "splitVersion");

    Value *out = NULL;
    int64_t cap = 0, n = 0, pos = 0;
    while (pos < slen) {
        const char *cp;
        int64_t cl;
        next_component(sbytes, slen, &pos, &cp, &cl);
        if (cl == 0) break;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = asnix_make_string_value(cp, (size_t)cl);
    }
    Value result = asnix_bshared_make_list(out, n);
    free(out);
    return result;
}
static Value build_splitVersion(void) { return asnix_bshared_make_primop((void *)prim_splitVersion, 1); }

static Value compareVersions2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t l1, l2;
    const char *b1, *b2;
    asnix_bshared_string_bytes(args[0], &b1, &l1, "compareVersions");
    asnix_bshared_string_bytes(args[1], &b2, &l2, "compareVersions");
    return asnix_make_int(compare_versions(b1, l1, b2, l2));
}
static Value build_compareVersions(void) { return asnix_bshared_make_curried_native((void *)compareVersions2_impl, 2); }

static Value replaceStrings3_impl(const Value *args, int64_t argc) {
    (void)argc;
    Value from_list = args[0], to_list = args[1], s_v = args[2];
    ASNIX_REQUIRE_LIST(from_list, "replaceStrings");
    ASNIX_REQUIRE_LIST(to_list, "replaceStrings");
    int64_t nfrom = asnix_bshared_list_count(from_list);
    int64_t nto = asnix_bshared_list_count(to_list);
    if (nfrom != nto) asnix_bshared_fatal("replaceStrings", "'from' and 'to' lists have different lengths");
    const Value *from_elems = asnix_bshared_list_elems(from_list);
    const Value *to_elems = asnix_bshared_list_elems(to_list);

    const char **from_bytes = nfrom > 0 ? malloc((size_t)nfrom * sizeof(char *)) : NULL;
    int64_t *from_lens = nfrom > 0 ? malloc((size_t)nfrom * sizeof(int64_t)) : NULL;
    for (int64_t i = 0; i < nfrom; i++) {
        asnix_bshared_string_bytes(asnix_force_for_print(from_elems[i]), &from_bytes[i], &from_lens[i], "replaceStrings");
    }

    int64_t slen;
    const char *sbytes;
    asnix_bshared_string_bytes(s_v, &sbytes, &slen, "replaceStrings");

    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    asnix_ctx_union_into(&ctx, asnix_string_context_of(s_v));

    size_t cap = (size_t)slen + 16, outlen = 0;
    char *out = malloc(cap);
    int64_t p = 0;
    while (p <= slen) {
        int64_t found = -1;
        for (int64_t i = 0; i < nfrom; i++) {
            int64_t fl = from_lens[i];
            if (p + fl <= slen && (fl == 0 || memcmp(sbytes + p, from_bytes[i], (size_t)fl) == 0)) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            const char *tb;
            int64_t tl;
            Value to_forced = asnix_force_for_print(to_elems[found]);
            asnix_bshared_string_bytes(to_forced, &tb, &tl, "replaceStrings");
            asnix_ctx_union_into(&ctx, asnix_string_context_of(to_forced));
            if (outlen + (size_t)tl + 1 > cap) {
                cap = (outlen + (size_t)tl + 1) * 2;
                out = realloc(out, cap);
            }
            if (tl > 0) memcpy(out + outlen, tb, (size_t)tl);
            outlen += (size_t)tl;
            if (from_lens[found] == 0) {
                if (p < slen) out[outlen++] = sbytes[p];
                p++;
            } else {
                p += from_lens[found];
            }
        } else {
            if (p < slen) {
                if (outlen + 1 > cap) { cap *= 2; out = realloc(out, cap); }
                out[outlen++] = sbytes[p];
            }
            p++;
        }
    }
    Value result = asnix_make_string_value(out, outlen);
    if (!asnix_ctx_is_empty(&ctx)) asnix_string_attach_context(result, &ctx);
    free(out);
    free(from_bytes);
    free(from_lens);
    return result;
}
static Value build_replaceStrings(void) { return asnix_bshared_make_curried_native((void *)replaceStrings3_impl, 3); }

static Value prim_stringToCharacters(const Value *args, int64_t argc) {
    (void)argc;
    int64_t len;
    const char *bytes;
    asnix_bshared_string_bytes(args[0], &bytes, &len, "stringToCharacters");
    Value *out = len > 0 ? malloc((size_t)len * sizeof(Value)) : NULL;
    for (int64_t i = 0; i < len; i++) out[i] = asnix_make_string_value(bytes + i, 1);
    Value result = asnix_bshared_make_list(out, len);
    free(out);
    return result;
}
static Value build_stringToCharacters(void) { return asnix_bshared_make_primop((void *)prim_stringToCharacters, 1); }

typedef struct {
    char *buf;
    size_t len, cap;
} JsonBuf;

static void jb_init(JsonBuf *b) {
    b->cap = 64;
    b->buf = malloc(b->cap);
    b->len = 0;
}

static void jb_ensure(JsonBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < b->len + extra) ncap *= 2;
    b->buf = realloc(b->buf, ncap);
    b->cap = ncap;
}

static void jb_putc(JsonBuf *b, char c) {
    jb_ensure(b, 1);
    b->buf[b->len++] = c;
}

static void jb_puts(JsonBuf *b, const char *s, size_t n) {
    jb_ensure(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

static int64_t find_escape_json(const unsigned char *s, int64_t len) {
    static const uint64_t LO = 0x0101010101010101ULL;
    static const uint64_t HI = 0x8080808080808080ULL;
    int64_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t x;
        memcpy(&x, s + i, 8);
        uint64_t q = x ^ (LO * (uint64_t)'"');
        uint64_t bs = x ^ (LO * (uint64_t)'\\');
        uint64_t any = ((x - LO * 0x20) & ~x) | ((q - LO) & ~q) | ((bs - LO) & ~bs);
        if (any & HI) {
            for (int64_t j = i; j < i + 8; j++) {
                unsigned char c = s[j];
                if (c == '"' || c == '\\' || c < 0x20) return j;
            }
        }
    }
    for (; i < len; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\' || c < 0x20) return i;
    }
    return len;
}

static void jb_escape_string(JsonBuf *out, const char *bytes, int64_t len) {
    jb_putc(out, '"');
    const unsigned char *p = (const unsigned char *)bytes;
    int64_t pos = 0;
    for (;;) {
        int64_t i = pos + find_escape_json(p + pos, len - pos);
        if (i != pos) jb_puts(out, bytes + pos, (size_t)(i - pos));
        if (i == len) break;
        pos = i + 1;
        unsigned char c = p[i];
        switch (c) {
            case '"': jb_puts(out, "\\\"", 2); break;
            case '\\': jb_puts(out, "\\\\", 2); break;
            case '\b': jb_puts(out, "\\b", 2); break;
            case '\f': jb_puts(out, "\\f", 2); break;
            case '\n': jb_puts(out, "\\n", 2); break;
            case '\r': jb_puts(out, "\\r", 2); break;
            case '\t': jb_puts(out, "\\t", 2); break;
            default: {
                char esc[8];
                int n = snprintf(esc, sizeof esc, "\\u%04x", c);
                jb_puts(out, esc, (size_t)n);
                break;
            }
        }
    }
    jb_putc(out, '"');
}

static int utf8_is_valid(const char *bytes, int64_t len) {
    int64_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)bytes[i];
        int extra;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return 0;
        if (i + extra >= len) return 0;
        for (int k = 1; k <= extra; k++) {
            if (((unsigned char)bytes[i + k] & 0xC0) != 0x80) return 0;
        }
        i += extra + 1;
    }
    return 1;
}

static size_t format_json_float(double d, char *buf, size_t bufsize) {
    int n = 0;
    for (int prec = 1; prec <= 17; prec++) {
        n = snprintf(buf, bufsize, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (n < 0) return 0;
    size_t len = (size_t)n;
    if (len + 3 > bufsize) return len;
    int has_dot_or_exp = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { has_dot_or_exp = 1; break; }
    }
    if (!has_dot_or_exp) {
        buf[len++] = '.';
        buf[len++] = '0';
        buf[len] = '\0';
    }
    return len;
}

static int json_name_cmp(const char *an, size_t al, const char *bn, size_t bl) {
    size_t n = al < bl ? al : bl;
    int c = n > 0 ? memcmp(an, bn, n) : 0;
    if (c != 0) return c;
    return (al > bl) - (al < bl);
}

static void json_encode_ctx(Value v, JsonBuf *out, AsnixStringContext *ctx);
static void json_encode(Value v, JsonBuf *out) { json_encode_ctx(v, out, NULL); }

typedef struct {
    const char *name;
    size_t len;
    int64_t idx;
} JsonAttrEntry;

#define ASNIX_JSON_ATTRS_SORT_STACK_MAX 32

static void json_encode_attrs_ctx(Value v, JsonBuf *out, AsnixStringContext *ctx) {
    int64_t n = asnix_bshared_attrs_count(v);
    JsonAttrEntry entries_stack[ASNIX_JSON_ATTRS_SORT_STACK_MAX];
    int on_stack = (n <= ASNIX_JSON_ATTRS_SORT_STACK_MAX);
    JsonAttrEntry *entries = on_stack ? entries_stack : (n > 0 ? malloc((size_t)n * sizeof(JsonAttrEntry)) : NULL);
    for (int64_t i = 0; i < n; i++) {
        entries[i].name = asnix_sym_name(asnix_bshared_attrs_sym(v, i), &entries[i].len);
        entries[i].idx = i;
    }
    for (int64_t i = 1; i < n; i++) {
        JsonAttrEntry cur = entries[i];
        int64_t j = i;
        while (j > 0 && json_name_cmp(entries[j - 1].name, entries[j - 1].len, cur.name, cur.len) > 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = cur;
    }
    jb_putc(out, '{');
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) jb_putc(out, ',');
        jb_escape_string(out, entries[i].name, (int64_t)entries[i].len);
        jb_putc(out, ':');
        json_encode_ctx(asnix_force_for_print(asnix_bshared_attrs_value(v, entries[i].idx)), out, ctx);
    }
    jb_putc(out, '}');
    if (!on_stack) free(entries);
}

static void json_encode_ctx(Value v, JsonBuf *out, AsnixStringContext *ctx) {
    v = asnix_force_for_print(v);
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT: {
            char buf[32];
            int n = snprintf(buf, sizeof buf, "%lld", (long long)asnix_int_value(v));
            jb_puts(out, buf, (size_t)n);
            break;
        }
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: jb_puts(out, "true", 4); break;
                case ASNIX_IMM_FALSE: jb_puts(out, "false", 5); break;
                case ASNIX_IMM_NULL: jb_puts(out, "null", 4); break;
                default: asnix_bshared_fatal("toJSON", "cannot convert this value to JSON");
            }
            break;
        case ASNIX_TAG_PTR: {
            if (asnix_value_is_float(v)) {
                char buf[64];
                size_t n = format_json_float(asnix_float_value(v), buf, sizeof(buf));
                jb_puts(out, buf, n);
                break;
            }
            if (asnix_value_is_path(v)) {
                int64_t plen;
                const char *pbytes = asnix_ptr_bytes(v, &plen);
                char *path = malloc((size_t)plen + 1);
                memcpy(path, pbytes, (size_t)plen);
                path[plen] = '\0';
                const char *final_path = path;
                char store_path[300];
                if (!(plen > 11 && memcmp(path, "/nix/store/", 11) == 0)) {
                    asnix_copy_path_to_store(path, store_path, sizeof(store_path));
                    final_path = store_path;
                }
                jb_escape_string(out, final_path, (int64_t)strlen(final_path));
                free(path);
                break;
            }
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            if (!utf8_is_valid(bytes, len)) {
                asnix_bshared_fatal("toJSON", "string is not valid UTF-8");
            }
            if (ctx) asnix_ctx_union_into(ctx, asnix_string_context_of(v));
            jb_escape_string(out, bytes, len);
            break;
        }
        case ASNIX_TAG_LIST: {
            int64_t n = asnix_bshared_list_count(v);
            const Value *elems = asnix_bshared_list_elems(v);
            jb_putc(out, '[');
            for (int64_t i = 0; i < n; i++) {
                if (i > 0) jb_putc(out, ',');
                json_encode_ctx(elems[i], out, ctx);
            }
            jb_putc(out, ']');
            break;
        }
        case ASNIX_TAG_ATTRS: {
            uint32_t sym_tostring = asnix_intern("__toString", 10);
            uint32_t sym_outpath = asnix_intern("outPath", 7);
            if (asnix_bshared_attrs_find(v, sym_tostring) >= 0 || asnix_bshared_attrs_find(v, sym_outpath) >= 0) {
                json_encode_ctx(asnix_coerce_to_string(v), out, ctx);
            } else {
                json_encode_attrs_ctx(v, out, ctx);
            }
            break;
        }
        default:
            asnix_bshared_fatal("toJSON", "cannot convert a function to JSON");
    }
}

char *asnix_toJSON_cstr(Value v) {
    JsonBuf jb;
    jb_init(&jb);
    json_encode(v, &jb);
    jb_putc(&jb, '\0');
    return jb.buf;
}

static Value prim_toJSON(const Value *args, int64_t argc) {
    (void)argc;
    JsonBuf jb;
    jb_init(&jb);
    AsnixStringContext ctx;
    asnix_ctx_init(&ctx);
    json_encode_ctx(args[0], &jb, &ctx);
    jb_putc(&jb, '\0');
    Value result = asnix_make_string_value(jb.buf, strlen(jb.buf));
    if (!asnix_ctx_is_empty(&ctx)) asnix_string_attach_context(result, &ctx);
    free(jb.buf);
    return result;
}
static Value build_toJSON(void) { return asnix_bshared_make_primop((void *)prim_toJSON, 1); }

const AsnixBuiltinMiscDef ASNIX_COMPARE_BUILTINS[] = {
    { "lessThan", build_lessThan },
    { "splitVersion", build_splitVersion },
    { "compareVersions", build_compareVersions },
    { "replaceStrings", build_replaceStrings },
    { "stringToCharacters", build_stringToCharacters },
    { "toJSON", build_toJSON },
};

const size_t ASNIX_COMPARE_BUILTINS_COUNT = sizeof(ASNIX_COMPARE_BUILTINS) / sizeof(ASNIX_COMPARE_BUILTINS[0]);

