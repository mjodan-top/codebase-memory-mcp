#ifndef CBM_DAEMON_MCP_UDS_RUNNER_H
#define CBM_DAEMON_MCP_UDS_RUNNER_H

#include "daemon/uds_lifecycle.h"
#include "foundation/compat_thread.h"
#include "mcp/mcp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*cbm_mcp_uds_should_stop_t)(void *userdata);

typedef struct cbm_mcp_uds_worker {
    cbm_thread_t thread;
    void *task;
    int started;
} cbm_mcp_uds_worker_t;

/* Accept one pathname-UDS client, give it an isolated MCP session, and run it
 * asynchronously against the shared process core. The worker owns the accepted
 * connection until cbm_mcp_uds_worker_join() completes. */
int cbm_mcp_uds_accept_start(cbm_uds_owner_t *owner, cbm_mcp_core_t *core,
                             cbm_mcp_uds_worker_t *worker);

/* Wait for one accepted client stream to finish. Returns the MCP stream result. */
int cbm_mcp_uds_worker_join(cbm_mcp_uds_worker_t *worker);

/* Production daemon accept loop. Every accepted pathname-UDS connection gets a
 * detached, self-cleaning MCP server/session backed by the shared process core.
 * The caller stops the blocking loop by making should_stop true and closing
 * owner->listen_fd (normally from its signal/shutdown path). Active sessions
 * retain the core independently until their client streams close. */
int cbm_mcp_uds_serve(cbm_uds_owner_t *owner, cbm_mcp_core_t *core,
                      cbm_mcp_uds_should_stop_t should_stop, void *userdata);

#ifdef __cplusplus
}
#endif

#endif
