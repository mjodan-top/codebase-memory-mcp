#ifndef CBM_DAEMON_SYSTEMD_ACTIVATION_H
#define CBM_DAEMON_SYSTEMD_ACTIVATION_H

/*
 * systemd_activation.h — Linux user systemd socket activation (Issue #29).
 *
 * When the daemon is started BY systemd in response to a first client
 * connection on a `.socket` unit, systemd has already bound + listen()ed
 * the pathname UDS and passes it as an inherited fd starting at
 * SD_LISTEN_FDS_START, described by the LISTEN_PID / LISTEN_FDS
 * environment variables. The daemon must NOT bind its own socket in that
 * case; it validates the sd_listen_fds contract by hand (no libsystemd
 * dependency) and adopts the fd into the existing cbm_uds_owner_t
 * lifecycle seam (cbm_uds_owner_adopt() in uds_lifecycle.h).
 *
 * PV ref: https://www.freedesktop.org/software/systemd/man/latest/sd_listen_fds.html
 *   - fds are passed starting at SD_LISTEN_FDS_START (== 3),
 *   - LISTEN_PID must equal getpid() or the fds are not for us,
 *   - LISTEN_FDS holds the number of passed fds,
 *   - the variables should be unset after use so children do not
 *     misinterpret them (sd_listen_fds unset_environment semantics).
 *
 * The socket inode stays owned by systemd: cbm_uds_owner_adopt() records
 * no inode so cbm_uds_owner_close() will not unlink it (systemd needs the
 * path in place to re-activate the daemon on the next connection).
 *
 * On non-Linux platforms cbm_systemd_activation_fd() fails with
 * errno = ENOTSUP (fail-closed: never pretend activation happened).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* First fd passed by the service manager (sd_listen_fds contract). */
#define CBM_SD_LISTEN_FDS_START 3

/* Validate the sd_listen_fds environment contract from the given raw
 * variable values (portable parse seam — unit-testable on every platform).
 *
 * Returns CBM_SD_LISTEN_FDS_START on success. Returns -1 with errno set:
 *   ESRCH   not socket-activated for this process (variables missing/empty,
 *           LISTEN_PID malformed, or LISTEN_PID != self_pid),
 *   EINVAL  LISTEN_FDS is malformed, zero, or more than one fd (the
 *           daemon's single-listener contract accepts exactly one). */
int cbm_systemd_parse_listen_fds(const char *listen_pid, const char *listen_fds, long self_pid);

/* Full activation path: read LISTEN_PID/LISTEN_FDS from the environment,
 * validate them against getpid(), unset them (consume-once), and verify the
 * inherited fd really is in the listening state via SO_ACCEPTCONN (Linux
 * supports it on AF_UNIX; do NOT copy the macOS idempotent-listen() shape
 * here).
 *
 * Returns the fd (>= 0) on success. Returns -1 with errno set on failure:
 *   ESRCH / EINVAL  as in cbm_systemd_parse_listen_fds,
 *   EINVAL          inherited fd is not a listening socket,
 *   ENOTSUP         not Linux.
 * Never returns a fabricated fd. */
int cbm_systemd_activation_fd(void);

#ifdef __cplusplus
}
#endif

#endif
