/*
 * Direct unit coverage for toml.c's asnix_toml_parse -- calls the parser
 * straight (not through OP_CALL/builtins_toml.c's PrimOp wrapper, which
 * has nothing of its own to test beyond arg unwrapping), then walks the
 * returned attrset/list Value tree with builtins_shared.c's accessor
 * helpers, the same way every other builtins test in this directory
 * inspects aggregate results. Every expected shape here was additionally
 * cross-checked end-to-end against numtide/nix-conformance's real
 * lang/eval-okay-fromTOML-NNN.exp files (see the shell-loop verification
 * described in docs/architecture.md's "M23 scope notes") -- this file is
 * a convenient, fast, no-external-repo-needed regression net for the
 * same parser, not a replacement for that fuller check.
 */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asnix/arena.h"
#include "builtins_shared.h"
#include "symtab.h"
#include "toml.h"
#include "value_util.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static Value toml(const char *s) { return asnix_toml_parse(s, strlen(s)); }

static Value get(Value attrs, const char *name) {
    uint32_t sym = asnix_intern(name, strlen(name));
    int64_t idx = asnix_bshared_attrs_find(attrs, sym);
    CHECK(idx >= 0, name);
    if (idx < 0) return ASNIX_NULL;
    return asnix_bshared_attrs_value(attrs, idx);
}

static void check_int(Value v, int64_t want, const char *msg) {
    int ok = asnix_tag_of(v) == ASNIX_TAG_INT && asnix_int_value(v) == want;
    CHECK(ok, msg);
}

static void check_float(Value v, double want, const char *msg) {
    int ok = asnix_value_is_float(v) && asnix_float_value(v) == want;
    CHECK(ok, msg);
}

static void check_string(Value v, const char *want, const char *msg) {
    int ok = asnix_tag_of(v) == ASNIX_TAG_PTR;
    if (ok) {
        int64_t len;
        const char *bytes = asnix_ptr_bytes(v, &len);
        ok = (size_t)len == strlen(want) && memcmp(bytes, want, (size_t)len) == 0;
        if (!ok) fprintf(stderr, "  got:  \"%.*s\"\n  want: \"%s\"\n", (int)len, bytes, want);
    }
    CHECK(ok, msg);
}

static void check_bool(Value v, int want, const char *msg) {
    int ok = asnix_tag_of(v) == ASNIX_TAG_IMM && asnix_imm_subtag(v) == (want ? ASNIX_IMM_TRUE : ASNIX_IMM_FALSE);
    CHECK(ok, msg);
}

/* M82 (see docs/architecture.md): forks a child that parses `doc` (a
 * single TOML date/time literal) and expects it to call `toml_fatal`
 * (an unconditional `exit(1)`, same fork()+exit-code pattern established
 * by tests/c/test_abort.c and tests/asm/test_stacklimits.c/test_apply.c
 * for every other fatal path in this codebase) -- see toml.c's
 * `classify_date_or_time`'s own comment for why dates/times are now
 * rejected instead of represented as a `{ _type = "timestamp"; ...}`
 * attrset (the OLD behavior this helper used to check, before M82
 * found the `nix-instantiate` actually installed in this environment
 * rejects every TOML date/time shape unconditionally). */
static void check_date_time_rejected(const char *doc, const char *msg) {
    pid_t pid = fork();
    if (pid == 0) {
        close(2); /* silence toml_fatal's own stderr line -- not under test here */
        asnix_arena_init(0);
        toml(doc);
        _exit(42); /* reaching here means it wrongly succeeded */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1, msg);
}

/* Ground truth: lang/eval-okay-fromTOML-001..005.exp. */
static void test_integers_and_bools(void) {
    Value v = toml("a = 1\nb = -1\nc = +1\nd = 1_000_000\ne = 0x1f\nf = 0o17\ng = 0b1010\nh = true\ni = false");
    check_int(get(v, "a"), 1, "a = 1");
    check_int(get(v, "b"), -1, "b = -1");
    check_int(get(v, "c"), 1, "c = +1");
    check_int(get(v, "d"), 1000000, "d = 1_000_000");
    check_int(get(v, "e"), 31, "e = 0x1f");
    check_int(get(v, "f"), 15, "f = 0o17");
    check_int(get(v, "g"), 10, "g = 0b1010");
    check_bool(get(v, "h"), 1, "h = true");
    check_bool(get(v, "i"), 0, "i = false");
}

/* Ground truth: lang/eval-okay-fromTOML-012..017.exp (basic string
 * escapes) and -016 (\u/\U -> UTF-8). */
static void test_basic_string_escapes(void) {
    Value v = toml("a = \"tab\\there\"\nb = \"nl\\nhere\"\nc = \"quote\\\"here\"\nd = \"back\\\\slash\"\n"
                    "e = \"\\u00e9\\U0001F600\"\nf = \"\\b\\f\\r\"");
    check_string(get(v, "a"), "tab\there", "\\t escape");
    check_string(get(v, "b"), "nl\nhere", "\\n escape");
    check_string(get(v, "c"), "quote\"here", "\\\" escape");
    check_string(get(v, "d"), "back\\slash", "\\\\ escape");
    check_string(get(v, "e"), "\xc3\xa9\xf0\x9f\x98\x80", "\\u/\\U -> UTF-8 (\xc3\xa9 = U+00E9, \xf0\x9f\x98\x80 = U+1F600)");
    check_string(get(v, "f"), "\b\f\r", "\\b \\f \\r escapes (raw control bytes)");
}

/* Ground truth: lang/eval-okay-fromTOML-018..024.exp. */
static void test_literal_and_multiline_strings(void) {
    Value v = toml("a = 'literal \\n not escaped'\n"
                    "b = \"\"\"\nline one\nline two\"\"\"\n"
                    "c = \"\"\"a\\\n   b\"\"\"\n"
                    "d = '''\nraw\nlines'''\n"
                    "e = \"\"\"has \"one\" quote\"\"\"");
    check_string(get(v, "a"), "literal \\n not escaped", "literal string: no escape processing");
    check_string(get(v, "b"), "line one\nline two", "multi-line basic: opening newline trimmed");
    check_string(get(v, "c"), "ab", "multi-line basic: line-ending backslash splices away the newline+indent");
    check_string(get(v, "d"), "raw\nlines", "multi-line literal: opening newline trimmed, no escapes");
    check_string(get(v, "e"), "has \"one\" quote", "multi-line basic: single/double embedded quotes don't end the string");
}

/* Ground truth: lang/eval-okay-fromTOML-041..047.exp. */
static void test_tables_and_dotted_keys(void) {
    Value v1 = toml("[t]\nb = 1\n[u]\nc = 2");
    check_int(get(get(v1, "t"), "b"), 1, "[t] b = 1");
    check_int(get(get(v1, "u"), "c"), 2, "[u] c = 2");

    Value v2 = toml("[a]\n[a.b]\nc = 1");
    check_int(get(get(get(v2, "a"), "b"), "c"), 1, "[a] / [a.b] nests under the already-open table");

    Value v3 = toml("a.b = 1\na.c = 2");
    Value a = get(v3, "a");
    check_int(get(a, "b"), 1, "dotted key a.b = 1, merged across two statements");
    check_int(get(a, "c"), 2, "dotted key a.c = 2, merged across two statements");
}

/* Ground truth: lang/eval-okay-fromTOML-045..046.exp -- the array-of-
 * tables "step into the last element" rule. */
static void test_array_of_tables(void) {
    Value v1 = toml("[[a]]\nb = 1\n[[a]]\nb = 2");
    Value a1 = get(v1, "a");
    CHECK(asnix_tag_of(a1) == ASNIX_TAG_LIST, "[[a]] produces a list");
    CHECK(asnix_bshared_list_count(a1) == 2, "[[a]] twice produces a 2-element list");
    check_int(get(asnix_bshared_list_elems(a1)[0], "b"), 1, "a[0].b == 1");
    check_int(get(asnix_bshared_list_elems(a1)[1], "b"), 2, "a[1].b == 2");

    Value v2 = toml("[[a]]\n[[a.b]]\nc = 1");
    Value a2 = get(v2, "a");
    Value a2_0 = asnix_bshared_list_elems(a2)[0];
    Value b = get(a2_0, "b");
    CHECK(asnix_tag_of(b) == ASNIX_TAG_LIST, "[[a.b]] nests a NEW array-of-tables under a's latest element");
    check_int(get(asnix_bshared_list_elems(b)[0], "c"), 1, "a[0].b[0].c == 1");
}

/* Ground truth: lang/eval-okay-fromTOML-025..040.exp. */
static void test_arrays_and_inline_tables(void) {
    Value v = toml("a = [1, 2, 3]\nb = [[1, 2], [3]]\nc = { d = 1, e = \"two\" }\nf = { g = { h = 1 } }");
    Value arr = get(v, "a");
    CHECK(asnix_bshared_list_count(arr) == 3, "a = [1,2,3] has 3 elements");
    check_int(asnix_bshared_list_elems(arr)[2], 3, "a[2] == 3");

    Value nested = get(v, "b");
    Value inner0 = asnix_bshared_list_elems(nested)[0];
    check_int(asnix_bshared_list_elems(inner0)[1], 2, "b[0][1] == 2 (nested array)");

    Value c = get(v, "c");
    check_int(get(c, "d"), 1, "inline table c.d == 1");
    check_string(get(c, "e"), "two", "inline table c.e == \"two\"");

    Value f = get(v, "f");
    check_int(get(get(f, "g"), "h"), 1, "nested inline table f.g.h == 1");
}

/* Ground truth: lang/eval-okay-fromTOML-048..052/058..059.exp -- quoted/
 * literal/numeric-looking/non-ASCII keys all intern correctly regardless
 * of what the printer later decides about quoting them (main.c's
 * concern, not this parser's). */
static void test_quoted_and_unusual_keys(void) {
    Value v = toml("\"quoted key\" = 1\n'literal key' = 2\n\"\" = 3\na-b_c = 4\n1234 = 5\na = \"café\"");
    check_int(get(v, "quoted key"), 1, "\"quoted key\" = 1");
    check_int(get(v, "literal key"), 2, "'literal key' = 2");
    check_int(get(v, ""), 3, "\"\" = 3");
    check_int(get(v, "a-b_c"), 4, "a-b_c = 4 (bare hyphenated key)");
    check_int(get(v, "1234"), 5, "1234 = 5 (bare numeric-looking key)");
    check_string(get(v, "a"), "caf\xc3\xa9", "non-ASCII string value");
}

/* Ground truth: lang/eval-fail-fromTOML-timestamps.err.exp (M82) -- see
 * check_date_time_rejected's own comment for why this no longer checks
 * a `{ _type = "timestamp"; ...}` shape. */
static void test_timestamps(void) {
    check_date_time_rejected("a = 1979-05-27", "local date rejected");
    check_date_time_rejected("a = 07:32:00.5", "local time rejected");
    check_date_time_rejected("a = 1979-05-27T07:32:00Z", "offset date-time rejected");
    check_date_time_rejected("a = 1979-05-27 07:32:00Z", "space-separated date-time rejected");
    check_date_time_rejected("a = 1979-05-27t07:32:00z", "lowercase t/z date-time rejected");
}

/* Comments/blank-lines/CRLF: lang/eval-okay-fromTOML-053..057.exp. */
static void test_comments_and_crlf(void) {
    Value v = toml("a = 1 # trailing comment\n# leading comment\nb = 2\r\nc = 3");
    check_int(get(v, "a"), 1, "trailing comment ignored");
    check_int(get(v, "b"), 2, "leading comment ignored, CRLF line ending");
    check_int(get(v, "c"), 3, "plain LF after CRLF line");
}

/* M51: asnix now has a real Float Value kind (include/asnix/value.h's
 * ASNIX_TAG_PTR + heap.h's ASNIX_FLOAT_MARKER), so a TOML float/inf/nan
 * parses to a genuine Nix float value instead of fatally rejecting --
 * see toml.c's parse_float_token. Ground truth: lang/eval-okay-fromTOML-
 * 006/-007/-008/-060.exp all require exactly this (a genuine Nix float
 * value for `1.5`, `inf`, `nan`, `3.0`). NaN can't be compared with `==`
 * (always false, by IEEE-754 definition, even against itself) -- checked
 * via self-inequality instead, the standard NaN test. */
static void test_float(void) {
    Value v = toml("a = 1.5\nb = 3.0\nc = inf\nd = -inf\ne = nan\nf = -nan");
    check_float(get(v, "a"), 1.5, "basic TOML float");
    check_float(get(v, "b"), 3.0, "whole-number TOML float stays a Float, not an Int");
    check_float(get(v, "c"), 1.0 / 0.0, "TOML inf");
    check_float(get(v, "d"), -1.0 / 0.0, "TOML -inf");
    Value nan_v = get(v, "e");
    CHECK(asnix_value_is_float(nan_v) && asnix_float_value(nan_v) != asnix_float_value(nan_v), "TOML nan");
    Value neg_nan_v = get(v, "f");
    CHECK(asnix_value_is_float(neg_nan_v) && asnix_float_value(neg_nan_v) != asnix_float_value(neg_nan_v),
          "TOML -nan");
}

int main(void) {
    asnix_arena_init(0);
    test_integers_and_bools();
    test_basic_string_escapes();
    test_literal_and_multiline_strings();
    test_tables_and_dotted_keys();
    test_array_of_tables();
    test_arrays_and_inline_tables();
    test_quoted_and_unusual_keys();
    test_timestamps();
    test_comments_and_crlf();
    test_float();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_toml: all checks passed\n");
    return 0;
}
