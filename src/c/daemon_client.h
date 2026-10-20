#ifndef ASNIX_DAEMON_CLIENT_H
#define ASNIX_DAEMON_CLIENT_H

/*
 * A minimal client for the real Nix worker (daemon) protocol, scoped to
 * connect + handshake + `IsValidPath` (the simplest, read-only op, picked
 * as the first one to prove the handshake/framing mechanism against a
 * real daemon before expanding -- same "smallest slice first" discipline
 * as M8's derivation work). See docs/architecture.md's M8 notes for the
 * full writeup: wire format, magic numbers, and op codes were cross-
 * checked against two independent real implementations before writing
 * any code -- ~/nix/src/libstore/include/nix/store/worker-protocol.hh
 * (cppnix's own header) and ~/gnix/fix/src/store/daemon/{wire,client}.zig
 * (a from-scratch reimplementation) agree exactly on both.
 *
 * Deliberately narrow: no build/realization ops, no NAR transfer, no
 * structured-activity forwarding (the stderr sideband's start/stop-
 * activity and result messages are skipped, not acted on) -- just enough
 * to ask a live daemon "is this store path valid" and get a real answer.
 */

typedef struct AsnixDaemonConn AsnixDaemonConn;

/* Connects to the daemon over its Unix socket (NIX_REMOTE if set, else
 * /nix/var/nix/daemon-socket/socket) and completes the version handshake.
 * Returns NULL (after printing a message to stderr) on any failure --
 * socket connect, magic mismatch, or incompatible protocol major version. */
AsnixDaemonConn *asnix_daemon_connect(void);

void asnix_daemon_close(AsnixDaemonConn *conn);

/* Returns 1 if `path` is a valid store path, 0 if not, -1 on a protocol
 * or I/O error (message already printed to stderr). */
int asnix_daemon_is_valid_path(AsnixDaemonConn *conn, const char *path);

#endif /* ASNIX_DAEMON_CLIENT_H */
