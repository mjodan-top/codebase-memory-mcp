#include "daemon/launchd_activation.h"

#include <errno.h>
#include <stddef.h>

#ifdef __APPLE__

#include <launch.h>
#include <stdlib.h>
#include <unistd.h>

int cbm_launchd_activation_fd(const char *socket_key) {
    const char *key = (socket_key && socket_key[0] != '\0') ? socket_key : CBM_LAUNCHD_SOCKET_KEY;
    int *fds = NULL;
    size_t count = 0;
    int rc = launch_activate_socket(key, &fds, &count);
    if (rc != 0) {
        /* launch_activate_socket returns the error value directly (it does
         * not always set errno), so propagate it explicitly. */
        errno = rc;
        return -1;
    }
    if (!fds || count == 0) {
        free(fds);
        errno = EINVAL;
        return -1;
    }
    if (count != 1) {
        /* Single-listener contract: refuse ambiguous multi-fd activation
         * instead of silently serving only one of them. */
        for (size_t i = 0; i < count; i++)
            close(fds[i]);
        free(fds);
        errno = EINVAL;
        return -1;
    }
    int fd = fds[0];
    free(fds);
    return fd;
}

#else /* !__APPLE__ */

int cbm_launchd_activation_fd(const char *socket_key) {
    (void)socket_key;
    errno = ENOTSUP;
    return -1;
}

#endif
