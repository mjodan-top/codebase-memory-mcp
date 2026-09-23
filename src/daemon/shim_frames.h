#ifndef CBM_DAEMON_SHIM_FRAMES_H
#define CBM_DAEMON_SHIM_FRAMES_H

/*
 * shim_frames.h — the MINIMAL MCP frame awareness the stdio->UDS shim needs to
 * survive a daemon restart (reconnect + replay + in-flight error).
 *
 * The shim still forwards every frame's bytes verbatim; it only needs to know
 * where a frame ends and, for each complete frame, four facts: is it a request,
 * a notification or a response, what is its raw JSON id, and is it
 * "initialize" / "notifications/initialized". Framing mirrors what the daemon
 * reader (cbm_mcp_server_run in src/mcp/mcp.c) accepts:
 *
 *   - newline-delimited JSON: one message per line ('\n', optional '\r');
 *   - LSP-style: a first line starting with "Content-Length:", further header
 *     lines until an empty line, then exactly Content-Length body bytes.
 *
 * Deliberately does NOT include mcp/mcp.h (the shim's structural guarantee).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CBM_SHIM_FRAME_ID_MAX = 128,              /* raw JSON id text, incl. NUL */
    CBM_SHIM_FRAME_HEADER_MAX = 8192,         /* Content-Length header block cap */
    CBM_SHIM_FRAME_MAX = 32 * 1024 * 1024,    /* larger frames -> raw passthrough */
};

typedef struct cbm_shim_frame {
    size_t total;       /* bytes of the whole frame incl. framing */
    size_t body_off;    /* offset of the JSON body inside the frame */
    size_t body_len;    /* JSON body length (trailing CR/LF trimmed in line mode) */
    int content_length; /* 1 = Content-Length framed, 0 = newline framed */
} cbm_shim_frame_t;

/* Scan buf[0..len) for one complete frame. Returns 1 and fills *out when a
 * complete frame is present, 0 when more bytes are needed, -1 when the stream
 * cannot be framed within the caps above (caller should fall back to raw
 * byte relay). */
int cbm_shim_frame_scan(const char *buf, size_t len, size_t max_frame, cbm_shim_frame_t *out);

typedef enum cbm_shim_msg_kind {
    CBM_SHIM_MSG_OTHER = 0,     /* not JSON / not a JSON-RPC object / batch */
    CBM_SHIM_MSG_REQUEST,       /* method + id */
    CBM_SHIM_MSG_NOTIFICATION,  /* method, no id */
    CBM_SHIM_MSG_RESPONSE,      /* id, no method (result or error) */
} cbm_shim_msg_kind_t;

typedef struct cbm_shim_msg {
    cbm_shim_msg_kind_t kind;
    int is_initialize;             /* request "initialize" */
    int is_initialized;            /* notification "notifications/initialized" */
    char id[CBM_SHIM_FRAME_ID_MAX]; /* raw JSON of id ("7", "\"abc\""), "" if none */
} cbm_shim_msg_t;

/* Classify one JSON body. Never fails: unparsable input yields MSG_OTHER. */
void cbm_shim_msg_inspect(const char *body, size_t len, cbm_shim_msg_t *out);

/* Format the JSON-RPC error the shim returns to the host for a request that
 * was in flight when the daemon connection dropped. id_json is the raw JSON id
 * (as captured by cbm_shim_msg_inspect); framing follows content_length.
 * Returns the frame length written (NUL-terminated), or -1 if out is too small. */
int cbm_shim_format_lost_error(char *out, size_t cap, const char *id_json, int content_length);

#define CBM_SHIM_LOST_ERROR_CODE (-32000)
#define CBM_SHIM_LOST_ERROR_MESSAGE                                                          \
    "codebase-memory daemon restarted while this request was in flight; it was not "      \
    "completed. Please retry the call."

#ifdef __cplusplus
}
#endif

#endif
