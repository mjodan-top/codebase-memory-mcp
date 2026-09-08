#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "daemon/mcp_shim.h"

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

/* All diagnostics go to stderr, one line, machine-greppable
 * ("shim.<state>=<detail>"), never touching stdout (MCP transport). */
static void diag(const char *state, const char *detail_key, const char *detail_val) {
    g_last_diag_state = state;
    g_last_diag_detail = detail_val;
    g_last_diag_errno = 0;
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

/* Append one key=value line describing this shim's connect outcome. */
static void shim_journal(const char *event, const char *socket_path) {
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

/* Relay raw bytes bidirectionally between (stdin_fd -> uds_fd) and
 * (uds_fd -> stdout_fd) until stdin reaches EOF (clean shutdown, half-closes
 * the write side of uds_fd and keeps draining any remaining daemon output)
 * or the uds connection is lost while still attached (fail-closed midstream
 * loss). Never buffers/parses MCP frames — pure byte relay, so newline-JSON
 * and Content-Length framed messages are both transported transparently. */
static int shim_relay(int stdin_fd, int uds_fd, int stdout_fd) {
    char buf[CBM_SHIM_RELAY_BUF_SIZE];
    int stdin_open = 1;
    int uds_read_open = 1;

    while (uds_read_open) {
        struct pollfd fds[2];
        int nfds = 0;
        int stdin_idx = -1;
        int uds_idx;

        if (stdin_open) {
            stdin_idx = nfds;
            fds[nfds].fd = stdin_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        uds_idx = nfds;
        fds[nfds].fd = uds_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        int pr = poll(fds, (nfds_t)nfds, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            diag_errno("relay.poll_failed", errno);
            return CBM_SHIM_EXIT_MIDSTREAM_LOST;
        }

        if (stdin_open && (fds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(stdin_fd, buf, sizeof(buf));
            if (n > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(uds_fd, buf + off, (size_t)(n - off));
                    if (w < 0) {
                        if (errno == EINTR)
                            continue;
                        diag_errno("relay.uds_write_failed", errno);
                        return CBM_SHIM_EXIT_MIDSTREAM_LOST;
                    }
                    off += w;
                }
            } else if (n == 0) {
                /* Client (host) sent EOF: clean shutdown. Half-close the
                 * write side toward the daemon so it observes EOF too, then
                 * keep draining any in-flight daemon output before exiting. */
                stdin_open = 0;
                if (shutdown(uds_fd, SHUT_WR) < 0 && errno != ENOTCONN) {
                    diag_errno("relay.shutdown_wr_failed", errno);
                }
            } else {
                if (errno == EINTR)
                    continue;
                diag_errno("relay.stdin_read_failed", errno);
                return CBM_SHIM_EXIT_MIDSTREAM_LOST;
            }
        }

        if (fds[uds_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(uds_fd, buf, sizeof(buf));
            if (n > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(stdout_fd, buf + off, (size_t)(n - off));
                    if (w < 0) {
                        if (errno == EINTR)
                            continue;
                        diag_errno("relay.stdout_write_failed", errno);
                        return CBM_SHIM_EXIT_MIDSTREAM_LOST;
                    }
                    off += w;
                }
            } else if (n == 0) {
                uds_read_open = 0;
                /* If our own stdin already reached EOF first, this is a
                 * clean, expected shutdown (S_EXITED). If the daemon closed
                 * while we were still attached and stdin was still open,
                 * that is exactly the fail-closed midstream-loss case
                 * (S_MIDSTREAM_LOST_FAILCLOSED, e.g. daemon SIGKILLed). */
                if (stdin_open) {
                    diag("midstream_loss", "reason", "daemon_closed_connection");
                    return CBM_SHIM_EXIT_MIDSTREAM_LOST;
                }
            } else {
                if (errno == EINTR)
                    continue;
                diag_errno("relay.uds_read_failed", errno);
                return CBM_SHIM_EXIT_MIDSTREAM_LOST;
            }
        }
    }
    return CBM_SHIM_EXIT_OK;
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

    int rc = shim_relay(stdin_fd, uds_fd, stdout_fd);
    close(uds_fd);
    if (rc == CBM_SHIM_EXIT_MIDSTREAM_LOST) {
        /* The daemon died with a live session attached — exactly the 2026-09-07
         * shape. Journaling it pins the moment the daemon went away, which the
         * connect-time lines alone can only bracket. */
        shim_journal("shim.session_lost", resolved);
    }
    return rc;
}
