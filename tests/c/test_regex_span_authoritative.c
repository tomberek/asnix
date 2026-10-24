/* M184: asnix_prim_match2_impl (builtins_regex.c) makes regex_engine.c's
 * own asnix_regex_match_anchored AUTHORITATIVE for the whole-string
 * match/no-match decision itself, not just capture recompute, once a
 * pattern's custom-engine verdict has been empirically proven (over
 * several real calls) to agree with glibc's regexec() -- see that
 * file's top comment and docs/architecture.md's M184 section for the
 * full design.
 *
 * The correctness argument M184 relies on: `match`'s own question --
 * "does the custom engine's pattern match subject[0,slen) EXACTLY,
 * consuming the whole string" -- is a plain EXISTENCE question over a
 * FIXED end position. M68's own quirks (ordered-alternation-wins,
 * greedy-repetition's-trailing-empty-iteration) are specifically about
 * WHICH decomposition a backtracking engine prefers when more than one
 * reaches that exact fixed end position -- never about WHETHER one
 * does. A complete backtracking search (full DFS over every
 * alternative/repetition-count choice, which is what regex_engine.c's
 * `m`/`run_cont`/`match_rep` already is in anchored mode -- no search
 * budget there at all, see asnix_regex_match_anchored's own comment)
 * must therefore agree with ANY other correct engine (including
 * glibc's genuinely-POSIX-leftmost-longest regexec) on pure existence,
 * independent of tie-break convention.
 *
 * This test exercises that claim directly and mechanically: for each
 * (pattern, subject) pair below, compiles the pattern BOTH ways
 * (glibc's regcomp/regexec and regex_engine.c's own
 * asnix_regex_compile/asnix_regex_match_anchored) and asserts the two
 * engines' existence verdicts agree -- deliberately including patterns
 * with ZERO capture groups (nsub == 0), the one class that, before
 * M184, NEVER touched regex_engine.c in production at all (not even
 * for capture recompute, since there's nothing to recompute), so this
 * is the specific class that had the least pre-existing real-world
 * exercise of the custom engine's parser/matcher before this
 * milestone. */
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/c/regex_engine.h"

static int fails = 0;

/* 1 if glibc's regexec (anchored to the whole string via the same
 * "does the leftmost-longest match starting at 0 reach the end"
 * trick asnix_prim_match2_impl itself uses) matches `s` against `re`
 * end-to-end, 0 otherwise. Fatal on a regcomp failure -- every pattern
 * in this file's battery is expected to be valid POSIX ERE. */
static int glibc_whole_match(const char *re, const char *s) {
    regex_t preg;
    int rc = regcomp(&preg, re, REG_EXTENDED);
    if (rc != 0) { printf("regcomp FAILED for %s\n", re); exit(1); }
    regmatch_t m0;
    int mrc = regexec(&preg, s, 1, &m0, 0);
    int64_t slen = (int64_t)strlen(s);
    int ok = (mrc == 0 && m0.rm_so == 0 && m0.rm_eo == (regoff_t)slen);
    regfree(&preg);
    return ok;
}

static void check_agree(const char *re, const char *s) {
    int64_t relen = (int64_t)strlen(re), slen = (int64_t)strlen(s);
    int glibc_ok = glibc_whole_match(re, s);

    int64_t nsub;
    AsnixCustomRegex *c = asnix_regex_compile(re, relen, &nsub);
    if (!c) { printf("CUSTOM COMPILE FAIL re=%s (not itself a failure of this test's claim -- callers fall back to glibc when this happens -- but flagging since every pattern in this battery is expected to parse)\n", re); fails++; return; }

    AsnixRegexGroup *groups = nsub > 0 ? malloc(sizeof(AsnixRegexGroup) * (size_t)nsub) : NULL;
    int custom_ok = asnix_regex_match_anchored(c, s, slen, 0, slen, 0, groups);

    if (!!glibc_ok != !!custom_ok) {
        printf("DISAGREE re=%s s=%s glibc=%d custom=%d\n", re, s, glibc_ok, custom_ok);
        fails++;
    }
    free(groups);
    asnix_regex_free(c);
}

int main(void) {
    /* Zero-capture-group patterns -- the class that never touched
     * regex_engine.c in production before M184. Deliberately includes
     * top-level (unparenthesized) alternation and repetition-adjacent-
     * to-overlapping-content shapes, the structural ingredients of
     * M68's own quirks, just without the capture groups that quirk's
     * own examples happened to use. */
    check_agree("a|ab", "a");
    check_agree("a|ab", "ab");
    check_agree("a|ab", "b");
    check_agree("aa|a", "aa");
    check_agree("aa|a", "a");
    check_agree("x*xy", "xxy");
    check_agree("x*xy", "xy");
    check_agree("x*y?", "xxx");
    check_agree("a*a*", "aaaa");
    check_agree("a*b*a*", "aaabbbaaa");
    check_agree(".*a.*", "bbbabbb");
    check_agree("[0-9]+\\.[0-9]+\\.[0-9]+", "1.2.3");
    check_agree("[0-9]+\\.[0-9]+\\.[0-9]+", "1.2");
    check_agree("^[[:space:]]*[0-9]+[[:space:]]*$", "  42  ");
    check_agree("", "");
    check_agree("", "a");
    check_agree("a{2,4}a*", "aaaaaa");
    check_agree("a{2,4}a*", "a");
    check_agree("(ab)*", "ababab");          /* one group, but trivial/unambiguous -- sanity */

    /* The actual M68 quirk examples, now probed for EXISTENCE agreement
     * specifically (not capture content, which is a separate, already-
     * covered concern in test_regex_engine_scratch.c) -- across several
     * subjects each, including ones that don't reach a full match at
     * all. */
    check_agree("(aa|a)(aa|a)", "aaa");
    check_agree("(aa|a)(aa|a)", "aaaa");
    check_agree("(aa|a)(aa|a)", "aa");
    check_agree("(aa|a)(aa|a)", "a");
    check_agree("(aa|a)(aa|a)", "aaaaa");
    check_agree("(aa|a)(aa|a)", "b");
    check_agree("(a|aa)(a|aa)", "aaa");
    check_agree("(a|aa)(a|aa)", "aaaa");
    check_agree("(a*)*", "aa");
    check_agree("(a*)*", "");
    check_agree("(a*)*", "aaaaaa");
    check_agree("((a)*)*", "aa");
    check_agree("(x*)(xy)?", "xy");
    check_agree("(x*)(xy)?", "xxy");
    check_agree("(x*)(xy)?", "x");
    check_agree("(x*)(xy)?", "y");
    check_agree("(x+)(xy)?", "xxy");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("test_regex_span_authoritative: all checks passed\n");
    return 0;
}
