#ifndef ASNIX_DAEMON_CLIENT_H
#define ASNIX_DAEMON_CLIENT_H

#include <stddef.h>

typedef struct AsnixDaemonConn AsnixDaemonConn;

AsnixDaemonConn *asnix_daemon_connect(void);

void asnix_daemon_close(AsnixDaemonConn *conn);

int asnix_daemon_is_valid_path(AsnixDaemonConn *conn, const char *path);

char *asnix_daemon_add_text_to_store(AsnixDaemonConn *conn, const char *name, const char *text, size_t text_len);

int asnix_daemon_build_drv(AsnixDaemonConn *conn, const char *drv_path);

int asnix_daemon_trusted(AsnixDaemonConn *conn);

int asnix_daemon_set_options(AsnixDaemonConn *conn, int use_substitutes);

#endif

