#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/c/regex_engine.h"

static int fails = 0;

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
    check_agree("(ab)*", "ababab");

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

