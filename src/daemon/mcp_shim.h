#ifndef CBM_DAEMON_MCP_SHIM_H
#define CBM_DAEMON_MCP_SHIM_H

/*
 * mcp_shim.h — stdio -> pathname UDS thin shim (Issue #27).
 *
 * This is the DEFAULT process entry point when codebase-memory-mcp is
 * launched with no subcommand: it presents a standard MCP stdio transport to
 * the host, but never itself builds an MCP server or opens a store/graph. It
 * only connects to an already-running daemon over a pathname UNIX domain
 * socket and forwards raw bytes in both directions.
 *
 * STRUCTURAL GUARANTEE (verified by the "never per-process fallback" e2e
 * proof): this translation unit does not include mcp/mcp.h and does not call
 * cbm_mcp_server_new / cbm_mcp_server_new_with_core / cbm_watcher_new /
 * cbm_http_server_new, under ANY branch, including every failure path below.
 * A daemon-absent, stale-socket, permission, version-mismatch, or midstream
 * loss condition always ends in a structured stderr diagnostic and a
 * non-zero exit — never a local server construction.
 *
 * stdout carries ONLY bytes read verbatim from the daemon's UDS connection
 * (which are, by construction, legal MCP frames written by cbm_mcp_server_run
 * on the daemon side). All shim-owned diagnostics go to stderr.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cbm_shim_exit {
    CBM_SHIM_EXIT_OK = 0,
    CBM_SHIM_EXIT_USAGE = 64,
    CBM_SHIM_EXIT_DAEMON_ABSENT = 71,
    CBM_SHIM_EXIT_STALE_SOCKET = 72,
    CBM_SHIM_EXIT_PERMISSION_DENIED = 73,
    CBM_SHIM_EXIT_VERSION_MISMATCH = 74,
    CBM_SHIM_EXIT_CONNECT_TIMEOUT = 75,
    CBM_SHIM_EXIT_MIDSTREAM_LOST = 76,
    CBM_SHIM_EXIT_HANDSHAKE_ERROR = 77,
} cbm_shim_exit_t;

typedef struct cbm_shim_options {
    const char *socket_path;  /* NULL -> resolve default via cbm_uds_socket_path_resolve */
    int connect_timeout_ms;   /* bounded connect() deadline; <=0 -> default */
    int handshake_timeout_ms; /* bounded handshake deadline; <=0 -> default */
} cbm_shim_options_t;

/* Runs the shim against the given stdio file descriptors (normally 0/1) using
 * a raw fd pair rather than stdin/stdout FILE* so the forwarding loop can
 * poll() both directions without stdio buffering hazards. Blocks until stdin
 * EOF, the daemon closes the connection, or an unrecoverable transport error
 * occurs. Returns one of the cbm_shim_exit_t codes above; never returns a
 * negative value. All diagnostics are written to stderr before returning. */
int cbm_mcp_shim_run(const cbm_shim_options_t *opts, int stdin_fd, int stdout_fd);

#ifdef __cplusplus
}
#endif

#endif
