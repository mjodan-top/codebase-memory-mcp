#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "daemon/mcp_uds_runner.h"

#include "daemon/shim_handshake.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CBM_MCP_UDS_HANDSHAKE_TIMEOUT_MS = 3000,
};

#ifndef _WIN32
#include <unistd.h>
#endif

typedef struct cbm_mcp_uds_task {
    cbm_uds_connection_t connection;
    cbm_mcp_server_t *server;
    int result;
    int self_cleanup;
} cbm_mcp_uds_task_t;

#ifndef _WIN32
static void finish_task(cbm_mcp_uds_task_t *task) {
    cbm_uds_connection_close(&task->connection);
    cbm_mcp_server_free(task->server);
    free(task);
}

static void *run_client_stream(void *arg) {
    cbm_mcp_uds_task_t *task = (cbm_mcp_uds_task_t *)arg;
    int read_fd = task->connection.fd;

    /* Issue #27: negotiate the shim<->daemon version/capability handshake on
     * the raw fd BEFORE any MCP framing begins. A failed/mismatched/timed-out
     * handshake closes the connection here without ever reaching
     * cbm_mcp_server_run — the peer (shim) observes this as a clean close and
     * fails closed on its own side (VERSION_MISMATCH / HANDSHAKE_ERROR exit). */
    cbm_shim_hs_result_t hs = cbm_shim_handshake_server(read_fd, CBM_MCP_UDS_HANDSHAKE_TIMEOUT_MS);
    if (hs != CBM_SHIM_HS_OK) {
        close(read_fd);
        task->connection.fd = -1;
        task->result = -1;
        if (task->self_cleanup)
            finish_task(task);
        return NULL;
    }

    int write_fd = dup(read_fd);
    FILE *in = NULL;
    FILE *out = NULL;

    task->connection.fd = -1;
    if (write_fd < 0) {
        close(read_fd);
        task->result = -1;
        if (task->self_cleanup)
            finish_task(task);
        return NULL;
    }

    in = fdopen(read_fd, "rb");
    out = fdopen(write_fd, "wb");
    if (!in || !out) {
        if (in)
            fclose(in);
        else
            close(read_fd);
        if (out)
            fclose(out);
        else
            close(write_fd);
        task->result = -1;
        if (task->self_cleanup)
            finish_task(task);
        return NULL;
    }

    setvbuf(out, NULL, _IONBF, 0);
    task->result = cbm_mcp_server_run(task->server, in, out);
    fclose(out);
    fclose(in);
    if (task->self_cleanup)
        finish_task(task);
    return NULL;
}

static int accept_start(cbm_uds_owner_t *owner, cbm_mcp_core_t *core, cbm_mcp_uds_worker_t *worker,
                        int detached) {
    cbm_mcp_uds_task_t *task = (cbm_mcp_uds_task_t *)calloc(1, sizeof(*task));
    if (!task)
        return -1;
    task->connection.fd = -1;
    task->self_cleanup = detached;
    task->server = cbm_mcp_server_new_with_core(core);
    if (!task->server) {
        free(task);
        return -1;
    }
    if (cbm_uds_owner_accept(owner, &task->connection) != 0) {
        finish_task(task);
        return -1;
    }

    cbm_thread_t thread;
    if (cbm_thread_create(&thread, 0, run_client_stream, task) != 0) {
        finish_task(task);
        errno = EAGAIN;
        return -1;
    }
    if (detached) {
        if (cbm_thread_detach(&thread) != 0) {
            (void)cbm_thread_join(&thread);
            errno = EAGAIN;
            return -1;
        }
        return 0;
    }

    worker->thread = thread;
    worker->task = task;
    worker->started = 1;
    return 0;
}
#endif

int cbm_mcp_uds_accept_start(cbm_uds_owner_t *owner, cbm_mcp_core_t *core,
                             cbm_mcp_uds_worker_t *worker) {
    if (!owner || !core || !worker) {
        errno = EINVAL;
        return -1;
    }
    memset(worker, 0, sizeof(*worker));

#ifdef _WIN32
    (void)owner;
    (void)core;
    errno = ENOTSUP;
    return -1;
#else
    return accept_start(owner, core, worker, 0);
#endif
}

int cbm_mcp_uds_worker_join(cbm_mcp_uds_worker_t *worker) {
    if (!worker || !worker->started || !worker->task) {
        errno = EINVAL;
        return -1;
    }
    if (cbm_thread_join(&worker->thread) != 0) {
        return -1;
    }
    cbm_mcp_uds_task_t *task = (cbm_mcp_uds_task_t *)worker->task;
    int result = task->result;
    cbm_uds_connection_close(&task->connection);
    cbm_mcp_server_free(task->server);
    free(task);
    memset(worker, 0, sizeof(*worker));
    return result;
}

int cbm_mcp_uds_serve(cbm_uds_owner_t *owner, cbm_mcp_core_t *core,
                      cbm_mcp_uds_should_stop_t should_stop, void *userdata) {
    if (!owner || !core) {
        errno = EINVAL;
        return -1;
    }

#ifdef _WIN32
    (void)should_stop;
    (void)userdata;
    errno = ENOTSUP;
    return -1;
#else
    for (;;) {
        if (should_stop && should_stop(userdata))
            return 0;
        if (accept_start(owner, core, NULL, 1) == 0)
            continue;
        if ((should_stop && should_stop(userdata)) || owner->listen_fd < 0 || errno == EBADF ||
            errno == EINVAL)
            return 0;
        return -1;
    }
#endif
}
