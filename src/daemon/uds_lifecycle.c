#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "daemon/uds_lifecycle.h"

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static void emit_state(cbm_uds_owner_t *owner, cbm_uds_state_t state) {
    if (owner && owner->state_callback) {
        owner->state_callback(state, owner->state_userdata);
    }
}

const char *cbm_uds_state_name(cbm_uds_state_t state) {
    switch (state) {
    case CBM_UDS_D_STARTING:
        return "D_STARTING";
    case CBM_UDS_D_BIND_CLAIM:
        return "D_BIND_CLAIM";
    case CBM_UDS_D_LISTENING:
        return "D_LISTENING";
    case CBM_UDS_D_SERVING:
        return "D_SERVING";
    case CBM_UDS_D_SINGLETON_LOSER:
        return "D_SINGLETON_LOSER";
    case CBM_UDS_D_STALE_RECLAIMED:
        return "D_STALE_RECLAIMED";
    case CBM_UDS_D_DRAINING:
        return "D_DRAINING";
    case CBM_UDS_D_EXITED:
        return "D_EXITED";
    }
    return "D_UNKNOWN";
}

static void owner_reset_runtime(cbm_uds_owner_t *owner) {
    owner->listen_fd = -1;
    owner->claim_fd = -1;
    owner->socket_dev = 0;
    owner->socket_ino = 0;
    owner->socket_path[0] = '\0';
}

void cbm_uds_owner_configure(cbm_uds_owner_t *owner, unsigned long expected_peer_uid,
                             cbm_uds_state_callback_t callback, void *userdata) {
    if (!owner)
        return;
    memset(owner, 0, sizeof(*owner));
    owner_reset_runtime(owner);
    owner->expected_peer_uid = expected_peer_uid;
    owner->state_callback = callback;
    owner->state_userdata = userdata;
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static int ensure_private_parent(const char *socket_path) {
    char parent[sizeof(((cbm_uds_owner_t *)0)->socket_path)];
    size_t len = strlen(socket_path);
    if (len == 0 || len >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent, socket_path, len + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    if (mkdir(parent, 0700) < 0 && errno != EEXIST)
        return -1;
    struct stat st;
    if (lstat(parent, &st) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int cbm_uds_socket_path_resolve(char *out, size_t out_size, const char *override_path) {
    if (!out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }
    char candidate[sizeof(((cbm_uds_owner_t *)0)->socket_path)];
    int n;
    if (override_path && override_path[0] != '\0') {
        n = snprintf(candidate, sizeof(candidate), "%s", override_path);
    } else {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (runtime && runtime[0] != '\0')
            n = snprintf(candidate, sizeof(candidate), "%s/codebase-memory/daemon.sock", runtime);
        else
            n = snprintf(candidate, sizeof(candidate), "/tmp/codebase-memory-%lu/daemon.sock",
                         (unsigned long)geteuid());
    }
    if (n < 0 || (size_t)n >= sizeof(candidate) || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_private_parent(candidate) < 0)
        return -1;
    memcpy(out, candidate, (size_t)n + 1);
    return 0;
}

static int claim_single_owner(const char *socket_path) {
    char claim_path[sizeof(((cbm_uds_owner_t *)0)->socket_path) + 6];
    int n = snprintf(claim_path, sizeof(claim_path), "%s.lock", socket_path);
    if (n < 0 || (size_t)n >= sizeof(claim_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(claim_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        int saved = errno ? errno : EPERM;
        close(fd);
        errno = saved;
        return -1;
    }
    if (fchmod(fd, 0600) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        int saved = (errno == EACCES || errno == EAGAIN) ? EADDRINUSE : errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int socket_has_live_listener(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(socket_path);
    memcpy(addr.sun_path, socket_path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);

    int rc;
    do {
        rc = connect(fd, (const struct sockaddr *)&addr, addr_len);
    } while (rc < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (rc == 0)
        return 1;
    if (saved == ECONNREFUSED || saved == ENOENT)
        return 0;
    errno = saved;
    return -1;
}

static int remove_stale_socket(cbm_uds_owner_t *owner, const char *socket_path) {
    struct stat st;
    if (lstat(socket_path, &st) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
        errno = EADDRINUSE;
        return -1;
    }

    int live = socket_has_live_listener(socket_path);
    if (live != 0) {
        if (live > 0)
            errno = EADDRINUSE;
        return -1;
    }

    if (unlink(socket_path) < 0)
        return -1;
    emit_state(owner, CBM_UDS_D_STALE_RECLAIMED);
    return 0;
}

int cbm_uds_owner_open(cbm_uds_owner_t *owner, const char *socket_path, int backlog) {
    if (!owner || !socket_path || socket_path[0] == '\0' || backlog <= 0) {
        errno = EINVAL;
        return -1;
    }
    cbm_uds_state_callback_t callback = owner->state_callback;
    void *userdata = owner->state_userdata;
    unsigned long expected_uid = owner->expected_peer_uid;
    owner_reset_runtime(owner);
    owner->state_callback = callback;
    owner->state_userdata = userdata;
    owner->expected_peer_uid =
        expected_uid == (unsigned long)-1 ? (unsigned long)geteuid() : expected_uid;
    emit_state(owner, CBM_UDS_D_STARTING);

    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(owner->socket_path) ||
        path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_private_parent(socket_path) < 0)
        return -1;
    memcpy(owner->socket_path, socket_path, path_len + 1);

    owner->claim_fd = claim_single_owner(socket_path);
    if (owner->claim_fd < 0) {
        if (errno == EADDRINUSE)
            emit_state(owner, CBM_UDS_D_SINGLETON_LOSER);
        owner_reset_runtime(owner);
        owner->state_callback = callback;
        owner->state_userdata = userdata;
        owner->expected_peer_uid = expected_uid;
        return -1;
    }
    emit_state(owner, CBM_UDS_D_BIND_CLAIM);

    if (remove_stale_socket(owner, socket_path) < 0) {
        int saved = errno;
        cbm_uds_owner_close(owner);
        errno = saved;
        return -1;
    }
    owner->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (owner->listen_fd < 0 || set_cloexec(owner->listen_fd) < 0) {
        int saved = errno;
        cbm_uds_owner_close(owner);
        errno = saved;
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    if (bind(owner->listen_fd, (const struct sockaddr *)&addr, addr_len) < 0) {
        int saved = errno;
        cbm_uds_owner_close(owner);
        errno = saved;
        return -1;
    }
    struct stat st;
    if (lstat(socket_path, &st) < 0 || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
        int saved = errno ? errno : EIO;
        cbm_uds_owner_close(owner);
        errno = saved;
        return -1;
    }
    owner->socket_dev = (unsigned long long)st.st_dev;
    owner->socket_ino = (unsigned long long)st.st_ino;
    if (chmod(socket_path, 0600) < 0 || listen(owner->listen_fd, backlog) < 0) {
        int saved = errno;
        cbm_uds_owner_close(owner);
        errno = saved;
        return -1;
    }
    emit_state(owner, CBM_UDS_D_LISTENING);
    return 0;
}

static int peer_uid(int fd, unsigned long *uid_out) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    uid_t uid;
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) < 0)
        return -1;
    *uid_out = (unsigned long)uid;
    return 0;
#elif defined(__linux__)
    struct ucred cred;
    socklen_t len = (socklen_t)sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        return -1;
    *uid_out = (unsigned long)cred.uid;
    return 0;
#else
    (void)fd;
    (void)uid_out;
    errno = ENOTSUP;
    return -1;
#endif
}

int cbm_uds_owner_accept(cbm_uds_owner_t *owner, cbm_uds_connection_t *connection) {
    if (!owner || owner->listen_fd < 0 || !connection) {
        errno = EINVAL;
        return -1;
    }
    connection->fd = -1;
    connection->peer_uid = (unsigned long)-1;
    int fd;
    do {
        fd = accept(owner->listen_fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    unsigned long uid = (unsigned long)-1;
    if (peer_uid(fd, &uid) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (uid != owner->expected_peer_uid) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    connection->fd = fd;
    connection->peer_uid = uid;
    emit_state(owner, CBM_UDS_D_SERVING);
    return 0;
}

void cbm_uds_connection_close(cbm_uds_connection_t *connection) {
    if (!connection)
        return;
    if (connection->fd >= 0)
        close(connection->fd);
    connection->fd = -1;
    connection->peer_uid = (unsigned long)-1;
}

void cbm_uds_owner_close(cbm_uds_owner_t *owner) {
    if (!owner)
        return;
    if (owner->listen_fd >= 0 || owner->claim_fd >= 0)
        emit_state(owner, CBM_UDS_D_DRAINING);
    if (owner->listen_fd >= 0) {
        close(owner->listen_fd);
        owner->listen_fd = -1;
    }
    if (owner->socket_path[0] != '\0' && owner->socket_ino != 0) {
        struct stat st;
        if (lstat(owner->socket_path, &st) == 0 && S_ISSOCK(st.st_mode) &&
            (unsigned long long)st.st_dev == owner->socket_dev &&
            (unsigned long long)st.st_ino == owner->socket_ino)
            (void)unlink(owner->socket_path);
    }
    owner->socket_dev = 0;
    owner->socket_ino = 0;
    owner->socket_path[0] = '\0';
    if (owner->claim_fd >= 0) {
        struct flock lock;
        memset(&lock, 0, sizeof(lock));
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        (void)fcntl(owner->claim_fd, F_SETLK, &lock);
        close(owner->claim_fd);
        owner->claim_fd = -1;
    }
    emit_state(owner, CBM_UDS_D_EXITED);
}

#else

#include <errno.h>
#include <string.h>

const char *cbm_uds_state_name(cbm_uds_state_t state) {
    (void)state;
    return "D_UNSUPPORTED";
}
int cbm_uds_socket_path_resolve(char *out, size_t out_size, const char *override_path) {
    (void)out;
    (void)out_size;
    (void)override_path;
    errno = ENOTSUP;
    return -1;
}
void cbm_uds_owner_configure(cbm_uds_owner_t *owner, unsigned long expected_peer_uid,
                             cbm_uds_state_callback_t callback, void *userdata) {
    if (owner) {
        memset(owner, 0, sizeof(*owner));
        owner->listen_fd = -1;
        owner->claim_fd = -1;
        owner->expected_peer_uid = expected_peer_uid;
        owner->state_callback = callback;
        owner->state_userdata = userdata;
    }
}
int cbm_uds_owner_open(cbm_uds_owner_t *owner, const char *socket_path, int backlog) {
    (void)owner;
    (void)socket_path;
    (void)backlog;
    errno = ENOTSUP;
    return -1;
}
int cbm_uds_owner_accept(cbm_uds_owner_t *owner, cbm_uds_connection_t *connection) {
    (void)owner;
    (void)connection;
    errno = ENOTSUP;
    return -1;
}
void cbm_uds_connection_close(cbm_uds_connection_t *connection) {
    if (connection)
        connection->fd = -1;
}
void cbm_uds_owner_close(cbm_uds_owner_t *owner) {
    if (owner) {
        owner->listen_fd = -1;
        owner->claim_fd = -1;
    }
}

#endif
