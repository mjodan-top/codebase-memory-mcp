#include "daemon/systemd_activation.h"

#include <errno.h>
#include <stdlib.h>

int cbm_systemd_parse_listen_fds(const char *listen_pid, const char *listen_fds, long self_pid) {
    /* Missing/empty variables mean systemd did not activate this process:
     * that is "no fds for us" (ESRCH), distinct from a malformed handoff. */
    if (!listen_pid || listen_pid[0] == '\0' || !listen_fds || listen_fds[0] == '\0') {
        errno = ESRCH;
        return -1;
    }

    char *end = NULL;
    errno = 0;
    long pid = strtol(listen_pid, &end, 10);
    if (errno != 0 || !end || *end != '\0' || pid <= 0 || pid != self_pid) {
        /* sd_listen_fds contract: fds addressed to another PID are simply
         * not ours — refuse instead of stealing a stranger's listener. */
        errno = ESRCH;
        return -1;
    }

    end = NULL;
    errno = 0;
    long nfds = strtol(listen_fds, &end, 10);
    if (errno != 0 || !end || *end != '\0' || nfds < 0) {
        errno = EINVAL;
        return -1;
    }
    if (nfds != 1) {
        /* Single-listener contract: refuse zero or ambiguous multi-fd
         * activation instead of silently serving only one of them. */
        errno = EINVAL;
        return -1;
    }
    return CBM_SD_LISTEN_FDS_START;
}

#ifdef __linux__

#include <sys/socket.h>
#include <unistd.h>

int cbm_systemd_activation_fd(void) {
    int fd =
        cbm_systemd_parse_listen_fds(getenv("LISTEN_PID"), getenv("LISTEN_FDS"), (long)getpid());
    int saved = errno;
    /* Consume-once: unset regardless of outcome so children never
     * misinterpret the activation environment (sd_listen_fds
     * unset_environment semantics). */
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    if (fd < 0) {
        errno = saved;
        return -1;
    }

    /* Linux supports SO_ACCEPTCONN on AF_UNIX: require the inherited fd to
     * already be in the listening state. (macOS lacks this option and uses
     * an idempotent listen() in the launchd path — do not mirror that shape
     * here; a real check is available, so use it.) */
    int accepting = 0;
    socklen_t optlen = (socklen_t)sizeof(accepting);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &optlen) < 0)
        return -1;
    if (!accepting) {
        errno = EINVAL;
        return -1;
    }
    return fd;
}

#else /* !__linux__ */

int cbm_systemd_activation_fd(void) {
    errno = ENOTSUP;
    return -1;
}

#endif
