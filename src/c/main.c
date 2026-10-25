#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/arena.h"
#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/irpass.h"
#include "asnix/stats.h"
#include "asnix/value.h"
#include "builtins_shared.h" /* M57i: asnix_bshared_attrs_* / make_attrs, CLI --arg/-A support */
#include "compiler.h" /* lazy-thunk-compile experiment: asnix_lazy_thunk_enabled */
#include "eval_file.h"
#include "flake_eval.h" /* M126: --flake DIR[#attr.path] */
#include "irpass_cfold.h" /* M118: ASNIX_IR_OPT -- see this file's own hook below */
#include "irpass_cmpnot.h"
#include "irpass_constprop.h"
#include "irpass_dce.h"
#include "irpass_dce_if.h"
#include "irpass_jmpnop.h"
#include "irpass_deadframe.h"
#include "irpass_jmpthread.h"
#include "irpass_notfold.h"
#include "irpass_strconcat.h"
#include "irpass_streq.h"
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

/* M58-adjacent (no new milestone number -- a same-day follow-up, see
 * docs/architecture.md's M58 scope notes): real Nix's printer tracks
 * which LIST heap objects are currently *active* -- on the stack of
 * values being printed right now, i.e. an actual ancestor in the DFS
 * -- and prints `«repeated»` instead of recursing when a value already
 * on that stack comes back around. Before this fix, `print_value_inner`
 * had no cycle guard at all: M58 made `derivation`'s result genuinely
 * self-referential (`drv.out` is now the whole output object, not a
 * bare string), so printing one all the way (`d.all`, or bare `d`)
 * recursed forever and segfaulted -- not hit by anything in tests/e2e
 * or tests/conformance (which only ever print leaf fields like
 * `.outPath`), but a real, now-reachable crash for any future caller
 * that *does* print a whole derivation. A fixed-size stack of
 * currently-active LIST base pointers (already untagged) is checked on
 * entry and popped on exit -- same "small, bounded, defensive cap"
 * convention this file's attrset printer already uses for its 64-entry
 * cap.
 *
 * M80 (see docs/architecture.md): this ancestor-stack approach is
 * *correct* for LIST (real Nix's own `printList`, print.cc, keys its
 * `seen` set by `&v` -- the address of the Value slot itself, which can
 * only ever recur via a genuine cycle, not merely by two different
 * bindings sharing the same list payload) but was WRONG for ATTRS: real
 * Nix's `printAttrs` keys its `seen` set by `v.attrs()` -- the shared
 * `Bindings*` heap pointer -- and that set is never popped, persisting
 * for the entire top-level print call, not just while an ancestor is
 * active (confirmed directly against real Nix's print.cc source AND
 * against `nix-instantiate --eval`: `let a = { x = 1; }; in { p = a; q
 * = a; }` prints `{ p = { x = 1; }; q = «repeated»; }` even though `q`
 * is a SIBLING of `p`, not a cyclic ancestor). asnix's old comment here
 * claimed this was "confirmed directly" to NOT happen for ATTRS (citing
 * M58's own derivation `drvAttrs` case) -- that claim stands for
 * `drvAttrs` specifically only because real Nix's `derivation` builtin
 * happens to construct a genuinely distinct `Bindings*` at each site
 * (not because non-ancestor ATTRS sharing is never collapsed in
 * general, which the `a`/`p`/`q` example above disproves). See
 * `print_attrs_seen_mark` below for the ATTRS-specific whole-print-call
 * tracking this split off into. */
#define ASNIX_PRINT_MAX_DEPTH 4096
static const void *g_print_active[ASNIX_PRINT_MAX_DEPTH];
static int g_print_active_count = 0;

static int print_active_push(const void *base) {
    for (int i = 0; i < g_print_active_count; i++) {
        if (g_print_active[i] == base) return 0; /* already an ancestor -- caller must not recurse */
    }
    if (g_print_active_count < ASNIX_PRINT_MAX_DEPTH) g_print_active[g_print_active_count++] = base;
    return 1;
}

static void print_active_pop(void) {
    if (g_print_active_count > 0) g_print_active_count--;
}

/* M80: ATTRS dedup, scoped to the ENTIRE top-level print call (never
 * popped -- reset once, in `print_value`, which main.c's `main` calls
 * exactly once per process) -- see the comment above for why this must
 * differ from LIST's ancestor-only stack. Same "small, bounded,
 * defensive cap" convention as `g_print_active` above; a real Nix
 * process printing a pathologically large number of distinct attrsets
 * would use a real std::set with no such cap, but nothing in this
 * project's fixtures/corpora comes remotely close to 4096 distinct
 * ATTRS values in one print call. */
static const void *g_attrs_seen[ASNIX_PRINT_MAX_DEPTH];
static int g_attrs_seen_count = 0;

static int print_attrs_seen_mark(const void *base) {
    for (int i = 0; i < g_attrs_seen_count; i++) {
        if (g_attrs_seen[i] == base) return 0; /* already printed earlier in this print call */
    }
    if (g_attrs_seen_count < ASNIX_PRINT_MAX_DEPTH) g_attrs_seen[g_attrs_seen_count++] = base;
    return 1;
}

static void print_value_concrete(Value v); /* forward decl -- see print_peel_wrappers/print_value_inner below */

/* M92: peels off Cell/Thunk wrappers one at a time -- rather than a
 * single opaque `asnix_force_for_print` call -- so each wrapper's OWN
 * identity (a Cell's `target` address, or a Thunk's own heap address)
 * can be pushed onto the ancestor stack individually, same as real Nix's
 * printer keys its `seen` set by `&v` (the Value SLOT's own address,
 * print.cc ~line 412) rather than by whatever heap payload that slot
 * forces to. This distinction only matters for a genuinely self-
 * referential structure built through a Cell (e.g. `let x = [x]; in x`)
 * -- see this function's own long comment below for why collapsing
 * Cell/Thunk layers through one opaque force call would print one fewer
 * level of `[`/`]` nesting than real Nix does (confirmed via
 * numtide/nix-conformance's eval-okay-print.nix). Returns the fully-
 * peeled (never Thunk, never Cell) value and writes `*pushed_out` with
 * how many ancestor-stack entries it pushed -- the caller must pop
 * exactly that many, AFTER it's done printing the peeled value, in
 * exact-reverse order doesn't matter (print_active_pop is a plain LIFO
 * counter, not keyed by WHICH entry). If a repeat is found mid-peel,
 * this already prints "«repeated»" itself and returns with
 * `*pushed_out` left at the count the caller must still pop (everything
 * pushed so far, by this same call, before the repeat was found) -- the
 * caller must NOT print anything more for this value in that case. */
static int print_peel_wrappers(Value *v_out, int *pushed_out) {
    Value v = *v_out;
    int pushed = 0;
    for (;;) {
        if (asnix_value_is_cell(v)) {
            Value *target = asnix_cell_target(v);
            if (!print_active_push((const void *)target)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = *target;
            continue;
        }
        if (asnix_tag_of(v) == ASNIX_TAG_THUNK) {
            const void *thunk_addr = (const void *)(v.raw & ~0x7ULL);
            if (!print_active_push(thunk_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_force_for_print(v); /* M6: forces exactly this one Thunk -- Cell-
                                            * transparent (force.S) so it can never itself
                                            * return a raw Cell, only another Thunk (the M23
                                            * lazy-passthrough case, looped by the `for`
                                            * here) or a genuinely concrete value. */
            continue;
        }
        if (asnix_value_is_lazyapp(v)) {
            /* M113: same ancestor-tracking shape as the Thunk case just
             * above -- push THIS LazyApp object's own heap address (not
             * whatever it resolves to) before resolving it, so a
             * genuinely self-referential structure built through one
             * (constructible the same way a Cell/Thunk cycle is) still
             * prints "«repeated»" instead of looping forever. */
            const void *lazyapp_addr = (const void *)(v.raw & ~0x7ULL);
            if (!print_active_push(lazyapp_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_resolve_lazyapp(v); /* one step -- may itself still be an unforced
                                            * Thunk/Cell/LazyApp, looped by the `for` here. */
            continue;
        }
        if (asnix_value_is_thunkmap(v)) {
            /* M118: same ancestor-tracking shape as Cell/LazyApp above,
             * but keyed by asnix_thunkmap_slot(v) -- the SPECIFIC output
             * attrset entry this element's result will be memoized into
             * -- rather than by v's own masked pointer. That masked
             * pointer names the shared ThunkMapDesc, identical across
             * EVERY element one mapAttrs/zipAttrsWith call produced; using
             * it here would make unrelated sibling elements (e.g.
             * printing `mapAttrs (n: v: v) { a = 1; b = 2; }`) falsely
             * detect each other as "«repeated»" the first time a second
             * element of the same call gets peeled. See heap.h's
             * ASNIX_THUNKMAP_MARKER comment. */
            const void *slot_addr = (const void *)asnix_thunkmap_slot(v);
            if (!print_active_push(slot_addr)) {
                for (int i = 0; i < pushed; i++) print_active_pop();
                printf("«repeated»");
                *pushed_out = 0;
                return 0;
            }
            pushed++;
            v = asnix_resolve_thunkmap(v); /* one step -- may itself still be an unforced
                                             * Thunk/Cell/LazyApp/ThunkMap, looped by the
                                             * `for` here. */
            continue;
        }
        break;
    }
    *v_out = v;
    *pushed_out = pushed;
    return 1;
}

static void print_value_inner(Value v) {
    int pushed;
    if (!print_peel_wrappers(&v, &pushed)) return; /* already printed "«repeated»" */
    print_value_concrete(v);
    for (int i = 0; i < pushed; i++) print_active_pop();
}

/* M92: the actual tag-dispatch switch, unchanged from before this
 * milestone EXCEPT that ASNIX_TAG_LIST no longer pushes/pops its own
 * forced-heap-object address onto the ancestor stack (see
 * print_peel_wrappers above, which now owns ALL of the ancestor-stack
 * bookkeeping, keyed by Cell-target/Thunk addresses one layer at a time
 * -- not by "whatever concrete object we eventually forced to", which
 * would be the SAME heap List object reached through two DIFFERENT
 * wrapper chains in a self-referential structure like `let x = [x]; in
 * x`, collapsing the cycle one bracket-level too early. ASNIX_TAG_ATTRS
 * keeps its own SEPARATE, unchanged `print_attrs_seen_mark` -- see that
 * function's own comment for why ATTRS genuinely needs whole-print-call,
 * object-address-keyed dedup (collapsing non-ancestor SIBLING reuse too,
 * e.g. `{p=a;q=a;}`), unlike LIST's ancestor-only semantics. `v` arriving
 * here is always already fully peeled (never Thunk, never Cell). */
static void print_value_concrete(Value v) {
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
            /* M51: a Float prints via the SAME "general"/"%.6g"-shaped
             * rule real Nix's own top-level value printer uses (verified
             * directly against `nix-instantiate --eval`: `1.5` -> "1.5",
             * `2.2250738585072014e-308` -> "2.22507e-308" (6 significant
             * digits), `0.0` -> "0", `builtins.add 2.0 2` -> "4" (no
             * forced ".0" on an integral float) -- real Nix's printer
             * (print.cc's printFloat) just does `ostream << double`,
             * i.e. plain C++ default float formatting, which is
             * bit-for-bit glibc's `%.6g`, including nan/inf spelling for
             * non-finite values ("nan"/"-nan"/"inf"/"-inf", checked
             * directly against glibc's printf -- no special-casing
             * needed). NOT the same formatting `toString`/coerceToString
             * uses (value_util.c's asnix_coerce_to_string, "%f"-style) --
             * two genuinely different real-Nix formatting rules for two
             * different call sites, confirmed by directly comparing
             * `nix-instantiate --eval '1.5'` ("1.5") against
             * `nix-instantiate --eval 'toString 1.5'` ("1.500000"). */
            if (asnix_value_is_float(v)) {
                printf("%.6g", asnix_float_value(v));
                return;
            }
            int64_t len;
            const char *bytes = asnix_ptr_bytes(v, &len);
            /* Paths print bare/unquoted (an absolute filesystem path, not
             * string syntax) -- matches `nix-instantiate --eval`. */
            if (asnix_value_is_path(v)) printf("%.*s", (int)len, bytes);
            else print_string_bytes(bytes, len);
            return;
        }
        case ASNIX_TAG_LIST: {
            /* M92: no print_active_push/pop here anymore -- see this
             * function's own header comment. Cycle protection against a
             * genuine self-referential list (only reachable through a
             * Cell -- see heap.h's ASNIX_CELL_MARKER comment, a plain
             * OP_MK_LIST can never embed a not-yet-built list inside
             * itself) is now entirely print_peel_wrappers's job, keyed
             * by each Cell/Thunk wrapper's own address on the way here,
             * not by this List's own (possibly-shared-with-a-different-
             * wrapper-chain) heap address. */
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
            /* M88: `v` may be a lazy union header (heap.h) -- resolve to
             * the flat form before marking it "seen" (dedup is keyed on
             * the heap pointer actually walked below) or reading any raw
             * layout. */
            v = asnix_bshared_attrs_resolve(v);
            const char *base = (const char *)(v.raw & ~0x7ULL);
            if (!print_attrs_seen_mark(base)) { printf("«repeated»"); return; }
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
            /* no print_active_pop() here -- ATTRS dedup is whole-print-
             * call scoped (print_attrs_seen_mark), never popped, see
             * that function's own comment above. */
            return;
        }
        /* M67/M80: real Nix's printer (print.cc) shows `<PRIMOP>` for an
         * unapplied builtin, `<LAMBDA>` for a bytecode closure, and
         * `<PRIMOP-APP>` for a *partially*-applied multi-arg builtin --
         * found via eval-okay-print.nix (`builtins.trace [...] [ null
         * toString (deepSeq "x") (a: a) ... ]`), verified against real
         * `nix-instantiate --eval` (also directly: `[ builtins.sub
         * (builtins.sub 1) ]` prints `[ <PRIMOP> <PRIMOP-APP> ]`
         * regardless of the builtin's real arity -- real Nix has exactly
         * one "PrimOpApp" state for "not fully applied yet", it doesn't
         * matter how many of N args still remain).
         *
         * ASNIX_TAG_PRIMOP is always a *fully unapplied* native builtin:
         * every multi-arg builtin visible from Nix source is pre-curried
         * into a chain of ordinary closures (asnix_bshared_make_curried_
         * native, builtins_shared.c) precisely because Nix's call syntax
         * is always unary (`f a b` parses as `(f a) b`) -- a bare
         * arity-N ASNIX_TAG_PRIMOP value only ever exists for the N==1
         * case (e.g. `toString`), where "0 args applied" is the only
         * state reachable before the single call fully invokes it. So
         * this is unconditionally correct, not an approximation. */
        case ASNIX_TAG_PRIMOP:
            printf("<PRIMOP>");
            return;
        /* ASNIX_TAG_CLOSURE is NOT always a genuine user-written lambda
         * -- a *partially*-applied multi-arg native builtin (e.g.
         * `deepSeq "x"`, one of two args bound; also a multi-arg
         * builtin's "zero args applied" top-of-curry-chain state, e.g.
         * bare `deepSeq`, which the M67 comment this replaces missed
         * entirely -- see builtins_m23.c's build_deepSeq, ALWAYS a
         * Closure, never an ASNIX_TAG_PRIMOP, for any arity>=2 builtin)
         * is, per the above, ALSO represented as one of
         * make_curried_native's (or make_curried2's/
         * make_curried_native_noforce's) synthetic wrapper closures.
         * M80: every one of those now stamps its OFF_FORMALS field with
         * `asnix_bshared_native_formals_marker()`'s ONE canonical
         * address (instead of each call site's own fresh, unshared
         * empty-attrset allocation) specifically so this case can tell
         * the two apart reliably -- compiler.c's own AST_LAMBDA case
         * always calls the plain, un-marked `asnix_bshared_make_attrs`
         * for every real compiled lambda, so a genuine user closure's
         * formals pointer can never coincide with the marker. Once
         * recognized as a curry stage, `UP == NULL` means "zero args
         * applied yet" (`asnix_bshared_make_curried_native`'s own
         * top-level closure, returned with UP explicitly NULL since
         * nothing has been bound yet) -- real Nix's `<PRIMOP>` state --
         * and any other UP (a real, >=1-slot Env, captured by actually
         * calling into the chain) is `<PRIMOP-APP>`, matching real Nix's
         * single undifferentiated "some but not all args bound" state
         * regardless of how many of the curry chain's remaining stages
         * are still pending. Every other ASNIX_TAG_CLOSURE (FORMALS not
         * the marker) is a genuine user lambda, `<LAMBDA>`. */
        case ASNIX_TAG_CLOSURE: {
            const char *base = (const char *)(v.raw & ~0x7ULL);
            Value formals = *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
            if (formals.raw == asnix_bshared_native_formals_marker().raw) {
                void *up = *(void **)(base + ASNIX_CLOSURE_OFF_UP);
                printf(up == NULL ? "<PRIMOP>" : "<PRIMOP-APP>");
            } else {
                printf("<LAMBDA>");
            }
            return;
        }
        default:
            printf("<unprintable value, tag=%d>", asnix_tag_of(v));
            return;
    }
}

static void print_value(Value v) {
    g_attrs_seen_count = 0; /* fresh whole-print-call scope, see print_attrs_seen_mark */
    print_value_inner(v);
    printf("\n");
}

/* M57i: `--arg name EXPR` / `--argstr name STR` / `-A attrpath` CLI flag
 * support (real Nix's `nix-instantiate --eval` has all three). Scoped to
 * exactly these three -- no `-I` (search-path entries feeding `<name>`
 * resolution: would need a first-class `builtins.findFile`/`nixPath`,
 * a separate, much larger feature this pass doesn't build; see
 * docs/architecture.md's M57 scope notes) and no generic flag-skipping
 * for anything else real `nix-instantiate` accepts -- an unrecognized
 * flag is a hard usage error here, not silently ignored (the external
 * nix-conformance adapter script is the one responsible for filtering
 * a `.flags` sidecar's other, unrelated flags -- e.g.
 * `--extra-experimental-features`, `--lint-*` -- down to just these
 * three before ever invoking this binary; see that script's own
 * comment). */
typedef struct {
    const char *name;
    Value value;
} CliArg;

/* `-A a.b.c`: force-and-select one dotted attrpath segment at a time
 * into the final result, same semantics as a chain of plain `.`
 * selections in the source itself (fatal on a non-attrset or a missing
 * key partway through, naming the specific segment that failed). */
static Value select_attrpath(Value v, const char *attrpath) {
    const char *p = attrpath;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t seglen = dot ? (size_t)(dot - p) : strlen(p);
        v = asnix_force_for_print(v);
        if (asnix_tag_of(v) != ASNIX_TAG_ATTRS) {
            fprintf(stderr, "asnix: -A: '%.*s' is not a set\n", (int)seglen, p);
            exit(1);
        }
        uint32_t sym = asnix_intern(p, seglen);
        int64_t idx = asnix_bshared_attrs_find(v, sym);
        if (idx < 0) {
            fprintf(stderr, "asnix: -A: attribute '%.*s' missing\n", (int)seglen, p);
            exit(1);
        }
        v = asnix_bshared_attrs_value(v, idx);
        p = dot ? dot + 1 : p + seglen;
    }
    return asnix_force_for_print(v);
}

/* `--arg`/`--argstr`: real Nix's `autoCallFunction` (eval.cc) -- if the
 * loaded expression is a lambda-PATTERN function (`{a, b ? d, ...}:
 * ...`; a plain single-identifier lambda has no named formals to match
 * against, so it's left alone, same as real Nix), build an argument
 * attrset containing only the formals a CLI arg actually matched by
 * name; a formal left unmatched is fine if it has a default (the
 * lambda's own OP_CHECK_FORMALS/default machinery supplies it when
 * called, exactly as if that formal were simply omitted from the call
 * site in ordinary source) but fatal if it's required. Extra CLI args
 * that don't match any formal are simply unused, matching real Nix. */
static Value auto_call(Value fn, const CliArg *args, size_t nargs) {
    if (asnix_tag_of(fn) != ASNIX_TAG_CLOSURE) return fn;
    const char *base = (const char *)(uintptr_t)(fn.raw & ~(uint64_t)0x7);
    Value formals = *(const Value *)(base + ASNIX_CLOSURE_OFF_FORMALS);
    int64_t nformals = asnix_bshared_attrs_count(formals);
    if (nformals == 0) return fn; /* not a formals-pattern function */

    uint32_t *syms = malloc((size_t)nformals * sizeof(uint32_t));
    Value *vals = malloc((size_t)nformals * sizeof(Value));
    int64_t total = 0;
    for (int64_t i = 0; i < nformals; i++) {
        uint32_t fsym = asnix_bshared_attrs_sym(formals, i);
        size_t flen;
        const char *fname = asnix_sym_name(fsym, &flen);
        int64_t matched = -1;
        for (size_t j = 0; j < nargs; j++) {
            if (strlen(args[j].name) == flen && memcmp(args[j].name, fname, flen) == 0) {
                matched = (int64_t)j;
                break;
            }
        }
        if (matched >= 0) {
            syms[total] = fsym;
            vals[total] = args[matched].value;
            total++;
        } else if (asnix_imm_subtag(asnix_bshared_attrs_value(formals, i)) != ASNIX_IMM_TRUE) {
            fprintf(stderr, "asnix: you must specify a value for the argument '%.*s'\n", (int)flen, fname);
            exit(1);
        }
        /* else: has a default -- leave it out, same as an ordinary
         * call site that omits an optional formal. */
    }
    Value args_attrs = asnix_bshared_make_attrs(syms, vals, total);
    free(syms);
    free(vals);
    return asnix_apply(fn, &args_attrs, 1);
}

int main(int argc, char **argv) {
    asnix_arena_init(0);
    /* M118: opt-in bytecode-to-bytecode pass pipeline, same
     * env-var-flag convention as ASNIX_SHOW_STATS below -- off by
     * default, so this changes nothing about a normal run. See
     * asnix/irpass.h. */
    asnix_lazy_thunk_enabled = getenv("ASNIX_LAZY_THUNK") != NULL; /* lazy-thunk-compile experiment */
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_cfold();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_cmpnot();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_dce();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_dce_if();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_jmpnop();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_jmpthread();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_strconcat();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_streq();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_notfold();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_deadframe();
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_register_constprop();

    const char *file_path = NULL;
    const char *flake_ref = NULL; /* M126: --flake DIR[#attr.path], see flake_eval.h */
    const char *attrpath = NULL;
    CliArg *cli_args = NULL;
    size_t n_cli_args = 0, cap_cli_args = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--flake") == 0 && i + 1 < argc) {
            flake_ref = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--arg") == 0 && i + 2 < argc) {
            if (n_cli_args == cap_cli_args) {
                cap_cli_args = cap_cli_args ? cap_cli_args * 2 : 4;
                cli_args = realloc(cli_args, cap_cli_args * sizeof(CliArg));
            }
            cli_args[n_cli_args].name = argv[i + 1];
            cli_args[n_cli_args].value = asnix_eval_expr_source(argv[i + 2]);
            n_cli_args++;
            i += 2;
        } else if (strcmp(argv[i], "--argstr") == 0 && i + 2 < argc) {
            if (n_cli_args == cap_cli_args) {
                cap_cli_args = cap_cli_args ? cap_cli_args * 2 : 4;
                cli_args = realloc(cli_args, cap_cli_args * sizeof(CliArg));
            }
            cli_args[n_cli_args].name = argv[i + 1];
            cli_args[n_cli_args].value = asnix_make_string_value(argv[i + 2], strlen(argv[i + 2]));
            n_cli_args++;
            i += 2;
        } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            attrpath = argv[i + 1];
            i += 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "asnix: unrecognized flag '%s' (only --arg/--argstr/-A/--flake are supported)\n", argv[i]);
            return 1;
        } else if (file_path) {
            fprintf(stderr, "usage: %s [--arg name expr] [--argstr name str] [-A attrpath] <file.nix>\n", argv[0]);
            return 1;
        } else {
            file_path = argv[i];
        }
    }
    if (flake_ref && file_path) {
        fprintf(stderr, "asnix: --flake cannot be combined with a plain <file.nix> argument\n");
        return 1;
    }
    if (!file_path && !flake_ref) {
        fprintf(stderr,
                "usage: %s [--arg name expr] [--argstr name str] [-A attrpath] <file.nix>\n"
                "       %s --flake <dir>[#attr.path] [-A attrpath]\n",
                argv[0], argv[0]);
        return 1;
    }

    Value result;
    if (flake_ref) {
        /* M126: `DIR#attr.path` -- split at the first '#' the same way
         * iets's own `parse_flake_ref` does (flake_eval.h/.c has no
         * notion of a fragment at all; selecting the attrpath is exactly
         * `select_attrpath` below, the same function `-A` already uses
         * on a plain file). An empty fragment (`DIR#`) means "no
         * selection", same as a bare `DIR`. */
        const char *hash = strchr(flake_ref, '#');
        char dir_buf[1024];
        const char *fragment = NULL;
        if (hash) {
            size_t dirlen = (size_t)(hash - flake_ref);
            if (dirlen >= sizeof(dir_buf)) {
                fprintf(stderr, "asnix: --flake: path '%s' is too long\n", flake_ref);
                return 1;
            }
            memcpy(dir_buf, flake_ref, dirlen);
            dir_buf[dirlen] = '\0';
            if (hash[1]) fragment = hash + 1;
        } else {
            snprintf(dir_buf, sizeof(dir_buf), "%s", flake_ref);
        }
        result = asnix_eval_flake_outputs(dir_buf);
        if (fragment) result = select_attrpath(result, fragment);
    } else {
        result = asnix_eval_file(file_path);
    }
    if (n_cli_args > 0) result = auto_call(result, cli_args, n_cli_args);
    if (attrpath) result = select_attrpath(result, attrpath);
    print_value(result);
    if (getenv("ASNIX_SHOW_STATS")) asnix_stats_print();
    /* M118: report which bytecode passes actually fired, and how many
     * times, across the whole evaluation (every nested lambda/thunk
     * body's own ib_finish() call included) -- only when the pipeline
     * was actually turned on; a no-op print otherwise (0 passes
     * registered -> asnix_irpass_print_stats prints nothing at all). */
    if (getenv("ASNIX_IR_OPT")) asnix_irpass_print_stats(stderr);
    return 0;
}
