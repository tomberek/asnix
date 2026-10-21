#include <ctype.h>
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
static void print_string_bytes(const char *bytes, int64_t len) {
    putchar('"');
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
    putchar('"');
}

/* Real Nix's printer quotes an attrset key that isn't a valid bare
 * identifier (`[a-zA-Z_][a-zA-Z0-9_'-]*`, matching lexer.c's own
 * ident_start/ident_cont rules post-M23's hyphen fix) -- e.g. a purely
 * numeric key like "1" prints as `"1" = ...` not `1 = ...` (confirmed:
 * `nix-instantiate --eval -E 'builtins.groupBy (x: toString x) [1]'`
 * => `{ "1" = [ 1 ]; }`). Previously unconditionally bare (M9's original
 * printer never had a reason to hit a non-bare key until M23's groupBy
 * started producing numeric-string keys) -- found via
 * numtide/nix-conformance's eval-okay-groupBy.nix. */
static int is_bare_ident(const char *s, int64_t len) {
    if (len == 0) return 0;
    char c0 = s[0];
    if (!(isalpha((unsigned char)c0) || c0 == '_')) return 0;
    for (int64_t i = 1; i < len; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '\'' || c == '-')) return 0;
    }
    return 1;
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
                if (is_bare_ident(names[i], (int64_t)name_lens[i])) {
                    printf("%.*s = ", (int)name_lens[i], names[i]);
                } else {
                    print_string_bytes(names[i], (int64_t)name_lens[i]);
                    printf(" = ");
                }
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
