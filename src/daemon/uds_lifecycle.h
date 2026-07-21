#ifndef CBM_DAEMON_UDS_LIFECYCLE_H
#define CBM_DAEMON_UDS_LIFECYCLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cbm_uds_state {
    CBM_UDS_D_STARTING = 0,
    CBM_UDS_D_BIND_CLAIM,
    CBM_UDS_D_LISTENING,
    CBM_UDS_D_SERVING,
    CBM_UDS_D_SINGLETON_LOSER,
    CBM_UDS_D_STALE_RECLAIMED,
    CBM_UDS_D_DRAINING,
    CBM_UDS_D_EXITED
} cbm_uds_state_t;

typedef void (*cbm_uds_state_callback_t)(cbm_uds_state_t state, void *userdata);

typedef struct cbm_uds_owner {
    int listen_fd;
    int claim_fd;
    unsigned long long socket_dev;
    unsigned long long socket_ino;
    unsigned long expected_peer_uid;
    cbm_uds_state_callback_t state_callback;
    void *state_userdata;
    char socket_path[108];
} cbm_uds_owner_t;

typedef struct cbm_uds_connection {
    int fd;
    unsigned long peer_uid;
} cbm_uds_connection_t;

const char *cbm_uds_state_name(cbm_uds_state_t state);

/* Resolve a user-private pathname UDS and create/validate its immediate parent.
 * override_path may be NULL; otherwise it is copied verbatim after validation. */
int cbm_uds_socket_path_resolve(char *out, size_t out_size, const char *override_path);

/* Claim one pathname UDS, bind it, and begin listening.
 * Returns 0 on success and -1 with errno set on failure. */
int cbm_uds_owner_open(cbm_uds_owner_t *owner, const char *socket_path, int backlog);

/* Configure lifecycle observation and peer policy before cbm_uds_owner_open().
 * expected_peer_uid defaults to the effective uid when set to (unsigned long)-1. */
void cbm_uds_owner_configure(cbm_uds_owner_t *owner, unsigned long expected_peer_uid,
                             cbm_uds_state_callback_t callback, void *userdata);

/* Adopt an already-listening AF_UNIX fd inherited from a service manager
 * (macOS launchd socket activation, Issue #29). The manager bound + listened
 * the socket at socket_path before spawning us; adopt verifies the fd really
 * is a listening AF_UNIX socket, still claims the single-owner lock, and
 * enters the normal lifecycle (D_STARTING -> D_BIND_CLAIM -> D_LISTENING).
 * The socket inode stays owned by the manager: cbm_uds_owner_close() will
 * NOT unlink it. Returns 0 on success, -1 with errno set on failure (the fd
 * is not closed on failure; the caller owns it until adopt succeeds). */
int cbm_uds_owner_adopt(cbm_uds_owner_t *owner, int listen_fd, const char *socket_path);

/* Accept one real AF_UNIX connection and reject peers outside expected_peer_uid. */
int cbm_uds_owner_accept(cbm_uds_owner_t *owner, cbm_uds_connection_t *connection);
void cbm_uds_connection_close(cbm_uds_connection_t *connection);

/* Close the listener and unlink only the exact socket inode created by owner. */
void cbm_uds_owner_close(cbm_uds_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif
