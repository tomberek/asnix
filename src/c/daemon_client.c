#include "daemon_client.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Wire constants -- see daemon_client.h's header comment for the two
 * independent sources these were cross-checked against. */
#define ASNIX_WORKER_MAGIC_1     0x6e697863
#define ASNIX_WORKER_MAGIC_2     0x6478696f
#define ASNIX_PROTOCOL_VERSION   ((1 << 8) | 35)
#define ASNIX_MIN_PROTOCOL_MINOR 26

#define ASNIX_STDERR_NEXT           0x6f6c6d67
#define ASNIX_STDERR_LAST           0x616c7473
#define ASNIX_STDERR_ERROR          0x63787470
#define ASNIX_STDERR_START_ACTIVITY 0x53545254
#define ASNIX_STDERR_STOP_ACTIVITY  0x53544f50
#define ASNIX_STDERR_RESULT         0x52534c54

#define ASNIX_OP_IS_VALID_PATH 1
/* op 7, NOT the obsolete op 8 (AddTextToStore, dead since protocol 1.25)
 * -- cross-checked against ~/nix's remote-store.cc (the >= {1,25} branch
 * real clients take) and ~/gnix/fix's client.zig, which agree exactly. */
#define ASNIX_OP_ADD_TO_STORE 7
#define ASNIX_OP_BUILD_PATHS  9
#define ASNIX_OP_SET_OPTIONS  19

#define ASNIX_BUILD_MODE_NORMAL 0

struct AsnixDaemonConn {
    int fd;
    uint64_t daemon_version;
    int trusted; /* -1 = unknown/not read (daemon_minor < 35) */
};

static int write_full(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1; /* EOF or error: the daemon hung up or died */
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_u64(int fd, uint64_t v) {
    unsigned char buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (unsigned char)(v >> (8 * i));
    return write_full(fd, buf, 8);
}

static int read_u64(int fd, uint64_t *out) {
    unsigned char buf[8];
    if (read_full(fd, buf, 8) != 0) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[i] << (8 * i);
    *out = v;
    return 0;
}

static size_t pad_len(size_t len) {
    return (8 - (len % 8)) % 8;
}

static int write_string(int fd, const char *s, size_t len) {
    if (write_u64(fd, len) != 0) return -1;
    if (write_full(fd, s, len) != 0) return -1;
    size_t pad = pad_len(len);
    if (pad == 0) return 0;
    static const unsigned char zeros[8] = { 0 };
    return write_full(fd, zeros, pad);
}

static int write_cstr(int fd, const char *s) {
    return write_string(fd, s, strlen(s));
}

/* Reads a length-prefixed, zero-padded string, discarding the content
 * (the client doesn't need anything but a handful of specific strings
 * right now, e.g. the error message in read_error below). */
static int skip_string(int fd) {
    uint64_t len;
    if (read_u64(fd, &len) != 0) return -1;
    char buf[4096];
    uint64_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        if (read_full(fd, buf, chunk) != 0) return -1;
        remaining -= chunk;
    }
    size_t pad = pad_len((size_t)len);
    if (pad == 0) return 0;
    char padbuf[8];
    return read_full(fd, padbuf, pad);
}

/* Like skip_string, but keeps the content (malloc'd, NUL-terminated;
 * caller frees) -- used only for the one string read_error actually
 * reports. */
static char *read_string_alloc(int fd) {
    uint64_t len;
    if (read_u64(fd, &len) != 0) return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    if (len > 0 && read_full(fd, buf, (size_t)len) != 0) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    size_t pad = pad_len((size_t)len);
    if (pad > 0) {
        char padbuf[8];
        if (read_full(fd, padbuf, pad) != 0) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

/* Error serialization for protocol >= 26: type, level, name, msg,
 * havePos, then a trace list of (havePos, hint) pairs. Keeps `msg`
 * (reported to stderr), skips everything else -- same scope fix's own
 * client settles for (see daemon_client.h's header comment). */
static void read_error_and_report(int fd) {
    skip_string(fd); /* type ("Error") */
    uint64_t level;
    read_u64(fd, &level);
    skip_string(fd); /* name */
    char *msg = read_string_alloc(fd);
    uint64_t have_pos;
    read_u64(fd, &have_pos);
    uint64_t n_traces = 0;
    read_u64(fd, &n_traces);
    for (uint64_t i = 0; i < n_traces; i++) {
        uint64_t trace_have_pos;
        read_u64(fd, &trace_have_pos);
        skip_string(fd);
    }
    fprintf(stderr, "asnix: daemon error: %s\n", msg ? msg : "(no message)");
    free(msg);
}

/* Drains the STDERR_* sideband the daemon sends after the handshake and
 * after every op, up to STDERR_LAST. Returns 0 on success, -1 on a
 * daemon-reported error (already printed) or a protocol/I/O error. */
static int process_stderr(int fd) {
    for (;;) {
        uint64_t tag;
        if (read_u64(fd, &tag) != 0) {
            fprintf(stderr, "asnix: daemon: connection closed while reading stderr sideband\n");
            return -1;
        }
        switch (tag) {
            case ASNIX_STDERR_LAST: return 0;
            case ASNIX_STDERR_ERROR: read_error_and_report(fd); return -1;
            case ASNIX_STDERR_NEXT: skip_string(fd); break;
            case ASNIX_STDERR_START_ACTIVITY: {
                /* act id, level, type, text, fields list, parent id --
                 * not forwarded anywhere (no build/activity UI yet), just
                 * consumed so framing stays in sync. */
                uint64_t act, level, type;
                read_u64(fd, &act);
                read_u64(fd, &level);
                read_u64(fd, &type);
                skip_string(fd);
                uint64_t nfields = 0;
                read_u64(fd, &nfields);
                for (uint64_t i = 0; i < nfields; i++) {
                    uint64_t field_type;
                    read_u64(fd, &field_type);
                    if (field_type == 0)
                        read_u64(fd, &act); /* int field, reuse `act` as scratch */
                    else
                        skip_string(fd); /* string field */
                }
                uint64_t parent;
                read_u64(fd, &parent);
                break;
            }
            case ASNIX_STDERR_STOP_ACTIVITY: {
                uint64_t act;
                read_u64(fd, &act);
                break;
            }
            case ASNIX_STDERR_RESULT: {
                uint64_t act, type, nfields;
                read_u64(fd, &act);
                read_u64(fd, &type);
                read_u64(fd, &nfields);
                for (uint64_t i = 0; i < nfields; i++) {
                    uint64_t field_type;
                    read_u64(fd, &field_type);
                    if (field_type == 0)
                        read_u64(fd, &act);
                    else
                        skip_string(fd);
                }
                break;
            }
            default:
                fprintf(stderr, "asnix: daemon: unrecognized stderr sideband tag 0x%llx\n",
                        (unsigned long long)tag);
                return -1;
        }
    }
}

AsnixDaemonConn *asnix_daemon_connect(void) {
    const char *path = getenv("NIX_REMOTE");
    if (!path || path[0] == '\0' || strcmp(path, "daemon") == 0) path = "/nix/var/nix/daemon-socket/socket";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "asnix: daemon: socket() failed\n");
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "asnix: daemon: socket path too long\n");
        close(fd);
        return NULL;
    }
    strcpy(addr.sun_path, path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "asnix: daemon: connect to '%s' failed (is nix-daemon running?)\n", path);
        close(fd);
        return NULL;
    }

    if (write_u64(fd, ASNIX_WORKER_MAGIC_1) != 0) goto io_error;

    uint64_t magic2;
    if (read_u64(fd, &magic2) != 0) goto io_error;
    if (magic2 != ASNIX_WORKER_MAGIC_2) {
        fprintf(stderr, "asnix: daemon: worker magic mismatch (not a Nix daemon?)\n");
        close(fd);
        return NULL;
    }

    uint64_t daemon_version;
    if (read_u64(fd, &daemon_version) != 0) goto io_error;
    uint64_t daemon_major = daemon_version & 0xff00;
    uint64_t daemon_minor = daemon_version & 0x00ff;
    uint64_t our_major = ASNIX_PROTOCOL_VERSION & 0xff00;
    if (daemon_major != our_major) {
        fprintf(stderr, "asnix: daemon: protocol major version mismatch (daemon=%llu.%llu, asnix=1.%d)\n",
                (unsigned long long)(daemon_major >> 8), (unsigned long long)daemon_minor,
                ASNIX_PROTOCOL_VERSION & 0xff);
        close(fd);
        return NULL;
    }
    if (daemon_minor < ASNIX_MIN_PROTOCOL_MINOR) {
        fprintf(stderr, "asnix: daemon: protocol too old (daemon minor=%llu, need >=%d)\n",
                (unsigned long long)daemon_minor, ASNIX_MIN_PROTOCOL_MINOR);
        close(fd);
        return NULL;
    }

    if (write_u64(fd, ASNIX_PROTOCOL_VERSION) != 0) goto io_error;
    /* Two obsolete fields modern daemons still expect on the wire: CPU
     * affinity (minor>=14) and reserveSpace (minor>=11). Both always 0. */
    if (daemon_minor >= 14 && write_u64(fd, 0) != 0) goto io_error;
    if (daemon_minor >= 11 && write_u64(fd, 0) != 0) goto io_error;

    if (daemon_minor >= 33 && skip_string(fd) != 0) goto io_error; /* nix version string, informational */
    int trusted = -1;
    if (daemon_minor >= 35) {
        uint64_t trusted_raw;
        if (read_u64(fd, &trusted_raw) != 0) goto io_error;
        trusted = (int)trusted_raw;
    }

    if (process_stderr(fd) != 0) {
        close(fd);
        return NULL;
    }

    AsnixDaemonConn *conn = malloc(sizeof(AsnixDaemonConn));
    conn->fd = fd;
    conn->daemon_version = daemon_version;
    conn->trusted = trusted;
    return conn;

io_error:
    fprintf(stderr, "asnix: daemon: I/O error during handshake\n");
    close(fd);
    return NULL;
}

void asnix_daemon_close(AsnixDaemonConn *conn) {
    if (!conn) return;
    close(conn->fd);
    free(conn);
}

int asnix_daemon_is_valid_path(AsnixDaemonConn *conn, const char *path) {
    if (write_u64(conn->fd, ASNIX_OP_IS_VALID_PATH) != 0) return -1;
    if (write_cstr(conn->fd, path) != 0) return -1;
    if (process_stderr(conn->fd) != 0) return -1;
    uint64_t result;
    if (read_u64(conn->fd, &result) != 0) return -1;
    return result != 0 ? 1 : 0;
}

int asnix_daemon_trusted(AsnixDaemonConn *conn) {
    return conn->trusted;
}

/* Skips a length-prefixed SET of strings: u64 count, then that many
 * length-prefixed (padded) strings -- the wire shape AddToStore's
 * `references` request field and ValidPathInfo's `references`/`sigs`
 * response fields both use (see daemon_client.h's AddToStore comment and
 * ~/nix's length-prefixed-protocol-helper.hh, cross-checked against
 * ~/gnix/fix's client.zig readStrings). */
static int skip_string_set(int fd) {
    uint64_t count;
    if (read_u64(fd, &count) != 0) return -1;
    for (uint64_t i = 0; i < count; i++) {
        if (skip_string(fd) != 0) return -1;
    }
    return 0;
}

/* Reads AddToStore's response -- a full ValidPathInfo (~/nix's
 * worker-protocol.cc UnkeyedValidPathInfo::write, cross-checked against
 * ~/gnix/fix's client.zig readValidPathInfo): path, deriver (optional
 * string, empty = none), narHash, references (string set),
 * registrationTime (u64), narSize (u64), ultimate (bool-as-u64), sigs
 * (string set), ca (string). Only `path` is useful to this client; every
 * other field is skipped but must still be correctly walked past to stay
 * in sync with the daemon's byte stream. Returns a malloc'd, NUL-
 * terminated copy of `path` (caller frees), or NULL on a protocol/IO
 * error. */
static char *read_valid_path_info_path(int fd) {
    char *path = read_string_alloc(fd);
    if (!path) return NULL;
    if (skip_string(fd) != 0 /* deriver */
        || skip_string(fd) != 0 /* narHash */
        || skip_string_set(fd) != 0 /* references */) {
        free(path);
        return NULL;
    }
    uint64_t registration_time, nar_size, ultimate;
    if (read_u64(fd, &registration_time) != 0 || read_u64(fd, &nar_size) != 0 ||
        read_u64(fd, &ultimate) != 0) {
        free(path);
        return NULL;
    }
    if (skip_string_set(fd) != 0 /* sigs */ || skip_string(fd) != 0 /* ca */) {
        free(path);
        return NULL;
    }
    return path;
}

char *asnix_daemon_add_text_to_store(AsnixDaemonConn *conn, const char *name, const char *text, size_t text_len) {
    if (write_u64(conn->fd, ASNIX_OP_ADD_TO_STORE) != 0) return NULL;
    if (write_cstr(conn->fd, name) != 0) return NULL;
    if (write_cstr(conn->fd, "text:sha256") != 0) return NULL; /* content-address method -- a .drv is always this */
    if (write_u64(conn->fd, 0) != 0) return NULL;              /* references: empty set (no input drvs/sources yet) */
    if (write_u64(conn->fd, 0) != 0) return NULL;              /* repair = false (true is rejected for untrusted clients) */

    /* FramedSink: one or more [u64 len][raw bytes, NOT padded -- distinct
     * from write_string's length-prefixed-and-padded convention used
     * everywhere else in this protocol] chunks, terminated by a single
     * zero-length chunk. One chunk is enough for a .drv's text. */
    if (write_u64(conn->fd, (uint64_t)text_len) != 0) return NULL;
    if (text_len > 0 && write_full(conn->fd, text, text_len) != 0) return NULL;
    if (write_u64(conn->fd, 0) != 0) return NULL; /* terminating zero-length frame */

    if (process_stderr(conn->fd) != 0) return NULL;
    return read_valid_path_info_path(conn->fd);
}

int asnix_daemon_set_options(AsnixDaemonConn *conn, int use_substitutes) {
    if (write_u64(conn->fd, ASNIX_OP_SET_OPTIONS) != 0) return -1;
    if (write_u64(conn->fd, 0) != 0) return -1; /* keepFailed */
    if (write_u64(conn->fd, 0) != 0) return -1; /* keepGoing */
    if (write_u64(conn->fd, 0) != 0) return -1; /* tryFallback */
    if (write_u64(conn->fd, 0) != 0) return -1; /* verbosity (lvlError) */
    if (write_u64(conn->fd, 1) != 0) return -1; /* maxBuildJobs */
    if (write_u64(conn->fd, 0) != 0) return -1; /* maxSilentTime */
    if (write_u64(conn->fd, 1) != 0) return -1; /* obsolete useBuildHook -- real clients always send true */
    if (write_u64(conn->fd, 1) != 0) return -1; /* verboseBuild slot: nonzero -> daemon sees verboseBuild=false */
    if (write_u64(conn->fd, 0) != 0) return -1; /* obsolete logType */
    if (write_u64(conn->fd, 0) != 0) return -1; /* obsolete printBuildTrace */
    if (write_u64(conn->fd, 1) != 0) return -1; /* buildCores */
    if (write_u64(conn->fd, (uint64_t)(use_substitutes ? 1 : 0)) != 0) return -1; /* useSubstitutes */
    if (write_u64(conn->fd, 0) != 0) return -1; /* settings overrides: empty set */
    return process_stderr(conn->fd);
}

int asnix_daemon_build_drv(AsnixDaemonConn *conn, const char *drv_path) {
    /* BuildPaths (op 9), not BuildDerivation (op 36): the .drv is already
     * in the store (via asnix_daemon_add_text_to_store above), so this
     * references it by path rather than re-sending the whole derivation
     * inline -- also the only option available to an untrusted client in
     * the first place (~/nix's daemon.cc rejects BuildDerivation outright
     * for an input-addressed derivation from a non-trusted client,
     * stricter than BuildPaths's own build-time check). The legacy
     * DerivedPath encoding for "build every output of this .drv" is
     * "<drvPath>!*" (cross-checked against ~/nix's derived-path.cc and
     * ~/gnix/fix's client.zig, which agree exactly). */
    char derived_path[320];
    int n = snprintf(derived_path, sizeof(derived_path), "%s!*", drv_path);
    if (n < 0 || (size_t)n >= sizeof(derived_path)) {
        fprintf(stderr, "asnix: daemon: drv path too long\n");
        return -1;
    }

    if (write_u64(conn->fd, ASNIX_OP_BUILD_PATHS) != 0) return -1;
    if (write_u64(conn->fd, 1) != 0) return -1; /* one DerivedPath in the list */
    if (write_cstr(conn->fd, derived_path) != 0) return -1;
    if (write_u64(conn->fd, ASNIX_BUILD_MODE_NORMAL) != 0) return -1;

    /* Real success/failure is signaled via the STDERR_ERROR sideband
     * during this op (e.g. "you are not privileged to build input-
     * addressed derivations" for an untrusted client -- a real, expected
     * daemon permission boundary, not a bug in this client), not by the
     * dummy result sentinel that follows it on success. */
    if (process_stderr(conn->fd) != 0) return -1;
    uint64_t dummy_result;
    if (read_u64(conn->fd, &dummy_result) != 0) return -1;
    return 0;
}
