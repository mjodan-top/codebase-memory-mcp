#ifndef CBM_DAEMON_LAUNCHD_ACTIVATION_H
#define CBM_DAEMON_LAUNCHD_ACTIVATION_H

/*
 * launchd_activation.h — macOS launchd socket activation (Issue #29).
 *
 * When the daemon is started BY launchd in response to a first client
 * connection, launchd has already bound + listen()ed the pathname UDS
 * declared in the LaunchAgent plist's <Sockets> dictionary. The daemon must
 * NOT bind its own socket in that case; it asks launchd for the inherited
 * listening fd via the official launch_activate_socket() API and adopts it
 * into the existing cbm_uds_owner_t lifecycle seam
 * (cbm_uds_owner_adopt() in uds_lifecycle.h).
 *
 * PV ref: https://developer.apple.com/documentation/xpc/launch_activate_socket
 *   int launch_activate_socket(const char *name, int **fds, size_t *cnt);
 *   - returns 0 and a malloc()ed fd array on success (caller frees),
 *   - ENOENT when the socket name is not in the plist,
 *   - ESRCH when the process was not launched by launchd (or the socket
 *     set was not created by launchd on this process's behalf),
 *   - EALREADY when the fds were already activated once.
 *
 * On non-Apple platforms every function fails with errno = ENOTSUP
 * (fail-closed: never pretend activation happened).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* The <Sockets> dictionary key the LaunchAgent plist must use. */
#define CBM_LAUNCHD_SOCKET_KEY "Listeners"

/* Ask launchd for the single inherited listening UDS fd registered under
 * socket_key (NULL means CBM_LAUNCHD_SOCKET_KEY).
 *
 * Returns the fd (>= 0) on success. Returns -1 with errno set on failure:
 *   ESRCH   not launched by launchd — caller should fall back or fail,
 *   ENOENT  plist has no such socket key,
 *   EINVAL  launchd handed back zero or more than one fd (the daemon's
 *           single-listener contract only accepts exactly one),
 *   ENOTSUP not macOS.
 * Never returns a fabricated fd. */
int cbm_launchd_activation_fd(const char *socket_key);

#ifdef __cplusplus
}
#endif

#endif
