/* open_memstream (used by toXML) is POSIX, not C11 -- needs an explicit
 * feature-test macro under -std=c11 (same reason eval_file.c defines
 * _DEFAULT_SOURCE for realpath()). */
#define _POSIX_C_SOURCE 200809L

#include "builtins_m54.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irbuild.h"
#include "base32.h"
#include "base64.h"
#include "builtins_shared.h"
#include "eval_file.h"
#include "hash_shim.h"
#include "symtab.h"
#include "value_util.h"

static void fatal(const char *who, const char *msg) {
    fprintf(stderr, "asnix: %s: %s\n", who, msg);
    exit(1);
}

/* ---------------------------------------------------------------------
 * bitAnd / bitOr / bitXor (M54) -- `__bitAnd`/`__bitOr`/`__bitXor` in real
 * Nix (confirmed via cppnix source, ~/nix/src/libexpr/primops.cc: all
 * three `__`-prefixed, builtins-only). Plain arity-2 integer bitwise ops,
 * curried the same way every other true-arity-2 builtin in this codebase
 * is (asnix_bshared_make_curried_native -- see builtins_misc.c's
 * make_curried_native for the full rationale). Verified against real
 * `nix-instantiate --eval`:
 *   bitAnd 6 3  == 2    (0b110 & 0b011 = 0b010)
 *   bitOr  6 3  == 7    (0b110 | 0b011 = 0b111)
 *   bitXor 6 3  == 5    (0b110 ^ 0b011 = 0b101)
 *   bitAnd 0 0  == 0
 * --------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------
 * trace msg value (M54) -- `__trace` in real Nix (builtins-only).
 * Real Nix's own `prim_trace` forces (and prints) `msg` but does NOT
 * force `value` -- it's returned exactly as received, matching the
 * project's established "lazy pass-through" idiom (same as `getAttr`,
 * M23). Verified against real `nix-instantiate --eval`:
 *   trace "hi" 1                       -> stderr "trace: hi", result 1
 *   trace {a=1;} 1                     -> stderr "trace: { a = 1; }"
 *   trace ["a" "b"] 1                  -> stderr "trace: [ \"a\" \"b\" ]"
 *   let y = trace "x" (abort "boom"); in 1  -> 1, NO stderr output at all
 *     (y unused -> its thunk never forced -> trace never even runs,
 *     ordinary M6 let-laziness, nothing trace-specific)
 * The crucial top-level formatting rule (confirmed against 4 separate
 * nix-instantiate cases): when `msg` forces to a STRING, its RAW bytes
 * are printed unquoted ("trace: hi", not "trace: \"hi\""); every other
 * type (including a string NESTED inside a list/attrset) uses the
 * ordinary quoted/structural printer. This project's 4 actual conformance
 * callers (eval-okay-tryeval-failed-thunk-reeval/sort-peeksort/
 * inherit-from/print) only ever trace a plain top-level string, so the
 * nested-container/function-value branches below are a best-effort,
 * not-fully-faithful fallback (no attrset-name quoting/reserved-keyword
 * handling the way main.c's own printer has, and no `<PRIMOP>`/`<LAMBDA>`/
 * cycle-detection notation the way real Nix's repl-style printer has) --
 * documented, deliberate scope cut, see docs/architecture.md's M54 notes.
 *
 * Needs its argument UNFORCED (NOFORCE) so `value` doesn't get forced
 * -- and therefore potentially abort/throw -- by op_call.S's ordinary
 * pre-call argument-forcing (M25/M38) before this function even runs,
 * which would both force `value` too early (before `msg` is even
 * printed) and defeat the "pass `value` through exactly as received"
 * contract. The real native call still only happens once something
 * forces the overall `trace msg value` application to begin with (two
 * ordinary Closure-call curry stages first, forcing nothing, M25) --
 * msg is forced/printed HERE, manually, and `value` is returned raw;
 * op_call.S's unconditional POST-call force (M23/M38) still forces the
 * returned `value` to WHNF once that outer application is itself
 * forced, exactly mirroring getAttr's own "lazy passthrough" shape. */
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

/* Same curry-chain shape as asnix_bshared_make_curried_native
 * (builtins_shared.c), but building the real native primop via
 * asnix_bshared_make_primop_noforce instead -- needed only by `trace`
 * so far, hence not promoted into builtins_shared.c's own general
 * helper (which every OTHER curried-native builtin in this codebase
 * wants force-args-on, the ordinary default). */
static Value make_curried_native_noforce(void *native_fn, int64_t arity) {
    Value native_primop = asnix_bshared_make_primop_noforce(native_fn, arity);
    Value empty_formals = asnix_bshared_make_attrs(NULL, NULL, 0);

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
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_BODY) = prog->ops;
    *(int64_t *)((char *)mem + ASNIX_CLOSURE_OFF_ARITY) = 1;
    *(void **)((char *)mem + ASNIX_CLOSURE_OFF_POOL) = prog->pool;
    *(uint64_t *)((char *)mem + ASNIX_CLOSURE_OFF_FORMALS) = empty_formals.raw;
    return (Value){ .raw = (uint64_t)(uintptr_t)mem | ASNIX_TAG_CLOSURE };
}

static Value build_trace(void) { return make_curried_native_noforce((void *)trace2_impl, 2); }

/* ---------------------------------------------------------------------
 * zipAttrsWith f listOfAttrsets (M54) -- `__zipAttrsWith` in real Nix
 * (builtins-only). For each distinct key appearing in ANY of the given
 * attrsets, collects every value present under that key -- in the
 * LIST's own order, not alphabetical, not per-attrset insertion order --
 * into a list, then calls `f key thatList`; the result is one attrset
 * with those per-key results. Verified against real `nix-instantiate
 * --eval`:
 *   zipAttrsWith (n: v: v) [{a=1;b=2;} {a=3;c=4;}]
 *     == { a = [ 1 3 ]; b = [ 2 ]; c = [ 4 ]; }
 *   zipAttrsWith (n: v: v) [] == {}
 * Each output value is a genuine lazy call (asnix_bshared_make_lazy_
 * call2, same M23 laziness fix every other map-shaped builtin in this
 * codebase already uses for its callback), not an eagerly-invoked one --
 * `f` must not run just from zipAttrsWith being called, only once a
 * given key's result is actually forced. */
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

    /* Collect every (sym) appearing in any input attrset, then dedupe
     * and sort -- plain insertion sort, same "always small" rationale
     * as every other such sort in this codebase. */
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
    uint32_t *distinct = total_n > 0 ? asnix_arena_alloc((uint64_t)total_n * sizeof(uint32_t)) : NULL;
    int64_t distinct_n = 0;
    for (int64_t i = 0; i < total_n; i++) {
        if (distinct_n == 0 || distinct[distinct_n - 1] != all_syms[i]) distinct[distinct_n++] = all_syms[i];
    }

    Value *out_vals = distinct_n > 0 ? asnix_arena_alloc((uint64_t)distinct_n * sizeof(Value)) : NULL;
    for (int64_t k = 0; k < distinct_n; k++) {
        uint32_t sym = distinct[k];
        Value *per_key = n > 0 ? asnix_arena_alloc((uint64_t)n * sizeof(Value)) : NULL;
        int64_t cnt = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t idx = asnix_bshared_attrs_find(sets[i], sym);
            if (idx >= 0) per_key[cnt++] = asnix_bshared_attrs_value(sets[i], idx);
        }
        Value value_list = asnix_bshared_make_list(per_key, cnt);
        size_t namelen;
        const char *name = asnix_sym_name(sym, &namelen);
        Value name_val = asnix_make_string_value(name, namelen);
        out_vals[k] = asnix_bshared_make_lazy_call2(f, name_val, value_list);
    }
    return asnix_bshared_make_attrs(distinct, out_vals, distinct_n);
}

static Value build_zipAttrsWith(void) { return asnix_bshared_make_curried_native((void *)zipAttrsWith2_impl, 2); }

/* ---------------------------------------------------------------------
 * toXML v (M54) -- `__toXML` in real Nix (builtins-only). Serializes
 * `v` as Nix's own XML schema, ported field-for-field from real Nix's
 * `printValueAsXML`/`XMLWriter` (~/nix/src/libexpr/value-to-xml.cc,
 * ~/nix/src/libutil/xml-writer.cc): 2-space indent per nesting level, a
 * self-closing `<tag attr="val" />` for leaf values, `<tag>`/`</tag>` on
 * their own lines for containers, attrset keys in ALPHABETICAL order
 * (matching this project's existing attrNames convention). Verified
 * against numtide/nix-conformance's eval-okay-toxml.nix/eval-okay-
 * toxml2.nix expected output byte-for-byte (including the exact `<?xml
 * version='1.0' encoding='utf-8'?>` header line and indentation) before
 * trusting this.
 *
 * Deliberately scoped out (not exercised by either target test, and
 * each would need infrastructure this project doesn't have): real
 * Nix's `isDerivation`-special-cased `<derivation>` element (asnix's
 * own derivation support is itself a documented, narrower M8 scope
 * cut), position attributes on `<attr>`/`<function>` (prim_toXML always
 * passes `location=false`, so real Nix itself never emits these either
 * -- nothing lost), and a fully faithful `<function>`/`<varpat>` (a
 * plain, non-pattern lambda's single argument NAME isn't tracked
 * anywhere in this project's Closure representation, only pattern-
 * style formals are, see M37's functionArgs) -- closures print as an
 * empty `<function><varpat name="" /></function>` and primops/
 * primop-applications print `<unevaluated />` (matching real Nix's own
 * "FIXME: Serialize primops and primopapps" fallback exactly, just
 * applied to lambdas too here). */
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

/* ---------------------------------------------------------------------
 * convertHash { hash, hashAlgo?, toHashFormat } (M54) -- `__convertHash`
 * in real Nix (builtins-only). Ported from real Nix's own `Hash::
 * parseAny`/`Hash::to_string` (~/nix/src/libutil/hash.cc): `hash` may be
 * bare (format detected purely from its decoded LENGTH against the
 * known algorithm digest size), `algo:body`-prefixed, or an SRI string
 * (`algo-base64body`) -- `hashAlgo` is required unless the algo can be
 * read from one of those two prefix forms (and if both are present, they
 * must agree). `toHashFormat` is one of "base16"/"nix32"/"base32"
 * (deprecated alias for nix32 -- same encoding) /"base64"/"sri" (always
 * algo-prefixed, with '-'; base16/nix32/base64 are never algo-prefixed in
 * convertHash's output -- confirmed directly against real Nix:
 * `convertHash { hash="e3b0..."; hashAlgo="sha256"; toHashFormat=
 * "base16"; }` == "e3b0..." with NO "sha256:" prefix). Verified against
 * numtide/nix-conformance's eval-okay-convertHash.nix/.exp, which
 * exercises all 4 algorithms x all 5 formats x bare/colon-prefixed input,
 * before trusting this. */
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

/* Parses `s` (the `hash` attribute's raw bytes) -- with an optional
 * already-known algorithm name from the `hashAlgo` attribute
 * (`given_algo`/`given_algo_len`, NULL/0 if that attribute was omitted)
 * -- into `out_raw` (caller-sized >= 64 bytes) and reports the resolved
 * algorithm's digest size/canonical name. */
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
        return; /* unreachable -- fatal() never returns, silences -Wmaybe-uninitialized */
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
    return ASNIX_NULL; /* unreachable */
}
static Value build_convertHash(void) { return asnix_bshared_make_primop((void *)prim_convertHash, 1); }

/* ---------------------------------------------------------------------
 * getContext s / appendContext msg ctxAttrs (M54) -- `__getContext`/
 * `__appendContext` in real Nix (both builtins-only). asnix has NO
 * string-context tracking at all anywhere in this codebase (confirmed:
 * grepping every src/c/ C file for "context" turns up only unrelated hits --
 * error-context (`addErrorContext`, M39), parser context, register-
 * save context) -- derivation-output string context is a substantially
 * larger, separate feature (tracked elsewhere, not attempted here).
 * Scoped to exactly the one case that's realistically supportable
 * without it: a plain string/value with NO actual context attached.
 * Verified against real `nix-instantiate --eval`:
 *   getContext "plain"           == {}
 *   getContext ""                == {}
 *   appendContext "plain" {}     == "plain"   (same string, unaffected)
 * `appendContext`'s second argument is validated (must be an attrset --
 * real Nix also validates its exact {outputs=[...];}/{allOutputs=true;}/
 * {path=true;} per-key shape, not replicated here since nothing is ever
 * actually stored) but otherwise ignored: since nothing in this
 * evaluator ever tracks or consults context, returning `msg` completely
 * unchanged already reproduces every observable case above. The three
 * conformance tests that exercise these (eval-okay-context-builtins/
 * substring-context/context-inputs) each fundamentally depend on real
 * derivation-output context propagation through `${drv}`/`drv.outPath`/
 * `builtins.toFile`/etc, which this scope cut does not provide -- they
 * are expected to keep failing for that documented, pre-existing reason
 * (asnix's derivation support is itself a narrower M8 scope cut), not
 * because getContext/appendContext themselves are wrong. */
static Value prim_getContext(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "getContext");
    return asnix_bshared_make_attrs(NULL, NULL, 0);
}
static Value build_getContext(void) { return asnix_bshared_make_primop((void *)prim_getContext, 1); }

static Value appendContext2_impl(const Value *args, int64_t argc) {
    (void)argc;
    ASNIX_REQUIRE_STRING(args[0], "appendContext");
    ASNIX_REQUIRE_ATTRS(args[1], "appendContext");
    return args[0];
}
static Value build_appendContext(void) { return asnix_bshared_make_curried_native((void *)appendContext2_impl, 2); }

/* ---------------------------------------------------------------------
 * scopedImport scope path (M54) -- one of real Nix's bare root-scope
 * globals (confirmed via cppnix source, ~/nix/src/libexpr/primops.cc:
 * `.name = "scopedImport"`, no `__` prefix -- see M27's bare-global-rule
 * notes). `scope`'s attribute names become extra bare identifiers,
 * resolvable (and shadowing any bare global/builtins.X of the same
 * name) inside `path`'s own lexical scope -- see compiler.c's
 * asnix_compile_scoped/eval_file.c's asnix_eval_file_scoped for the
 * actual mechanism (a real compile-time ScopeFrame + runtime Env hop,
 * exactly mirroring cppnix's own StaticEnv-splicing implementation, NOT
 * a `with`-style runtime fallback -- `with` would give scope's names the
 * WRONG priority here, see compiler.c's comment). The path argument
 * accepts the same two shapes `builtins.import` already does (a Path, or
 * an absolute-path String) via the same resolve-and-reject-relative-
 * strings logic (builtins.c's prim_import). Verified against real
 * nix-instantiate (numtide/nix-conformance's own eval-okay-import.nix,
 * which exercises both the basic shadowing case and a recursive
 * `scopedImport (overrides // s) fn`-via-overridden-bare-`import`
 * pattern) before trusting this. */
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
        return ASNIX_NULL; /* unreachable */
    }
    char *path = asnix_arena_alloc((size_t)len + 1);
    memcpy(path, bytes, (size_t)len);
    path[len] = '\0';
    return asnix_eval_file_scoped(path, scope);
}
static Value build_scopedImport(void) { return asnix_bshared_make_curried_native((void *)scopedImport2_impl, 2); }

Value asnix_make_scopedImport_primop(void) { return build_scopedImport(); }

const AsnixBuiltinM54Def ASNIX_M54_BUILTINS[] = {
    { "bitAnd", build_bitAnd },
    { "bitOr", build_bitOr },
    { "bitXor", build_bitXor },
    { "trace", build_trace },
    { "zipAttrsWith", build_zipAttrsWith },
    { "toXML", build_toXML },
    { "convertHash", build_convertHash },
    { "getContext", build_getContext },
    { "appendContext", build_appendContext },
    { "scopedImport", build_scopedImport },
};

const size_t ASNIX_M54_BUILTINS_COUNT = sizeof(ASNIX_M54_BUILTINS) / sizeof(ASNIX_M54_BUILTINS[0]);
