#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "test_framework.h"
#include "daemon/uds_lifecycle.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
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

#endif /* !_WIN32 */

SUITE(uds_lifecycle) {
    RUN_TEST(uds_owner_configure_defaults_runtime_handles);
#ifndef _WIN32
    RUN_TEST(uds_state_names);
    RUN_TEST(uds_path_resolve_creates_private_parent);
    RUN_TEST(uds_path_resolve_rejects_public_parent);
    RUN_TEST(uds_owner_open_close_tracks_lifecycle_and_inode);
#endif
}
