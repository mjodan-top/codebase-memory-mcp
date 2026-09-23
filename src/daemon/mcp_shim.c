#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "daemon/mcp_shim.h"

#include "daemon/shim_frames.h"
#include "daemon/shim_handshake.h"
#include "daemon/uds_lifecycle.h"

/* Path/dir helpers only (no MCP server, store, or graph): the structural
 * guarantee documented in mcp_shim.h stays intact — see the journal block
 * below for why the cache-dir convention is reused instead of re-derived. */
#include "foundation/compat_fs.h" /* cbm_mkdir_p */
#include "foundation/platform.h"  /* cbm_resolve_cache_dir */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    CBM_SHIM_DEFAULT_CONNECT_TIMEOUT_MS = 3000,
    CBM_SHIM_DEFAULT_HANDSHAKE_TIMEOUT_MS = 3000,
    CBM_SHIM_RELAY_BUF_SIZE = 65536,
};

/* Last diagnostic emitted by this process, remembered so the connect-outcome
 * journal below can name the failure WITHOUT duplicating the state string
 * literals at a second site: every state name has exactly one definition
 * point, namely its diag()/diag_errno() call. The shim's connect path is
 * single-threaded (the relay thread-free poll loop runs after it), so plain
 * file-static storage is sufficient. */
static const char *g_last_diag_state = NULL;
static const char *g_last_diag_detail = NULL;
static int g_last_diag_errno = 0;
/* Set while silently retrying a reconnect: state is still recorded for the
 * journal, but each failed attempt does not print its own stderr line. */
static int g_diag_quiet = 0;

/* All diagnostics go to stderr, one line, machine-greppable
 * ("shim.<state>=<detail>"), never touching stdout (MCP transport). */
static void diag(const char *state, const char *detail_key, const char *detail_val) {
    g_last_diag_state = state;
    g_last_diag_detail = detail_val;
    g_last_diag_errno = 0;
    if (g_diag_quiet) {
        return;
    }
    if (detail_val) {
        (void)fprintf(stderr, "codebase-memory-mcp: shim.%s %s=%s\n", state, detail_key,
                      detail_val);
    } else {
        (void)fprintf(stderr, "codebase-memory-mcp: shim.%s\n", state);
    }
}

static void diag_errno(const char *state, int err) {
    g_last_diag_state = state;
    g_last_diag_detail = NULL;
    g_last_diag_errno = err;
    if (g_diag_quiet) {
        return;
    }
    (void)fprintf(stderr, "codebase-memory-mcp: shim.%s errno=%d error=%s\n", state, err,
                  strerror(err));
}

/* ── Connect-outcome journal (persistent + mechanically aggregatable) ─────
 *
 * WHY THIS EXISTS (2026-09-07 incident): the daemon was killed by SIGPIPE and
 * stayed dead for ~30 hours. Every new session's shim correctly failed closed,
 * but its ONLY trace was one stderr line inside that session (swallowed by the
 * host agent), while the daemon's own log simply stopped growing. Nothing
 * persistent could answer "how many sessions lost the daemon, starting when,
 * for what reason" — the failure was 100% reproducible yet invisible.
 *
 * So every shim start appends exactly one line here, for BOTH outcomes: a
 * failure line alone cannot yield a rate, and AGENTS.md §5 requires the
 * degraded and healthy sides to be equally countable. This machine
 * deliberately runs no active alerting; the log IS the discovery mechanism, so
 * the format is stable key=value (matching foundation/log.h) and one event per
 * line, countable by event name over any time window.
 *
 * Fail-open by construction: a journal that cannot be written must never
 * change the shim's exit code, its stdout (MCP transport), or its errno.
 *
 * Not cbm_log(): that writes to stderr and would need a global sink callback
 * to reach a file — extra global state and a tee of unrelated lines in a role
 * that deliberately initializes nothing. A single write() to an O_APPEND fd is
 * also atomic for short lines, so concurrent shim processes (one per session)
 * never interleave. The cache-dir + /logs location is NOT re-derived here: it
 * reuses cbm_resolve_cache_dir(), the same authority mcp.c and
 * index_supervisor.c use for their logs, so CBM_CACHE_DIR keeps moving all of
 * them together. */
enum {
    CBM_SHIM_JOURNAL_LINE_MAX = 512,
    CBM_SHIM_JOURNAL_PATH_MAX = 1024,
};

/* Resolve the journal file path. CBM_SHIM_LOG overrides it (same convention as
 * CBM_INDEX_LOG); CBM_SHIM_LOG=off|0|none disables journaling entirely.
 * Returns 0 on success, -1 when disabled or unresolvable (caller stays silent). */
static int shim_journal_path(char *out, size_t out_size) {
    const char *override = getenv("CBM_SHIM_LOG");
    if (override && override[0]) {
        if (strcmp(override, "off") == 0 || strcmp(override, "0") == 0 ||
            strcmp(override, "none") == 0) {
            return -1;
        }
        int n = snprintf(out, out_size, "%s", override);
        return (n > 0 && (size_t)n < out_size) ? 0 : -1;
    }
    const char *cdir = cbm_resolve_cache_dir();
    if (!cdir || !cdir[0]) {
        return -1;
    }
    char dir[CBM_SHIM_JOURNAL_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/logs", cdir);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        return -1;
    }
    (void)cbm_mkdir_p(dir, 0755);
    n = snprintf(out, out_size, "%s/shim.log", dir);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

static void shim_journal_utc_now(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm tmv;
    if (gmtime_r(&now, &tmv) && strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tmv) > 0) {
        return;
    }
    (void)snprintf(out, out_size, "unknown");
}

/* Append one key=value line describing this shim's connect outcome. extra
 * (may be NULL) is appended verbatim as additional key=value pairs. */
static void shim_journal_ex(const char *event, const char *socket_path, const char *extra) {
    const int saved_errno = errno; /* never perturb the caller's errno */
    char path[CBM_SHIM_JOURNAL_PATH_MAX];
    if (shim_journal_path(path, sizeof(path)) != 0) {
        errno = saved_errno;
        return;
    }

    char ts[32];
    shim_journal_utc_now(ts, sizeof(ts));

    /* No diag() ran => nothing went wrong on the way in. */
    const char *state = g_last_diag_state ? g_last_diag_state : "attached";
    const char *level = g_last_diag_state ? "warn" : "info";
    char line[CBM_SHIM_JOURNAL_LINE_MAX];
    int n = snprintf(line, sizeof(line), "ts=%s level=%s msg=%s state=%s", ts, level, event, state);
    if (n > 0 && (size_t)n < sizeof(line) && g_last_diag_errno != 0) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " errno=%d error=%s", g_last_diag_errno,
                      strerror(g_last_diag_errno));
    }
    if (n > 0 && (size_t)n < sizeof(line) && g_last_diag_detail) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " detail=%s", g_last_diag_detail);
    }
    if (n > 0 && (size_t)n < sizeof(line) && extra && extra[0]) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", extra);
    }
    if (n > 0 && (size_t)n < sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " socket=%s pid=%d",
                      socket_path ? socket_path : "-", (int)getpid());
    }
    if (n < 0) {
        errno = saved_errno;
        return;
    }
    /* Bounded line length: on truncation still terminate with a newline, so a
     * long socket path can never fuse two records into one unparsable line. */
    size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 2;
    line[len] = '\n';
    len++;

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        errno = saved_errno;
        return;
    }
    ssize_t w;
    do {
        w = write(fd, line, len);
    } while (w < 0 && errno == EINTR);
    (void)w;
    (void)close(fd);
    errno = saved_errno;
}

static void shim_journal(const char *event, const char *socket_path) {
    shim_journal_ex(event, socket_path, NULL);
}

static long long shim_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Connect to socket_path with a bounded deadline. Returns the connected fd on
 * success, or -1 with *out_exit set to the appropriate fail-closed exit code
 * (daemon absent / stale socket / permission denied / connect timeout /
 * generic io error) on failure. Never blocks past timeout_ms. */
static int shim_connect(const char *socket_path, int timeout_ms, cbm_shim_exit_t *out_exit) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        diag_errno("connect.socket_failed", errno);
        *out_exit = CBM_SHIM_EXIT_HANDSHAKE_ERROR;
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        diag_errno("connect.nonblock_failed", errno);
        close(fd);
        *out_exit = CBM_SHIM_EXIT_HANDSHAKE_ERROR;
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        diag("connect.path_too_long", "path", socket_path);
        close(fd);
        *out_exit = CBM_SHIM_EXIT_USAGE;
        return -1;
    }
    memcpy(addr.sun_path, socket_path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);

    long long deadline_at = shim_now_ms() + timeout_ms;
    int rc = connect(fd, (const struct sockaddr *)&addr, addr_len);
    if (rc < 0 && errno == EINPROGRESS) {
        for (;;) {
            long long remaining = deadline_at - shim_now_ms();
            if (remaining <= 0) {
                diag("connect.timeout", "path", socket_path);
                close(fd);
                *out_exit = CBM_SHIM_EXIT_CONNECT_TIMEOUT;
                return -1;
            }
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            int pr = poll(&pfd, 1, (int)(remaining > 60000 ? 60000 : remaining));
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                diag_errno("connect.poll_failed", errno);
                close(fd);
                *out_exit = CBM_SHIM_EXIT_HANDSHAKE_ERROR;
                return -1;
            }
            if (pr == 0) {
                diag("connect.timeout", "path", socket_path);
                close(fd);
                *out_exit = CBM_SHIM_EXIT_CONNECT_TIMEOUT;
                return -1;
            }
            int so_err = 0;
            socklen_t so_len = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) < 0) {
                diag_errno("connect.getsockopt_failed", errno);
                close(fd);
                *out_exit = CBM_SHIM_EXIT_HANDSHAKE_ERROR;
                return -1;
            }
            rc = so_err == 0 ? 0 : -1;
            errno = so_err;
            break;
        }
    }

    if (rc < 0) {
        int err = errno;
        close(fd);
        switch (err) {
        case ENOENT:
            diag_errno("connect.daemon_absent", err);
            *out_exit = CBM_SHIM_EXIT_DAEMON_ABSENT;
            break;
        case ECONNREFUSED:
            diag_errno("connect.stale_socket", err);
            *out_exit = CBM_SHIM_EXIT_STALE_SOCKET;
            break;
        case EACCES:
        case EPERM:
            diag_errno("connect.permission_denied", err);
            *out_exit = CBM_SHIM_EXIT_PERMISSION_DENIED;
            break;
        default:
            diag_errno("connect.failed", err);
            *out_exit = CBM_SHIM_EXIT_HANDSHAKE_ERROR;
            break;
        }
        return -1;
    }

    /* Restore blocking mode: the relay loop below uses poll()+read()/write()
     * with its own deadlines, not O_NONBLOCK semantics. */
    if (fcntl(fd, F_SETFL, flags) < 0) {
        diag_errno("connect.restore_blocking_failed", errno);
        close(fd);
        *out_exit = CBM_SHIM_EXIT_HANDSHAKE_ERROR;
        return -1;
    }
    return fd;
}

/* ── Reconnect-capable relay ───────────────────────────────────────────────
 *
 * WHY (2026-09-23 data): a daemon restart used to end every attached session
 * with exit 76; the host then failed every later MCP call in ~2ms with
 * "Transport closed" until the whole agent session ended (10 of 71 MCP calls
 * in 24h). The daemon comes back within seconds (launchd/systemd), so the shim
 * now reattaches to the SAME socket instead of dying:
 *
 *   1. frames are still forwarded verbatim; the shim only splits the stream at
 *      frame boundaries (shim_frames.h mirrors the daemon reader's two
 *      framings) so it can remember the host's initialize request, its
 *      notifications/initialized, and the ids of requests awaiting a response;
 *   2. on connection loss while stdin is open: every in-flight request gets a
 *      JSON-RPC error with its original id (never silence, never a fake result),
 *      then connect+handshake are retried with backoff for a bounded budget;
 *   3. on reattach the cached initialize + initialized frames are replayed so
 *      the fresh daemon session is READY, and the daemon's answer to the
 *      replayed initialize is swallowed (the host already has one);
 *   4. budget exhausted / version changed: exactly the old fail-closed exit
 *      (76 + shim.midstream_loss), plus a shim.reconnect_failed journal line.
 *
 * If either direction ever stops parsing as frames (foreign framing, frame
 * over CBM_SHIM_FRAME_MAX), that direction degrades to the old raw byte relay
 * and reconnect is disabled for the rest of the session — the shim never
 * guesses where a frame ends. */

enum {
    CBM_SHIM_DEFAULT_RECONNECT_TIMEOUT_MS = 30000,
    CBM_SHIM_RECONNECT_BACKOFF_MIN_MS = 50,
    CBM_SHIM_RECONNECT_BACKOFF_MAX_MS = 1000,
};

typedef struct shim_buf {
    char *data;
    size_t len;
    size_t cap;
} shim_buf_t;

static int shim_buf_append(shim_buf_t *b, const char *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) {
            cap *= 2;
        }
        char *nd = realloc(b->data, cap);
        if (!nd) {
            return -1;
        }
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void shim_buf_consume(shim_buf_t *b, size_t n) {
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static int shim_buf_set(shim_buf_t *b, const char *src, size_t n) {
    b->len = 0;
    return shim_buf_append(b, src, n);
}

typedef struct shim_pending {
    char id[CBM_SHIM_FRAME_ID_MAX];
    int content_length;
} shim_pending_t;

typedef struct shim_relay_state {
    shim_buf_t in;        /* host bytes not yet forming a complete frame */
    shim_buf_t out;       /* daemon bytes not yet forming a complete frame */
    shim_buf_t init_req;  /* last host initialize frame, verbatim */
    shim_buf_t init_note; /* last host notifications/initialized frame */
    char init_id[CBM_SHIM_FRAME_ID_MAX];
    char swallow_id[CBM_SHIM_FRAME_ID_MAX]; /* replayed initialize awaiting its answer */
    shim_pending_t *pending;
    size_t npending;
    size_t pending_cap;
    int in_raw;  /* host->daemon direction degraded to raw relay */
    int out_raw; /* daemon->host direction degraded to raw relay */
} shim_relay_state_t;

static void shim_relay_state_free(shim_relay_state_t *st) {
    free(st->in.data);
    free(st->out.data);
    free(st->init_req.data);
    free(st->init_note.data);
    free(st->pending);
}

static int write_all(int fd, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static void pending_add(shim_relay_state_t *st, const char *id, int content_length) {
    if (st->npending == st->pending_cap) {
        size_t cap = st->pending_cap ? st->pending_cap * 2 : 16;
        shim_pending_t *np = realloc(st->pending, cap * sizeof(*np));
        if (!np) {
            return; /* untracked: at worst this request gets no restart error */
        }
        st->pending = np;
        st->pending_cap = cap;
    }
    shim_pending_t *p = &st->pending[st->npending++];
    (void)snprintf(p->id, sizeof(p->id), "%s", id);
    p->content_length = content_length;
}

static void pending_remove(shim_relay_state_t *st, const char *id) {
    for (size_t i = 0; i < st->npending; i++) {
        if (strcmp(st->pending[i].id, id) == 0) {
            st->pending[i] = st->pending[st->npending - 1];
            st->npending--;
            return;
        }
    }
}

static int reconnect_budget_ms(const cbm_shim_options_t *opts) {
    if (opts->reconnect_timeout_ms < 0) {
        return 0;
    }
    if (opts->reconnect_timeout_ms > 0) {
        return opts->reconnect_timeout_ms;
    }
    const char *env = getenv("CBM_SHIM_RECONNECT_TIMEOUT_MS");
    if (env && env[0]) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end && *end == '\0' && v >= 0 && v <= 3600000) {
            return (int)v;
        }
    }
    return CBM_SHIM_DEFAULT_RECONNECT_TIMEOUT_MS;
}

/* Host -> daemon: forward every complete frame, recording what reconnect needs.
 * Returns 0 on success, -1 when the daemon write failed (connection lost). */
static int pump_host_frames(shim_relay_state_t *st, int uds_fd) {
    while (st->in.len > 0) {
        if (st->in_raw) {
            int rc = write_all(uds_fd, st->in.data, st->in.len);
            st->in.len = 0;
            return rc;
        }
        cbm_shim_frame_t fr;
        int sr = cbm_shim_frame_scan(st->in.data, st->in.len, CBM_SHIM_FRAME_MAX, &fr);
        if (sr == 0) {
            return 0;
        }
        if (sr < 0) {
            st->in_raw = 1;
            diag("relay.frames_unparsed", "direction", "host_to_daemon");
            continue;
        }
        cbm_shim_msg_t msg;
        cbm_shim_msg_inspect(st->in.data + fr.body_off, fr.body_len, &msg);
        if (msg.kind == CBM_SHIM_MSG_REQUEST) {
            if (msg.is_initialize &&
                shim_buf_set(&st->init_req, st->in.data, fr.total) == 0) {
                (void)snprintf(st->init_id, sizeof(st->init_id), "%s", msg.id);
                st->init_note.len = 0;
            }
            pending_add(st, msg.id, fr.content_length);
        } else if (msg.kind == CBM_SHIM_MSG_NOTIFICATION && msg.is_initialized) {
            (void)shim_buf_set(&st->init_note, st->in.data, fr.total);
        }
        int rc = write_all(uds_fd, st->in.data, fr.total);
        shim_buf_consume(&st->in, fr.total);
        if (rc < 0) {
            return -1;
        }
    }
    return 0;
}

/* Daemon -> host: forward complete frames, settle pending ids, swallow the
 * answer to a replayed initialize. Returns 0, or -1 when stdout failed. */
static int pump_daemon_frames(shim_relay_state_t *st, int stdout_fd) {
    while (st->out.len > 0) {
        if (st->out_raw) {
            int rc = write_all(stdout_fd, st->out.data, st->out.len);
            st->out.len = 0;
            return rc;
        }
        cbm_shim_frame_t fr;
        int sr = cbm_shim_frame_scan(st->out.data, st->out.len, CBM_SHIM_FRAME_MAX, &fr);
        if (sr == 0) {
            return 0;
        }
        if (sr < 0) {
            st->out_raw = 1;
            diag("relay.frames_unparsed", "direction", "daemon_to_host");
            continue;
        }
        cbm_shim_msg_t msg;
        cbm_shim_msg_inspect(st->out.data + fr.body_off, fr.body_len, &msg);
        int forward = 1;
        if (msg.kind == CBM_SHIM_MSG_RESPONSE) {
            if (st->swallow_id[0] && strcmp(msg.id, st->swallow_id) == 0) {
                st->swallow_id[0] = '\0';
                forward = 0;
            } else {
                pending_remove(st, msg.id);
            }
        }
        int rc = forward ? write_all(stdout_fd, st->out.data, fr.total) : 0;
        shim_buf_consume(&st->out, fr.total);
        if (rc < 0) {
            return -1;
        }
    }
    return 0;
}

/* Answer every request the lost daemon never answered, with its original id. */
static int fail_pending(shim_relay_state_t *st, int stdout_fd) {
    char frame[1024];
    for (size_t i = 0; i < st->npending; i++) {
        int n = cbm_shim_format_lost_error(frame, sizeof(frame), st->pending[i].id,
                                           st->pending[i].content_length);
        if (n > 0 && write_all(stdout_fd, frame, (size_t)n) < 0) {
            return -1;
        }
    }
    st->npending = 0;
    return 0;
}

typedef struct shim_reconnect_ctx {
    const char *socket_path;
    int connect_timeout_ms;
    int handshake_timeout_ms;
    int budget_ms;
} shim_reconnect_ctx_t;

/* Retry connect+handshake on the same socket within the budget, then replay
 * the cached initialize/initialized frames. Returns the new fd or -1. */
static int shim_reconnect(const shim_reconnect_ctx_t *rc, shim_relay_state_t *st, int *attempts,
                          long long *elapsed_ms) {
    long long t0 = shim_now_ms();
    long long deadline = t0 + rc->budget_ms;
    int backoff = CBM_SHIM_RECONNECT_BACKOFF_MIN_MS;
    *attempts = 0;
    int fd = -1;
    g_diag_quiet = 1;
    for (;;) {
        long long remaining = deadline - shim_now_ms();
        if (remaining <= 0) {
            break;
        }
        (*attempts)++;
        cbm_shim_exit_t ce = CBM_SHIM_EXIT_OK;
        int ct = rc->connect_timeout_ms < remaining ? rc->connect_timeout_ms : (int)remaining;
        fd = shim_connect(rc->socket_path, ct > 0 ? ct : 1, &ce);
        if (fd >= 0) {
            cbm_shim_hs_result_t hs = cbm_shim_handshake_client(fd, rc->handshake_timeout_ms);
            if (hs == CBM_SHIM_HS_OK) {
                break;
            }
            diag("reconnect.handshake_failed", "result", cbm_shim_hs_result_name(hs));
            close(fd);
            fd = -1;
            if (hs == CBM_SHIM_HS_VERSION_MISMATCH) {
                break; /* a different daemon build: never talk to it mid-session */
            }
        }
        remaining = deadline - shim_now_ms();
        if (remaining <= 0) {
            break;
        }
        struct timespec ts;
        long long nap = backoff < remaining ? backoff : remaining;
        ts.tv_sec = (time_t)(nap / 1000);
        ts.tv_nsec = (long)((nap % 1000) * 1000000);
        (void)nanosleep(&ts, NULL);
        backoff = backoff * 2 > CBM_SHIM_RECONNECT_BACKOFF_MAX_MS ? CBM_SHIM_RECONNECT_BACKOFF_MAX_MS
                                                                   : backoff * 2;
    }
    g_diag_quiet = 0;
    *elapsed_ms = shim_now_ms() - t0;
    if (fd < 0) {
        return -1;
    }
    /* Replay the session preamble so the fresh daemon session is READY. */
    st->swallow_id[0] = '\0';
    if (st->init_req.len > 0) {
        if (write_all(fd, st->init_req.data, st->init_req.len) < 0) {
            close(fd);
            return -1;
        }
        (void)snprintf(st->swallow_id, sizeof(st->swallow_id), "%s", st->init_id);
        if (st->init_note.len > 0 && write_all(fd, st->init_note.data, st->init_note.len) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* Relay between (stdin_fd -> uds) and (uds -> stdout_fd) until stdin reaches
 * EOF (clean shutdown: half-close toward the daemon, drain its output) or the
 * daemon connection is lost and cannot be re-established within the budget
 * (fail-closed midstream loss). *uds_fd is replaced on every reattach. */
static int shim_relay(int stdin_fd, int *uds_fd, int stdout_fd, const shim_reconnect_ctx_t *rctx) {
    char buf[CBM_SHIM_RELAY_BUF_SIZE];
    int stdin_open = 1;
    shim_relay_state_t st;
    memset(&st, 0, sizeof(st));
    int result = CBM_SHIM_EXIT_OK;

    for (;;) {
        int lost = 0;
        const char *lost_state = NULL;
        int lost_errno = 0;

        struct pollfd fds[2];
        int nfds = 0;
        int stdin_idx = -1;
        if (stdin_open) {
            stdin_idx = nfds;
            fds[nfds].fd = stdin_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        int uds_idx = nfds;
        fds[nfds].fd = *uds_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        int pr = poll(fds, (nfds_t)nfds, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            diag_errno("relay.poll_failed", errno);
            result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
            break;
        }

        if (stdin_open && (fds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(stdin_fd, buf, sizeof(buf));
            if (n > 0) {
                if (shim_buf_append(&st.in, buf, (size_t)n) < 0) {
                    diag("relay.oom", "direction", "host_to_daemon");
                    result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
                    break;
                }
                if (pump_host_frames(&st, *uds_fd) < 0) {
                    lost = 1;
                    lost_state = "relay.uds_write_failed";
                    lost_errno = errno;
                }
            } else if (n == 0) {
                /* Host EOF: forward any unterminated tail verbatim, half-close
                 * toward the daemon, keep draining its in-flight output. */
                stdin_open = 0;
                if (st.in.len > 0) {
                    (void)write_all(*uds_fd, st.in.data, st.in.len);
                    st.in.len = 0;
                }
                if (shutdown(*uds_fd, SHUT_WR) < 0 && errno != ENOTCONN) {
                    diag_errno("relay.shutdown_wr_failed", errno);
                }
            } else if (errno != EINTR) {
                diag_errno("relay.stdin_read_failed", errno);
                result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
                break;
            }
        }

        if (!lost && (fds[uds_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(*uds_fd, buf, sizeof(buf));
            if (n > 0) {
                if (shim_buf_append(&st.out, buf, (size_t)n) < 0) {
                    diag("relay.oom", "direction", "daemon_to_host");
                    result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
                    break;
                }
                if (pump_daemon_frames(&st, stdout_fd) < 0) {
                    diag_errno("relay.stdout_write_failed", errno);
                    result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
                    break;
                }
            } else if (n == 0) {
                if (!stdin_open) {
                    /* Clean S_EXITED: flush an unterminated tail verbatim. */
                    if (st.out.len > 0) {
                        (void)write_all(stdout_fd, st.out.data, st.out.len);
                    }
                    break;
                }
                lost = 1;
                lost_state = "daemon_closed_connection";
            } else if (errno != EINTR) {
                lost = 1;
                lost_state = "relay.uds_read_failed";
                lost_errno = errno;
            }
        }

        if (!lost) {
            continue;
        }

        /* ── Daemon connection lost while the host is still attached ── */
        int can_reconnect = rctx->budget_ms > 0 && !st.in_raw && !st.out_raw;
        if (lost_errno) {
            diag_errno(lost_state, lost_errno);
        }
        if (!can_reconnect) {
            diag("midstream_loss", "reason", "daemon_closed_connection");
            result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
            break;
        }
        close(*uds_fd);
        *uds_fd = -1;
        st.out.len = 0; /* a torn partial response belongs to a pending request */
        st.swallow_id[0] = '\0';
        diag("reconnecting", "reason", lost_state);
        if (fail_pending(&st, stdout_fd) < 0) {
            diag_errno("relay.stdout_write_failed", errno);
            result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
            break;
        }
        int attempts = 0;
        long long elapsed = 0;
        int nfd = shim_reconnect(rctx, &st, &attempts, &elapsed);
        char extra[96];
        (void)snprintf(extra, sizeof(extra), "attempts=%d elapsed_ms=%lld", attempts, elapsed);
        if (nfd < 0) {
            /* Not diag(): keep the last attempt's precise state (daemon_absent,
             * stale_socket, ...) as the journal record's state= field. */
            (void)fprintf(stderr, "codebase-memory-mcp: shim.reconnect_failed %s\n", extra);
            shim_journal_ex("shim.reconnect_failed", rctx->socket_path, extra);
            diag("midstream_loss", "reason", "daemon_closed_connection");
            result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
            break;
        }
        *uds_fd = nfd;
        g_last_diag_state = NULL; /* reattached: journal state=attached */
        g_last_diag_detail = NULL;
        g_last_diag_errno = 0;
        (void)fprintf(stderr, "codebase-memory-mcp: shim.reconnect_ok %s\n", extra);
        shim_journal_ex("shim.reconnect_ok", rctx->socket_path, extra);
        /* Host frames that arrived meanwhile are still queued in stdin/st.in. */
        if (pump_host_frames(&st, *uds_fd) < 0) {
            diag_errno("relay.uds_write_failed", errno);
            diag("midstream_loss", "reason", "daemon_closed_connection");
            result = CBM_SHIM_EXIT_MIDSTREAM_LOST;
            break;
        }
    }
    shim_relay_state_free(&st);
    return result;
}

int cbm_mcp_shim_run(const cbm_shim_options_t *opts, int stdin_fd, int stdout_fd) {
    /* A daemon that is SIGKILLed (or any peer that resets the connection)
     * mid-write would otherwise deliver SIGPIPE to this process on the next
     * write(2) to uds_fd, whose default disposition is to terminate the shim
     * immediately — before it can emit the structured
     * shim.midstream_loss diagnostic on stderr or return
     * CBM_SHIM_EXIT_MIDSTREAM_LOST. Ignoring SIGPIPE turns that write into a
     * normal EPIPE return, which shim_relay already handles as a fail-closed
     * midstream-loss exit. Scoped to the shim role only (daemon/legacy roles
     * are unaffected; this function is only ever reached via run_shim). */
    (void)signal(SIGPIPE, SIG_IGN);

    cbm_shim_options_t defaults = {0};
    if (!opts) {
        opts = &defaults;
    }
    int connect_timeout_ms = opts->connect_timeout_ms > 0 ? opts->connect_timeout_ms
                                                          : CBM_SHIM_DEFAULT_CONNECT_TIMEOUT_MS;
    int handshake_timeout_ms = opts->handshake_timeout_ms > 0
                                   ? opts->handshake_timeout_ms
                                   : CBM_SHIM_DEFAULT_HANDSHAKE_TIMEOUT_MS;

    char resolved[108];
    if (cbm_uds_socket_path_resolve(resolved, sizeof(resolved), opts->socket_path) != 0) {
        diag_errno("resolve_failed", errno);
        shim_journal("shim.connect_failed", opts->socket_path);
        return CBM_SHIM_EXIT_USAGE;
    }

    cbm_shim_exit_t connect_exit = CBM_SHIM_EXIT_OK;
    int uds_fd = shim_connect(resolved, connect_timeout_ms, &connect_exit);
    if (uds_fd < 0) {
        /* state/errno come from the diag_errno() shim_connect already emitted,
         * so the journal reports the precise reason (stale socket vs. daemon
         * absent vs. permission denied) without a second copy of those names. */
        shim_journal("shim.connect_failed", resolved);
        return (int)connect_exit;
    }

    cbm_shim_hs_result_t hs = cbm_shim_handshake_client(uds_fd, handshake_timeout_ms);
    if (hs != CBM_SHIM_HS_OK) {
        diag("handshake_failed", "result", cbm_shim_hs_result_name(hs));
        shim_journal("shim.connect_failed", resolved);
        close(uds_fd);
        switch (hs) {
        case CBM_SHIM_HS_VERSION_MISMATCH:
            return CBM_SHIM_EXIT_VERSION_MISMATCH;
        case CBM_SHIM_HS_TIMEOUT:
            return CBM_SHIM_EXIT_CONNECT_TIMEOUT;
        default:
            return CBM_SHIM_EXIT_HANDSHAKE_ERROR;
        }
    }

    /* Healthy side of the ratio: without this line "3 failures" is not a rate
     * and a permanently broken daemon looks the same as an idle machine. */
    shim_journal("shim.connect_ok", resolved);

    shim_reconnect_ctx_t rctx = {
        .socket_path = resolved,
        .connect_timeout_ms = connect_timeout_ms,
        .handshake_timeout_ms = handshake_timeout_ms,
        .budget_ms = reconnect_budget_ms(opts),
    };
    int rc = shim_relay(stdin_fd, &uds_fd, stdout_fd, &rctx);
    if (uds_fd >= 0) {
        close(uds_fd);
    }
    if (rc == CBM_SHIM_EXIT_MIDSTREAM_LOST) {
        /* The daemon died with a live session attached and did not come back
         * within the reconnect budget — the 2026-09-07 shape. Journaling it pins
         * the moment the session was given up, which the connect-time lines
         * alone can only bracket. */
        shim_journal("shim.session_lost", resolved);
    }
    return rc;
}
