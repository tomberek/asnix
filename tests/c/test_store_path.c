#include <stdio.h>
#include <string.h>

#include "base32.h"
#include "store_path.h"

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static const uint8_t digest_foo_txt[32] = { 0x7e, 0x83, 0x02, 0x0a, 0x3f, 0xd4, 0x60, 0x5a, 0xfc, 0x92, 0x02,
                                             0xc0, 0xc3, 0xfb, 0x63, 0x3f, 0x87, 0xef, 0x23, 0xa8, 0xf2, 0xdc,
                                             0x67, 0xcb, 0x3a, 0x9b, 0xc7, 0xdf, 0x1a, 0x46, 0x04, 0xe2 };
static const uint8_t digest_bar_txt[32] = { 0x1f, 0x77, 0x38, 0x54, 0xe6, 0xea, 0xca, 0xd1, 0x44, 0x50, 0x18,
                                             0x25, 0x66, 0xc8, 0x4b, 0x23, 0xd8, 0x10, 0x4c, 0x7c, 0xe6, 0xfb,
                                             0xea, 0xe3, 0x64, 0xf0, 0x7f, 0x4c, 0xf2, 0xd6, 0x18, 0xd0 };
static const uint8_t digest_empty_txt[32] = { 0x6b, 0x73, 0x1c, 0xcb, 0x12, 0x6f, 0xfb, 0x7d, 0x4f, 0xe1, 0xec,
                                               0x84, 0x8b, 0x00, 0x42, 0x84, 0x84, 0xb1, 0x75, 0x21, 0x7c, 0x1a,
                                               0x70, 0xdc, 0x17, 0xaf, 0x09, 0x0b, 0xfb, 0x2d, 0xac, 0xd9 };
static const uint8_t digest_x[32] = { 0x99, 0x66, 0xc9, 0x23, 0x14, 0xd4, 0x54, 0x26, 0x0f, 0x17, 0x19,
                                       0xe4, 0x60, 0x47, 0x5e, 0x07, 0x82, 0x7a, 0x6b, 0x5d, 0x27, 0xbb,
                                       0xc5, 0x57, 0x34, 0xe9, 0x90, 0xc4, 0xb7, 0x4e, 0x43, 0xd4 };

static void check_path(const uint8_t digest[32], const char *name, const char *expected) {
    char out[256];
    size_t n = asnix_make_store_path_from_digest(digest, name, out, sizeof(out));
    char msg[320];
    snprintf(msg, sizeof(msg), "store path for '%s' matches real nix-instantiate output", name);
    CHECK(n > 0 && strcmp(out, expected) == 0, msg);
    if (strcmp(out, expected) != 0) fprintf(stderr, "  got='%s' want='%s'\n", out, expected);
}

static void test_base32_basic(void) {
    uint8_t zeros[20] = { 0 };
    char out[33] = { 0 };
    CHECK(asnix_base32_encoded_length(20) == 32, "base32 encoded length of 20 bytes is 32 chars");
    asnix_base32_encode(zeros, 20, out);
    out[32] = '\0';
    int all_zero_char = 1;
    for (int i = 0; i < 32; i++)
        if (out[i] != '0') all_zero_char = 0;
    CHECK(all_zero_char, "base32 of all-zero bytes is all '0' characters");
}

static void test_store_path_preimage(void) {
    char out[256];
    size_t n = asnix_store_path_preimage("text", "sha256:deadbeef", "foo.txt", out, sizeof(out));
    CHECK(n > 0 && strcmp(out, "text:sha256:deadbeef:/nix/store:foo.txt") == 0,
          "preimage assembly matches the real makeStorePath format");
}

int main(void) {
    test_base32_basic();
    test_store_path_preimage();
    check_path(digest_foo_txt, "foo.txt", "/nix/store/m0iyz1rzcgxw68h6skk8b9sg0p0napwc-foo.txt");
    check_path(digest_bar_txt, "bar.txt", "/nix/store/gi611n139g46dx80hsv9vd8shavx537r-bar.txt");
    check_path(digest_empty_txt, "empty.txt", "/nix/store/45sv31448808npa0rjs7dwn00lbnqs8p-empty.txt");
    check_path(digest_x, "x", "/nix/store/bmmpm0h7br3n0c2sb6wf5i1x41s0rpdy-x");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_store_path: all checks passed\n");
    return 0;
}

