#ifndef CBM_DAEMON_SHIM_HANDSHAKE_H
#define CBM_DAEMON_SHIM_HANDSHAKE_H

/*
 * shim_handshake.h — Version/capability handshake shared by the stdio→UDS
 * thin shim (client role, mcp_shim.c) and the daemon's per-connection accept
 * worker (server role, mcp_uds_runner.c). Runs ONCE at the start of every
 * accepted UDS connection, before any MCP JSON-RPC byte is exchanged.
 *
 * Wire format: plain ASCII lines, NOT JSON — this makes the handshake bytes
 * trivially distinguishable from any real MCP JSON-RPC message (which always
 * starts with '{' or "Content-Length:") without needing a parser, and keeps
 * the shim free of any MCP/JSON dependency.
 *
 *   client -> server: "CBM-SHIM-HELLO <version>\n"
 *   server -> client: "CBM-DAEMON-HELLO <version> OK\n"
 *                  or "CBM-DAEMON-HELLO <version> VERSION_MISMATCH\n"
 *
 * Both sides enforce a bounded deadline on every read/write so a wedged peer
 * cannot hang the caller forever (Issue #27: fail-closed, never block
 * indefinitely).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the handshake wire format OR the MCP framing contract it guards
 * changes in an incompatible way. */
#define CBM_SHIM_PROTOCOL_VERSION 1

typedef enum cbm_shim_hs_result {
    CBM_SHIM_HS_OK = 0,
    CBM_SHIM_HS_VERSION_MISMATCH,
    CBM_SHIM_HS_IO_ERROR,  /* short read/write, connection reset mid-handshake */
    CBM_SHIM_HS_TIMEOUT,   /* deadline exceeded waiting for peer bytes */
    CBM_SHIM_HS_MALFORMED, /* bytes received but did not parse as the protocol */
} cbm_shim_hs_result_t;

const char *cbm_shim_hs_result_name(cbm_shim_hs_result_t result);

/* Client role (used by the shim): send the hello line and read back the
 * daemon's verdict. fd is a connected UDS socket. deadline_ms bounds the
 * whole exchange (both the write and the read). */
cbm_shim_hs_result_t cbm_shim_handshake_client(int fd, int deadline_ms);

/* Server role (used by the daemon's accept worker): read the client hello,
 * validate its version, and reply with OK or VERSION_MISMATCH. Returns
 * CBM_SHIM_HS_OK only when the reply was OK; any other return value means
 * the caller must close the connection without proceeding to MCP framing. */
cbm_shim_hs_result_t cbm_shim_handshake_server(int fd, int deadline_ms);

#ifdef __cplusplus
}
#endif

#endif
