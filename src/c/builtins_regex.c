/*
 * builtins.match / builtins.split -- see builtins_regex.h. Both verified
 * against real `nix-instantiate --eval --strict` (2.35.2) before being
 * trusted; see each function's own comment for the specific expressions
 * checked, and /tmp/nix-conformance/lang/corpora/regex.nix for the full
 * breadth of POSIX ERE shapes this is meant to cover.
 */
#include "builtins_regex.h"

#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asnix/value.h"
#include "builtins_shared.h"
#include "value_util.h"

/* Nix strings aren't NUL-terminated (heap.h/value_util.h's
 * asnix_ptr_bytes -- explicit length), but regcomp/regexec need C
 * strings. Plain malloc/free scratch copy (freed by every caller before
 * returning) -- not arena-allocated, since nothing here needs to outlive
 * this one builtin call, same convention builtins_compare.c's
 * replaceStrings/toJSON already use for their own scratch buffers. */
static char *nul_copy(const char *bytes, int64_t len) {
    char *buf = malloc((size_t)len + 1);
    if (len > 0) memcpy(buf, bytes, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* Real Nix's regex engine is libstdc++'s `std::regex` (built with
 * `std::regex::extended`), not glibc's `<regex.h>` -- the two disagree on
 * the validity of a specific, small family of patterns that are outside
 * strict POSIX ERE but that glibc's `regcomp` happens to accept anyway as
 * a GNU extension/leniency. Found via nix-conformance's
 * `lang/corpora/regex-invalid.nix` (EREs "which libstdc++ rejects" --
 * every entry there is cross-checked against real `nix-instantiate`,
 * which does reject all of them): without this pre-check, `regcomp`
 * alone would accept several of them, so `builtins.match "\\n" "x"`
 * would wrongly succeed/return `null` here instead of being the fatal
 * error real Nix gives. Checked directly against real nix-instantiate
 * (see /tmp/check_escapes.sh-style probing) for the exact boundary of
 * each rule below before trusting it:
 *
 * - Escaping anything other than the 12 real POSIX ERE metacharacters
 *   (`. ( ) * + ? [ { ^ $ | \`) is rejected -- this is the single
 *   biggest category: `\n` `\w` `\d` `\b` `\/` (GNU-extension-only
 *   shorthand escapes glibc accepts, verified real Nix rejects all of
 *   them) and, perhaps surprisingly, `\]` and `\}` too (confirmed
 *   directly: `\(` `\)` `\[` `\{` are all valid escapes but `\]` and `\}`
 *   are NOT -- asymmetric, but that's genuinely what real Nix does,
 *   since `]` and `}` aren't "special" on their own in POSIX ERE grammar,
 *   only in context after a matching `[`/`{`). A backreference like
 *   `\1` falls under this same rule (a digit isn't in the 12-char list)
 *   -- POSIX ERE has no backreference construct at all (unlike BRE's
 *   `\1`..`\9`), so this isn't a separate "backreferences unsupported"
 *   scope cut, just the same escape-whitelist rule catching it for free.
 * - `{` immediately followed by `,` (e.g. `a{,2}`, missing the required
 *   lower bound) -- a GNU-extension-lenient interval form glibc accepts
 *   (treating the missing bound as 0) that real Nix rejects; POSIX ERE
 *   intervals are `{n}`/`{n,}`/`{n,m}`, `n` is not optional.
 * - An unmatched `)` with no corresponding `(` (e.g. `a)`) -- glibc
 *   treats a lone `)` as an ordinary/literal character (same leniency it
 *   gives unmatched `]`/`}`); real Nix rejects it.
 *
 * Bracket-expression-aware (POSIX `[...]`/`[^...]`, including the
 * leading-`]`-is-literal rule and `[. .]`/`[: :]`/`[= =]` sub-expressions)
 * so none of the above rules misfire on a `(`/`)`/`{`/`\` that's actually
 * just an ordinary bracket member (e.g. `[\n]` -- a literal backslash and
 * `n`, two bracket members, not an escape sequence at all, since
 * backslash has no special meaning inside `[...]` in POSIX ERE -- stays
 * valid, matching real Nix).
 *
 * Deliberately NOT chased (see docs/architecture.md's scope notes):
 * `(a){24998}` (real Nix rejects this specific group-repetition count as
 * "too big" at some internal complexity threshold well below glibc's own
 * much higher limit -- an inherent engine-complexity-limit mismatch, not
 * a parseable syntax rule) and `[[.-.]]`/`[a-[.c.]]` (obscure POSIX
 * collating-symbol-as-range-endpoint forms libstdc++'s collating-symbol
 * support rejects but glibc's accepts -- replicating that exactly would
 * mean reverse-engineering libstdc++'s own undocumented collating-symbol
 * grammar boundary, not deriving a rule from the POSIX spec). */
static int pattern_rejected_by_real_nix(const char *p, int64_t len) {
    static const char *valid_escapes = ".()*+?[{^$|\\";
    int in_bracket = 0, bracket_first = 0, paren_depth = 0;
    int64_t i = 0;
    while (i < len) {
        char c = p[i];
        if (!in_bracket) {
            if (c == '\\') {
                if (i + 1 >= len) return 0; /* trailing '\' -- glibc itself already rejects this (rc=5) */
                if (!strchr(valid_escapes, p[i + 1])) return 1;
                i += 2;
                continue;
            }
            if (c == '(') { paren_depth++; i++; continue; }
            if (c == ')') {
                if (paren_depth == 0) return 1;
                paren_depth--;
                i++;
                continue;
            }
            if (c == '{' && i + 1 < len && p[i + 1] == ',') return 1;
            if (c == '[') {
                in_bracket = 1;
                bracket_first = 1;
                i++;
                if (i < len && p[i] == '^') i++;
                continue;
            }
            i++;
            continue;
        }
        /* inside a bracket expression -- '\', '(', ')', '{' are all
         * ordinary members here, not special (POSIX ERE rule). */
        if (bracket_first && c == ']') {
            bracket_first = 0;
            i++;
            continue;
        }
        bracket_first = 0;
        if (c == '[' && i + 1 < len && (p[i + 1] == '.' || p[i + 1] == ':' || p[i + 1] == '=')) {
            char kind = p[i + 1];
            int64_t j = i + 2;
            while (j + 1 < len && !(p[j] == kind && p[j + 1] == ']')) j++;
            if (j + 1 >= len) return 0; /* unterminated -- glibc itself already rejects this */
            i = j + 2;
            continue;
        }
        if (c == ']') {
            in_bracket = 0;
            i++;
            continue;
        }
        i++;
    }
    return 0; /* unterminated '[' / unmatched '(' are already glibc's own rejections */
}

/* Fatal on a bad pattern -- matches real Nix's own fatal-on-bad-regcomp
 * behavior (verified: `builtins.match "(" "a"` -> "error: invalid
 * regular expression '('" on real nix-instantiate; exact wording isn't
 * reproduced, just the fatal-not-null outcome, same class of
 * documented-not-exact-wording scope cut every other fatal error in this
 * project already makes). */
static void compile_or_fatal(regex_t *preg, const char *pattern, int64_t pattern_len, const char *who) {
    if (pattern_rejected_by_real_nix(pattern, pattern_len)) {
        asnix_bshared_fatal(who, "invalid regular expression (rejected by real Nix's stricter ERE grammar)");
    }
    int rc = regcomp(preg, pattern, REG_EXTENDED);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, preg, errbuf, sizeof errbuf);
        asnix_bshared_fatal(who, errbuf);
    }
}

/* Builds the flat list of `nsub` capture-group values from a completed
 * regexec()'s pmatch[] array (pmatch[0] is the whole match, pmatch[1..nsub]
 * are the groups -- same indexing regexec itself uses). `base` is the
 * pointer regexec was actually called against (subject, or subject+pos
 * for split's windowed search below) -- pmatch offsets are relative to
 * whatever pointer regexec saw, not necessarily the start of the whole
 * subject. A group that didn't participate in the match (rm_so == -1 --
 * e.g. an alternation branch not taken) becomes `null` in the list, not
 * an empty string: verified directly, `builtins.match "(a)?(b)" "b"` ==
 * `[ null "b" ]`. */
static Value groups_to_list(const regmatch_t *pmatch, size_t nsub, const char *base) {
    Value *vals = nsub > 0 ? malloc(nsub * sizeof(Value)) : NULL;
    for (size_t i = 0; i < nsub; i++) {
        regmatch_t m = pmatch[i + 1];
        if (m.rm_so < 0) {
            vals[i] = ASNIX_NULL;
        } else {
            vals[i] = asnix_make_string_value(base + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
        }
    }
    Value result = asnix_bshared_make_list(vals, (int64_t)nsub);
    free(vals);
    return result;
}

/* ---------------------------------------------------------------------
 * builtins.match regex str -- real Nix implicitly anchors `regex` to the
 * ENTIRE string (as if wrapped in `^...$`), returning `null` if it
 * doesn't match end-to-end, or the flat list of the regex's own capture
 * groups (NOT including the whole match) if it does.
 *
 * Rather than literally wrapping the pattern text in `^(?:...)$` (POSIX
 * ERE has no non-capturing group -- `\(...\)` IS a capture group, which
 * would shift every real group's index by one), this relies on a
 * property of POSIX regexec() itself: it always returns the match that
 * starts at the *earliest* possible position in the string, and -- for
 * any fixed start position -- the *longest* match starting there
 * (leftmost-longest, REG_EXTENDED's whole point). So if a match exists
 * starting at offset 0, regexec() is guaranteed to find one starting at
 * offset 0 (nothing can be "earlier"), and that match is guaranteed to
 * be the LONGEST one starting at offset 0. Therefore: call regexec()
 * plain (no pattern rewriting, no flags), and check whether the single
 * match it reports happens to start at 0 and end at the full length --
 * if the longest match starting at 0 doesn't reach the end, no
 * whole-string match is possible at all, so `null` is correct either
 * way. This is exactly `std::regex_match`'s own semantics (which is what
 * real Nix's primops.cc actually calls), just reimplemented against
 * POSIX regexec() instead of libstdc++'s regex engine.
 *
 * Verified against real nix-instantiate: `match "foo" "foo"` == `[]`
 * (no groups); `match "(f)(o+)" "foo"` == `[ "f" "oo" ]`; `match "(a)?(b)"
 * "b"` == `[ null "b" ]` (unparticipated group is null, not ""); `match
 * "a|ab" "ab"` == `[]` (leftmost-longest picks the full "ab" branch, a
 * whole-string match -- Perl-style leftmost-first-branch would have tried
 * "a" first, matched only a prefix, and returned null instead); `match "a"
 * ""` == null; `match "" ""` == `[]`; `match "." "a\nc"`-shaped: `.`
 * matches a literal newline too (no REG_NEWLINE passed, matching real
 * Nix's own regex construction, which doesn't set multiline/dotall-excl
 * flags either -- verified `match "a.c" "a\nc"` == `[]`, i.e. matches).
 * --------------------------------------------------------------------- */
static Value match2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    asnix_bshared_string_bytes(args[0], &pbytes, &plen, "match");
    asnix_bshared_string_bytes(args[1], &sbytes, &slen, "match");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    regex_t preg;
    compile_or_fatal(&preg, pattern, plen, "match");
    free(pattern);

    size_t nsub = preg.re_nsub;
    regmatch_t *pmatch = malloc((nsub + 1) * sizeof(regmatch_t));
    int mrc = regexec(&preg, subject, nsub + 1, pmatch, 0);

    Value result;
    if (mrc != 0 || pmatch[0].rm_so != 0 || pmatch[0].rm_eo != (regoff_t)slen) {
        result = ASNIX_NULL;
    } else {
        result = groups_to_list(pmatch, nsub, subject);
    }

    free(pmatch);
    regfree(&preg);
    free(subject);
    return result;
}
static Value build_match(void) { return asnix_bshared_make_curried_native((void *)match2_impl, 2); }

/* ---------------------------------------------------------------------
 * builtins.split regex str -- finds every non-overlapping match of
 * `regex` ANYWHERE in `str` (not anchored), returning a flat list
 * alternating [literalTextBeforeMatch, listOfCaptureGroupsForThatMatch,
 * ..., finalTrailingLiteralText]. A pattern with no capture groups still
 * produces an empty list `[]` at each match position (not omitted).
 *
 * `pos` tracks where the next regexec() search window starts;
 * `last_end` tracks the end of the last match actually recorded (the
 * two diverge exactly when a zero-width match was just processed).
 * REG_NOTBOL is passed whenever pos > 0 so a literal `^` in the pattern
 * only ever matches the true start of the whole subject, not wherever
 * the search window happens to start (standard "search from offset N"
 * convention, same as e.g. GNU grep's own multi-match scanning) --
 * REG_NOTEOL is never needed since the search window's end always
 * coincides with the subject's own true end (the window is a suffix,
 * never truncated on the right), so `$` already means the real end.
 *
 * Zero-width matches (e.g. "x*" can always match an empty string) are
 * real and must not infinite-loop: after recording one, `pos` advances
 * to `rm_eo + 1` (one past the empty match) rather than `rm_eo` (which
 * would just find the exact same empty match again at the same spot
 * forever) -- the skipped byte still ends up correctly included in the
 * next iteration's literal-text span, since that span is always
 * computed from `last_end` (which only moves to an actual match
 * boundary), not from `pos` itself.
 *
 * Verified against real nix-instantiate for the trickiest cases before
 * trusting this algorithm (see this function's own trace-through in the
 * commit/PR description for the full by-hand simulation): `split "x*"
 * "ab"` == `[ "" [] "a" [] "b" [] "" ]`; `split "-" "a-b-c"` == `[ "a" []
 * "b" [] "c" ]`; `split "(-)" "a-b-c"` == `[ "a" ["-"] "b" ["-"] "c" ]`;
 * `split "" ""` == `[ "" [] "" ]`; `split "a" ""` == `[ "" ]` (regexec
 * finds no match at all in an empty subject against a pattern that
 * requires a literal char, so the whole loop never fires and the single
 * trailing-text element is just the (empty) subject); `split ",*"
 * "a,,b"` == `[ "" [] "a" [] "" [] "b" [] "" ]`; `split "(a)(b)?" "aab"`
 * == `[ "" ["a" null] "" ["a" "b"] "" ]`.
 * --------------------------------------------------------------------- */
static Value split2_impl(const Value *args, int64_t argc) {
    (void)argc;
    int64_t plen, slen;
    const char *pbytes, *sbytes;
    asnix_bshared_string_bytes(args[0], &pbytes, &plen, "split");
    asnix_bshared_string_bytes(args[1], &sbytes, &slen, "split");

    char *pattern = nul_copy(pbytes, plen);
    char *subject = nul_copy(sbytes, slen);

    regex_t preg;
    compile_or_fatal(&preg, pattern, plen, "split");
    free(pattern);

    size_t nsub = preg.re_nsub;
    regmatch_t *pmatch = malloc((nsub + 1) * sizeof(regmatch_t));

    Value *out = NULL;
    int64_t cap = 0, n = 0;
    int64_t pos = 0, last_end = 0;

    while (pos <= slen) {
        int eflags = pos > 0 ? REG_NOTBOL : 0;
        int mrc = regexec(&preg, subject + pos, nsub + 1, pmatch, eflags);
        if (mrc != 0) break;

        int64_t abs_so = pos + pmatch[0].rm_so;
        int64_t abs_eo = pos + pmatch[0].rm_eo;

        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, (size_t)cap * sizeof(Value));
        }
        out[n++] = asnix_make_string_value(subject + last_end, (size_t)(abs_so - last_end));
        out[n++] = groups_to_list(pmatch, nsub, subject + pos);

        last_end = abs_eo;
        pos = (abs_eo == abs_so) ? abs_eo + 1 : abs_eo;
    }

    if (n + 1 > cap) {
        cap = cap ? cap + 1 : 1;
        out = realloc(out, (size_t)cap * sizeof(Value));
    }
    out[n++] = asnix_make_string_value(subject + last_end, (size_t)(slen - last_end));

    Value result = asnix_bshared_make_list(out, n);
    free(out);
    free(pmatch);
    regfree(&preg);
    free(subject);
    return result;
}
static Value build_split(void) { return asnix_bshared_make_curried_native((void *)split2_impl, 2); }

const AsnixBuiltinMiscDef ASNIX_REGEX_BUILTINS[] = {
    { "match", build_match },
    { "split", build_split },
};

const size_t ASNIX_REGEX_BUILTINS_COUNT = sizeof(ASNIX_REGEX_BUILTINS) / sizeof(ASNIX_REGEX_BUILTINS[0]);
