/*
 * lessThan, splitVersion, compareVersions, replaceStrings,
 * stringToCharacters, toJSON -- see builtins_compare.h. Every builtin
 * below was verified against real `nix-instantiate --eval` (2.35.2)
 * before being trusted; see each function's own comment for the specific
 * expressions checked, and ~/nix/src/libstore/names.cc /
 * ~/nix/src/libexpr/primops.cc for the ported-from-source pieces.
 */
#include "builtins_compare.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/heap.h"
#include "asnix/ir.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

/* ---------------------------------------------------------------------
 * lessThan e1 e2 -- verified: `builtins.lessThan "a" "b"` == true,
 * `builtins.lessThan 1 2` == true, `builtins.lessThan "b" "a"` == false.
 * See builtins_shared.h's asnix_bshared_value_lt for the documented
 * int/string-only scope cut vs. real Nix's lessThan (which also orders
 * paths and lists).
 * --------------------------------------------------------------------- */
static Value lessThan2_impl(const Value *args, int64_t argc) {
    (void)argc;
    return asnix_bshared_value_lt(args[0], args[1], "lessThan") ? ASNIX_TRUE : ASNIX_FALSE;
}
static Value build_lessThan(void) { return asnix_bshared_make_curried_native((void *)lessThan2_impl, 2); }

/* ---------------------------------------------------------------------
 * splitVersion / compareVersions -- ported directly from
 * ~/nix/src/libstore/names.cc's `nextComponent`/`componentsLT`/
 * `compareVersions` (byte-index-based reimplementation of the same
 * iterator-based algorithm, since asnix strings aren't NUL-terminated).
 *
 * nextComponent: skip leading '.'/'-' separators (dropped, not part of
 * any component); if a digit follows, consume the maximal digit run;
 * otherwise consume the maximal run of non-digit, non-separator bytes.
 * Verified against real Nix for several genuinely non-obvious cases
 * before trusting this port: `splitVersion "1pre"` == ["1" "pre"]
 * (digit/non-digit transition splits even with no explicit separator);
 * `splitVersion "1.2.3abc"` == ["1" "2" "3" "abc"] (same transition rule
 * applies *after* explicit-separator splitting too); `splitVersion "1_2"`
 * == ["1" "_" "2"] (a single non-digit, non-letter byte is still just
 * "non-digit" -- there is no third character class); `splitVersion
 * "foo_bar"` == ["foo_bar"] (one component: no digit/non-digit transition
 * anywhere in an all-non-digit run, regardless of which non-digit bytes
 * it contains); `splitVersion "1..2"` == ["1" "2"] (consecutive
 * separators collapse, no empty component in between); `splitVersion ""`
 * == [].
 *
 * componentsLT: numeric compare if both components are all-digits;
 * otherwise a handful of special cases for Nix's pre-release convention
 * (an empty component or literally "pre" sorts before anything numeric;
 * "pre" sorts before any other non-numeric component; a non-numeric
 * component sorts before a numeric one, "assume 2.3a < 2.3.1" per the
 * upstream comment) then falls back to plain lexicographic byte compare.
 * Verified: `compareVersions "1.0" "1"` == 1 (missing trailing component
 * is treated as "", and "0" > ""); `compareVersions "1pre" "1"` == -1;
 * `compareVersions "1" "1pre"` == 1; `compareVersions "1.9" "1.10"` ==
 * -1 (numeric, not lexicographic, comparison of "9" vs "10");
 * `compareVersions "1.2.3" "1.2.3abc"` == -1: `splitVersion "1.2.3abc"`
 * has an *extra* 4th component ("abc", from the same digit/non-digit
 * transition rule as "1pre" above) that "1.2.3" doesn't have, so the 4th
 * round compares "" (v1 exhausted) against "abc" -- neither is numeric
 * nor "pre", so this falls all the way through to the final
 * lexicographic-length fallback (`l1 < l2`, i.e. `0 < 3`), making the
 * missing component sort before the present one.
 * --------------------------------------------------------------------- */
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
    if (is2) return 1; /* "2.3a" < "2.3.1" */
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

/* ---------------------------------------------------------------------
 * replaceStrings from to s -- ported directly from
 * ~/nix/src/libexpr/primops.cc's prim_replaceStrings: scans `s` one
 * position at a time (0..len inclusive -- the "inclusive" matters for a
 * `from` entry that's the empty string, which can match at the very end);
 * at each position, tries every `from[i]` *in list order* and uses the
 * *first* one that matches at that exact position (not the longest
 * match); on a match, appends the corresponding `to[i]` and advances by
 * `from[i]`'s length (or by one character, copied through verbatim, if
 * `from[i]` is empty -- otherwise an empty `from` entry would loop
 * forever at the same position); on no match, copies the current
 * character through verbatim and advances by one.
 *
 * Verified against real Nix for the two trickiest cases before trusting
 * this port: `replaceStrings [""] ["X"] "ab"` == "XaXbX" (empty-`from`
 * inserts `to` at every position, including both ends) and
 * `replaceStrings ["a"] ["aa"] "banana"` == "baanaanaa" (a replacement
 * that re-contains the original `from` text does NOT recurse/re-match --
 * `p` always advances past what was just consumed, consistent with the
 * single left-to-right scan above).
 * --------------------------------------------------------------------- */
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
    const char **to_bytes = nfrom > 0 ? malloc((size_t)nfrom * sizeof(char *)) : NULL;
    int64_t *to_lens = nfrom > 0 ? malloc((size_t)nfrom * sizeof(int64_t)) : NULL;
    for (int64_t i = 0; i < nfrom; i++) {
        /* List elements are lazy (e.g. a `map`-produced list's elements are
         * deliberately unforced thunks, M23) -- force each one before
         * reading its bytes, same as OP_SELECT/OP_VAR_REF do for attrs/vars
         * on read. Found via real nixpkgs: `replaceStrings [...] (map f
         * someList) s`-shaped calls crashed with "expected a string
         * argument" on an unforced Thunk. */
        asnix_bshared_string_bytes(asnix_force_for_print(from_elems[i]), &from_bytes[i], &from_lens[i], "replaceStrings");
        asnix_bshared_string_bytes(asnix_force_for_print(to_elems[i]), &to_bytes[i], &to_lens[i], "replaceStrings");
    }

    int64_t slen;
    const char *sbytes;
    asnix_bshared_string_bytes(s_v, &sbytes, &slen, "replaceStrings");

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
            int64_t tl = to_lens[found];
            if (outlen + (size_t)tl + 1 > cap) {
                cap = (outlen + (size_t)tl + 1) * 2;
                out = realloc(out, cap);
            }
            if (tl > 0) memcpy(out + outlen, to_bytes[found], (size_t)tl);
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
    free(out);
    free(from_bytes);
    free(from_lens);
    free(to_bytes);
    free(to_lens);
    return result;
}
static Value build_replaceStrings(void) { return asnix_bshared_make_curried_native((void *)replaceStrings3_impl, 3); }

/* ---------------------------------------------------------------------
 * stringToCharacters s -- not actually a core Nix builtin (confirmed:
 * `builtins ? stringToCharacters` is false); it's `lib.stringToCharacters
 * = s: genList (p: substring p 1 s) (stringLength s)`
 * (lib/strings.nix). Added here under `builtins` for the same reason as
 * `attrByPath`/`foldl` elsewhere in this pass. Verified:
 * `lib.stringToCharacters "abc"` == ["a" "b" "c"]; `lib.stringToCharacters
 * ""` == [].
 * --------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------
 * toJSON v -- recursive, compact (no whitespace) JSON serialization,
 * matching real Nix's `nlohmann::json::dump()` output byte-for-byte for
 * every case checked: `builtins.toJSON { a=1; b=[1 2 "x\"y"]; c=null;
 * d=true; e=false; }` == "{\"a\":1,\"b\":[1,2,\"x\\\"y\"],\"c\":null,
 * \"d\":true,\"e\":false}" (object keys in alphabetical order, same as
 * this project's own attrset printer -- see builtins_attrs.c's
 * sorted_name_order); `builtins.toJSON "a\nb\tc\\d"` ==
 * "\"a\\nb\\tc\\\\d\"" (standard short escapes for \n \t \\, and \" for
 * quotes -- not ported from a special Nix rule, this is plain JSON string
 * escaping: \b \f \n \r \t \" \\ get their two-character escapes, other
 * control bytes (<0x20) get \u00XX, everything else (including raw UTF-8
 * continuation bytes -- this project has no Unicode awareness anywhere
 * else either) passes through unescaped, matching nlohmann's default
 * `dump()` with `ensure_ascii=false`); `builtins.toJSON []` == "[]";
 * `builtins.toJSON {}` == "{}"; `builtins.toJSON (-5)` == "-5";
 * `builtins.toJSON (a: a)` errors on real Nix ("cannot convert a function
 * to JSON") -- functions/primops are rejected here too.
 *
 * A Path argument is emitted as its own plain (absolute) text, not
 * copied into the Nix store the way real Nix's `toJSON` does (verified:
 * `builtins.toJSON ./.` there produces a `"/nix/store/..."` string) --
 * this project has no `AddToStore` wired up at all (see
 * docs/architecture.md's M8/M15 scope notes on string context), so this
 * is the same already-accepted, already-documented divergence `toString`
 * and `"${path}"` interpolation both already have, not a new one.
 * --------------------------------------------------------------------- */
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

static void jb_escape_string(JsonBuf *out, const char *bytes, int64_t len) {
    jb_putc(out, '"');
    for (int64_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        switch (c) {
            case '"': jb_puts(out, "\\\"", 2); break;
            case '\\': jb_puts(out, "\\\\", 2); break;
            case '\b': jb_puts(out, "\\b", 2); break;
            case '\f': jb_puts(out, "\\f", 2); break;
            case '\n': jb_puts(out, "\\n", 2); break;
            case '\r': jb_puts(out, "\\r", 2); break;
            case '\t': jb_puts(out, "\\t", 2); break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    int n = snprintf(esc, sizeof esc, "\\u%04x", c);
                    jb_puts(out, esc, (size_t)n);
                } else {
                    jb_putc(out, (char)c);
                }
        }
    }
    jb_putc(out, '"');
}

static int json_name_cmp(const char *an, size_t al, const char *bn, size_t bl) {
    size_t n = al < bl ? al : bl;
    int c = n > 0 ? memcmp(an, bn, n) : 0;
    if (c != 0) return c;
    return (al > bl) - (al < bl);
}

static void json_encode(Value v, JsonBuf *out);

static void json_encode_attrs(Value v, JsonBuf *out) {
    int64_t n = asnix_bshared_attrs_count(v);
    int64_t *order = n > 0 ? malloc((size_t)n * sizeof(int64_t)) : NULL;
    for (int64_t i = 0; i < n; i++) order[i] = i;
    for (int64_t i = 1; i < n; i++) {
        int64_t j = i;
        while (j > 0) {
            size_t al, bl;
            const char *an = asnix_sym_name(asnix_bshared_attrs_sym(v, order[j - 1]), &al);
            const char *bn = asnix_sym_name(asnix_bshared_attrs_sym(v, order[j]), &bl);
            if (json_name_cmp(an, al, bn, bl) <= 0) break;
            int64_t tmp = order[j - 1];
            order[j - 1] = order[j];
            order[j] = tmp;
            j--;
        }
    }
    jb_putc(out, '{');
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) jb_putc(out, ',');
        size_t len;
        const char *name = asnix_sym_name(asnix_bshared_attrs_sym(v, order[i]), &len);
        jb_escape_string(out, name, (int64_t)len);
        jb_putc(out, ':');
        json_encode(asnix_force_for_print(asnix_bshared_attrs_value(v, order[i])), out);
    }
    jb_putc(out, '}');
    free(order);
}

static void json_encode(Value v, JsonBuf *out) {
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
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len); /* string or path, see header comment */
            jb_escape_string(out, bytes, len);
            break;
        }
        case ASNIX_TAG_LIST: {
            int64_t n = asnix_bshared_list_count(v);
            const Value *elems = asnix_bshared_list_elems(v);
            jb_putc(out, '[');
            for (int64_t i = 0; i < n; i++) {
                if (i > 0) jb_putc(out, ',');
                json_encode(elems[i], out);
            }
            jb_putc(out, ']');
            break;
        }
        case ASNIX_TAG_ATTRS:
            json_encode_attrs(v, out);
            break;
        default:
            asnix_bshared_fatal("toJSON", "cannot convert a function to JSON");
    }
}

static Value prim_toJSON(const Value *args, int64_t argc) {
    (void)argc;
    JsonBuf jb;
    jb_init(&jb);
    json_encode(args[0], &jb);
    Value result = asnix_make_string_value(jb.buf, jb.len);
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
