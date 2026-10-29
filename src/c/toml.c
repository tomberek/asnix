#include "toml.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/value.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "value_util.h"

static void toml_fatal(const char *msg) {
    fprintf(stderr, "asnix: fromTOML: %s\n", msg);
    exit(1);
}

typedef struct {
    const char *s;
    size_t n;
    size_t i;
} Parser;

static int p_at_end(const Parser *p) { return p->i >= p->n; }
static char p_cur(const Parser *p) { return p_at_end(p) ? '\0' : p->s[p->i]; }
static char p_at(const Parser *p, size_t off) { return (p->i + off < p->n) ? p->s[p->i + off] : '\0'; }

static void expect_char(Parser *p, char c) {
    if (p_cur(p) != c) {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected '%c'", c);
        toml_fatal(buf);
    }
    p->i++;
}

static void skip_ws(Parser *p) {
    while (!p_at_end(p) && (p_cur(p) == ' ' || p_cur(p) == '\t')) p->i++;
}

static void skip_to_eol(Parser *p) {
    while (!p_at_end(p) && p_cur(p) != '\n') p->i++;
}

static void skip_ws_comments_newlines(Parser *p) {
    for (;;) {
        if (!p_at_end(p) && (p_cur(p) == ' ' || p_cur(p) == '\t' || p_cur(p) == '\r' || p_cur(p) == '\n')) {
            p->i++;
            continue;
        }
        if (!p_at_end(p) && p_cur(p) == '#') {
            skip_to_eol(p);
            continue;
        }
        break;
    }
}

typedef struct TNode TNode;
typedef enum { TN_TABLE, TN_ARRTABLE, TN_LEAF } TNKind;

typedef struct {
    uint32_t sym;
    TNode *child;
} TNEntry;

struct TNode {
    TNKind kind;
    TNEntry *entries;
    size_t n_entries, cap_entries;
    TNode **items;
    size_t n_items, cap_items;
    Value leaf;
    int declared_as_table;
};

static TNode *tnode_new(TNKind k) {
    TNode *t = calloc(1, sizeof(TNode));
    t->kind = k;
    return t;
}

static TNode *tnode_find_child(TNode *t, uint32_t sym) {
    for (size_t i = 0; i < t->n_entries; i++) {
        if (t->entries[i].sym == sym) return t->entries[i].child;
    }
    return NULL;
}

static void tnode_add_child(TNode *t, uint32_t sym, TNode *child) {
    if (t->n_entries == t->cap_entries) {
        t->cap_entries = t->cap_entries ? t->cap_entries * 2 : 4;
        t->entries = realloc(t->entries, t->cap_entries * sizeof(TNEntry));
    }
    t->entries[t->n_entries].sym = sym;
    t->entries[t->n_entries].child = child;
    t->n_entries++;
}

static void tnode_arrtable_push(TNode *t, TNode *item) {
    if (t->n_items == t->cap_items) {
        t->cap_items = t->cap_items ? t->cap_items * 2 : 4;
        t->items = realloc(t->items, t->cap_items * sizeof(TNode *));
    }
    t->items[t->n_items++] = item;
}

static TNode *tnode_get_or_create_table_child(TNode *t, uint32_t sym) {
    TNode *c = tnode_find_child(t, sym);
    if (!c) {
        c = tnode_new(TN_TABLE);
        tnode_add_child(t, sym, c);
        return c;
    }
    if (c->kind == TN_ARRTABLE) {
        if (c->n_items == 0) toml_fatal("internal error: empty array-of-tables navigated");
        return c->items[c->n_items - 1];
    }
    if (c->kind == TN_TABLE) return c;
    toml_fatal("a key is used both as a value and as a table");
    return NULL;
}

static void tnode_set_leaf(TNode *t, uint32_t sym, Value v) {
    if (tnode_find_child(t, sym)) toml_fatal("duplicate key in TOML document");
    TNode *leaf = tnode_new(TN_LEAF);
    leaf->leaf = v;
    tnode_add_child(t, sym, leaf);
}

static void tnode_insert_leaf_path(TNode *base, const uint32_t *syms, size_t n, Value v) {
    TNode *cur = base;
    for (size_t i = 0; i + 1 < n; i++) cur = tnode_get_or_create_table_child(cur, syms[i]);
    tnode_set_leaf(cur, syms[n - 1], v);
}

static TNode *tnode_enter_table(TNode *root, const uint32_t *syms, size_t n) {
    TNode *cur = root;
    for (size_t i = 0; i + 1 < n; i++) cur = tnode_get_or_create_table_child(cur, syms[i]);
    uint32_t last = syms[n - 1];
    TNode *child = tnode_find_child(cur, last);
    if (!child) {
        child = tnode_new(TN_TABLE);
        tnode_add_child(cur, last, child);
    } else if (child->kind != TN_TABLE) {
        toml_fatal("[table] header redefines a non-table key");
    } else if (child->declared_as_table) {
        toml_fatal("[table] header redefines an already-declared table");
    }
    child->declared_as_table = 1;
    return child;
}

static TNode *tnode_enter_array_table(TNode *root, const uint32_t *syms, size_t n) {
    TNode *cur = root;
    for (size_t i = 0; i + 1 < n; i++) cur = tnode_get_or_create_table_child(cur, syms[i]);
    uint32_t last = syms[n - 1];
    TNode *child = tnode_find_child(cur, last);
    if (!child) {
        child = tnode_new(TN_ARRTABLE);
        tnode_add_child(cur, last, child);
    } else if (child->kind != TN_ARRTABLE) {
        toml_fatal("[[array of tables]] header redefines a non-array key");
    }
    TNode *item = tnode_new(TN_TABLE);
    tnode_arrtable_push(child, item);
    return item;
}

static Value finalize_tnode(TNode *t) {
    Value out;
    switch (t->kind) {
        case TN_LEAF:
            out = t->leaf;
            break;
        case TN_TABLE: {
            size_t n = t->n_entries;
            uint32_t *syms = malloc((n ? n : 1) * sizeof(uint32_t));
            Value *vals = malloc((n ? n : 1) * sizeof(Value));
            for (size_t i = 0; i < n; i++) {
                syms[i] = t->entries[i].sym;
                vals[i] = finalize_tnode(t->entries[i].child);
            }
            out = asnix_bshared_make_attrs(syms, vals, (int64_t)n);
            free(syms);
            free(vals);
            break;
        }
        case TN_ARRTABLE: {
            size_t n = t->n_items;
            Value *vals = malloc((n ? n : 1) * sizeof(Value));
            for (size_t i = 0; i < n; i++) vals[i] = finalize_tnode(t->items[i]);
            out = asnix_bshared_make_list(vals, (int64_t)n);
            free(vals);
            break;
        }
        default:
            toml_fatal("internal error: bad TNode kind");
            out = asnix_make_imm(ASNIX_IMM_NULL);
            break;
    }
    return out;
}

static void tnode_free(TNode *t) {
    if (!t) return;
    for (size_t i = 0; i < t->n_entries; i++) tnode_free(t->entries[i].child);
    free(t->entries);
    for (size_t i = 0; i < t->n_items; i++) tnode_free(t->items[i]);
    free(t->items);
    free(t);
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    toml_fatal("invalid hex digit in \\u/\\U escape");
    return 0;
}

static uint32_t parse_hex_digits(const char *s, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 4) | (uint32_t)hex_digit_value(s[i]);
    return v;
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static size_t scan_single_line_string_end(Parser *p, char quote, int escaped) {
    size_t i = p->i;
    while (i < p->n) {
        char c = p->s[i];
        if (escaped && c == '\\') {
            i += 2;
            continue;
        }
        if (c == quote) return i;
        if (c == '\n') toml_fatal("newline inside a single-line TOML string");
        i++;
    }
    toml_fatal("unterminated TOML string");
    return 0;
}

static size_t scan_multiline_string_end(Parser *p, char quote, int escaped) {
    size_t i = p->i;
    while (i < p->n) {
        char c = p->s[i];
        if (escaped && c == '\\') {
            i += 2;
            continue;
        }
        if (c == quote) {
            if (i + 2 < p->n && p->s[i + 1] == quote && p->s[i + 2] == quote) return i;
            i++;
            continue;
        }
        i++;
    }
    toml_fatal("unterminated multi-line TOML string");
    return 0;
}

static char *decode_basic(const char *s, size_t n, int multiline, size_t *out_len) {
    char *buf = malloc(n + 1);
    size_t o = 0, i = 0;
    while (i < n) {
        char c = s[i];
        if (c != '\\') {
            buf[o++] = c;
            i++;
            continue;
        }
        i++;
        if (i >= n) toml_fatal("dangling backslash at end of TOML string");
        if (multiline) {
            size_t j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && (s[j] == '\n' || (s[j] == '\r' && j + 1 < n && s[j + 1] == '\n'))) {
                i = j;
                while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
                continue;
            }
        }
        char e = s[i++];
        switch (e) {
            case 'b': buf[o++] = '\b'; break;
            case 't': buf[o++] = '\t'; break;
            case 'n': buf[o++] = '\n'; break;
            case 'f': buf[o++] = '\f'; break;
            case 'r': buf[o++] = '\r'; break;
            case '"': buf[o++] = '"'; break;
            case '\\': buf[o++] = '\\'; break;
            case 'u':
                if (i + 4 > n) toml_fatal("truncated \\u escape in TOML string");
                o += utf8_encode(parse_hex_digits(s + i, 4), buf + o);
                i += 4;
                break;
            case 'U':
                if (i + 8 > n) toml_fatal("truncated \\U escape in TOML string");
                o += utf8_encode(parse_hex_digits(s + i, 8), buf + o);
                i += 8;
                break;
            default: toml_fatal("invalid escape sequence in TOML string");
        }
    }
    *out_len = o;
    return buf;
}

static char *parse_toml_string(Parser *p, size_t *out_len) {
    char quote = p_cur(p);
    int basic = (quote == '"');
    int multiline = (p_at(p, 1) == quote && p_at(p, 2) == quote);
    size_t delim_len = multiline ? 3 : 1;
    p->i += delim_len;

    if (multiline) {
        if (p_cur(p) == '\n') p->i++;
        else if (p_cur(p) == '\r' && p_at(p, 1) == '\n') p->i += 2;
    }

    size_t content_start = p->i;
    size_t content_end = multiline ? scan_multiline_string_end(p, quote, basic) : scan_single_line_string_end(p, quote, basic);
    size_t raw_len = content_end - content_start;
    char *decoded;
    size_t decoded_len;
    if (basic) {
        decoded = decode_basic(p->s + content_start, raw_len, multiline, &decoded_len);
    } else {
        decoded = malloc(raw_len ? raw_len : 1);
        memcpy(decoded, p->s + content_start, raw_len);
        decoded_len = raw_len;
    }
    p->i = content_end + delim_len;
    if (memchr(decoded, '\0', decoded_len)) toml_fatal("string contains a null byte");
    *out_len = decoded_len;
    return decoded;
}

static int bare_key_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static char *parse_key_segment(Parser *p, size_t *out_len) {
    if (p_cur(p) == '"' || p_cur(p) == '\'') return parse_toml_string(p, out_len);
    size_t start = p->i;
    while (!p_at_end(p) && bare_key_char(p_cur(p))) p->i++;
    if (p->i == start) toml_fatal("expected a key");
    size_t len = p->i - start;
    char *buf = malloc(len ? len : 1);
    memcpy(buf, p->s + start, len);
    *out_len = len;
    return buf;
}

static void parse_key_path(Parser *p, uint32_t **syms_out, size_t *n_out) {
    size_t cap = 4, n = 0;
    uint32_t *syms = malloc(cap * sizeof(uint32_t));
    for (;;) {
        size_t len;
        char *buf = parse_key_segment(p, &len);
        if (n == cap) {
            cap *= 2;
            syms = realloc(syms, cap * sizeof(uint32_t));
        }
        syms[n++] = asnix_intern(buf, len);
        free(buf);
        skip_ws(p);
        if (p_cur(p) == '.') {
            p->i++;
            skip_ws(p);
            continue;
        }
        break;
    }
    *syms_out = syms;
    *n_out = n;
}

static int is_digit_run(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return n > 0;
}

static int looks_like_date_or_time(const char *tok, size_t n) {
    if (n >= 10 && is_digit_run(tok, 4) && tok[4] == '-' && is_digit_run(tok + 5, 2) && tok[7] == '-' &&
        is_digit_run(tok + 8, 2))
        return 1;
    if (n >= 8 && is_digit_run(tok, 2) && tok[2] == ':' && is_digit_run(tok + 3, 2) && tok[5] == ':' &&
        is_digit_run(tok + 6, 2))
        return 1;
    return 0;
}

static int valid_time_value(const char *s, size_t n) {
    if (!(n >= 8 && is_digit_run(s, 2) && s[2] == ':' && is_digit_run(s + 3, 2) && s[5] == ':' &&
          is_digit_run(s + 6, 2)))
        return 0;
    int hh = (s[0] - '0') * 10 + (s[1] - '0');
    int mm = (s[3] - '0') * 10 + (s[4] - '0');
    int ss = (s[6] - '0') * 10 + (s[7] - '0');
    return hh <= 23 && mm <= 59 && ss <= 60;
}

static int valid_date_value(const char *s) {
    int mo = (s[5] - '0') * 10 + (s[6] - '0');
    int dd = (s[8] - '0') * 10 + (s[9] - '0');
    return mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31;
}

static size_t write_normalized_frac(const char *digits, size_t n, char *out) {
    size_t take = n > 9 ? 9 : n;
    memcpy(out, digits, take);
    size_t len = take;
    while (len < 3) out[len++] = '0';
    return len;
}

static size_t normalize_time(const char *s, size_t n, char *out) {
    memcpy(out, s, 8);
    size_t o = 8, idx = 8;
    if (idx < n && s[idx] == '.') {
        idx++;
        size_t start = idx;
        while (idx < n && s[idx] >= '0' && s[idx] <= '9') idx++;
        size_t flen = idx - start;
        if (flen > 0) {
            out[o++] = '.';
            o += write_normalized_frac(s + start, flen, out + o);
        }
    }
    if (idx < n) {
        if (s[idx] == 'Z' || s[idx] == 'z') {
            out[o++] = 'Z';
            idx++;
        } else if (s[idx] == '+' || s[idx] == '-') {
            memcpy(out + o, s + idx, n - idx);
            o += n - idx;
            idx = n;
        }
    }
    return o;
}

static Value make_timestamp_value(const char *text, size_t len) {
    uint32_t sym_type = asnix_intern("_type", 5);
    uint32_t sym_value = asnix_intern("value", 5);
    uint32_t syms[2] = { sym_type, sym_value };
    Value vals[2] = { asnix_make_string_value("timestamp", 9), asnix_make_string_value(text, len) };
    return asnix_bshared_make_attrs(syms, vals, 2);
}

static Value classify_date_or_time(const char *tok, size_t n) {
    char buf[96];
    size_t o;
    (void)make_timestamp_value;
    if (n >= 10 && tok[4] == '-' && tok[7] == '-') {
        if (!valid_date_value(tok)) toml_fatal("invalid calendar date in TOML document");
        memcpy(buf, tok, 10);
        o = 10;
        if (n > 10) {
            char sep = tok[10];
            if (sep != 'T' && sep != 't' && sep != ' ') toml_fatal("malformed TOML date-time separator");
            if (!valid_time_value(tok + 11, n - 11))
                toml_fatal("invalid or incomplete time-of-day in TOML date-time (requires HH:MM:SS)");
            buf[o++] = 'T';
            o += normalize_time(tok + 11, n - 11, buf + o);
        }
    } else {
        if (!valid_time_value(tok, n)) toml_fatal("invalid or incomplete TOML local-time value (requires HH:MM:SS)");
        o = normalize_time(tok, n, buf);
    }
    (void)o;
    toml_fatal("Dates and times are not supported");
    return asnix_make_imm(ASNIX_IMM_NULL);
}

static void validate_digit_separators(const char *digits, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (digits[i] != '_') continue;
        if (i == 0 || i + 1 >= n || digits[i - 1] == '_' || digits[i + 1] == '_')
            toml_fatal("invalid digit separator placement in TOML integer literal");
    }
}

static Value parse_decimal_or_prefixed_int(const char *digits, size_t n, int base, int neg) {
    validate_digit_separators(digits, n);
    char buf[160];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (digits[i] == '_') continue;
        if (o >= sizeof(buf) - 1) toml_fatal("integer literal too long");
        buf[o++] = digits[i];
    }
    buf[o] = '\0';
    if (o == 0) toml_fatal("invalid integer literal");
    if (base == 10 && o > 1 && buf[0] == '0') toml_fatal("leading zero in decimal TOML integer literal");
    char *endptr;
    long long val = strtoll(buf, &endptr, base);
    if (endptr == buf) toml_fatal("invalid TOML value");
    if (neg) val = -val;
    return asnix_make_int((int64_t)val);
}

static Value parse_float_token(const char *rem, size_t rem_n, int neg) {
    char buf[160];
    size_t o = 0;
    if (neg) buf[o++] = '-';
    for (size_t i = 0; i < rem_n; i++) {
        if (rem[i] == '_') continue;
        if (o >= sizeof(buf) - 1) toml_fatal("float literal too long");
        buf[o++] = rem[i];
    }
    buf[o] = '\0';
    return asnix_make_float_value(strtod(buf, NULL));
}

static Value classify_bare_token(const char *tok, size_t n) {
    if (n == 4 && memcmp(tok, "true", 4) == 0) return asnix_make_imm(ASNIX_IMM_TRUE);
    if (n == 5 && memcmp(tok, "false", 5) == 0) return asnix_make_imm(ASNIX_IMM_FALSE);

    if (looks_like_date_or_time(tok, n)) return classify_date_or_time(tok, n);

    size_t idx = 0;
    int neg = 0;
    if (n > 0 && (tok[0] == '+' || tok[0] == '-')) {
        neg = (tok[0] == '-');
        idx = 1;
    }
    const char *rem = tok + idx;
    size_t rem_n = n - idx;
    if (rem_n == 0) toml_fatal("invalid TOML value");

    for (size_t i = 0; i < rem_n; i++) {
        if (rem[i] == ':') toml_fatal("invalid or incomplete TOML local-time value (requires HH:MM:SS)");
    }

    if ((rem_n == 3 && memcmp(rem, "inf", 3) == 0) || (rem_n == 3 && memcmp(rem, "nan", 3) == 0)) {
        return parse_float_token(rem, rem_n, neg);
    }

    if (rem_n > 2 && rem[0] == '0' && (rem[1] == 'x' || rem[1] == 'X')) return parse_decimal_or_prefixed_int(rem + 2, rem_n - 2, 16, neg);
    if (rem_n > 2 && rem[0] == '0' && rem[1] == 'o') return parse_decimal_or_prefixed_int(rem + 2, rem_n - 2, 8, neg);
    if (rem_n > 2 && rem[0] == '0' && rem[1] == 'b') return parse_decimal_or_prefixed_int(rem + 2, rem_n - 2, 2, neg);

    for (size_t i = 0; i < rem_n; i++) {
        if (rem[i] == '.' || rem[i] == 'e' || rem[i] == 'E') return parse_float_token(rem, rem_n, neg);
    }
    return parse_decimal_or_prefixed_int(rem, rem_n, 10, neg);
}

static int bare_value_char(char c) {
    return isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.' || c == '_' || c == ':';
}

static Value parse_value(Parser *p);

static Value parse_array(Parser *p) {
    expect_char(p, '[');
    size_t cap = 4, n = 0;
    Value *items = malloc(cap * sizeof(Value));
    skip_ws_comments_newlines(p);
    if (p_cur(p) == ']') {
        p->i++;
        free(items);
        return asnix_bshared_make_list(NULL, 0);
    }
    for (;;) {
        Value v = parse_value(p);
        if (n == cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(Value));
        }
        items[n++] = v;
        skip_ws_comments_newlines(p);
        if (p_cur(p) == ',') {
            p->i++;
            skip_ws_comments_newlines(p);
            if (p_cur(p) == ']') {
                p->i++;
                break;
            }
            continue;
        }
        if (p_cur(p) == ']') {
            p->i++;
            break;
        }
        toml_fatal("expected ',' or ']' in TOML array");
    }
    Value out = asnix_bshared_make_list(items, (int64_t)n);
    free(items);
    return out;
}

static Value parse_inline_table(Parser *p) {
    expect_char(p, '{');
    TNode *tbl = tnode_new(TN_TABLE);
    skip_ws(p);
    if (p_cur(p) == '}') {
        p->i++;
        Value out = finalize_tnode(tbl);
        tnode_free(tbl);
        return out;
    }
    for (;;) {
        skip_ws(p);
        uint32_t *syms;
        size_t nsyms;
        parse_key_path(p, &syms, &nsyms);
        skip_ws(p);
        expect_char(p, '=');
        skip_ws(p);
        Value v = parse_value(p);
        tnode_insert_leaf_path(tbl, syms, nsyms, v);
        free(syms);
        skip_ws(p);
        if (p_cur(p) == ',') {
            p->i++;
            continue;
        }
        if (p_cur(p) == '}') {
            p->i++;
            break;
        }
        toml_fatal("expected ',' or '}' in inline table");
    }
    Value out = finalize_tnode(tbl);
    tnode_free(tbl);
    return out;
}

static Value parse_value(Parser *p) {
    skip_ws(p);
    if (p_at_end(p)) toml_fatal("expected a value");
    char c = p_cur(p);
    if (c == '"' || c == '\'') {
        size_t len;
        char *buf = parse_toml_string(p, &len);
        Value v = asnix_make_string_value(buf, len);
        free(buf);
        return v;
    }
    if (c == '[') return parse_array(p);
    if (c == '{') return parse_inline_table(p);

    size_t start = p->i;
    while (!p_at_end(p) && bare_value_char(p_cur(p))) p->i++;
    size_t len = p->i - start;
    if (len == 0) toml_fatal("expected a value");

    if (len == 10 && p_cur(p) == ' ' && isdigit((unsigned char)p_at(p, 1)) && isdigit((unsigned char)p_at(p, 2)) &&
        p_at(p, 3) == ':') {
        char combined[96];
        memcpy(combined, p->s + start, 10);
        combined[10] = 'T';
        p->i++;
        size_t time_start = p->i;
        while (!p_at_end(p) && bare_value_char(p_cur(p))) p->i++;
        size_t time_len = p->i - time_start;
        memcpy(combined + 11, p->s + time_start, time_len);
        return classify_bare_token(combined, 11 + time_len);
    }

    return classify_bare_token(p->s + start, len);
}

Value asnix_toml_parse(const char *text, size_t len) {
    Parser p = { text, len, 0 };
    TNode *root = tnode_new(TN_TABLE);
    TNode *cur_table = root;

    skip_ws_comments_newlines(&p);
    while (!p_at_end(&p)) {
        if (p_cur(&p) == '[') {
            int is_array = (p_at(&p, 1) == '[');
            p.i += is_array ? 2 : 1;
            skip_ws(&p);
            uint32_t *syms;
            size_t nsyms;
            parse_key_path(&p, &syms, &nsyms);
            skip_ws(&p);
            if (is_array) {
                expect_char(&p, ']');
                expect_char(&p, ']');
                cur_table = tnode_enter_array_table(root, syms, nsyms);
            } else {
                expect_char(&p, ']');
                cur_table = tnode_enter_table(root, syms, nsyms);
            }
            free(syms);
        } else {
            uint32_t *syms;
            size_t nsyms;
            parse_key_path(&p, &syms, &nsyms);
            skip_ws(&p);
            expect_char(&p, '=');
            Value v = parse_value(&p);
            tnode_insert_leaf_path(cur_table, syms, nsyms, v);
            free(syms);
        }

        skip_ws(&p);
        if (!p_at_end(&p) && p_cur(&p) == '#') skip_to_eol(&p);
        if (!p_at_end(&p)) {
            if (p_cur(&p) == '\n') {
                p.i++;
            } else if (p_cur(&p) == '\r' && p_at(&p, 1) == '\n') {
                p.i += 2;
            } else {
                toml_fatal("expected end of line");
            }
        }
        skip_ws_comments_newlines(&p);
    }

    Value out = finalize_tnode(root);
    tnode_free(root);
    return out;
}

