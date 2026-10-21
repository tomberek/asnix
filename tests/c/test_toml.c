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

/* Timestamps (TOML date/time literals) are real Nix's `{ _type =
 * "timestamp"; value = "<text>"; }` shape, see toml.c's
 * make_timestamp_value -- confirmed against lang/eval-okay-fromTOML-062
 * through -076.exp. */
static void check_timestamp(Value v, const char *want_value, const char *msg) {
    CHECK(asnix_tag_of(v) == ASNIX_TAG_ATTRS, msg);
    if (asnix_tag_of(v) != ASNIX_TAG_ATTRS) return;
    check_string(get(v, "_type"), "timestamp", msg);
    check_string(get(v, "value"), want_value, msg);
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

/* Ground truth: lang/eval-okay-fromTOML-062..076.exp. */
static void test_timestamps(void) {
    Value v = toml("a = 1979-05-27\nb = 07:32:00.5\nc = 1979-05-27T07:32:00Z\nd = 1979-05-27 07:32:00Z\n"
                    "e = 1979-05-27t07:32:00z");
    check_timestamp(get(v, "a"), "1979-05-27", "local date");
    check_timestamp(get(v, "b"), "07:32:00.500", "local time: fractional seconds padded to 3 digits");
    check_timestamp(get(v, "c"), "1979-05-27T07:32:00Z", "offset date-time");
    check_timestamp(get(v, "d"), "1979-05-27T07:32:00Z", "space-separated date-time normalized to 'T'");
    check_timestamp(get(v, "e"), "1979-05-27T07:32:00Z", "lowercase t/z normalized to uppercase T/Z");
}

/* Comments/blank-lines/CRLF: lang/eval-okay-fromTOML-053..057.exp. */
static void test_comments_and_crlf(void) {
    Value v = toml("a = 1 # trailing comment\n# leading comment\nb = 2\r\nc = 3");
    check_int(get(v, "a"), 1, "trailing comment ignored");
    check_int(get(v, "b"), 2, "leading comment ignored, CRLF line ending");
    check_int(get(v, "c"), 3, "plain LF after CRLF line");
}

/* asnix has no float/double Value representation at all (see
 * include/asnix/value.h) -- a TOML float must fail loudly, not silently
 * truncate to an int or fake a string, so this follows the same
 * fork()+exit-code pattern test_abort.c established for other
 * unconditionally-fatal paths. Ground truth for *why* this is a scope
 * cut, not an oversight: lang/eval-okay-fromTOML-006/-007/-008/-060.exp
 * all require a genuine Nix float value (`1.5`, `inf`, `nan`, `3.0`). */
static void test_float_is_fatal(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { CHECK(0, "pipe() failed"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        asnix_arena_init(0);
        toml("a = 1.5");
        _exit(42); /* reaching here means the float wasn't rejected */
    }
    close(pipefd[1]);
    char buf[256] = { 0 };
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1, "a TOML float exits with code 1, not a crash or a silent int");
    CHECK(n > 0 && strstr(buf, "not supported") != NULL, "the error names the unsupported-float reason");
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
    test_float_is_fatal();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_toml: all checks passed\n");
    return 0;
}
