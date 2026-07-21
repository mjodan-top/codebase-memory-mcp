#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "test_framework.h"
#include "daemon/systemd_activation.h"
#include "daemon/uds_lifecycle.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifndef _WIN32

TEST(uds_state_names) {
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_STARTING), "D_STARTING");
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_BIND_CLAIM), "D_BIND_CLAIM");
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_LISTENING), "D_LISTENING");
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_SERVING), "D_SERVING");
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_SINGLETON_LOSER), "D_SINGLETON_LOSER");
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_STALE_RECLAIMED), "D_STALE_RECLAIMED");
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_DRAINING), "D_DRAINING");
    ASSERT_STR_EQ(cbm_uds_state_name(CBM_UDS_D_EXITED), "D_EXITED");
    PASS();
}

TEST(uds_owner_configure_defaults_runtime_handles) {
    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, (unsigned long)-1, NULL, NULL);
    ASSERT_EQ(owner.listen_fd, -1);
    ASSERT_EQ(owner.claim_fd, -1);
    ASSERT_EQ(owner.socket_path[0], '\0');
    ASSERT_EQ(owner.expected_peer_uid, (unsigned long)-1);
    PASS();
}

typedef struct state_trace {
    cbm_uds_state_t states[8];
    size_t count;
} state_trace_t;

static void record_state(cbm_uds_state_t state, void *userdata) {
    state_trace_t *trace = (state_trace_t *)userdata;
    if (trace->count < sizeof(trace->states) / sizeof(trace->states[0]))
        trace->states[trace->count++] = state;
}

static int make_case_dir(char *path, size_t path_size) {
    const char *pattern = "/tmp/cbm_uds_unit.XXXXXX";
    if (strlen(pattern) + 1 > path_size)
        return -1;
    memcpy(path, pattern, strlen(pattern) + 1);
    return mkdtemp(path) ? 0 : -1;
}

TEST(uds_path_resolve_creates_private_parent) {
    char root[64];
    ASSERT_EQ(make_case_dir(root, sizeof(root)), 0);
    ASSERT_EQ(chmod(root, 0700), 0);

    char requested[108];
    char resolved[108];
    ASSERT_LT(snprintf(requested, sizeof(requested), "%s/runtime/daemon.sock", root),
              (int)sizeof(requested));
    ASSERT_EQ(cbm_uds_socket_path_resolve(resolved, sizeof(resolved), requested), 0);
    ASSERT_STR_EQ(resolved, requested);

    char parent[108];
    ASSERT_LT(snprintf(parent, sizeof(parent), "%s/runtime", root), (int)sizeof(parent));
    struct stat st;
    ASSERT_EQ(lstat(parent, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_EQ(st.st_mode & 0077, 0);
    ASSERT_EQ(st.st_uid, geteuid());

    ASSERT_EQ(rmdir(parent), 0);
    ASSERT_EQ(rmdir(root), 0);
    PASS();
}

TEST(uds_path_resolve_rejects_public_parent) {
    char root[64];
    ASSERT_EQ(make_case_dir(root, sizeof(root)), 0);
    ASSERT_EQ(chmod(root, 0755), 0);

    char requested[108];
    char resolved[108];
    ASSERT_LT(snprintf(requested, sizeof(requested), "%s/daemon.sock", root),
              (int)sizeof(requested));
    errno = 0;
    ASSERT_EQ(cbm_uds_socket_path_resolve(resolved, sizeof(resolved), requested), -1);
    ASSERT_EQ(errno, EPERM);

    ASSERT_EQ(chmod(root, 0700), 0);
    ASSERT_EQ(rmdir(root), 0);
    PASS();
}

TEST(uds_owner_open_close_tracks_lifecycle_and_inode) {
    char root[64];
    ASSERT_EQ(make_case_dir(root, sizeof(root)), 0);
    ASSERT_EQ(chmod(root, 0700), 0);

    char socket_path[108];
    ASSERT_LT(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", root),
              (int)sizeof(socket_path));

    state_trace_t trace = {0};
    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, (unsigned long)-1, record_state, &trace);
    ASSERT_EQ(cbm_uds_owner_open(&owner, socket_path, 4), 0);
    ASSERT_EQ(trace.count, 3);
    ASSERT_EQ(trace.states[0], CBM_UDS_D_STARTING);
    ASSERT_EQ(trace.states[1], CBM_UDS_D_BIND_CLAIM);
    ASSERT_EQ(trace.states[2], CBM_UDS_D_LISTENING);

    struct stat st;
    ASSERT_EQ(lstat(socket_path, &st), 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));
    ASSERT_EQ(st.st_mode & 0077, 0);
    ASSERT_NEQ(owner.socket_ino, 0);

    cbm_uds_owner_close(&owner);
    ASSERT_EQ(trace.count, 5);
    ASSERT_EQ(trace.states[3], CBM_UDS_D_DRAINING);
    ASSERT_EQ(trace.states[4], CBM_UDS_D_EXITED);
    ASSERT_EQ(lstat(socket_path, &st), -1);
    ASSERT_EQ(errno, ENOENT);

    char lock_path[114];
    ASSERT_LT(snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path),
              (int)sizeof(lock_path));
    ASSERT_EQ(unlink(lock_path), 0);
    ASSERT_EQ(rmdir(root), 0);
    PASS();
}

/* Issue #29: adopting a manager-bound listening fd (launchd socket
 * activation shape) must enter the normal lifecycle, serve real accepts, and
 * must NOT unlink the manager-owned socket inode on close. */
TEST(uds_owner_adopt_inherited_listener_serves_and_preserves_inode) {
    char root[64];
    ASSERT_EQ(make_case_dir(root, sizeof(root)), 0);
    ASSERT_EQ(chmod(root, 0700), 0);

    char socket_path[108];
    ASSERT_LT(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", root),
              (int)sizeof(socket_path));

    /* Play the service manager: bind + listen before the "daemon" runs. */
    int manager_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(manager_fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);
    socklen_t addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(socket_path) + 1);
    ASSERT_EQ(bind(manager_fd, (const struct sockaddr *)&addr, addr_len), 0);
    ASSERT_EQ(chmod(socket_path, 0600), 0);
    ASSERT_EQ(listen(manager_fd, 4), 0);

    state_trace_t trace = {0};
    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, (unsigned long)-1, record_state, &trace);
    ASSERT_EQ(cbm_uds_owner_adopt(&owner, manager_fd, socket_path), 0);
    ASSERT_EQ(owner.listen_fd, manager_fd);
    ASSERT_EQ(trace.count, 3);
    ASSERT_EQ(trace.states[0], CBM_UDS_D_STARTING);
    ASSERT_EQ(trace.states[1], CBM_UDS_D_BIND_CLAIM);
    ASSERT_EQ(trace.states[2], CBM_UDS_D_LISTENING);
    /* Manager owns the inode: adopt must not record it for unlink-on-close. */
    ASSERT_EQ(owner.socket_ino, 0);

    /* A real client connect must be accepted through the adopted fd. */
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(client_fd >= 0);
    ASSERT_EQ(connect(client_fd, (const struct sockaddr *)&addr, addr_len), 0);
    cbm_uds_connection_t conn;
    ASSERT_EQ(cbm_uds_owner_accept(&owner, &conn), 0);
    ASSERT_EQ(conn.peer_uid, (unsigned long)geteuid());
    cbm_uds_connection_close(&conn);
    close(client_fd);

    cbm_uds_owner_close(&owner);
    /* Socket inode survives close: launchd needs it for re-activation. */
    struct stat st;
    ASSERT_EQ(lstat(socket_path, &st), 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));

    ASSERT_EQ(unlink(socket_path), 0);
    char lock_path[114];
    ASSERT_LT(snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path),
              (int)sizeof(lock_path));
    ASSERT_EQ(unlink(lock_path), 0);
    ASSERT_EQ(rmdir(root), 0);
    PASS();
}

/* Adopt must refuse fds that are not listening AF_UNIX sockets. */
TEST(uds_owner_adopt_rejects_non_listening_fd) {
    char root[64];
    ASSERT_EQ(make_case_dir(root, sizeof(root)), 0);
    ASSERT_EQ(chmod(root, 0700), 0);

    char socket_path[108];
    ASSERT_LT(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", root),
              (int)sizeof(socket_path));

    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, (unsigned long)-1, NULL, NULL);

    /* Not a socket at all. */
    errno = 0;
    ASSERT_EQ(cbm_uds_owner_adopt(&owner, STDIN_FILENO, socket_path), -1);
    ASSERT_TRUE(errno != 0);

    /* AF_UNIX socket that never listen()ed. */
    int plain_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(plain_fd >= 0);
    errno = 0;
    ASSERT_EQ(cbm_uds_owner_adopt(&owner, plain_fd, socket_path), -1);
    ASSERT_EQ(errno, EINVAL);
    close(plain_fd);

    ASSERT_EQ(rmdir(root), 0);
    PASS();
}

/* Issue #29: sd_listen_fds environment contract (hand-parsed, no
 * libsystemd). PV ref:
 * https://www.freedesktop.org/software/systemd/man/latest/sd_listen_fds.html */
TEST(systemd_parse_rejects_pid_mismatch) {
    /* fds addressed to another PID are not ours: refuse (ESRCH). */
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds("12345", "1", 999L), -1);
    ASSERT_EQ(errno, ESRCH);
    /* Malformed PID is equally "not for us". */
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds("12x45", "1", 12345L), -1);
    ASSERT_EQ(errno, ESRCH);
    PASS();
}

TEST(systemd_parse_rejects_missing_environment) {
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds(NULL, "1", 100L), -1);
    ASSERT_EQ(errno, ESRCH);
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds("100", NULL, 100L), -1);
    ASSERT_EQ(errno, ESRCH);
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds("", "", 100L), -1);
    ASSERT_EQ(errno, ESRCH);
    PASS();
}

TEST(systemd_parse_rejects_fd_count_not_exactly_one) {
    /* Zero fds, multiple fds, and garbage all violate the single-listener
     * contract: EINVAL, never a fabricated fd. */
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds("100", "0", 100L), -1);
    ASSERT_EQ(errno, EINVAL);
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds("100", "2", 100L), -1);
    ASSERT_EQ(errno, EINVAL);
    errno = 0;
    ASSERT_EQ(cbm_systemd_parse_listen_fds("100", "one", 100L), -1);
    ASSERT_EQ(errno, EINVAL);
    PASS();
}

TEST(systemd_parse_accepts_exact_contract) {
    ASSERT_EQ(cbm_systemd_parse_listen_fds("4242", "1", 4242L), CBM_SD_LISTEN_FDS_START);
    PASS();
}

#ifdef __linux__
/* Linux-only: full systemd receive path — env vars + inherited listening fd
 * at SD_LISTEN_FDS_START, SO_ACCEPTCONN listener check, adoption into the
 * lifecycle seam, real accept, and inode preservation on close. */
TEST(systemd_activation_receives_adopts_and_preserves_inode) {
    char root[64];
    ASSERT_EQ(make_case_dir(root, sizeof(root)), 0);
    ASSERT_EQ(chmod(root, 0700), 0);

    char socket_path[108];
    ASSERT_LT(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", root),
              (int)sizeof(socket_path));

    /* Play systemd: bind + listen, then park the fd at SD_LISTEN_FDS_START
     * and publish the sd_listen_fds environment addressed to us. */
    int manager_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(manager_fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);
    socklen_t addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(socket_path) + 1);
    ASSERT_EQ(bind(manager_fd, (const struct sockaddr *)&addr, addr_len), 0);
    ASSERT_EQ(chmod(socket_path, 0600), 0);
    ASSERT_EQ(listen(manager_fd, 4), 0);
    ASSERT_EQ(dup2(manager_fd, CBM_SD_LISTEN_FDS_START), CBM_SD_LISTEN_FDS_START);
    close(manager_fd);

    char pid_str[32];
    ASSERT_LT(snprintf(pid_str, sizeof(pid_str), "%ld", (long)getpid()), (int)sizeof(pid_str));
    ASSERT_EQ(setenv("LISTEN_PID", pid_str, 1), 0);
    ASSERT_EQ(setenv("LISTEN_FDS", "1", 1), 0);

    int fd = cbm_systemd_activation_fd();
    ASSERT_EQ(fd, CBM_SD_LISTEN_FDS_START);
    /* Consume-once: the activation environment must be gone. */
    ASSERT_TRUE(getenv("LISTEN_PID") == NULL);
    ASSERT_TRUE(getenv("LISTEN_FDS") == NULL);

    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, (unsigned long)-1, NULL, NULL);
    ASSERT_EQ(cbm_uds_owner_adopt(&owner, fd, socket_path), 0);
    /* Manager owns the inode: adopt must not record it for unlink-on-close. */
    ASSERT_EQ(owner.socket_ino, 0);

    /* A real client connect must be accepted through the inherited fd. */
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(client_fd >= 0);
    ASSERT_EQ(connect(client_fd, (const struct sockaddr *)&addr, addr_len), 0);
    cbm_uds_connection_t conn;
    ASSERT_EQ(cbm_uds_owner_accept(&owner, &conn), 0);
    ASSERT_EQ(conn.peer_uid, (unsigned long)geteuid());
    cbm_uds_connection_close(&conn);
    close(client_fd);

    cbm_uds_owner_close(&owner);
    /* Socket inode survives close: systemd needs it for re-activation. */
    struct stat st;
    ASSERT_EQ(lstat(socket_path, &st), 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));

    ASSERT_EQ(unlink(socket_path), 0);
    char lock_path[114];
    ASSERT_LT(snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path),
              (int)sizeof(lock_path));
    ASSERT_EQ(unlink(lock_path), 0);
    ASSERT_EQ(rmdir(root), 0);
    PASS();
}

/* Linux-only: a bound-but-not-listening inherited fd must be refused via
 * SO_ACCEPTCONN (never silently listen() on the manager's behalf). */
TEST(systemd_activation_rejects_non_listening_inherited_fd) {
    char root[64];
    ASSERT_EQ(make_case_dir(root, sizeof(root)), 0);
    ASSERT_EQ(chmod(root, 0700), 0);

    char socket_path[108];
    ASSERT_LT(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", root),
              (int)sizeof(socket_path));

    int bound_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(bound_fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);
    socklen_t addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(socket_path) + 1);
    ASSERT_EQ(bind(bound_fd, (const struct sockaddr *)&addr, addr_len), 0);
    ASSERT_EQ(dup2(bound_fd, CBM_SD_LISTEN_FDS_START), CBM_SD_LISTEN_FDS_START);
    close(bound_fd);

    char pid_str[32];
    ASSERT_LT(snprintf(pid_str, sizeof(pid_str), "%ld", (long)getpid()), (int)sizeof(pid_str));
    ASSERT_EQ(setenv("LISTEN_PID", pid_str, 1), 0);
    ASSERT_EQ(setenv("LISTEN_FDS", "1", 1), 0);

    errno = 0;
    ASSERT_EQ(cbm_systemd_activation_fd(), -1);
    ASSERT_EQ(errno, EINVAL);

    close(CBM_SD_LISTEN_FDS_START);
    ASSERT_EQ(unlink(socket_path), 0);
    ASSERT_EQ(rmdir(root), 0);
    PASS();
}
#else
/* Non-Linux: the full activation path must fail closed with ENOTSUP even if
 * a plausible sd_listen_fds environment is present. */
TEST(systemd_activation_fails_closed_off_linux) {
    char pid_str[32];
    ASSERT_LT(snprintf(pid_str, sizeof(pid_str), "%ld", (long)getpid()), (int)sizeof(pid_str));
    ASSERT_EQ(setenv("LISTEN_PID", pid_str, 1), 0);
    ASSERT_EQ(setenv("LISTEN_FDS", "1", 1), 0);
    errno = 0;
    ASSERT_EQ(cbm_systemd_activation_fd(), -1);
    ASSERT_EQ(errno, ENOTSUP);
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    PASS();
}
#endif /* __linux__ */

#endif /* !_WIN32 */

SUITE(uds_lifecycle) {
    RUN_TEST(uds_owner_configure_defaults_runtime_handles);
#ifndef _WIN32
    RUN_TEST(uds_state_names);
    RUN_TEST(uds_path_resolve_creates_private_parent);
    RUN_TEST(uds_path_resolve_rejects_public_parent);
    RUN_TEST(uds_owner_open_close_tracks_lifecycle_and_inode);
    RUN_TEST(uds_owner_adopt_inherited_listener_serves_and_preserves_inode);
    RUN_TEST(uds_owner_adopt_rejects_non_listening_fd);
    RUN_TEST(systemd_parse_rejects_pid_mismatch);
    RUN_TEST(systemd_parse_rejects_missing_environment);
    RUN_TEST(systemd_parse_rejects_fd_count_not_exactly_one);
    RUN_TEST(systemd_parse_accepts_exact_contract);
#ifdef __linux__
    RUN_TEST(systemd_activation_receives_adopts_and_preserves_inode);
    RUN_TEST(systemd_activation_rejects_non_listening_inherited_fd);
#else
    RUN_TEST(systemd_activation_fails_closed_off_linux);
#endif
#endif
}
