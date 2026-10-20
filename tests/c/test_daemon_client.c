/* Tests the daemon protocol client against a REAL, live nix-daemon --
 * this is environment-dependent (needs a running daemon reachable via
 * NIX_REMOTE / the default socket), so it skips gracefully (exit 0, not
 * a failure) rather than breaking `make test` on a machine/CI without
 * one. Where it *can* run, it checks more than just "one call works":
 * repeated isValidPath calls on the same connection must stay consistent
 * and correctly resynchronize the stream between calls -- the part of
 * the protocol most likely to silently break if the stderr-sideband
 * draining (process_stderr in daemon_client.c) ever mis-parses a field. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "daemon_client.h"

/* Finds a real, definitely-valid store path to test the *positive* case
 * against, without hardcoding anything machine-specific: resolves
 * `nix-instantiate`'s real location (which must be under /nix/store on
 * any machine that can run it at all) and truncates to the store-path
 * component. Returns 0 and leaves `out` untouched if this doesn't work
 * out for any reason -- the caller treats that as "skip this one check",
 * not a failure, since it's a best-effort positive-case addition on top
 * of the already-meaningful negative-case checks above. */
static int find_a_real_store_path(char *out, size_t out_cap) {
    FILE *p = popen("readlink -f \"$(command -v nix-instantiate)\" 2>/dev/null", "r");
    if (!p) return 0;
    char buf[512];
    int ok = fgets(buf, sizeof(buf), p) != NULL;
    pclose(p);
    if (!ok) return 0;

    const char *prefix = "/nix/store/";
    if (strncmp(buf, prefix, strlen(prefix)) != 0) return 0;
    const char *slash = strchr(buf + strlen(prefix), '/');
    if (!slash) return 0;
    size_t len = (size_t)(slash - buf);
    if (len >= out_cap) return 0;
    memcpy(out, buf, len);
    out[len] = '\0';
    return 1;
}

static int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

int main(void) {
    AsnixDaemonConn *conn = asnix_daemon_connect();
    if (!conn) {
        printf("test_daemon_client: SKIPPED (no reachable nix-daemon in this environment)\n");
        return 0;
    }

    /* Well-formed (32-char base32 hash + '-' + name) but definitely
     * absent paths -- a malformed hash length instead produces a daemon-
     * side parse *error* (confirmed while writing this test: the daemon
     * closes the connection after one), which is a different, also-
     * correct behavior this test isn't trying to exercise. */
    const char *bogus1 = "/nix/store/00000000000000000000000000000000-definitely-not-real";
    const char *bogus2 = "/nix/store/11111111111111111111111111111111-also-not-real";

    int r1 = asnix_daemon_is_valid_path(conn, bogus1);
    CHECK(r1 == 0, "a made-up path is reported invalid");

    int r2 = asnix_daemon_is_valid_path(conn, bogus2);
    CHECK(r2 == 0, "a second call on the same connection also works (stream stays in sync)");

    int r3 = asnix_daemon_is_valid_path(conn, bogus1);
    CHECK(r3 == 0, "a third call (repeating the first path) is still consistent");

    char real_path[300];
    if (find_a_real_store_path(real_path, sizeof(real_path))) {
        int r4 = asnix_daemon_is_valid_path(conn, real_path);
        CHECK(r4 == 1, "a real, definitely-present store path is reported valid");
    } else {
        printf("test_daemon_client: (positive-case check skipped -- couldn't resolve a real store path)\n");
    }

    asnix_daemon_close(conn);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_daemon_client: all checks passed (live daemon)\n");
    return 0;
}
