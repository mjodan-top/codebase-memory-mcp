#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "daemon/mcp_uds_runner.h"
#include "test_framework.h"

#ifndef _WIN32
#include <errno.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef _WIN32
static int connect_pathname_uds(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t len = strlen(path);
    if (len >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, path, len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len + 1);
    if (connect(fd, (const struct sockaddr *)&addr, addr_len) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int send_request(FILE *io, const char *json) {
    size_t len = strlen(json);
    return fprintf(io, "Content-Length: %zu\r\n\r\n%s", len, json) > 0 && fflush(io) == 0 ? 0 : -1;
}

static char *read_response(FILE *io) {
    char *line = NULL;
    size_t cap = 0;
    long content_len = -1;
    while (getline(&line, &cap, io) > 0) {
        if (strncmp(line, "Content-Length:", 15) == 0)
            content_len = strtol(line + 15, NULL, 10);
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0)
            break;
    }
    free(line);
    if (content_len <= 0 || content_len > 1024 * 1024)
        return NULL;
    char *body = (char *)malloc((size_t)content_len + 1);
    if (!body)
        return NULL;
    if (fread(body, 1, (size_t)content_len, io) != (size_t)content_len) {
        free(body);
        return NULL;
    }
    body[content_len] = '\0';
    return body;
}

static int round_trip_contains(FILE *in, FILE *out, const char *request, const char *expected) {
    if (send_request(out, request) != 0)
        return -1;
    char *response = read_response(in);
    int ok = response && strstr(response, expected) != NULL;
    free(response);
    return ok ? 0 : -1;
}

static int client_process(const char *path, int ready_fd, int command_fd, int client_id) {
    int fd = connect_pathname_uds(path);
    if (fd < 0)
        return 10;
    int write_fd = dup(fd);
    if (write_fd < 0) {
        close(fd);
        return 11;
    }
    FILE *in = fdopen(fd, "rb");
    FILE *out = fdopen(write_fd, "wb");
    if (!in || !out) {
        if (in)
            fclose(in);
        else
            close(fd);
        if (out)
            fclose(out);
        else
            close(write_fd);
        return 11;
    }
    setvbuf(out, NULL, _IONBF, 0);

    if (round_trip_contains(
            in, out, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
            "\"id\":1") != 0 ||
        send_request(out, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") != 0 ||
        round_trip_contains(
            in, out, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}",
            "query_graph") != 0) {
        fclose(out);
        fclose(in);
        return 12;
    }

    char ready = 'R';
    if (write(ready_fd, &ready, 1) != 1) {
        fclose(out);
        fclose(in);
        return 13;
    }
    char command = 0;
    if (read(command_fd, &command, 1) != 1) {
        fclose(out);
        fclose(in);
        return 14;
    }

    if (client_id == 1) {
        if (command != 'Q' ||
            round_trip_contains(in, out, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}",
                                "\"id\":3") != 0) {
            fclose(out);
            fclose(in);
            return 15;
        }
    } else {
        if (command != 'C' ||
            round_trip_contains(in, out,
                                "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                                "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}",
                                "projects") != 0 ||
            round_trip_contains(in, out, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}",
                                "\"id\":4") != 0) {
            fclose(out);
            fclose(in);
            return 16;
        }
    }
    fclose(out);
    fclose(in);
    return 0;
}

static pid_t spawn_client(const char *path, int ready_pipe[2], int command_pipe[2], int id,
                          const cbm_uds_owner_t *owner) {
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    close(ready_pipe[0]);
    close(command_pipe[1]);
    close(owner->listen_fd);
    close(owner->claim_fd);
    int rc = client_process(path, ready_pipe[1], command_pipe[0], id);
    close(ready_pipe[1]);
    close(command_pipe[0]);
    _exit(rc);
}

static int child_succeeded(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

typedef struct serve_context {
    cbm_uds_owner_t *owner;
    cbm_mcp_core_t *core;
    atomic_int stop;
    int result;
} serve_context_t;

static int serve_should_stop(void *userdata) {
    serve_context_t *context = (serve_context_t *)userdata;
    return atomic_load(&context->stop);
}

static void *serve_thread_main(void *userdata) {
    serve_context_t *context = (serve_context_t *)userdata;
    context->result = cbm_mcp_uds_serve(context->owner, context->core, serve_should_stop, context);
    return NULL;
}

static int run_short_client(const char *path, int request_id) {
    int fd = connect_pathname_uds(path);
    if (fd < 0)
        return -1;
    int write_fd = dup(fd);
    if (write_fd < 0) {
        close(fd);
        return -1;
    }
    FILE *in = fdopen(fd, "rb");
    FILE *out = fdopen(write_fd, "wb");
    if (!in || !out) {
        if (in)
            fclose(in);
        else
            close(fd);
        if (out)
            fclose(out);
        else
            close(write_fd);
        return -1;
    }
    setvbuf(out, NULL, _IONBF, 0);

    char initialize[128];
    char ping[96];
    if (snprintf(initialize, sizeof(initialize),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"initialize\",\"params\":{}}",
                 request_id) <= 0 ||
        snprintf(ping, sizeof(ping), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"ping\"}",
                 request_id + 100) <= 0 ||
        round_trip_contains(in, out, initialize, "\"result\"") != 0 ||
        send_request(out, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") != 0 ||
        round_trip_contains(in, out, ping, "\"result\"") != 0) {
        fclose(out);
        fclose(in);
        return -1;
    }

    fclose(out);
    fclose(in);
    return 0;
}

static void remove_socket_lock(const char *socket_path) {
    char lock_path[114];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path) > 0)
        (void)unlink(lock_path);
}
#endif

TEST(mcp_uds_two_process_clients_share_core_but_isolate_sessions) {
#ifdef _WIN32
    SKIP();
#else
    char dir_template[] = "/tmp/cbm-mcp-runner-XXXXXX";
    char *dir = mkdtemp(dir_template);
    ASSERT_NOT_NULL(dir);
    char socket_path[108];
    ASSERT_TRUE(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", dir) > 0);

    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, (unsigned long)-1, NULL, NULL);
    ASSERT_EQ(cbm_uds_owner_open(&owner, socket_path, 4), 0);
    cbm_mcp_core_t *core = cbm_mcp_core_new(NULL);
    ASSERT_NOT_NULL(core);

    int ready_a[2], command_a[2], ready_b[2], command_b[2];
    ASSERT_EQ(pipe(ready_a), 0);
    ASSERT_EQ(pipe(command_a), 0);
    ASSERT_EQ(pipe(ready_b), 0);
    ASSERT_EQ(pipe(command_b), 0);

    pid_t child_a = spawn_client(socket_path, ready_a, command_a, 1, &owner);
    ASSERT_TRUE(child_a > 0);
    close(ready_a[1]);
    close(command_a[0]);
    cbm_mcp_uds_worker_t worker_a;
    ASSERT_EQ(cbm_mcp_uds_accept_start(&owner, core, &worker_a), 0);

    pid_t child_b = spawn_client(socket_path, ready_b, command_b, 2, &owner);
    ASSERT_TRUE(child_b > 0);
    close(ready_b[1]);
    close(command_b[0]);
    cbm_mcp_uds_worker_t worker_b;
    ASSERT_EQ(cbm_mcp_uds_accept_start(&owner, core, &worker_b), 0);

    char ready = 0;
    ASSERT_EQ(read(ready_a[0], &ready, 1), 1);
    ASSERT_EQ(ready, 'R');
    ASSERT_EQ(read(ready_b[0], &ready, 1), 1);
    ASSERT_EQ(ready, 'R');

    ASSERT_EQ(write(command_a[1], "Q", 1), 1);
    ASSERT_TRUE(child_succeeded(child_a));
    ASSERT_EQ(cbm_mcp_uds_worker_join(&worker_a), 0);

    /* B remains live after A's full session/server release and can still use
     * the shared process core for a real store-backed MCP tool call. */
    ASSERT_EQ(write(command_b[1], "C", 1), 1);
    ASSERT_TRUE(child_succeeded(child_b));
    ASSERT_EQ(cbm_mcp_uds_worker_join(&worker_b), 0);

    close(ready_a[0]);
    close(command_a[1]);
    close(ready_b[0]);
    close(command_b[1]);
    cbm_mcp_core_free(core);
    cbm_uds_owner_close(&owner);
    char lock_path[114];
    ASSERT_LT(snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path),
              (int)sizeof(lock_path));
    ASSERT_EQ(unlink(lock_path), 0);
    ASSERT_EQ(rmdir(dir), 0);
    PASS();
#endif
}

TEST(mcp_uds_serve_detaches_and_reclaims_completed_sessions) {
#ifdef _WIN32
    SKIP_PLATFORM("pathname UDS runner is Unix-only");
#else
    char dir_template[] = "/tmp/cbm-mcp-serve-XXXXXX";
    char *dir = mkdtemp(dir_template);
    ASSERT_NOT_NULL(dir);
    char socket_path[108];
    ASSERT_TRUE(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", dir) > 0);

    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, (unsigned long)-1, NULL, NULL);
    ASSERT_EQ(cbm_uds_owner_open(&owner, socket_path, 16), 0);
    cbm_mcp_core_t *core = cbm_mcp_core_new(NULL);
    ASSERT_NOT_NULL(core);

    serve_context_t context = {.owner = &owner, .core = core, .result = -1};
    atomic_init(&context.stop, 0);
    cbm_thread_t serve_thread;
    ASSERT_EQ(cbm_thread_create(&serve_thread, 0, serve_thread_main, &context), 0);

    for (int i = 1; i <= 16; i++)
        ASSERT_EQ(run_short_client(socket_path, i), 0);

    atomic_store(&context.stop, 1);
    int wake_fd = connect_pathname_uds(socket_path);
    ASSERT_TRUE(wake_fd >= 0);
    close(wake_fd);
    ASSERT_EQ(cbm_thread_join(&serve_thread), 0);
    ASSERT_EQ(context.result, 0);

    cbm_mcp_core_free(core);
    cbm_uds_owner_close(&owner);
    remove_socket_lock(socket_path);
    ASSERT_EQ(rmdir(dir), 0);
    PASS();
#endif
}

SUITE(mcp_uds_runner) {
    RUN_TEST(mcp_uds_two_process_clients_share_core_but_isolate_sessions);
    RUN_TEST(mcp_uds_serve_detaches_and_reclaims_completed_sessions);
}

#ifdef CBM_MCP_UDS_RUNNER_MAIN
int tf_pass_count = 0;
int tf_fail_count = 0;
int tf_skip_count = 0;

int main(void) {
    RUN_SUITE(mcp_uds_runner);
    TEST_SUMMARY();
}
#endif
