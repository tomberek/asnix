#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"
#include "eval_file.h"
#include "symtab.h"
#include "value_util.h"

/* Prints a string literal's bytes the way real Nix's own printer does
 * (confirmed against `nix-instantiate --eval` -- see docs/architecture.md's
 * "Known bugs" list, the entry about conformance-driven printer escaping):
 * `\n`/`\r`/`\t` become the two-character escape, `"` and `\` are
 * backslash-escaped, and a literal `${` run is escaped to `\${` (so the
 * printed form could be pasted back into Nix source without accidentally
 * starting an interpolation) -- but a bare `$` *not* followed by `{` is
 * left alone (confirmed: `"e$f"` prints as `"e$f"`, not `"e\$f"`). Before
 * this fix, `print_value_inner` just `printf`'d the raw bytes between
 * quotes verbatim, so a string containing an actual newline byte (e.g. from
 * `"a\<literal-newline>b"` or `''a''\<literal-newline>b''`, both of which
 * decode to the 2-byte sequence {'a','\n','b'} per the Strings milestone's
 * escape rules) printed as a raw embedded newline instead of the two
 * visible characters `\n` -- found re-screening numtide/nix-conformance's
 * eval-okay-backslash-newline-{1,2}. (A second agent working in parallel,
 * on the builtins-expansion/toJSON pass, independently found and fixed the
 * same underlying bug -- its narrower `print_escaped_string` handled
 * `" \ \n \r \t` but not the `${` case; this version was kept as the more
 * complete of the two.) */
static void print_string_contents(const char *bytes, int64_t len) {
    for (int64_t i = 0; i < len; i++) {
        char c = bytes[i];
        switch (c) {
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '$':
                if (i + 1 < len && bytes[i + 1] == '{') {
                    fputs("\\${", stdout);
                    i++; /* consume the '{' too */
                } else {
                    putchar('$');
                }
                break;
            default: putchar(c); break;
        }
    }
}

static void print_string_bytes(const char *bytes, int64_t len) {
    putchar('"');
    print_string_contents(bytes, len);
    putchar('"');
}

/* Real Nix's attrset printer only prints an attribute name bare
 * (unquoted) when it's a syntactically valid identifier (matches
 * `[A-Za-z_][A-Za-z0-9_'-]*`) AND isn't one of a small set of reserved
 * keywords that would otherwise parse as something else entirely if
 * pasted back in bare (`let`, `if`, ...) -- confirmed directly against
 * `nix-instantiate --eval` (`builtins.listToAttrs [{name="rec";...}]`
 * prints `"rec" = ...` quoted, but `{name="true";...}` prints `true =
 * ...` bare: `true`/`false`/`null` are literal tokens, not identifiers,
 * so they're not in this reserved set even though they're keywords
 * elsewhere in the grammar). Every *other* name -- empty, digit-leading
 * (`"1234"`), containing a space, non-ASCII (`"café"`), anything produced
 * by an arbitrary TOML/JSON quoted key -- must be printed quoted exactly
 * like a string literal (same escaping as print_string_bytes).
 *
 * Before this fix, `print_value_inner`'s ASNIX_TAG_ATTRS case printed
 * every attribute name bare, unconditionally (`printf("%.*s = ", ...)`)
 * -- found via `builtins.fromTOML`, whose corpus deliberately includes
 * quoted/literal-string TOML keys (`"quoted key"`, `""`, `"1234"`,
 * `"café"`) that must round-trip through Nix's own attrset printer, but
 * this is a general attrset-printing bug, not specific to TOML: any
 * builtin that can construct an attrset with an arbitrary-string key
 * (`listToAttrs`, `fromJSON` if it existed, `builtins.attrNames`-and-
 * reconstruct, ...) was already affected. See docs/architecture.md's
 * Known Bugs list. */
static int attr_name_needs_quoting(const char *name, size_t len) {
    static const struct { const char *kw; size_t len; } RESERVED[] = {
        { "let", 3 }, { "in", 2 }, { "if", 2 }, { "then", 4 }, { "else", 4 },
        { "rec", 3 }, { "inherit", 7 }, { "assert", 6 }, { "with", 4 }, { "or", 2 },
    };
    if (len == 0) return 1;
    char c0 = name[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return 1;
    for (size_t i = 1; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '\'' || c == '-';
        if (!ok) return 1;
    }
    for (size_t k = 0; k < sizeof(RESERVED) / sizeof(RESERVED[0]); k++) {
        if (len == RESERVED[k].len && memcmp(name, RESERVED[k].kw, len) == 0) return 1;
    }
    return 0;
}

static void print_attr_name(const char *name, size_t len) {
    if (attr_name_needs_quoting(name, (size_t)len)) {
        putchar('"');
        print_string_contents(name, (int64_t)len);
        putchar('"');
    } else {
        fwrite(name, 1, len, stdout);
    }
}

static void print_value_inner(Value v) {
    v = asnix_force_for_print(v); /* list elements and attrset values are lazy (M6) */
    switch (asnix_tag_of(v)) {
        case ASNIX_TAG_INT:
            printf("%lld", (long long)asnix_int_value(v));
            return;
        case ASNIX_TAG_IMM:
            switch (asnix_imm_subtag(v)) {
                case ASNIX_IMM_TRUE: printf("true"); return;
                case ASNIX_IMM_FALSE: printf("false"); return;
                case ASNIX_IMM_NULL: printf("null"); return;
                default: printf("<imm %d>", asnix_imm_subtag(v)); return;
            }
        case ASNIX_TAG_PTR: {
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            /* Paths print bare/unquoted (an absolute filesystem path, not
             * string syntax) -- matches `nix-instantiate --eval`. */
            if (asnix_value_is_path(v)) printf("%.*s", (int)len, bytes);
            else print_string_bytes(bytes, len);
            return;
        }
        case ASNIX_TAG_LIST: {
            const char *base = (const char *)(v.raw & ~0x7ULL);
            int64_t count = *(const int64_t *)(base + ASNIX_LIST_OFF_COUNT);
            const Value *elems = (const Value *)(base + ASNIX_LIST_OFF_ELEMS);
            printf("[ ");
            for (int64_t i = 0; i < count; i++) {
                print_value_inner(elems[i]);
                printf(" ");
            }
            printf("]");
            return;
        }
        case ASNIX_TAG_ATTRS: {
            /* Entries are stored sorted by symbol id (intern/first-seen
             * order, for OP_SELECT's binary search) -- real Nix prints
             * attrset keys sorted by *name*, which is a different order
             * in general (found by checking against
             * numtide/nix-conformance's eval-okay-attrs6: `{ b=1; a=2; }`
             * printed `{ b = 1; a = 2; }` here vs. Nix's `{ a = 2; b = 1;
             * }`). Re-sort by name for display only; the stored order is
             * untouched. */
            const char *base = (const char *)(v.raw & ~0x7ULL);
            int64_t count = *(const int64_t *)(base + ASNIX_ATTRS_OFF_COUNT);
            const char *names[64];
            size_t name_lens[64];
            Value vals[64];
            int64_t n = count > 64 ? 64 : count; /* defensive cap; no attrset in current fixtures is anywhere near this */
            for (int64_t i = 0; i < n; i++) {
                const char *entry = base + ASNIX_ATTRS_OFF_ENTRIES + i * ASNIX_ATTR_SIZE;
                uint32_t sym = *(const uint32_t *)(entry + ASNIX_ATTR_OFF_SYM);
                names[i] = asnix_sym_name(sym, &name_lens[i]);
                vals[i] = *(const Value *)(entry + ASNIX_ATTR_OFF_VALUE);
            }
            for (int64_t i = 1; i < n; i++) {
                int64_t j = i;
                while (j > 0) {
                    size_t minlen = name_lens[j - 1] < name_lens[j] ? name_lens[j - 1] : name_lens[j];
                    int cmp = memcmp(names[j - 1], names[j], minlen);
                    if (cmp == 0) cmp = (int)name_lens[j - 1] - (int)name_lens[j];
                    if (cmp <= 0) break;
                    const char *tn = names[j - 1];
                    names[j - 1] = names[j];
                    names[j] = tn;
                    size_t tl = name_lens[j - 1];
                    name_lens[j - 1] = name_lens[j];
                    name_lens[j] = tl;
                    Value tv = vals[j - 1];
                    vals[j - 1] = vals[j];
                    vals[j] = tv;
                    j--;
                }
            }
            printf("{ ");
            for (int64_t i = 0; i < n; i++) {
                print_attr_name(names[i], name_lens[i]);
                printf(" = ");
                print_value_inner(vals[i]);
                printf("; ");
            }
            printf("}");
            return;
        }
        default:
            printf("<unprintable value, tag=%d>", asnix_tag_of(v));
            return;
    }
}

static void print_value(Value v) {
    print_value_inner(v);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.nix>\n", argv[0]);
        return 1;
    }

    asnix_arena_init(0);

    Value result = asnix_eval_file(argv[1]);
    print_value(result);
    return 0;
}
