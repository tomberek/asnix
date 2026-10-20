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

struct AsnixDaemonConn {
    int fd;
    uint64_t daemon_version;
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
    if (daemon_minor >= 35) {
        uint64_t trusted;
        if (read_u64(fd, &trusted) != 0) goto io_error;
    }

    if (process_stderr(fd) != 0) {
        close(fd);
        return NULL;
    }

    AsnixDaemonConn *conn = malloc(sizeof(AsnixDaemonConn));
    conn->fd = fd;
    conn->daemon_version = daemon_version;
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
