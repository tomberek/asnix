/* Validates asnix_build_derivation_aterm against two real references,
 * reproduced byte-for-byte (see docs/architecture.md's M8 notes):
 * numtide/nix-conformance's ia-advanced-attributes-defaults.drv, and a
 * real local `nix-instantiate` run for a second, independent case. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aterm.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void check_eq(const char *got, const char *want, const char *msg) {
    CHECK(strcmp(got, want) == 0, msg);
    if (strcmp(got, want) != 0) fprintf(stderr, "  got:  %s\n  want: %s\n", got, want);
}

static void test_advanced_attributes_defaults(void) {
    const char *args[] = { "-c", "echo hello > $out" };
    AtermEnvVar env_masked[] = {
        { "builder", "/bin/bash" },
        { "name", "advanced-attributes-defaults" },
        { "out", "" },
        { "system", "my-system" },
    };

    AtermOutput outs_masked[] = { { "out", "" } };
    char *masked = asnix_build_derivation_aterm(outs_masked, 1, "my-system", "/bin/bash", args, 2, env_masked, 4);
    check_eq(masked,
             "Derive([(\"out\",\"\",\"\",\"\")],[],[],\"my-system\",\"/bin/bash\",[\"-c\",\"echo hello > "
             "$out\"],[(\"builder\",\"/bin/bash\"),(\"name\",\"advanced-attributes-defaults\"),(\"out\",\"\"),("
             "\"system\",\"my-system\")])",
             "masked ATerm matches nix-conformance's real .drv (with out blanked)");
    free(masked);

    const char *real_out_path = "/nix/store/1qsc7svv43m4dw2prh6mvyf7cai5czji-advanced-attributes-defaults";
    AtermEnvVar env_real[] = {
        { "builder", "/bin/bash" },
        { "name", "advanced-attributes-defaults" },
        { "out", real_out_path },
        { "system", "my-system" },
    };
    AtermOutput outs_real[] = { { "out", real_out_path } };
    char *real = asnix_build_derivation_aterm(outs_real, 1, "my-system", "/bin/bash", args, 2, env_real, 4);
    char expected[512];
    snprintf(expected, sizeof(expected),
             "Derive([(\"out\",\"%s\",\"\",\"\")],[],[],\"my-system\",\"/bin/bash\",[\"-c\",\"echo hello > "
             "$out\"],[(\"builder\",\"/bin/bash\"),(\"name\",\"advanced-attributes-defaults\"),(\"out\",\"%s\"),("
             "\"system\",\"my-system\")])",
             real_out_path, real_out_path);
    check_eq(real, expected, "unmasked ATerm matches numtide/nix-conformance's real .drv file exactly");
    free(real);
}

static void test_asnix_test_derivation(void) {
    const char *args[] = { "-c", "echo hi" };
    AtermEnvVar env[] = {
        { "builder", "/bin/sh" },
        { "name", "asnix-test" },
        { "out", "/nix/store/jahl3hfswiax761wm6y2dxbhjgpjdnl1-asnix-test" },
        { "system", "x86_64-linux" },
    };
    AtermOutput outs[] = { { "out", "/nix/store/jahl3hfswiax761wm6y2dxbhjgpjdnl1-asnix-test" } };
    char *got = asnix_build_derivation_aterm(outs, 1, "x86_64-linux", "/bin/sh", args, 2, env, 4);
    check_eq(got,
             "Derive([(\"out\",\"/nix/store/jahl3hfswiax761wm6y2dxbhjgpjdnl1-asnix-test\",\"\",\"\")],[],[],"
             "\"x86_64-linux\",\"/bin/sh\",[\"-c\",\"echo hi\"],[(\"builder\",\"/bin/sh\"),(\"name\",\"asnix-test\"),"
             "(\"out\",\"/nix/store/jahl3hfswiax761wm6y2dxbhjgpjdnl1-asnix-test\"),(\"system\",\"x86_64-linux\")])",
             "ATerm for the asnix-test case matches a real local nix-instantiate run");
    free(got);
}

static void test_escaping(void) {
    const char *args[] = { "line1\nline2", "a\"quote", "back\\slash", "tab\there" };
    AtermEnvVar env[] = { { "k", "v" } };
    AtermOutput outs[] = { { "out", "" } };
    char *got = asnix_build_derivation_aterm(outs, 1, "s", "b", args, 4, env, 1);
    check_eq(got, "Derive([(\"out\",\"\",\"\",\"\")],[],[],\"s\",\"b\",[\"line1\\nline2\",\"a\\\"quote\",\"back\\"
                  "\\slash\",\"tab\\there\"],[(\"k\",\"v\")])",
             "only \\n \\r \\t \\\" \\\\ are escaped, matching Nix's exact rule");
    free(got);
}

/* M6x: multi-output `Outputs` section -- the map must list every
 * declared output, sorted ascending by name, each still with the
 * trailing empty hashAlgo/hash fields (this project never produces
 * fixed-output/CA outputs here). Shape verified against real Nix's
 * `~/nix/src/libstore/derivation/aterm.cc` unparse logic directly (see
 * docs/architecture.md's scope notes for this milestone). */
static void test_multi_output(void) {
    const char *args[] = { "-c", "echo hi" };
    AtermEnvVar env[] = {
        { "bin", "/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-x-bin" },
        { "builder", "/bin/sh" },
        { "name", "x" },
        { "out", "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-x" },
        { "outputs", "out bin" },
        { "system", "x86_64-linux" },
    };
    AtermOutput outs[] = {
        { "bin", "/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-x-bin" },
        { "out", "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-x" },
    };
    char *got = asnix_build_derivation_aterm(outs, 2, "x86_64-linux", "/bin/sh", args, 2, env, 6);
    check_eq(got,
             "Derive([(\"bin\",\"/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-x-bin\",\"\",\"\"),(\"out\","
             "\"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-x\",\"\",\"\")],[],[],\"x86_64-linux\",\"/bin/sh\","
             "[\"-c\",\"echo hi\"],[(\"bin\",\"/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-x-bin\"),(\"builder\","
             "\"/bin/sh\"),(\"name\",\"x\"),(\"out\",\"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-x\"),(\"outputs\","
             "\"out bin\"),(\"system\",\"x86_64-linux\")])",
             "multi-output Outputs section lists every output, sorted ascending by name");
    free(got);
}

int main(void) {
    test_advanced_attributes_defaults();
    test_asnix_test_derivation();
    test_escaping();
    test_multi_output();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_aterm: all checks passed\n");
    return 0;
}
