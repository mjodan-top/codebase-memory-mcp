/*
 * main.c — Entry point for codebase-memory-mcp.
 *
 * Modes:
 *   (default)       Run as MCP server on stdin/stdout (JSON-RPC 2.0)
 *   cli <tool> <json>  Run a single tool call and print result
 *   --version       Print version and exit
 *   --help          Print usage and exit
 *   --ui=true/false Enable/disable HTTP UI server (persisted)
 *   --port=N        Set HTTP UI port (persisted, default 9749)
 *
 * Signal handling: SIGTERM/SIGINT trigger graceful shutdown.
 * Watcher runs in a background thread, polling for git changes.
 * HTTP UI server (optional) runs in a background thread on localhost.
 */
#include "cbm.h" // cbm_alloc_init — bind 3rd-party allocators to mimalloc before any sqlite/git init
#include "mcp/mcp.h"
#include "mcp/index_supervisor.h"
#include "watcher/watcher.h"
#include "pipeline/pipeline.h"
#include "store/store.h"
#include "cli/cli.h"
#include "cli/progress_sink.h"
#include "foundation/constants.h"
#include "foundation/xlock.h"
#include "daemon/mcp_shim.h"
#include "daemon/mcp_uds_runner.h"
#include "daemon/uds_lifecycle.h"

enum {
    MAIN_MIN_ARGC = 1,
    MAIN_CLI_ARGC = 2,
    MAIN_FLAG_OFF = 5, /* strlen("--ui=") */
    MAIN_PORT_OFF = 7, /* strlen("--port=") */
    MAIN_MAX_PORT = 65536,
    PARENT_WATCHDOG_STACK_SIZE = 64 * CBM_SZ_1K, /* watchdog only polls — tiny stack suffices */
};
#define SLEN(s) (sizeof(s) - 1)
#include "foundation/log.h"
#include "foundation/diagnostics.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/mem.h"
#include "foundation/profile.h"
#include "foundation/win_utf8.h" /* cbm_wide_to_utf8 — Windows UTF-8 argv (#423/#20); no-op on POSIX */
#ifdef _WIN32
#include <shellapi.h> /* CommandLineToArgvW — not pulled in by windows.h under WIN32_LEAN_AND_MEAN */
#endif
#include "ui/config.h"
#include "ui/http_server.h"
#include "ui/embedded_assets.h"
#include <yyjson/yyjson.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef CBM_VERSION
#define CBM_VERSION "dev"
#endif

/* ── Globals for signal handling ────────────────────────────────── */

static cbm_watcher_t *g_watcher = NULL;
static cbm_mcp_server_t *g_server = NULL;
static cbm_http_server_t *g_http_server = NULL;
static atomic_int g_shutdown = 0;

/* Daemon-role-only listener handle, deliberately separate from g_server/
 * g_watcher/g_http_server above: the daemon role never touches those, and the
 * (default) shim role never touches this. request_shutdown() below only
 * closes whichever of these two lifecycles is actually active for the
 * current process role, so a signal never mis-closes or mis-accesses state
 * that belongs to the other role. */
static cbm_uds_owner_t *g_daemon_owner = NULL;

/* Idempotent shutdown: cancels the active pipeline, stops background servers,
 * and closes stdin to unblock the MCP read loop. Invoked from the signal
 * handler and from the parent-death watchdog, hence the atomic_exchange guard
 * so the body runs at most once. Body is async-signal-safe (only atomic stores
 * and stop calls that themselves only set atomics). */
static void request_shutdown(void) {
    if (atomic_exchange(&g_shutdown, 1)) {
        return; /* already shutting down */
    }

    /* Cancel any in-progress pipeline (async-signal-safe: only does atomic_store) */
    if (g_server) {
        cbm_pipeline_t *p = cbm_mcp_server_active_pipeline(g_server);
        if (p) {
            cbm_pipeline_cancel(p);
        }
    }
    /* Release pipeline lock to prevent stale lock on restart */
    cbm_pipeline_unlock();

    if (g_watcher) {
        cbm_watcher_stop(g_watcher);
    }
    if (g_http_server) {
        cbm_http_server_stop(g_http_server);
    }
    /* Daemon role: close the listener fd to unblock cbm_mcp_uds_serve's
     * blocking accept() loop (mirrors cbm_uds_owner_close's own unlink path,
     * but here we only need the fd closed — cbm_mcp_uds_serve treats a closed
     * listen_fd as a normal stop signal). Guarded so this is a no-op for the
     * (default) shim role, which never sets g_daemon_owner. */
    if (g_daemon_owner && g_daemon_owner->listen_fd >= 0) {
        int fd = g_daemon_owner->listen_fd;
        g_daemon_owner->listen_fd = -1;
        (void)close(fd);
    }
    /* Close stdin to unblock getline in the MCP server loop (shim/legacy
     * in-process server roles only; the daemon role does not read stdin). */
    (void)fclose(stdin);
}

static void signal_handler(int sig) {
    (void)sig;
    request_shutdown();
}

/* ── Parent-process watchdog ────────────────────────────────────── */
/* parent-death watchdog — distilled from #407 (fixes #406, thanks @nvt-pankajsharma).
 *
 * When this stdio MCP server is launched by an agent that later dies without a
 * clean SIGTERM (e.g. the editor is force-killed), the orphaned server would
 * otherwise linger forever blocked on stdin. POSIX has no portable "notify on
 * parent death" primitive (PR_SET_PDEATHSIG is Linux-only), so we poll getppid:
 * once the parent dies the process is reparented (ppid changes, typically to 1)
 * and we shut down. Windows is unaffected (job objects handle this) — #ifndef. */

#ifndef _WIN32
static void *parent_watchdog_thread(void *arg) {
    pid_t initial_ppid = *(pid_t *)arg;
    const unsigned int poll_interval_us = 500000; /* 500ms */

    while (!atomic_load(&g_shutdown)) {
        cbm_usleep(poll_interval_us);
        if (atomic_load(&g_shutdown)) {
            break;
        }
        /* initial_ppid > 1 guards against an already-orphaned start (ppid==1),
         * where a changing ppid carries no signal. */
        if (initial_ppid > 1 && getppid() != initial_ppid) {
            static const char msg[] = "level=warn msg=parent.exited reason=ppid_changed\n";
            (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
            _exit(0);
        }
    }
    return NULL;
}
#endif

/* ── Watcher background thread ──────────────────────────────────── */

static void *watcher_thread(void *arg) {
    cbm_watcher_t *w = arg;
#define WATCHER_BASE_INTERVAL_MS 5000

    cbm_watcher_run(w, WATCHER_BASE_INTERVAL_MS);
    return NULL;
}

/* ── HTTP UI background thread ──────────────────────────────────── */

static void *http_thread(void *arg) {
    cbm_http_server_t *srv = arg;
    cbm_http_server_run(srv);
    return NULL;
}

/* ── Index callback for watcher ─────────────────────────────────── */

static int watcher_index_fn(const char *project_name, const char *root_path, void *user_data) {
    (void)user_data;

    /* Skip indexing if shutdown is in progress */
    if (atomic_load(&g_shutdown)) {
        return 0;
    }

    /* Non-blocking: skip if another pipeline is already running.
     * Watcher will retry on next poll cycle (5-60s). */
    if (!cbm_pipeline_try_lock()) {
        cbm_log_info("watcher.skip", "project", project_name, "reason", "pipeline_busy");
        return 0;
    }

    cbm_log_info("watcher.reindex", "project", project_name, "path", root_path);

    /* #832: route the re-index through the supervised worker subprocess so this
     * long-lived server process hands its RSS back to the OS on every cycle
     * instead of ratcheting (mimalloc v3 does not reclaim pages that worker
     * threads abandon at exit). The child writes the DB; the parent only needs the
     * return code. The pipeline lock (already held) still serialises re-indexes.
     * Degrade to the in-process pipeline when the supervisor is off (kill switch)
     * or the spawn fails. */
    if (cbm_index_supervisor_should_wrap()) {
        char *resp = cbm_mcp_index_run_supervised_path(root_path);
        if (resp) {
            free(resp);
            cbm_pipeline_unlock();
            return 0;
        }
        /* resp == NULL → spawn-failure degrade → fall through to in-process. */
    }

    cbm_pipeline_t *p = cbm_pipeline_new(root_path, NULL, CBM_MODE_FULL);
    if (p && project_name && project_name[0]) {
        (void)cbm_pipeline_apply_project_alias(p, project_name);
    }
    if (!p) {
        cbm_pipeline_unlock();
        return CBM_NOT_FOUND;
    }

    int rc = cbm_pipeline_run(p);
    cbm_pipeline_free(p);
    cbm_pipeline_unlock();
    return rc;
}
/* ── CLI mode ───────────────────────────────────────────────────── */

#define CLI_USAGE "Usage: codebase-memory-mcp cli [--progress] [--json] <tool_name> [json_args]\n"

/* Extract text content from MCP tool result envelope and print it.
 * MCP results: {"content":[{"type":"text","text":"..."}],"isError":...}
 * Returns 1 if the result was an error, 0 otherwise. */
static int cli_print_mcp_result(const char *result) {
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    if (!doc) {
        printf("%s\n", result);
        return 0;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *err_val = yyjson_obj_get(root, "isError");
    bool is_error = err_val && yyjson_get_bool(err_val);

    const char *text = NULL;
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (yyjson_is_arr(content) && yyjson_arr_size(content) > 0) {
        yyjson_val *tv = yyjson_obj_get(yyjson_arr_get_first(content), "text");
        text = tv ? yyjson_get_str(tv) : NULL;
    }

    if (text) {
        (void)fprintf(is_error ? stderr : stdout, "%s\n", text);
    } else {
        printf("%s\n", result);
    }

    yyjson_doc_free(doc);
    return is_error ? SKIP_ONE : 0;
}

/* Strip a flag from argv, returning true if found. */
static bool cli_strip_flag(int *argc, char **argv, const char *flag) {
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], flag) != 0) {
            continue;
        }
        for (int j = i; j < *argc - SKIP_ONE; j++) {
            argv[j] = argv[j + SKIP_ONE];
        }
        (*argc)--;
        return true;
    }
    return false;
}

/* Strip a flag AND its following value from argv, returning the value (a pointer
 * into the original argv strings, valid for the process lifetime) or NULL if the
 * flag is absent. */
static const char *cli_strip_flag_value(int *argc, char **argv, const char *flag) {
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], flag) != 0) {
            continue;
        }
        const char *value = (i + SKIP_ONE < *argc) ? argv[i + SKIP_ONE] : NULL;
        int remove_count = value ? 2 : 1;
        for (int j = i; j < *argc - remove_count; j++) {
            argv[j] = argv[j + remove_count];
        }
        *argc -= remove_count;
        return value;
    }
    return NULL;
}

/* Portable "is fd a terminal?" — _isatty on Windows, isatty on POSIX. */
#ifdef _WIN32
#define cli_isatty(fd) _isatty(fd)
#else
#define cli_isatty(fd) isatty(fd)
#endif

enum { CLI_SLURP_CHUNK = 4096 };

/* Read an open stream fully into a heap, NUL-terminated string. Caller frees.
 * Returns NULL on allocation failure. Reads binary-clean (UTF-8 JSON, no shell
 * quoting needed). */
static char *cli_slurp_stream(FILE *f) {
    size_t cap = CLI_SLURP_CHUNK;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    char tmp[CLI_SLURP_CHUNK];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) {
                cap *= 2;
            }
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    return buf;
}

/* Slurp a file path into a heap, NUL-terminated string. Caller frees. */
static char *cli_slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *s = cli_slurp_stream(f);
    (void)fclose(f);
    return s;
}

/* True if the first non-whitespace byte of s is '{' (raw-JSON detection). */
static bool cli_first_nonspace_is_brace(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    return *s == '{';
}

/* ── xlock e2e probe (issue #22) ───────────────────────────────────
 * `cli --xlock-probe <project>` drives ONLY the cross-process lock state
 * machine (cbm_xlock_t) — no indexing pipeline involved — so the per-state
 * e2e suite gets deterministic, sub-second, real-multi-process coverage of
 * every state without paying for a real full index. Behaviour is controlled
 * by env vars so the test harness can spawn many real OS processes with
 * different roles:
 *   CBM_XLOCK_PROBE_HOLD_MS  — ms to hold the lock once acquired as owner
 *                              (default 300).
 *   CBM_XLOCK_PROBE_CRASH    — "1": after the hold, _exit WITHOUT releasing
 *                              or marking done (simulates a crashed holder;
 *                              the kernel still drops the flock on exit).
 * Prints exactly one line of JSON to stdout describing the FINAL state
 * reached: {"state":"OWNER_INDEXING|RELEASED|WAITING|REUSED|CRASH_RECLAIMED"}
 * (WAITING is only the parting line printed on crash-simulation exit, since
 * that path never reaches RELEASED itself). Exit code 0 on any completed,
 * well-defined outcome. */
static int run_xlock_probe(const char *project) {
    int hold_ms = 300;
    const char *hold_env = getenv("CBM_XLOCK_PROBE_HOLD_MS");
    if (hold_env && hold_env[0]) {
        int v = atoi(hold_env);
        if (v >= 0) {
            hold_ms = v;
        }
    }
    bool simulate_crash = false;
    const char *crash_env = getenv("CBM_XLOCK_PROBE_CRASH");
    if (crash_env && strcmp(crash_env, "1") == 0) {
        simulate_crash = true;
    }

    cbm_xlock_t lk;
    bool got = cbm_xlock_try_acquire(&lk, project);
    if (got) {
        printf("{\"state\":\"OWNER_INDEXING\",\"pid\":%d}\n", (int)getpid());
        fflush(stdout);
        if (hold_ms > 0) {
            cbm_usleep((unsigned int)hold_ms * 1000);
        }
        if (simulate_crash) {
            printf("{\"state\":\"CRASH_RELEASE\",\"pid\":%d}\n", (int)getpid());
            fflush(stdout);
            /* Do NOT release or mark done — exit abruptly. The kernel still
             * drops the flock on process exit (that is the whole point). */
            _Exit(1);
        }
        cbm_xlock_mark_done(&lk);
        cbm_xlock_release(&lk);
        printf("{\"state\":\"RELEASED\",\"pid\":%d}\n", (int)getpid());
        return 0;
    }

    printf("{\"state\":\"WAITING\",\"pid\":%d}\n", (int)getpid());
    fflush(stdout);
    if (!cbm_xlock_wait_for_release(&lk, project)) {
        printf("{\"state\":\"ERROR\",\"pid\":%d}\n", (int)getpid());
        return 1;
    }
    if (lk.state == CBM_XLOCK_STATE_REUSED) {
        printf("{\"state\":\"REUSED\",\"pid\":%d}\n", (int)getpid());
        cbm_xlock_release(&lk);
        return 0;
    }
    /* CRASH_RECLAIMED: the previous holder died without finishing. We now
     * legitimately hold the flock ourselves — become the new owner and (in
     * this probe) simply finish the run instead of the real holder. */
    printf("{\"state\":\"CRASH_RECLAIMED\",\"pid\":%d}\n", (int)getpid());
    fflush(stdout);
    if (hold_ms > 0) {
        cbm_usleep((unsigned int)hold_ms * 1000);
    }
    cbm_xlock_mark_done(&lk);
    cbm_xlock_release(&lk);
    printf("{\"state\":\"RELEASED\",\"pid\":%d}\n", (int)getpid());
    return 0;
}

static int run_cli(int argc, char **argv) {
    if (argc < MAIN_MIN_ARGC) {
        (void)fprintf(stderr, CLI_USAGE);
        return SKIP_ONE;
    }

    const char *xlock_probe_project = cli_strip_flag_value(&argc, argv, "--xlock-probe");
    if (xlock_probe_project) {
        return run_xlock_probe(xlock_probe_project);
    }

    bool progress = cli_strip_flag(&argc, argv, "--progress");
    bool raw_json = cli_strip_flag(&argc, argv, "--json");

    /* Supervisor worker role: when this process was spawned as a supervised index
     * worker, run indexing in-process (never re-supervise) and write the result to
     * the given file for the parent to read back. Stripped here so the tool
     * dispatch below sees only the tool name + its args. */
    bool index_worker = cli_strip_flag(&argc, argv, "--index-worker");
    const char *response_out = cli_strip_flag_value(&argc, argv, "--response-out");
    cbm_index_set_worker_role(index_worker, response_out);

#ifndef _WIN32
    /* #845: a supervised worker must not outlive its supervisor. If the parent
     * dies without reaping us (agent killed, supervisor crashed), an orphaned
     * worker would index on unsupervised — observed contributing to memory
     * pressure during the 2026-07-04 host panics. Reuse the parent-death
     * watchdog (safe outside server mode: on ppid change it only writes to
     * stderr and _exit(0)s — no cleanup dependencies). Detached: the worker
     * exits by returning from run_cli; exit() tears the thread down. Failure
     * to start is non-fatal, same policy as the MCP-server watchdog. */
    if (index_worker) {
        static pid_t worker_initial_ppid; /* static: outlives run_cli for the thread */
        worker_initial_ppid = getppid();
        cbm_thread_t worker_watchdog_tid;
        if (cbm_thread_create(&worker_watchdog_tid, PARENT_WATCHDOG_STACK_SIZE,
                              parent_watchdog_thread, &worker_initial_ppid) == 0) {
            (void)cbm_thread_detach(&worker_watchdog_tid);
            cbm_log_info("worker.watchdog.start");
        } else {
            cbm_log_warn("worker.watchdog.unavailable", "reason", "thread_create_failed");
        }
    }
#endif

    if (argc < MAIN_MIN_ARGC) {
        (void)fprintf(stderr, CLI_USAGE);
        return SKIP_ONE;
    }

    const char *tool_name = argv[0];
    int rem_argc = argc - SKIP_ONE; /* args following the tool name */
    char **rem_argv = argv + SKIP_ONE;

    /* --help / -h : print per-tool help (from the tool's input_schema) and exit
     * before any server work. */
    for (int i = 0; i < rem_argc; i++) {
        if (strcmp(rem_argv[i], "--help") == 0 || strcmp(rem_argv[i], "-h") == 0) {
            if (cbm_cli_print_tool_help(tool_name) != 0) {
                (void)fprintf(stderr, "error: unknown tool '%s'\n", tool_name);
                return SKIP_ONE;
            }
            return 0;
        }
    }

    /* Resolve the JSON arguments. Precedence: --args-file, then raw JSON
     * (back-compat), then --flags, then piped stdin, then empty {}. */
    char *heap_args = NULL; /* freed before return when set */
    const char *args_json = "{}";

    int args_file_idx = -1;
    for (int i = 0; i < rem_argc; i++) {
        if (strcmp(rem_argv[i], "--args-file") == 0) {
            args_file_idx = i;
            break;
        }
    }

    if (args_file_idx >= 0) {
        if (args_file_idx + SKIP_ONE >= rem_argc) {
            (void)fprintf(stderr, "error: --args-file requires a path argument\n");
            return SKIP_ONE;
        }
        const char *path = rem_argv[args_file_idx + SKIP_ONE];
        heap_args = cli_slurp_file(path);
        if (!heap_args) {
            (void)fprintf(stderr, "error: cannot read args file '%s'\n", path);
            return SKIP_ONE;
        }
        args_json = heap_args;
    } else if (rem_argc >= SKIP_ONE && cli_first_nonspace_is_brace(rem_argv[0])) {
        /* raw-JSON back-compat: cli <tool> '{"k":"v"}' (deprecated path). Warn on
         * STDERR only — stdout must stay clean JSON for piping. */
        (void)fprintf(stderr,
                      "warning: passing raw JSON to 'cli %s' is deprecated and "
                      "will be removed in a future release; use flags (run 'cli "
                      "%s --help'), --args-file <path>, or piped stdin.\n",
                      tool_name, tool_name);
        args_json = rem_argv[0];
    } else if (rem_argc >= SKIP_ONE && strncmp(rem_argv[0], "--", 2) == 0) {
        /* flag form: cli <tool> --flag value --bare-bool ... */
        char *err = NULL;
        heap_args = cbm_cli_build_args_json(tool_name, rem_argc, rem_argv, &err);
        if (!heap_args) {
            (void)fprintf(stderr, "error: %s\n", err ? err : "invalid arguments");
            free(err);
            return SKIP_ONE;
        }
        args_json = heap_args;
    } else if (!cli_isatty(0)) {
        /* piped stdin (UTF-8 clean, no shell quoting): cli <tool> < args.json */
        heap_args = cli_slurp_stream(stdin);
        if (heap_args && heap_args[0]) {
            args_json = heap_args;
        } else {
            free(heap_args);
            heap_args = NULL;
            args_json = "{}";
        }
    }

    if (progress) {
        cbm_progress_sink_init(stderr);
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        (void)fprintf(stderr, "error: failed to create server\n");
        if (progress) {
            cbm_progress_sink_fini();
        }
        return SKIP_ONE;
    }

    char *result = cbm_mcp_handle_tool(srv, tool_name, args_json);
    int exit_code = 0;

    if (result) {
        /* Supervised worker: hand the full result string to the parent via the
         * response file before printing (parent reads it back on a clean exit). */
        const char *ro = cbm_index_worker_response_out();
        if (ro) {
            FILE *rf = cbm_fopen(ro, "wb");
            if (rf) {
                (void)fputs(result, rf);
                (void)fclose(rf);
            }
        }
        if (raw_json) {
            printf("%s\n", result);
        } else {
            exit_code = cli_print_mcp_result(result);
        }
        if (cbm_index_worker_active()) {
            /* Supervised worker: the response is delivered (file + stdout).
             * Skip the multi-GB teardown (server/store frees) — the process
             * dies now and the OS reclaims everything wholesale; piecemeal
             * free() of a kernel-scale graph costs minutes. _Exit skips
             * atexit/LSan by design for this prod worker path. */
            cbm_log_info("index.worker.fast_exit", "action", "_Exit");
            fflush(NULL);
            _Exit(exit_code);
        }
        free(result);
    }

    cbm_mcp_server_free(srv);
    if (progress) {
        cbm_progress_sink_fini();
    }
    free(heap_args);
    return exit_code;
}

/* Forward declaration: defined further below (near the legacy in-process
 * bootstrap it originally served), used by run_daemon(). */
static void setup_signal_handlers(void);

/* ── Shim mode (Issue #27 default entry point) ─────────────────── */
/* Parse an optional `--socket <path>` override from a daemon/shim-role argv
 * slice (already past the subcommand token, if any). Returns the value or
 * NULL if absent (caller then resolves the default path). */
static const char *parse_socket_flag(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + SKIP_ONE < argc) {
            return argv[i + SKIP_ONE];
        }
    }
    return NULL;
}

/* Default entry point: a pure stdio->UDS thin shim. Deliberately does NOT
 * construct a cbm_mcp_server_t, watcher, HTTP UI, or any other heavyweight
 * resource — every failure path (daemon absent, stale socket, permission
 * denied, version mismatch, connect timeout, midstream loss) is handled
 * entirely inside cbm_mcp_shim_run and ends in a non-zero exit, never a
 * local server fallback (Issue #27 fail-closed contract). */
static int run_shim(int argc, char **argv) {
    cbm_shim_options_t opts = {0};
    opts.socket_path = parse_socket_flag(argc, argv);
    return cbm_mcp_shim_run(&opts, STDIN_FILENO, STDOUT_FILENO);
}

/* ── Daemon mode (explicit role; Issue #26/#31 UDS multi-session core) ──── */
/* should_stop callback for cbm_mcp_uds_serve: true once request_shutdown()
 * (SIGTERM/SIGINT) has fired. */
static int daemon_should_stop(void *userdata) {
    (void)userdata;
    return atomic_load(&g_shutdown);
}

/* Explicit `daemon` role: owns the pathname UDS listener and hands each
 * accepted shim connection an isolated MCP session backed by a single shared
 * cbm_mcp_core_t (store/router). This is the only code path that opens the
 * UDS listener and constructs MCP server sessions; the default (shim) path
 * above never reaches any of this. */
static int run_daemon(int argc, char **argv) {
    const char *socket_override = parse_socket_flag(argc, argv);
    char resolved[108];
    if (cbm_uds_socket_path_resolve(resolved, sizeof(resolved), socket_override) != 0) {
        (void)fprintf(stderr,
                      "codebase-memory-mcp: daemon.socket_resolve_failed errno=%d error=%s\n",
                      errno, strerror(errno));
        return SKIP_ONE;
    }

    cbm_mem_init(cbm_mem_ram_fraction_for_total(cbm_system_info().total_ram));
    cbm_log_set_sink_ex(cbm_ui_log_append, CBM_LOG_SINK_TEE);
    cbm_log_info("daemon.start", "version", CBM_VERSION, "socket", resolved);

    setup_signal_handlers();

    cbm_mcp_core_t *core = cbm_mcp_core_new(NULL);
    if (!core) {
        cbm_log_error("daemon.err", "msg", "failed to create mcp core");
        return SKIP_ONE;
    }

    cbm_uds_owner_t owner;
    memset(&owner, 0, sizeof(owner));
    cbm_uds_owner_configure(&owner, (unsigned long)-1, NULL, NULL);
    if (cbm_uds_owner_open(&owner, resolved, 64) != 0) {
        (void)fprintf(stderr,
                      "codebase-memory-mcp: daemon.listen_failed path=%s errno=%d error=%s\n",
                      resolved, errno, strerror(errno));
        cbm_mcp_core_free(core);
        return SKIP_ONE;
    }
    g_daemon_owner = &owner;

    cbm_log_info("daemon.listening", "socket", resolved);
    int rc = cbm_mcp_uds_serve(&owner, core, daemon_should_stop, NULL);
    cbm_log_info("daemon.shutdown");

    g_daemon_owner = NULL;
    cbm_uds_owner_close(&owner);
    cbm_mcp_core_free(core);
    return rc == 0 ? 0 : SKIP_ONE;
}

/* ── Help ───────────────────────────────────────────────────────── */

static void print_help(void) {
    printf("codebase-memory-mcp %s\n\n", CBM_VERSION);
    printf("Usage:\n");
    printf("  codebase-memory-mcp              Run MCP server on stdio\n");
    printf("  codebase-memory-mcp cli <tool> [json]  Run a single tool\n");
    printf("  codebase-memory-mcp install [-y|-n] [--force] [--dry-run]\n");
    printf("  codebase-memory-mcp uninstall [-y|-n] [--dry-run]\n");
    printf("  codebase-memory-mcp update [-y|-n]\n");
    printf("  codebase-memory-mcp config <list|get|set|reset>\n");
    printf("  codebase-memory-mcp --version    Print version\n");
    printf("  codebase-memory-mcp --help       Print this help\n");
    printf("\nUI options:\n");
    printf("  --ui=true    Enable HTTP graph visualization (persisted)\n");
    printf("  --ui=false   Disable HTTP graph visualization (persisted)\n");
    printf("  --port=N     Set UI port (default 9749, persisted)\n");
    printf("\nSupported agents (auto-detected):\n");
    printf("  Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode,\n");
    printf("  Antigravity, Aider, KiloCode, Kiro\n");
    printf("\nTools: index_repository, search_graph, query_graph, trace_path,\n");
    printf("  get_code_snippet, get_graph_schema, get_architecture, search_code,\n");
    printf("  list_projects, delete_project, index_status, detect_changes,\n");
    printf("  manage_adr, ingest_traces\n");
}

/* ── Main ───────────────────────────────────────────────────────── */

/* Try to handle a subcommand (cli/install/uninstall/update/config/--version/--help).
 * Returns -1 if no subcommand matched, otherwise the exit code. */
static int handle_subcommand(int argc, char **argv) {
    /* First scan: global flags */
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0) {
            cbm_profile_enable();
        }
    }
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("codebase-memory-mcp %s\n", CBM_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "cli") == 0) {
            cbm_mem_init(cbm_mem_ram_fraction_for_total(cbm_system_info().total_ram));
            return run_cli(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "daemon") == 0) {
            /* Explicit daemon role (Issue #27): only entry point that opens
             * the UDS listener / constructs cbm_mcp_core_t. */
            cbm_index_supervisor_mark_host();
            return run_daemon(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "hook-augment") == 0) {
            cbm_mem_init(cbm_mem_ram_fraction_for_total(cbm_system_info().total_ram));
            return cbm_cmd_hook_augment();
        }
        if (strcmp(argv[i], "install") == 0) {
            return cbm_cmd_install(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "uninstall") == 0) {
            return cbm_cmd_uninstall(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "update") == 0) {
            return cbm_cmd_update(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "config") == 0) {
            return cbm_cmd_config(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
    }
    return CBM_NOT_FOUND;
}

/* Parse --ui= and --port= flags. Returns true if config was modified. */
/* Install platform-specific signal handlers. */
static void setup_signal_handlers(void) {
#ifdef _WIN32
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
#else
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
#endif
}

#ifdef _WIN32
/* On Windows the CRT hands main() an argv encoded in the active ANSI code page, so a
 * non-ASCII CLI argument (e.g. a repo path like café_日本語_repo) is mangled before the
 * program ever sees it — the documented `cli index_repository "<json>"` then fails with
 * "repo_path is required" (#423/#20). Rebuild argv from the wide command line
 * (GetCommandLineW → CommandLineToArgvW) and convert each element to UTF-8 so the rest
 * of the program receives the same UTF-8 bytes it gets on POSIX. Returns a
 * NULL-terminated argv and sets *out_argc, or NULL on any failure (caller then keeps
 * the original narrow argv). The returned block lives for the whole process (argv must
 * stay valid until exit), so it is intentionally never freed. */
static char **cbm_win_utf8_argv(int *out_argc) {
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        return NULL;
    }
    if (wargc <= 0) {
        LocalFree(wargv);
        return NULL;
    }
    char **u8argv = (char **)calloc((size_t)wargc + 1, sizeof(char *));
    if (!u8argv) {
        LocalFree(wargv);
        return NULL;
    }
    for (int i = 0; i < wargc; i++) {
        u8argv[i] = cbm_wide_to_utf8(wargv[i]);
        if (!u8argv[i]) {
            for (int j = 0; j < i; j++) {
                free(u8argv[j]);
            }
            free(u8argv);
            LocalFree(wargv);
            return NULL;
        }
    }
    LocalFree(wargv);
    *out_argc = wargc;
    return u8argv; /* NULL-terminated (calloc'd wargc+1) */
}
#endif /* _WIN32 */

int main(int argc, char **argv) {
    /* Defense-in-depth: bind tree-sitter and sqlite3 to mimalloc so a
     * correct binary does not rely on the fragile MI_OVERRIDE symbol override
     * (#424). MUST be the VERY FIRST statement: SQLITE_CONFIG_MALLOC has to run
     * before the first sqlite3_open* (cbm_mcp_server_new → cbm_store_open_memory
     * below opens sqlite early), else sqlite3_config returns SQLITE_MISUSE and
     * the bind is silently ignored. No-op in the test build. */
    cbm_alloc_init();
#ifdef _WIN32
    /* Replace the ANSI-code-page argv the CRT handed us with a UTF-8 argv rebuilt from
     * the wide command line, so non-ASCII CLI arguments survive (#423/#20). Falls back
     * to the original argv if the wide rebuild fails. Done after cbm_alloc_init (which
     * must stay the very first statement) but before argv is first read below. */
    {
        int win_argc = 0;
        char **win_argv = cbm_win_utf8_argv(&win_argc);
        if (win_argv) {
            argc = win_argc;
            argv = win_argv;
        }
    }
#endif
    /* #845: mark this process as the REAL binary so the index supervisor may
     * wrap index_repository in a worker subprocess. Must run before any
     * subcommand dispatch so MCP-server, CLI, and HTTP paths are all covered.
     * Embedders of cbm_mcp_handle_tool (test binaries) never mark themselves,
     * so they index in-process instead of re-invoking themselves as
     * `<self> cli --index-worker …` (recursive suite re-runs / spawn chains). */
    cbm_index_supervisor_mark_host();
    cbm_cli_set_version(CBM_VERSION);
    cbm_profile_init(); /* reads CBM_PROFILE env var, gates all prof macros */
    /* CBM_LOG_LEVEL support — distilled from #414 (closes #413). Apply before
     * the first log statement so the configured level governs all output. */
    cbm_log_init_from_env();
    int subcmd = handle_subcommand(argc, argv);
    if (subcmd >= 0) {
        return subcmd;
    }

    /* Default entry point (Issue #27): a pure stdio->UDS thin shim. This is
     * intentionally the FIRST thing main() does after subcommand dispatch —
     * before the parent-death watchdog, before cbm_mem_init, before any
     * server/watcher/HTTP construction below. Nothing past this point in
     * main() (the parent watchdog thread, cbm_mem_init, cbm_mcp_server_new,
     * the watcher, the HTTP UI) is ever reached on the default path; it only
     * remains live for embedders / legacy direct invocation that bypass
     * handle_subcommand's dispatch (there are none in this binary's own
     * argv parsing — the code below is unreachable via normal CLI usage but
     * is kept, unmodified, as the in-process server implementation the
     * `daemon` role above still depends on via run_daemon/cbm_mcp_core_t). */
    return run_shim(argc, argv);
}
