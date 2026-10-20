/*
 * Standalone, manually-invoked demonstration of real derivation
 * realization against the live system nix-daemon: computes a trivial,
 * harmless derivation exactly the way builtins.derivation (derivation.c)
 * does, adds its .drv to the store via AddToStore, and triggers a real
 * build via BuildPaths.
 *
 * Deliberately NOT part of `make test`'s tests/c/ glob: this writes to
 * the real, live Nix store and attempts a real build every time it runs,
 * which is not something a routine `make test` invocation should do by
 * default. Build and run explicitly:
 *
 *   cc -std=c11 -Iinclude -Isrc/c -o /tmp/realize_demo tools/realize_demo.c -L. -lasnix
 *   /tmp/realize_demo
 *
 * Expected outcome depends on this account's daemon trust verdict (printed
 * below -- on this dev machine it is unexpectedly 1/trusted despite no
 * explicit `trusted-users` entry found in /etc/nix/nix.conf, so BuildPaths
 * is not rejected on a trust boundary the way an ordinary untrusted
 * account would see ("you are not privileged to build input-addressed
 * derivations"); see daemon_client.h's asnix_daemon_build_drv comment for
 * that real permission boundary. SetOptions(useSubstitutes=0) is sent
 * first so the build goes straight to a local build rather than consulting
 * this machine's configured substituters (one of which is a private
 * company Cachix cache with an expired auth token, unrelated to asnix).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aterm.h"
#include "daemon_client.h"
#include "hash_shim.h"
#include "store_path.h"

static void sha256_or_die(const char *s, size_t len, uint8_t out[32]) {
    if (asnix_hash_sha256(s, len, out) != 0) {
        fprintf(stderr, "realize_demo: failed to load libcrypto\n");
        exit(1);
    }
}

/* Mirrors derivation.c's file-local compute_store_path exactly (hex-
 * encode the inner digest with its sha256: prefix, assemble the
 * "type:hash:storeDir:name" preimage, hash THAT) -- kept as a small,
 * independent copy here rather than exporting derivation.c's static
 * helper, since this tool is deliberately outside the normal build/test
 * graph and shouldn't widen derivation.c's own interface just for it. */
static void compute_store_path(const char *type, const uint8_t inner_digest[32], const char *name, char *out,
                                size_t out_cap) {
    static const char hexdigits[] = "0123456789abcdef";
    char hashstr[7 + 64 + 1];
    memcpy(hashstr, "sha256:", 7);
    for (int i = 0; i < 32; i++) {
        hashstr[7 + 2 * i] = hexdigits[inner_digest[i] >> 4];
        hashstr[7 + 2 * i + 1] = hexdigits[inner_digest[i] & 0xf];
    }
    hashstr[7 + 64] = '\0';
    char preimage[512];
    size_t n = asnix_store_path_preimage(type, hashstr, name, preimage, sizeof(preimage));
    uint8_t outer_digest[32];
    sha256_or_die(preimage, n, outer_digest);
    if (asnix_make_store_path_from_digest(outer_digest, name, out, out_cap) == 0) {
        fprintf(stderr, "realize_demo: store path computation failed\n");
        exit(1);
    }
}

int main(void) {
    const char *name = "asnix-daemon-realize-test";
    const char *system = "x86_64-linux";
    const char *builder = "/bin/sh";
    const char *args[2] = { "-c", "echo hi from asnix > $out" };
    AtermEnvVar env[1] = { { "out", "" } };

    /* Pass 1: masked (out="") -- computes the output path. */
    char *masked = asnix_build_derivation_aterm("", system, builder, args, 2, env, 1);
    uint8_t masked_digest[32];
    sha256_or_die(masked, strlen(masked), masked_digest);
    free(masked);

    char out_path[300];
    compute_store_path("output:out", masked_digest, name, out_path, sizeof(out_path));
    env[0].value = out_path;

    /* Pass 2: real ATerm text with the output path substituted in --
     * this is the actual .drv content to add to the store. */
    char *real_aterm = asnix_build_derivation_aterm(out_path, system, builder, args, 2, env, 1);
    uint8_t real_digest[32];
    sha256_or_die(real_aterm, strlen(real_aterm), real_digest);

    char drv_name[256];
    snprintf(drv_name, sizeof(drv_name), "%s.drv", name);
    char drv_path[300];
    compute_store_path("text", real_digest, drv_name, drv_path, sizeof(drv_path));

    printf("computed drvPath: %s\n", drv_path);
    printf("computed outPath: %s\n", out_path);

    AsnixDaemonConn *conn = asnix_daemon_connect();
    if (!conn) {
        fprintf(stderr, "realize_demo: no reachable nix-daemon\n");
        free(real_aterm);
        return 1;
    }
    printf("daemon trust verdict for this client: %d (1=trusted, 0=not, 2=unset, -1=unknown)\n",
           asnix_daemon_trusted(conn));

    if (asnix_daemon_set_options(conn, 0) != 0) {
        fprintf(stderr, "realize_demo: SetOptions failed\n");
        free(real_aterm);
        asnix_daemon_close(conn);
        return 1;
    }

    char *added_path = asnix_daemon_add_text_to_store(conn, drv_name, real_aterm, strlen(real_aterm));
    free(real_aterm);
    if (!added_path) {
        fprintf(stderr, "realize_demo: AddToStore failed\n");
        asnix_daemon_close(conn);
        return 1;
    }
    printf("daemon-computed path: %s\n", added_path);
    if (strcmp(added_path, drv_path) != 0) {
        fprintf(stderr, "realize_demo: MISMATCH -- our computed drvPath disagrees with the daemon's\n");
        free(added_path);
        asnix_daemon_close(conn);
        return 1;
    }
    printf("reproducibility check: OK (locally-computed drvPath matches the daemon's)\n");
    free(added_path);

    int rc = asnix_daemon_build_drv(conn, drv_path);
    printf("BuildPaths result: %s\n", rc == 0 ? "accepted (check output below)" : "rejected/failed (see error above)");

    int valid = asnix_daemon_is_valid_path(conn, out_path);
    printf("output path valid after build attempt: %s\n", valid == 1 ? "yes" : "no");

    asnix_daemon_close(conn);
    return 0;
}
