#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/c/regex_engine.h"

static int fails = 0;

static void check_anchored(const char *re, const char *s, const char *expect_csv /* "a,b,NULL,c" */) {
    int64_t relen = (int64_t)strlen(re), slen = (int64_t)strlen(s);
    int64_t nsub;
    AsnixCustomRegex *c = asnix_regex_compile(re, relen, &nsub);
    if (!c) { printf("COMPILE FAIL re=%s\n", re); fails++; return; }
    AsnixRegexGroup *groups = nsub > 0 ? malloc(sizeof(AsnixRegexGroup) * (size_t)nsub) : NULL;
    int ok = asnix_regex_match_anchored(c, s, slen, 0, slen, 0, groups);
    char got[1024]; got[0] = 0;
    if (!ok) {
        strcpy(got, "NOMATCH");
    } else {
        for (int64_t i = 0; i < nsub; i++) {
            char buf[256];
            if (groups[i].so < 0) snprintf(buf, sizeof buf, "%sNULL", i ? "," : "");
            else snprintf(buf, sizeof buf, "%s%.*s", i ? "," : "", (int)(groups[i].eo - groups[i].so), s + groups[i].so);
            strcat(got, buf);
        }
    }
    if (strcmp(got, expect_csv) != 0) {
        printf("MISMATCH re=%s s=%s expect=[%s] got=[%s]\n", re, s, expect_csv, got);
        fails++;
    }
    free(groups);
    asnix_regex_free(c);
}

int main(void) {
    /* Class A validated shapes (expected values taken from real nix-instantiate runs) */
    check_anchored("(a*)*", "aa", "");
    check_anchored("(a*)*", "", "");
    check_anchored("(a?)*", "aa", "");
    check_anchored("((a*)(b*))*", "aabb", ",,");
    check_anchored("((a)*)*", "aa", ",a");
    check_anchored("(a*)(a*)", "aa", "aa,");
    check_anchored("(a*)(a*)(a*)", "aa", "aa,,");
    check_anchored("(a+)*", "aaa", "aaa");
    check_anchored("(a*b*)*", "ab", "");
    check_anchored("(a*|b)*", "ab", "");
    check_anchored("(a|aa)(a|aa)", "aaa", "a,aa");
    check_anchored("(aa|a)(aa|a)", "aaa", "aa,a");
    check_anchored("(a|aa)*", "aaa", "a");
    check_anchored("(a{2})*", "aaaa", "aa");
    check_anchored("(a{0})*", "a", "NOMATCH");
    check_anchored("(a*){2}", "aa", "");
    check_anchored("(a*){1,2}", "aa", "");
    check_anchored("(a?){2}", "a", "");
    check_anchored("(x*)(xy)?", "xy", ",xy");
    check_anchored("(x+)(xy)?", "xxy", "x,xy");
    check_anchored("(x){0,1}(xy)?", "xy", "NULL,xy");
    check_anchored("(abc|a)(b*)(bcde)?", "abcde", "a,,bcde");
    check_anchored("(a)(b*)(bcde)?", "abcde", "a,,bcde");
    check_anchored("(a|ab)(c|bcd)", "abcd", "a,bcd");
    check_anchored("(ab|a)(c|bcd)", "abcd", "a,bcd");
    check_anchored("((a)|b)*", "ab", "b,a");
    check_anchored("(ab|a){1,2}", "aba", "a");
    check_anchored("(a|b){1,3}", "aba", "a");

    /* Class B shapes */
    check_anchored("[[:w:]]+", "a_1", "");
    check_anchored("[[.hyphen.][.space.][.tilde.][.period.]]+", "- ~.", "");
    check_anchored("[[.a.]-c]+", "abc", "");
    check_anchored("[[=a=]]+", "aA", "");
    check_anchored("[[=period=]x]+", ".x", "");
    check_anchored("[[:ALPHA:]]+", "aB", "");

    /* Basics / non-ambiguous, sanity */
    check_anchored("(f)(o+)", "foo", "f,oo");
    check_anchored("(a)?(b)", "b", "NULL,b");
    check_anchored("a|ab", "ab", "");
    check_anchored("^a$", "a", "");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("test_regex_engine_scratch: all checks passed\n");
    return 0;
}
