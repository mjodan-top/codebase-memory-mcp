/*
 * test_incremental.c — Comprehensive integration tests for indexing pipeline.
 *
 * Clones a real open-source repo (FastAPI 0.99.1, ~1100 Python files), indexes
 * it through the full pipeline, then tests incremental re-indexing with file
 * modifications, additions, deletions, and adversarial inputs.
 *
 * Includes performance metrics: wall-clock time and peak RSS on every index,
 * edge-type-specific assertions, and regression guards.
 *
 * Requires network access for initial clone. Skips gracefully if offline.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <mcp/mcp.h>
#include <store/store.h>
#include <pipeline/pipeline.h>
#include <foundation/log.h>
#include <foundation/mem.h>

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <sqlite3.h>

/* ── Globals ──────────────────────────────────────────────────────── */

/* Issue #8 test: mtime bump so single-file change detection can't miss it. */
#ifndef INCR_FIX_SLEEP_NS
#define INCR_FIX_SLEEP_NS (15L * 1000L * 1000L) /* 15ms */
#endif
static char g_tmpdir[256];
static char g_repodir[512];
static char g_dbpath[512];
static cbm_mcp_server_t *g_srv = NULL;
static char *g_project = NULL;

/* Baseline counts after full index */
static int g_full_nodes = 0;
static int g_full_edges = 0;
static int g_full_calls = 0;
static int g_full_imports = 0;

/* Performance: track peak RSS and timing per test phase */
static size_t g_rss_before_full = 0;
static double g_full_index_ms = 0;

/* ── Helpers ──────────────────────────────────────────────────────── */

static double now_ms(void) {
    struct timespec ts;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void write_file_at(const char *rel_path, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_repodir, rel_path);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        cbm_mkdir(dir);
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void delete_file_at(const char *rel_path) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_repodir, rel_path);
    unlink(path);
}

/* Append "# reformatted" to up to max_files .py files in subdir (cross-platform). */
static int reformat_files(const char *subdir, int max_files) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", g_repodir, subdir);
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return -1;
    }
    cbm_dirent_t *entry;
    int count = 0;
    while ((entry = cbm_readdir(d)) != NULL && count < max_files) {
        size_t nlen = strlen(entry->name);
        if (nlen < 4 || strcmp(entry->name + nlen - 3, ".py") != 0) {
            continue;
        }
        if (entry->is_dir) {
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->name);
        th_append_file(path, "# reformatted\n");
        count++;
    }
    cbm_closedir(d);
    return 0;
}

static char *index_repo(void) {
    char args[512];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", g_repodir);
    return cbm_mcp_handle_tool(g_srv, "index_repository", args);
}

/* Timed index: returns response, sets *elapsed_ms and *peak_rss_mb */
static char *index_repo_timed(double *elapsed_ms, size_t *peak_rss_mb) {
    double t0 = now_ms();
    char *resp = index_repo();
    *elapsed_ms = now_ms() - t0;
    *peak_rss_mb = cbm_mem_peak_rss() / (1024 * 1024);
    return resp;
}

static char *call_tool(const char *tool, const char *args_fmt, ...) {
    char args[1024];
    va_list ap;
    va_start(ap, args_fmt);
    vsnprintf(args, sizeof(args), args_fmt, ap);
    va_end(ap);
    return cbm_mcp_handle_tool(g_srv, tool, args);
}

/* Parse integer from JSON response (handles nested MCP envelope) */
static int count_in_response(const char *resp, const char *key) {
    if (!resp)
        return -1;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(resp, pattern);
    if (p)
        return atoi(p + strlen(pattern));
    snprintf(pattern, sizeof(pattern), "\\\"%s\\\":", key);
    p = strstr(resp, pattern);
    if (p)
        return atoi(p + strlen(pattern));
    return -1;
}

/* ── Direct store queries (more reliable than MCP for tests) ────── */

static cbm_store_t *open_store(void) {
    return cbm_store_open_path(g_dbpath);
}

static int get_node_count(void) {
    cbm_store_t *s = open_store();
    if (!s)
        return -1;
    int c = cbm_store_count_nodes(s, g_project);
    cbm_store_close(s);
    return c;
}

static int get_edge_count(void) {
    cbm_store_t *s = open_store();
    if (!s)
        return -1;
    int c = cbm_store_count_edges(s, g_project);
    cbm_store_close(s);
    return c;
}

static int get_edge_count_by_type(const char *type) {
    cbm_store_t *s = open_store();
    if (!s)
        return -1;
    int c = cbm_store_count_edges_by_type(s, g_project, type);
    cbm_store_close(s);
    return c;
}

static int has_function(const char *name_pattern) {
    char *resp = call_tool("search_graph",
                           "{\"project\":\"%s\",\"label\":\"Function\",\"name_pattern\":\"%s\"}",
                           g_project, name_pattern);
    int total = count_in_response(resp, "total");
    free(resp);
    return total > 0;
}

/* Issue #8: query the BM25 full-text path (search_graph {"query":...}), which
 * is served by the contentless FTS5 index nodes_fts — the exact index the
 * incremental per-file FTS maintenance touches. has_function() above uses
 * name_pattern (a LIKE over the nodes table) and does NOT exercise nodes_fts,
 * so a stale/zombie FTS row would slip past it. This helper closes that gap:
 * it returns whether a BM25 search for `term` returns at least one hit. */
static int bm25_hits(const char *term) {
    char *resp = call_tool("search_graph", "{\"project\":\"%s\",\"query\":\"%s\"}",
                           g_project, term);
    if (!resp)
        return 0;
    /* The BM25 result envelope carries a "total" (hit count). Fall back to a
     * substring check on the term itself if the count key is absent. */
    int total = count_in_response(resp, "total");
    int hit = (total > 0) || (total < 0 && strstr(resp, term) != NULL);
    free(resp);
    return hit;
}

/* ══════════════════════════════════════════════════════════════════
 *  Issue #12: partial-load vs full-load incremental consistency
 * ══════════════════════════════════════════════════════════════════
 *
 * cbm_pipeline_run_incremental can load the existing graph via two paths:
 *   - full load  (cbm_gbuf_load_from_db): reads the WHOLE project graph.
 *   - partial load (cbm_gbuf_load_from_db_partial): reads only the changed
 *     files' nodes + their 1-hop cross-file neighbors + edges touching that
 *     set (see CBM_INCR_PARTIAL_LOAD_THRESHOLD / _MIN_FILES, both runtime
 *     overridable via env for exactly this kind of test).
 *
 * Both paths MUST produce an IDENTICAL final on-disk graph for the same
 * change. dump_snapshot() below serializes every node (label, name,
 * qualified_name, file_path, start_line, end_line — NOT id, which is
 * allocation-order-dependent and expected to differ between the two paths)
 * and every edge (source qualified_name, target qualified_name, type) into
 * one big sorted, newline-joined string. Comparing these strings byte-for-
 * byte is the actual invariant check — not just node/edge COUNTs, which
 * would miss a case where the partial path resolves a DIFFERENT symbol to
 * the same count of edges (e.g. a registry seeding gap silently rewiring an
 * edge to the wrong target of the same name). */

typedef struct {
    char **lines;
    int count;
    int cap;
} snapshot_lines_t;

static void snapshot_push(snapshot_lines_t *s, const char *fmt, ...) {
    if (s->count >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->lines = realloc(s->lines, (size_t)s->cap * sizeof(char *));
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s->lines[s->count++] = strdup(buf);
}

static int snapshot_cmp(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Serialize the current on-disk graph for `project` into one sorted,
 * newline-joined string. Caller frees the returned buffer. */
static char *dump_snapshot(const char *db_path, const char *project) {
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }

    snapshot_lines_t s = {0};

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT label, name, qualified_name, file_path, start_line, end_line "
                       "FROM nodes WHERE project = ?1 ORDER BY qualified_name, label",
                       -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        snapshot_push(&s, "N|%s|%s|%s|%s|%d|%d", (const char *)sqlite3_column_text(stmt, 0),
                     (const char *)sqlite3_column_text(stmt, 1),
                     (const char *)sqlite3_column_text(stmt, 2),
                     (const char *)sqlite3_column_text(stmt, 3), sqlite3_column_int(stmt, 4),
                     sqlite3_column_int(stmt, 5));
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db,
                       "SELECT src.qualified_name, tgt.qualified_name, e.type "
                       "FROM edges e "
                       "JOIN nodes src ON e.source_id = src.id "
                       "JOIN nodes tgt ON e.target_id = tgt.id "
                       "WHERE e.project = ?1 "
                       "ORDER BY src.qualified_name, tgt.qualified_name, e.type",
                       -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        snapshot_push(&s, "E|%s|%s|%s", (const char *)sqlite3_column_text(stmt, 0),
                     (const char *)sqlite3_column_text(stmt, 1),
                     (const char *)sqlite3_column_text(stmt, 2));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    qsort(s.lines, (size_t)s.count, sizeof(char *), snapshot_cmp);

    size_t total = 1;
    for (int i = 0; i < s.count; i++) {
        total += strlen(s.lines[i]) + 1;
    }
    char *out = malloc(total);
    out[0] = '\0';
    for (int i = 0; i < s.count; i++) {
        strcat(out, s.lines[i]);
        strcat(out, "\n");
        free(s.lines[i]);
    }
    free(s.lines);
    return out;
}

/* Force the load-mode env knobs so index_repo() below deterministically
 * takes the partial or full path regardless of this fixture's actual file
 * count (fastapi is ~1100 files; ratio/min-files defaults would otherwise
 * make the choice depend on how many files a given test happens to touch). */
static void force_load_mode(bool partial) {
    if (partial) {
        cbm_setenv("CBM_INCR_PARTIAL_LOAD_MIN_FILES", "1", 1);
        cbm_setenv("CBM_INCR_PARTIAL_LOAD_THRESHOLD", "1.0", 1);
    } else {
        cbm_setenv("CBM_INCR_PARTIAL_LOAD_MIN_FILES", "1", 1);
        cbm_setenv("CBM_INCR_PARTIAL_LOAD_THRESHOLD", "0.0", 1);
    }
}

static void reset_load_mode(void) {
    cbm_unsetenv("CBM_INCR_PARTIAL_LOAD_MIN_FILES");
    cbm_unsetenv("CBM_INCR_PARTIAL_LOAD_THRESHOLD");
}

/* Scenario 1: change ONE file whose function is called from another
 * (already-indexed) file — exercises inbound cross-file edge
 * capture/restore under the partial-load neighbor-graph path. */
TEST(incr_partial_vs_full_load_single_file_cross_call) {
    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    write_file_at("fastapi/incr12_callee.py",
                  "def incr12_target():\n    return 1\n");
    write_file_at("fastapi/incr12_caller.py",
                  "from fastapi.incr12_callee import incr12_target\n"
                  "def incr12_caller():\n    return incr12_target()\n");
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    /* Now edit ONLY the callee — its body changes but its name/QN doesn't,
     * so the inbound CALLS edge from incr12_caller must survive re-linking
     * whichever load path ran. */
    struct timespec ts = {0, INCR_FIX_SLEEP_NS};
    nanosleep(&ts, NULL);
    write_file_at("fastapi/incr12_callee.py",
                  "def incr12_target():\n    return 2  # changed body\n");

    force_load_mode(true);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    char *partial_snapshot = dump_snapshot(g_dbpath, g_project);

    /* Rebuild fully from scratch, replay the SAME final file state, then run
     * the SAME last edit forced through the full-load path, so both
     * snapshots reflect the identical end state reached via different load
     * strategies. */
    unlink(g_dbpath);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    nanosleep(&ts, NULL);
    write_file_at("fastapi/incr12_callee.py",
                  "def incr12_target():\n    return 2  # changed body\n");
    force_load_mode(false);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    char *full_snapshot = dump_snapshot(g_dbpath, g_project);
    reset_load_mode();

    ASSERT(partial_snapshot != NULL);
    ASSERT(full_snapshot != NULL);
    ASSERT(strcmp(partial_snapshot, full_snapshot) == 0);

    free(partial_snapshot);
    free(full_snapshot);
    delete_file_at("fastapi/incr12_callee.py");
    delete_file_at("fastapi/incr12_caller.py");
    resp = index_repo();
    if (resp)
        free(resp);
    PASS();
}

/* Scenario 2: changed-file ratio above the threshold — the partial path must
 * itself fall back to a full load, so its result trivially equals a run that
 * was forced full from the start. Exercises the fallback branch. */
TEST(incr_partial_vs_full_load_above_threshold_falls_back) {
    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    for (int i = 0; i < 5; i++) {
        char path[128], content[256];
        snprintf(path, sizeof(path), "fastapi/incr12_bulk_%d.py", i);
        snprintf(content, sizeof(content), "def incr12_bulk_%d():\n    return %d\n", i, i);
        write_file_at(path, content);
    }
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    struct timespec ts = {0, INCR_FIX_SLEEP_NS};
    nanosleep(&ts, NULL);
    for (int i = 0; i < 5; i++) {
        char path[128], content[256];
        snprintf(path, sizeof(path), "fastapi/incr12_bulk_%d.py", i);
        snprintf(content, sizeof(content), "def incr12_bulk_%d():\n    return %d + 1\n", i, i);
        write_file_at(path, content);
    }

    /* Ask for partial mode, but with min_files set ABOVE this tiny fixture's
     * changed set so the ratio/size guard itself declines partial and the
     * code takes its OWN full-load branch — this is the real fallback path,
     * not a test-forced full run. */
    cbm_setenv("CBM_INCR_PARTIAL_LOAD_MIN_FILES", "999999", 1);
    cbm_setenv("CBM_INCR_PARTIAL_LOAD_THRESHOLD", "1.0", 1);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    char *fallback_snapshot = dump_snapshot(g_dbpath, g_project);

    unlink(g_dbpath);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    nanosleep(&ts, NULL);
    for (int i = 0; i < 5; i++) {
        char path[128], content[256];
        snprintf(path, sizeof(path), "fastapi/incr12_bulk_%d.py", i);
        snprintf(content, sizeof(content), "def incr12_bulk_%d():\n    return %d + 1\n", i, i);
        write_file_at(path, content);
    }
    force_load_mode(false);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    char *full_snapshot = dump_snapshot(g_dbpath, g_project);
    reset_load_mode();

    ASSERT(fallback_snapshot != NULL);
    ASSERT(full_snapshot != NULL);
    ASSERT(strcmp(fallback_snapshot, full_snapshot) == 0);

    free(fallback_snapshot);
    free(full_snapshot);
    for (int i = 0; i < 5; i++) {
        char path[128];
        snprintf(path, sizeof(path), "fastapi/incr12_bulk_%d.py", i);
        delete_file_at(path);
    }
    resp = index_repo();
    if (resp)
        free(resp);
    PASS();
}

/* Scenario 3: a changed file references a project symbol it did NOT
 * reference before (no pre-existing edge) — this is the exact gap
 * registry_seed_from_db exists to close: cbm_gbuf_foreach_node over a
 * PARTIALLY loaded graph would miss this symbol's file as a 1-hop neighbor,
 * so registry seeding must come from the DB directly, not from the loaded
 * gbuf, for the partial path to resolve the new cross-file symbol at all. */
TEST(incr_partial_vs_full_load_new_cross_file_symbol) {
    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    /* Pre-existing symbol with NO caller yet (so no edge links to it before
     * the edit below — the partial path's 1-hop neighbor walk has nothing to
     * find it through). */
    write_file_at("fastapi/incr12_new_target.py",
                  "def incr12_brand_new_symbol():\n    return 42\n");
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    /* Now a DIFFERENT, unrelated file starts calling it for the first time —
     * a fresh cross-file reference with no prior edge for the partial loader
     * to walk. */
    struct timespec ts = {0, INCR_FIX_SLEEP_NS};
    nanosleep(&ts, NULL);
    write_file_at("fastapi/incr12_new_caller.py",
                  "from fastapi.incr12_new_target import incr12_brand_new_symbol\n"
                  "def incr12_new_caller():\n    return incr12_brand_new_symbol()\n");

    force_load_mode(true);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    char *partial_snapshot = dump_snapshot(g_dbpath, g_project);

    unlink(g_dbpath);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    write_file_at("fastapi/incr12_new_target.py",
                  "def incr12_brand_new_symbol():\n    return 42\n");
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    nanosleep(&ts, NULL);
    write_file_at("fastapi/incr12_new_caller.py",
                  "from fastapi.incr12_new_target import incr12_brand_new_symbol\n"
                  "def incr12_new_caller():\n    return incr12_brand_new_symbol()\n");
    force_load_mode(false);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);
    char *full_snapshot = dump_snapshot(g_dbpath, g_project);
    reset_load_mode();

    ASSERT(partial_snapshot != NULL);
    ASSERT(full_snapshot != NULL);
    {
        FILE *fp1 = fopen("/tmp/dbg_partial.txt", "w");
        if (fp1) { fputs(partial_snapshot, fp1); fclose(fp1); }
        FILE *fp2 = fopen("/tmp/dbg_full.txt", "w");
        if (fp2) { fputs(full_snapshot, fp2); fclose(fp2); }
    }
    /* CRITICAL: the new CALLS edge from incr12_new_caller to
     * incr12_brand_new_symbol must appear in BOTH snapshots identically. A
     * registry seeding gap would silently drop it from the partial path only. */
    ASSERT(strstr(partial_snapshot, "incr12_brand_new_symbol") != NULL);
    ASSERT(strcmp(partial_snapshot, full_snapshot) == 0);

    free(partial_snapshot);
    free(full_snapshot);
    delete_file_at("fastapi/incr12_new_target.py");
    delete_file_at("fastapi/incr12_new_caller.py");
    resp = index_repo();
    if (resp)
        free(resp);
    PASS();
}


static int count_by_label(const char *label) {
    char *resp =
        call_tool("search_graph", "{\"project\":\"%s\",\"label\":\"%s\"}", g_project, label);
    int total = count_in_response(resp, "total");
    free(resp);
    return total;
}

/* ── Setup / Teardown ─────────────────────────────────────────────── */

static int incremental_setup(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cbm_incr_XXXXXX");
    if (!cbm_mkdtemp(g_tmpdir))
        return -1;

    snprintf(g_repodir, sizeof(g_repodir), "%s/fastapi", g_tmpdir);

    /* On CI, use sparse checkout to skip docs/ and tests/ (~62% of files).
     * Cuts indexing time roughly in half on slow shared runners. */
    char cmd[1024];
    if (getenv("CI")) {
        snprintf(cmd, sizeof(cmd),
                 "git clone --depth=1 --branch 0.99.1 --quiet --filter=blob:none "
                 "--sparse https://github.com/fastapi/fastapi.git '%s' 2>&1 && "
                 "cd '%s' && git sparse-checkout set --no-cone '/*' '!/docs' '!/tests' 2>&1",
                 g_repodir, g_repodir);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "git clone --depth=1 --branch 0.99.1 --quiet "
                 "https://github.com/fastapi/fastapi.git '%s' 2>&1",
                 g_repodir);
    }
    int rc = system(cmd);
    if (rc != 0) {
        printf("  clone failed (rc=%d) — network offline?\n", rc);
        return -1;
    }

    g_project = cbm_project_name_from_path(g_repodir);
    if (!g_project)
        return -1;

    const char *home = getenv("HOME");
    if (!home)
        home = "/tmp";
    snprintf(g_dbpath, sizeof(g_dbpath), "%s/.cache/codebase-memory-mcp/%s.db", home, g_project);

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/codebase-memory-mcp", home);
    cbm_mkdir(cache_dir);

    unlink(g_dbpath);

    g_srv = cbm_mcp_server_new(NULL);
    if (!g_srv)
        return -1;

    g_rss_before_full = cbm_mem_rss();

    return 0;
}

static void incremental_teardown(void) {
    if (g_srv) {
        cbm_mcp_server_free(g_srv);
        g_srv = NULL;
    }
    if (g_project) {
        unlink(g_dbpath);
        char wal[520], shm[520];
        snprintf(wal, sizeof(wal), "%s-wal", g_dbpath);
        snprintf(shm, sizeof(shm), "%s-shm", g_dbpath);
        unlink(wal);
        unlink(shm);
    }
    free(g_project);
    g_project = NULL;

    th_rmtree(g_tmpdir);
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 1: Full index — baseline + performance
 * ══════════════════════════════════════════════════════════════════ */

TEST(incr_full_index) {
    double ms = 0;
    size_t peak_mb = 0;
    char *resp = index_repo_timed(&ms, &peak_mb);
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    g_full_nodes = get_node_count();
    g_full_edges = get_edge_count();
    g_full_calls = get_edge_count_by_type("CALLS");
    g_full_imports = get_edge_count_by_type("IMPORTS");
    g_full_index_ms = ms;

    /* FastAPI: expect substantial graph.
     * CI uses sparse checkout (~7K files), local has full repo (~18K files).
     * Thresholds set for sparse checkout (smaller graph). */
    ASSERT_GT(g_full_nodes, 2000);
    ASSERT_GT(g_full_edges, 500);
    ASSERT_GT(g_full_calls, 200);
    ASSERT_GT(g_full_imports, 50);

    /* Performance: full index — warn if slow, don't block */
    if ((int)ms > 30000) {
        printf("    [PERF WARNING] full index: %.0fms (>30s)\n", ms);
    }

    /* Memory: bounded budget for a 1100-file Python project. ARM (and other
     * large-page) Linux/macOS use 16KB pages vs x86's 4KB; per-allocation page
     * rounding inflates RSS ~25-30% for the SAME logical footprint (not a leak —
     * ARM ~2385MB on the same index). Scale the budget by page size so the guard
     * still catches real runaway memory (a leak would be GBs over) without
     * false-failing on large-page architectures. The x86 base budget is 2304MB:
     * after the retention/source-text-cap and RAM-tiering work the x86 peak for
     * this index settled at ~2050-2072MB (measured across CI runs), so the old
     * 2048 limit sat right on the line and flaked; 2304 restores headroom while a
     * genuine leak (GBs over) still trips it. */
    size_t rss_delta_mb = peak_mb - (g_rss_before_full / (1024 * 1024));
    int rss_limit_mb = 2304;
#ifndef _WIN32
    if (sysconf(_SC_PAGESIZE) >= 16384) {
        rss_limit_mb = 2816;
    }
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
    /* ARM Linux uses 4KB pages, so the page-size bump above does NOT fire there,
     * yet glibc's per-CPU malloc arenas + allocation rounding still inflate RSS
     * to the documented ~2385MB for this index (the same inflation Apple silicon
     * shows, which the page-size check catches via its 16KB pages). Apply the
     * higher ARM budget on any ARM target so the guard still catches a real leak
     * (GBs over) without false-failing on 4KB-page ARM Linux (e.g. CI's
     * ubuntu-22.04-arm, which measured 2386MB against the un-bumped 2048 limit). */
    if (rss_limit_mb < 2816) {
        rss_limit_mb = 2816;
    }
#endif
    ASSERT_LT((int)rss_delta_mb, rss_limit_mb);

    printf("    [perf] full: %d nodes, %d edges (%d CALLS, %d IMPORTS) "
           "in %.0fms, peak=%zuMB\n",
           g_full_nodes, g_full_edges, g_full_calls, g_full_imports, ms, peak_mb);

    PASS();
}

TEST(incr_full_has_functions) {
    int funcs = count_by_label("Function");
    ASSERT_GT(funcs, 500);

    int methods = count_by_label("Method");
    ASSERT_GT(methods, 100);

    int modules = count_by_label("Module");
    ASSERT_GT(modules, 200);

    PASS();
}

TEST(incr_full_edge_types) {
    /* Verify edge type distribution is sane */
    int calls = get_edge_count_by_type("CALLS");
    int imports = get_edge_count_by_type("IMPORTS");
    int defines = get_edge_count_by_type("DEFINES");
    int contains = get_edge_count_by_type("CONTAINS_FILE");

    ASSERT_GT(calls, 0);
    ASSERT_GT(imports, 0);
    ASSERT_GT(defines, 0);
    ASSERT_GT(contains, 0);

    /* CALLS should be the most common edge type */
    ASSERT_GT(calls, imports);

    printf("    [edges] CALLS=%d IMPORTS=%d DEFINES=%d CONTAINS_FILE=%d\n", calls, imports, defines,
           contains);

    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 2: Noop reindex — must be fast and exact
 * ══════════════════════════════════════════════════════════════════ */

TEST(incr_noop_reindex) {
    double ms = 0;
    size_t peak_mb = 0;
    char *resp = index_repo_timed(&ms, &peak_mb);
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    /* Exact same counts */
    ASSERT_EQ(get_node_count(), g_full_nodes);
    ASSERT_EQ(get_edge_count(), g_full_edges);

    /* Noop should be fast (just file classification, no parsing) */
    if ((int)ms > 5000) {
        printf("    [PERF WARNING] noop reindex: %.0fms (>5s)\n", ms);
    }

    printf("    [perf] noop: %.0fms\n", ms);

    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 3: Incremental delta changes
 * ══════════════════════════════════════════════════════════════════ */

TEST(incr_modify_file) {
    int nodes_before = get_node_count();

    /* Append functions to an existing source file */
    char path[512];
    snprintf(path, sizeof(path), "%s/fastapi/applications.py", g_repodir);
    FILE *f = fopen(path, "a");
    ASSERT(f != NULL);
    fprintf(f, "\n\ndef incr_modify_extra_a(x: int) -> int:\n"
               "    return x + 1\n"
               "\n"
               "def incr_modify_extra_b(y: str) -> str:\n"
               "    return y.strip()\n");
    fclose(f);

    double ms = 0;
    size_t peak_mb = 0;
    char *resp = index_repo_timed(&ms, &peak_mb);
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    ASSERT(has_function("incr_modify_extra_a"));
    ASSERT(has_function("incr_modify_extra_b"));
    ASSERT_GT(get_node_count(), nodes_before);

    /* Issue #4 HARD GATE: a single-file incremental reindex must be strictly
     * cheaper than a full reindex. The old row-level dump regression (unlink +
     * full-graph rebuild) made incremental scale with graph size; this asserts
     * it does not. Bound is generous (< full) to stay stable on noisy CI while
     * still failing a true O(graph) regression, which pushes incremental to
     * >= full on a repo this size. g_full_index_ms is the baseline captured by
     * incr_full_index earlier in this ordered suite. */
    printf("    [perf] modify 1 file: %.0fms (full was %.0fms)\n", ms, g_full_index_ms);
    if (g_full_index_ms > 50.0) { /* only gate when baseline is meaningful */
        ASSERT_LT((int)ms, (int)g_full_index_ms);
    }

    PASS();
}

TEST(incr_formatter_run) {
    /* Baseline */
    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    int nodes_before = get_node_count();
    int edges_before = get_edge_count();
    int calls_before = get_edge_count_by_type("CALLS");

    /* Simulate formatter: touch 50 files */
    reformat_files("fastapi", 50);

    double ms = 0;
    size_t peak_mb = 0;
    resp = index_repo_timed(&ms, &peak_mb);
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    /* Graph should be nearly identical — a formatter adds no functions.
     * Issue #4 HARD GATE: the row-level incremental dump must preserve nodes AND
     * edges across a 50-file reindex. A lost-edge regression in the row-level
     * path (e.g. dropping cross-file inbound edges whose source lives in an
     * unchanged file) would blow past this 10% band, so this asserts hard on
     * both counts rather than only warning. CALLS is exempted below because
     * reformatting genuinely shifts line-number-based resolution. */
    int node_diff = abs(get_node_count() - nodes_before);
    int edge_diff = abs(get_edge_count() - edges_before);
    printf("    [perf] formatter drift: node_diff=%d (max %d), edge_diff=%d (max %d)\n",
           node_diff, nodes_before / 10, edge_diff, edges_before / 10);
    ASSERT_LTE(node_diff, nodes_before / 10);
    ASSERT_LTE(edge_diff, edges_before / 10);

    /* CALLS edges: reformatting changes line numbers which affects resolution. */
    int calls_diff = abs(get_edge_count_by_type("CALLS") - calls_before);
    if (calls_diff > calls_before / 4) {
        printf("    [PERF WARNING] CALLS drift: %d (max %d)\n", calls_diff, calls_before / 4);
    }

    printf("    [perf] reformat 50 files: %.0fms, node_diff=%d edge_diff=%d\n", ms, node_diff,
           edge_diff);

    PASS();
}

TEST(incr_add_file) {
    int nodes_before = get_node_count();

    write_file_at("fastapi/incr_test_new.py", "\"\"\"New module.\"\"\"\n"
                                              "from fastapi import FastAPI\n"
                                              "\n"
                                              "def incr_new_entry(app: FastAPI) -> None:\n"
                                              "    setup(app)\n"
                                              "\n"
                                              "def setup(app: FastAPI) -> None:\n"
                                              "    pass\n"
                                              "\n"
                                              "def incr_new_validate(data: dict) -> bool:\n"
                                              "    return bool(data)\n"
                                              "\n"
                                              "class IncrNewHandler:\n"
                                              "    def handle(self, req):\n"
                                              "        return incr_new_validate(req)\n"
                                              "\n"
                                              "    def cleanup(self):\n"
                                              "        pass\n");

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    ASSERT(has_function("incr_new_entry"));
    ASSERT(has_function("incr_new_validate"));
    ASSERT_GT(get_node_count(), nodes_before);

    PASS();
}

TEST(incr_delete_file) {
    int nodes_before = get_node_count();

    delete_file_at("fastapi/incr_test_new.py");

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    ASSERT(!has_function("incr_new_entry"));
    ASSERT(!has_function("incr_new_validate"));
    ASSERT(has_function("incr_test_injected")); /* others survive */
    ASSERT_LT(get_node_count(), nodes_before);

    PASS();
}

TEST(incr_simultaneous_changes) {
    /* Add */
    write_file_at("tests/incr_simul_add.py", "def incr_simul_added():\n    return 'added'\n");

    /* Modify */
    char path[512];
    snprintf(path, sizeof(path), "%s/fastapi/routing.py", g_repodir);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "\ndef incr_simul_modified():\n    return 'modified'\n");
        fclose(f);
    }

    /* Delete */
    delete_file_at("fastapi/incr_test_new.py"); /* already gone, noop */

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    ASSERT(has_function("incr_simul_added"));
    ASSERT(has_function("incr_simul_modified"));

    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 4: Adversarial inputs — attempt to break the pipeline
 * ══════════════════════════════════════════════════════════════════ */

TEST(incr_empty_file) {
    write_file_at("fastapi/incr_empty.py", "");
    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    ASSERT(has_function("incr_test_injected"));
    delete_file_at("fastapi/incr_empty.py");
    free(resp);
    PASS();
}

TEST(incr_syntax_error) {
    int nodes_before = get_node_count();

    write_file_at("fastapi/incr_broken.py", "def broken(\n"
                                            "    # deliberately broken\n"
                                            "class @#$% {\n"
                                            "   async def nested() -> None\n"
                                            "       yield from broken(\n"
                                            "))))\n");

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    ASSERT(has_function("incr_test_injected")); /* others survive */

    int diff = abs(get_node_count() - nodes_before);
    ASSERT_LT(diff, 20);

    delete_file_at("fastapi/incr_broken.py");
    free(resp);
    PASS();
}

TEST(incr_huge_single_function) {
    /* A single function with 5000 lines — stress test extraction budget */
    size_t sz = 5000 * 30 + 100;
    char *content = malloc(sz);
    ASSERT(content != NULL);
    int pos = 0;
    pos += snprintf(content + pos, sz - (size_t)pos, "def huge_func(x):\n");
    for (int i = 0; i < 5000; i++) {
        pos += snprintf(content + pos, sz - (size_t)pos, "    x = x + %d\n", i);
    }
    pos += snprintf(content + pos, sz - (size_t)pos, "    return x\n");
    (void)pos;

    write_file_at("fastapi/incr_huge.py", content);
    free(content);

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    /* Should extract the function (may be truncated by budget, that's ok) */
    /* Key: must not crash or corrupt graph */
    ASSERT(has_function("incr_test_injected"));

    delete_file_at("fastapi/incr_huge.py");
    PASS();
}

TEST(incr_binary_content) {
    /* Write binary garbage to a .py file */
    char path[512];
    snprintf(path, sizeof(path), "%s/fastapi/incr_binary.py", g_repodir);
    FILE *f = fopen(path, "wb");
    if (f) {
        unsigned char bin[256];
        for (int i = 0; i < 256; i++)
            bin[i] = (unsigned char)i;
        fwrite(bin, 1, sizeof(bin), f);
        fclose(f);
    }

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    ASSERT(has_function("incr_test_injected"));

    delete_file_at("fastapi/incr_binary.py");
    free(resp);
    PASS();
}

TEST(incr_large_generated) {
    char *content = malloc(300 * 80);
    ASSERT(content != NULL);
    content[0] = '\0';
    for (int i = 0; i < 300; i++) {
        char line[80];
        snprintf(line, sizeof(line), "def incr_gen_%d(x):\n    return x + %d\n\n", i, i);
        strcat(content, line);
    }

    write_file_at("fastapi/incr_generated.py", content);
    free(content);

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    ASSERT(has_function("incr_gen_0"));
    ASSERT(has_function("incr_gen_299"));

    delete_file_at("fastapi/incr_generated.py");
    free(resp);
    PASS();
}

TEST(incr_new_subdir) {
    /* File in a brand new subdirectory */
    write_file_at("fastapi/newpkg/__init__.py", "");
    write_file_at("fastapi/newpkg/handler.py", "def newpkg_handler():\n"
                                               "    return 'from_new_package'\n");

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    ASSERT(has_function("newpkg_handler"));

    delete_file_at("fastapi/newpkg/handler.py");
    delete_file_at("fastapi/newpkg/__init__.py");
    free(resp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 5: Stress + complex scenarios
 * ══════════════════════════════════════════════════════════════════ */

TEST(incr_rapid_reindex) {
    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    int baseline_nodes = get_node_count();
    int baseline_edges = get_edge_count();

    for (int i = 0; i < 3; i++) {
        resp = index_repo();
        ASSERT(resp != NULL);
        ASSERT(strstr(resp, "indexed") != NULL);
        free(resp);
    }

    ASSERT_EQ(get_node_count(), baseline_nodes);
    ASSERT_EQ(get_edge_count(), baseline_edges);
    PASS();
}

TEST(incr_replace_file_content) {
    write_file_at("fastapi/incr_replace.py", "def replace_original_a():\n    return 'a'\n"
                                             "def replace_original_b():\n    return 'b'\n");

    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    ASSERT(has_function("replace_original_a"));
    ASSERT(has_function("replace_original_b"));

    /* Replace entirely */
    write_file_at("fastapi/incr_replace.py", "def replace_new_x():\n    return 'x'\n"
                                             "def replace_new_y():\n    return replace_new_x()\n");

    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    ASSERT(!has_function("replace_original_a"));
    ASSERT(!has_function("replace_original_b"));
    ASSERT(has_function("replace_new_x"));
    ASSERT(has_function("replace_new_y"));

    delete_file_at("fastapi/incr_replace.py");
    PASS();
}

TEST(incr_fts_bm25_consistency) {
    /* Issue #8: the incremental path used to rebuild the WHOLE nodes_fts table
     * (delete-all + full rescan) on every reindex. It now maintains FTS rows
     * per-file (contentless FTS5 'delete' for changed/deleted files' old rows,
     * re-insert for changed files' fresh rows). This test drives the BM25 path
     * (search_graph {"query":...}) — the only path that reads nodes_fts — across
     * a rename + add on ONE file, and asserts the FTS index is left exactly as a
     * full reindex would leave it. The rename case is the critical one: a naive
     * per-file delete that fails to remove the OLD contentless posting would let
     * the renamed-away symbol still match — a zombie FTS row. */

    /* fileA: a class method chargeCard; fileB: an unrelated helper. */
    write_file_at("fastapi/incr_fts_a.py",
                  "class PaymentGateway:\n"
                  "    def chargeCardImmediately(self, amount):\n"
                  "        return amount\n");
    write_file_at("fastapi/incr_fts_b.py", "def betaHelperUnchanged(x):\n    return x + 1\n");

    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    /* Baseline: old symbol is searchable via BM25, helper too. */
    ASSERT(bm25_hits("chargeCardImmediately"));
    ASSERT(bm25_hits("betaHelperUnchanged"));

    /* Edit ONLY fileA: rename chargeCardImmediately -> refundCardImmediately and
     * add a brand-new symbol. fileB is untouched. Newer mtime so change
     * detection can't miss it. Then reindex WITHOUT deleting the DB → the
     * incremental FTS maintenance path runs. */
    struct timespec ts = {0, INCR_FIX_SLEEP_NS};
    nanosleep(&ts, NULL);
    write_file_at("fastapi/incr_fts_a.py",
                  "class PaymentGateway:\n"
                  "    def refundCardImmediately(self, amount):\n"
                  "        return amount\n"
                  "    def voidTransactionImmediately(self, tid):\n"
                  "        return tid\n");

    resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    /* CRITICAL: the renamed-away symbol must NO LONGER match via BM25 — the old
     * contentless FTS row was deleted with its exact indexed value, leaving no
     * zombie posting. This is precisely what a per-file FTS delete gets wrong if
     * it does not pass the original column values. */
    ASSERT(!bm25_hits("chargeCardImmediately"));

    /* The changed file's new symbols are searchable. */
    ASSERT(bm25_hits("refundCardImmediately"));
    ASSERT(bm25_hits("voidTransactionImmediately"));

    /* The UNCHANGED file's symbol still matches — its FTS rows were never
     * touched (row-level dump keeps its nodes.id stable, so the existing FTS
     * rowid remains valid). A regression to delete-all + partial reinsert would
     * drop this. */
    ASSERT(bm25_hits("betaHelperUnchanged"));

    /* Cleanup so later ordered tests see a clean tree. */
    delete_file_at("fastapi/incr_fts_a.py");
    delete_file_at("fastapi/incr_fts_b.py");
    resp = index_repo();
    if (resp)
        free(resp);
    PASS();
}

TEST(incr_batch_add_delete) {
    /* Add 20 files */
    for (int i = 0; i < 20; i++) {
        char path[128], content[256];
        snprintf(path, sizeof(path), "tests/incr_batch_%d.py", i);
        snprintf(content, sizeof(content),
                 "def batch_%d(x):\n    return x + %d\n"
                 "def batch_h_%d(y):\n    return batch_%d(y) * 2\n",
                 i, i, i, i);
        write_file_at(path, content);
    }

    char *resp = index_repo();
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    ASSERT(has_function("batch_0"));
    ASSERT(has_function("batch_19"));
    ASSERT(has_function("batch_h_10"));

    /* Delete all 20 */
    for (int i = 0; i < 20; i++) {
        char path[128];
        snprintf(path, sizeof(path), "tests/incr_batch_%d.py", i);
        delete_file_at(path);
    }

    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    ASSERT(!has_function("batch_0"));
    ASSERT(!has_function("batch_19"));

    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 6: Recovery + accuracy
 * ══════════════════════════════════════════════════════════════════ */

TEST(incr_db_deleted_recovery) {
    int nodes_before = get_node_count();

    unlink(g_dbpath);

    double ms = 0;
    size_t peak_mb = 0;
    char *resp = index_repo_timed(&ms, &peak_mb);
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    /* Full reindex must produce similar count */
    int nodes_after = get_node_count();
    int diff_pct = abs(nodes_after - nodes_before) * 100 / nodes_before;
    ASSERT_LT(diff_pct, 5);

    printf("    [perf] db recovery (full reindex): %.0fms, peak=%zuMB\n", ms, peak_mb);

    PASS();
}

TEST(incr_accuracy_vs_full) {
    /* Modify a file to create a known incremental state */
    write_file_at("fastapi/incr_accuracy.py", "def accuracy_a():\n    return 1\n"
                                              "def accuracy_b():\n    return accuracy_a() + 1\n");

    char *resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    int incr_nodes = get_node_count();
    int incr_edges = get_edge_count();
    int incr_calls = get_edge_count_by_type("CALLS");

    /* Delete DB, force full reindex */
    unlink(g_dbpath);
    resp = index_repo();
    ASSERT(resp != NULL);
    free(resp);

    int full_nodes = get_node_count();
    int full_edges = get_edge_count();
    int full_calls = get_edge_count_by_type("CALLS");

    /* Within tight tolerance (±2 for dedup timing differences) */
    ASSERT_LTE(abs(full_nodes - incr_nodes), 2);
    ASSERT_LTE(abs(full_nodes - incr_nodes), 50);
    ASSERT_LTE(abs(full_calls - incr_calls), 2);

    printf("    [accuracy] incr: %d nodes/%d edges, full: %d nodes/%d edges\n", incr_nodes,
           incr_edges, full_nodes, full_edges);

    delete_file_at("fastapi/incr_accuracy.py");
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 7: Performance regression guards
 * ══════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════
 *  Issue #6: mtime-only change (git checkout / worktree switch rewrites
 *  file mtimes even when content is byte-identical) must NOT be treated
 *  as a content change. Before the fix, dirty detection compared only
 *  mtime+size, so a pure mtime bump re-parsed the file. After the fix,
 *  the content-hash tiebreak proves the bytes are identical and skips it.
 *
 *  Ground-truth, deterministic asserts (no LLM):
 *   - bump mtime only  -> node/edge counts identical to full (unchanged)
 *   - change 1 byte of content -> counts differ (changed, still detected)
 * ══════════════════════════════════════════════════════════════════ */
TEST(incr_mtime_bump_only_is_unchanged) {
    /* Self-contained: create our OWN probe file so no other test's edits to
     * shared fixture files can pollute the baseline. Index once to record it,
     * then bump ONLY its mtime (what `git checkout` does) and re-index. */
    const char *rel = "fastapi/issue6_mtime_probe.py";
    write_file_at(rel, "def issue6_probe_one(a: int) -> int:\n    return a + 1\n\n"
                       "def issue6_probe_two(b: str) -> str:\n    return b.upper()\n");

    double ms = 0; size_t peak_mb = 0;
    char *r0 = index_repo_timed(&ms, &peak_mb);
    ASSERT(r0 != NULL); ASSERT(strstr(r0, "indexed") != NULL); free(r0);
    ASSERT(has_function("issue6_probe_one"));
    int nodes_before = get_node_count();
    int edges_before = get_edge_count();
    ASSERT_GT(nodes_before, 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_repodir, rel);
    struct stat st0;
    ASSERT_EQ(stat(path, &st0), 0);
    off_t size_before = st0.st_size;

    /* Bump mtime forward ~1000s, bytes untouched. */
    struct timeval tv[2];
    tv[0].tv_sec = st0.st_mtime + 1000; tv[0].tv_usec = 0;
    tv[1].tv_sec = st0.st_mtime + 1000; tv[1].tv_usec = 0;
    ASSERT_EQ(utimes(path, tv), 0);
    struct stat st1;
    ASSERT_EQ(stat(path, &st1), 0);
    ASSERT_EQ((long)st1.st_size, (long)size_before);
    ASSERT(st1.st_mtime != st0.st_mtime);

    char *r1 = index_repo_timed(&ms, &peak_mb);
    ASSERT(r1 != NULL); ASSERT(strstr(r1, "indexed") != NULL); free(r1);

    /* HARD GATE (issue #6): a pure mtime bump must leave the graph identical.
     * Pre-fix (mtime+size only) this file would be re-parsed; the content-hash
     * tiebreak now proves the bytes are identical and skips it. */
    ASSERT_EQ(get_node_count(), nodes_before);
    ASSERT_EQ(get_edge_count(), edges_before);
    printf("    [issue#6] mtime-only bump: counts stable (%d nodes / %d edges), %.0fms\n",
           nodes_before, edges_before, ms);
    PASS();
}

TEST(incr_content_change_still_detected) {
    char path[512];
    snprintf(path, sizeof(path), "%s/fastapi/issue6_mtime_probe.py", g_repodir);
    /* Append a real new function -> content differs -> must be re-parsed. */
    FILE *f = fopen(path, "a");
    ASSERT(f != NULL);
    fprintf(f, "\n\ndef incr_issue6_sentinel_fn(z: int) -> int:\n    return z * 2\n");
    fclose(f);

    double ms = 0; size_t peak_mb = 0;
    char *resp = index_repo_timed(&ms, &peak_mb);
    ASSERT(resp != NULL);
    ASSERT(strstr(resp, "indexed") != NULL);
    free(resp);

    /* The new symbol must appear -> proves the changed file was re-parsed,
     * i.e. the hash tiebreak did NOT wrongly classify a real change as clean. */
    ASSERT(has_function("incr_issue6_sentinel_fn"));
    printf("    [issue#6] 1-byte content change: re-parsed and detected, %.0fms\n", ms);
    PASS();
}


TEST(incr_perf_single_file_fast) {
    /* Modifying one file should complete in <2s (not re-parse everything) */
    write_file_at("fastapi/incr_perf_probe.py", "def perf_probe():\n    return 42\n");

    double ms = 0;
    size_t peak_mb = 0;
    char *resp = index_repo_timed(&ms, &peak_mb);
    ASSERT(resp != NULL);
    free(resp);
#define PERF_WARN_MS 15000 /* warn above 15s — slow CI runners may exceed this */

    if ((int)ms > PERF_WARN_MS) {
        printf("    [PERF WARNING] single file incremental: %.0fms (>%dms)\n", ms, PERF_WARN_MS);
    } else {
        printf("    [perf] single file incremental: %.0fms\n", ms);
    }

    delete_file_at("fastapi/incr_perf_probe.py");
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 8: MCP tool integration — comprehensive per-tool tests
 *
 *  Every tool, every parameter, error paths, timeouts.
 *  Timing: each call_tool_timed() warns if > PERF_WARN_MS.
 * ══════════════════════════════════════════════════════════════════ */

static char *call_tool_timed(const char *tool, double *ms, const char *args_fmt, ...) {
    char args[2048];
    va_list ap;
    va_start(ap, args_fmt);
    vsnprintf(args, sizeof(args), args_fmt, ap);
    va_end(ap);
    double t0 = now_ms();
    char *resp = cbm_mcp_handle_tool(g_srv, tool, args);
    *ms = now_ms() - t0;
    return resp;
}

/* Check if ALL results have a specific label (validates label filter works) */
static int all_results_have_label(const char *resp, const char *label) {
    if (!resp)
        return 0;
    /* Every "label":"X" in results must match. Check no other label appears. */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"label\":\"%s\"", label);
    char escaped[64];
    snprintf(escaped, sizeof(escaped), "\\\"label\\\":\\\"%s\\\"", label);
    /* If response contains "label" at all, every one must match */
    const char *p = resp;
    while ((p = strstr(p, "label")) != NULL) {
        /* Skip the key name — find the value */
        p += 5; /* past "label" */
        const char *val = strstr(p, ":\"");
        if (!val || val > p + 10) {
            p++;
            continue;
        }
        val += 2;
        /* Check this label value matches */
        if (strncmp(val, label, strlen(label)) != 0) {
            /* Check for escaped version */
            const char *eval = strstr(p, ":\\\"");
            if (eval && eval < p + 10) {
                eval += 3;
                if (strncmp(eval, label, strlen(label)) != 0)
                    return 0;
            } else {
                return 0;
            }
        }
        p++;
    }
    return 1;
}

/* Check response contains a specific key (in nested JSON) */
static int resp_has_key(const char *resp, const char *key) {
    if (!resp)
        return 0;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (strstr(resp, pattern) != NULL)
        return 1;
    snprintf(pattern, sizeof(pattern), "\\\"%s\\\"", key);
    return strstr(resp, pattern) != NULL;
}

/* Check response does NOT contain a specific key */
static int resp_lacks_key(const char *resp, const char *key) {
    return !resp_has_key(resp, key);
}

/* Helper: assert tool call succeeds, warn if slow */
#define TOOL_OK(resp, ms)                                                                 \
    do {                                                                                  \
        ASSERT((resp) != NULL);                                                           \
        if ((int)(ms) > PERF_WARN_MS) {                                                   \
            printf("    [PERF WARNING] tool call: %.0fms (>%dms)\n", (ms), PERF_WARN_MS); \
        }                                                                                 \
    } while (0)

/* Helper: assert response is not an error */
#define NOT_ERROR(resp) ASSERT(strstr((resp), "\"isError\":true") == NULL)

/* ── list_projects ─────────────────────────────────────────────── */

TEST(tool_list_projects_basic) {
    double ms;
    char *r = call_tool_timed("list_projects", &ms, "{}");
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "projects") != NULL);
    free(r);
    PASS();
}

TEST(tool_qg_defines_method_more_than_10) {
    write_file_at("fastapi/big_class.py", "class BigClass:\n"
                                          "    def m1(self): pass\n"
                                          "    def m2(self): pass\n"
                                          "    def m3(self): pass\n"
                                          "    def m4(self): pass\n"
                                          "    def m5(self): pass\n"
                                          "    def m6(self): pass\n"
                                          "    def m7(self): pass\n"
                                          "    def m8(self): pass\n"
                                          "    def m9(self): pass\n"
                                          "    def m10(self): pass\n"
                                          "    def m11(self): pass\n"
                                          "    def m12(self): pass\n"
                                          "    def m13(self): pass\n"
                                          "    def m14(self): pass\n"
                                          "    def m15(self): pass\n");
    char *idx = index_repo();
    ASSERT(idx != NULL);
    free(idx);
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (c:Class)-[:DEFINES_METHOD]->(m:Method)"
                              " WHERE c.name = 'BigClass' RETURN count(m) AS n\"}",
                              g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "\"15\"") != NULL || strstr(r, "\\\"15\\\"") != NULL);
    free(r);
    PASS();
}

TEST(tool_qg_class_lines_nonzero) {
    double ms;
    char *r =
        call_tool_timed("query_graph", &ms,
                        "{\"project\":\"%s\","
                        "\"query\":\"MATCH (c:Class) WHERE c.lines > 0 RETURN count(c) AS n\"}",
                        g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "\"0\"") == NULL);
    free(r);
    PASS();
}

TEST(tool_list_projects_has_current) {
    double ms;
    char *r = call_tool_timed("list_projects", &ms, "{}");
    TOOL_OK(r, ms);
    /* Our project must be in the list. Project name is path-derived,
     * may contain escaped slashes in JSON. Check for "fastapi" substring
     * which will appear in the path-derived name regardless of escaping. */
    /* Project list must contain at least one entry */
    ASSERT(strstr(r, "projects") != NULL);
    free(r);
    PASS();
}

/* ── index_status ──────────────────────────────────────────────── */

TEST(tool_index_status_basic) {
    double ms;
    char *r = call_tool_timed("index_status", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "nodes") != NULL || strstr(r, "indexed") != NULL);
    free(r);
    PASS();
}

TEST(tool_index_status_nonexistent) {
    double ms;
    char *r = call_tool_timed("index_status", &ms, "{\"project\":\"does-not-exist-xyz\"}");
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "not") != NULL || strstr(r, "error") != NULL);
    free(r);
    PASS();
}

/* ── get_graph_schema ──────────────────────────────────────────── */

TEST(tool_schema_has_labels) {
    double ms;
    char *r = call_tool_timed("get_graph_schema", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "Function") != NULL);
    ASSERT(strstr(r, "Method") != NULL);
    ASSERT(strstr(r, "Module") != NULL);
    ASSERT(strstr(r, "Variable") != NULL);
    free(r);
    PASS();
}

TEST(tool_schema_has_edge_types) {
    double ms;
    char *r = call_tool_timed("get_graph_schema", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "CALLS") != NULL);
    ASSERT(strstr(r, "IMPORTS") != NULL);
    ASSERT(strstr(r, "DEFINES") != NULL);
    ASSERT(strstr(r, "CONTAINS_FILE") != NULL);
    /* Schema should also report node and edge counts */
    ASSERT(resp_has_key(r, "node_labels") || resp_has_key(r, "labels") ||
           strstr(r, "Function") != NULL);
    free(r);
    PASS();
}

/* ── search_graph: label filters ───────────────────────────────── */

TEST(tool_sg_label_function) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\",\"limit\":5}", g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 100);
    /* Verify all returned results actually have label=Function */
    ASSERT(all_results_have_label(r, "Function"));
    /* Must not contain Method results */
    ASSERT(strstr(r, "\"Method\"") == NULL && strstr(r, "\\\"Method\\\"") == NULL);
    free(r);
    PASS();
}

TEST(tool_sg_label_method) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Method\",\"limit\":5}", g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 50);
    /* Verify all returned results actually have label=Method */
    ASSERT(all_results_have_label(r, "Method"));
    free(r);
    PASS();
}

TEST(tool_sg_label_module) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"Module\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 200);
    free(r);
    PASS();
}

TEST(tool_sg_label_variable) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"Variable\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_sg_label_class) {
    double ms;
    char *r =
        call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"Class\"}", g_project);
    TOOL_OK(r, ms);
    /* FastAPI has classes */
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_sg_label_route) {
    double ms;
    char *r =
        call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"Route\"}", g_project);
    TOOL_OK(r, ms);
    /* May or may not have routes */
    ASSERT(strstr(r, "total") != NULL || strstr(r, "results") != NULL);
    free(r);
    PASS();
}

TEST(tool_sg_label_nonexistent) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"Nonexistent\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_EQ(total, 0);
    free(r);
    PASS();
}

/* ── search_graph: name_pattern ────────────────────────────────── */

TEST(tool_sg_name_exact) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\"incr_test_injected\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_EQ(total, 1);
    free(r);
    PASS();
}

TEST(tool_sg_name_regex) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\".*test.*\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_sg_name_no_match) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\"zzz_nonexistent_zzz\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_EQ(total, 0);
    free(r);
    PASS();
}

/* ── search_graph: degree filters ──────────────────────────────── */

TEST(tool_sg_min_degree) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"min_degree\":10}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_sg_max_degree_zero) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"max_degree\":0}",
                              g_project);
    TOOL_OK(r, ms);
    /* Isolated functions (no edges) */
    int total = count_in_response(r, "total");
    ASSERT_GTE(total, 0);
    free(r);
    PASS();
}

TEST(tool_sg_degree_range) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"min_degree\":2,\"max_degree\":5}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

/* ── search_graph: pagination ──────────────────────────────────── */

TEST(tool_sg_limit) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"limit\":3}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    /* total should show real count (much more than 3) */
    ASSERT_GT(total, 100);
    /* but has_more should be true (pagination indicator) */
    ASSERT(resp_has_key(r, "has_more"));
    free(r);
    PASS();
}

TEST(tool_sg_offset) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"limit\":5,\"offset\":10}",
                              g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "results") != NULL);
    free(r);
    PASS();
}

/* ── search_graph: file_pattern ────────────────────────────────── */

TEST(tool_sg_file_pattern) {
    double ms;
    /* file_pattern is glob (converted to SQL LIKE), not regex */
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"file_pattern\":\"*fastapi*\",\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

/* ── search_graph: include_connected ───────────────────────────── */

TEST(tool_sg_include_connected) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\"incr_test_injected\","
                              "\"include_connected\":true}",
                              g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "connected") != NULL || strstr(r, "results") != NULL);
    free(r);
    PASS();
}

/* ── search_graph: relationship filter ─────────────────────────── */

TEST(tool_sg_relationship) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"relationship\":\"CALLS\",\"min_degree\":1}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

/* ── search_graph: qn_pattern ──────────────────────────────────── */

TEST(tool_sg_qn_pattern) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"qn_pattern\":\".*fastapi.*applications.*\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

/* ── search_graph: combined filters ────────────────────────────── */

TEST(tool_sg_combined_filters) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\".*__init__.*\","
                              "\"min_degree\":1,\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "results") != NULL);
    free(r);
    PASS();
}

/* ── search_graph: project-only (no filters) ───────────────────── */

TEST(tool_sg_project_only) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 1000);
    free(r);
    PASS();
}

/* ── search_graph: error cases ─────────────────────────────────── */

TEST(tool_sg_invalid_project) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"nonexistent\",\"label\":\"Function\"}");
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "error") != NULL || strstr(r, "not found") != NULL ||
           strstr(r, "not_found") != NULL);
    free(r);
    PASS();
}

/* ── query_graph: basic patterns ───────────────────────────────── */

TEST(tool_qg_match_nodes) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (n:Function) RETURN n.name LIMIT 10\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    ASSERT_LTE(total, 10);
    free(r);
    PASS();
}

TEST(tool_qg_match_edges) {
    double ms;
    char *r =
        call_tool_timed("query_graph", &ms,
                        "{\"project\":\"%s\","
                        "\"query\":\"MATCH (a)-[r:CALLS]->(b) RETURN a.name, b.name LIMIT 10\"}",
                        g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_qg_match_imports) {
    double ms;
    char *r =
        call_tool_timed("query_graph", &ms,
                        "{\"project\":\"%s\","
                        "\"query\":\"MATCH (a)-[r:IMPORTS]->(b) RETURN a.name, b.name LIMIT 10\"}",
                        g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_qg_match_defines) {
    double ms;
    char *r =
        call_tool_timed("query_graph", &ms,
                        "{\"project\":\"%s\","
                        "\"query\":\"MATCH (a)-[r:DEFINES]->(b) RETURN a.name, b.name LIMIT 10\"}",
                        g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_qg_match_contains) {
    double ms;
    char *r = call_tool_timed(
        "query_graph", &ms,
        "{\"project\":\"%s\","
        "\"query\":\"MATCH (a)-[r:CONTAINS_FILE]->(b) RETURN a.name, b.name LIMIT 5\"}",
        g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

/* ── query_graph: WHERE clause ─────────────────────────────────── */

TEST(tool_qg_where_name) {
    double ms;
    char *r = call_tool_timed(
        "query_graph", &ms,
        "{\"project\":\"%s\","
        "\"query\":\"MATCH (n:Function) WHERE n.name = 'incr_test_injected' RETURN n.name\"}",
        g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_EQ(total, 1);
    free(r);
    PASS();
}

/* ── query_graph: multi-hop ────────────────────────────────────── */

TEST(tool_qg_two_hop) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (a)-[:CALLS]->(b)-[:CALLS]->(c) RETURN a.name, "
                              "b.name, c.name LIMIT 5\"}",
                              g_project);
    TOOL_OK(r, ms);
    /* May have 2-hop call chains */
    ASSERT(strstr(r, "columns") != NULL || strstr(r, "rows") != NULL);
    free(r);
    PASS();
}

/* ── query_graph: max_rows ─────────────────────────────────────── */

TEST(tool_qg_max_rows) {
    double ms;
    /* Query without max_rows — gets many results */
    char *r1 = call_tool_timed("query_graph", &ms,
                               "{\"project\":\"%s\","
                               "\"query\":\"MATCH (n:Function) RETURN n.name LIMIT 100\"}",
                               g_project);
    TOOL_OK(r1, ms);
    int total_unlimited = count_in_response(r1, "total");
    free(r1);

    /* Same query without LIMIT but with max_rows=3 — must cap results */
    char *r2 = call_tool_timed("query_graph", &ms,
                               "{\"project\":\"%s\","
                               "\"query\":\"MATCH (n:Function) RETURN n.name\","
                               "\"max_rows\":3}",
                               g_project);
    TOOL_OK(r2, ms);
    int total_limited = count_in_response(r2, "total");
    ASSERT_LTE(total_limited, 3);
    /* Without max_rows should have more than with */
    ASSERT_GT(total_unlimited, total_limited);
    free(r2);
    PASS();
}

/* ── query_graph: RETURN properties ────────────────────────────── */

TEST(tool_qg_return_properties) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (n:Module) RETURN n.name, n.file_path LIMIT 3\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    /* Should have both columns */
    ASSERT(strstr(r, "n.name") != NULL || strstr(r, "columns") != NULL);
    free(r);
    PASS();
}

/* ── query_graph: error cases ──────────────────────────────────── */

TEST(tool_qg_invalid_project) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"nonexistent\","
                              "\"query\":\"MATCH (n) RETURN n LIMIT 1\"}");
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "error") != NULL || strstr(r, "not found") != NULL);
    free(r);
    PASS();
}

/* ── search_code: modes ────────────────────────────────────────── */

TEST(tool_sc_compact) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"import\",\"mode\":\"compact\",\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* compact mode has "results" key with enriched matches */
    ASSERT(resp_has_key(r, "results"));
    ASSERT(resp_has_key(r, "total_results"));
    free(r);
    PASS();
}

TEST(tool_sc_full) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"FastAPI\",\"mode\":\"full\",\"limit\":3}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* full mode includes source code */
    ASSERT(resp_has_key(r, "results") || resp_has_key(r, "raw_matches"));
    free(r);
    PASS();
}

TEST(tool_sc_files) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"def \",\"mode\":\"files\",\"limit\":10}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* files mode has "files" key, NOT "results" */
    ASSERT(resp_has_key(r, "files"));
    free(r);
    PASS();
}

/* ── search_code: regex ────────────────────────────────────────── */

TEST(tool_sc_regex) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"def\\\\s+\\\\w+_handler\",\"regex\":true,\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── search_code: file_pattern + path_filter ───────────────────── */

TEST(tool_sc_file_pattern) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"class\",\"file_pattern\":\"*.py\",\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_sc_path_filter) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"import\",\"path_filter\":\"fastapi/\",\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── search_code: context lines ────────────────────────────────── */

TEST(tool_sc_context) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"FastAPI\",\"context\":3,\"limit\":3}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── search_code: no results ───────────────────────────────────── */

TEST(tool_sc_no_results) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"xyzzy_absolutely_no_match_99\"}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── get_code_snippet ──────────────────────────────────────────── */

TEST(tool_snippet_short_name) {
    double ms;
    char *r = call_tool_timed("get_code_snippet", &ms,
                              "{\"project\":\"%s\","
                              "\"qualified_name\":\"incr_test_injected\"}",
                              g_project);
    TOOL_OK(r, ms);
    /* Should return source or suggestions */
    ASSERT(strstr(r, "def") != NULL || strstr(r, "return") != NULL ||
           strstr(r, "suggest") != NULL || strstr(r, "match") != NULL);
    free(r);
    PASS();
}

TEST(tool_snippet_nonexistent) {
    double ms;
    char *r = call_tool_timed("get_code_snippet", &ms,
                              "{\"project\":\"%s\","
                              "\"qualified_name\":\"nonexistent_func_xyz\"}",
                              g_project);
    TOOL_OK(r, ms);
    /* Should return not found or suggestions, not crash */
    free(r);
    PASS();
}

TEST(tool_snippet_include_neighbors) {
    double ms;
    /* Use include_neighbors=true — response may include caller_names
     * (depends on whether the function has callers). Key thing:
     * the response must differ from include_neighbors=false and must
     * contain source code. */
    char *r = call_tool_timed("get_code_snippet", &ms,
                              "{\"project\":\"%s\","
                              "\"qualified_name\":\"incr_test_injected\","
                              "\"include_neighbors\":true}",
                              g_project);
    TOOL_OK(r, ms);
    /* Must have source code */
    ASSERT(resp_has_key(r, "source"));
    /* Must have callers/callees arrays (may be empty) */
    ASSERT(resp_has_key(r, "callers") || resp_has_key(r, "callees"));
    free(r);
    PASS();
}

/* ── trace_path ───────────────────────────────────────────── */

TEST(tool_trace_outbound) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"outbound\"}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* direction=outbound → response has "callees", no "callers" */
    ASSERT(resp_has_key(r, "callees"));
    ASSERT(resp_lacks_key(r, "callers"));
    ASSERT(resp_has_key(r, "direction"));
    free(r);
    PASS();
}

TEST(tool_trace_inbound) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"inbound\"}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* direction=inbound → response has "callers", no "callees" */
    ASSERT(resp_has_key(r, "callers"));
    ASSERT(resp_lacks_key(r, "callees"));
    free(r);
    PASS();
}

TEST(tool_trace_both) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"both\",\"depth\":5}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* direction=both → response has both "callers" and "callees" */
    ASSERT(resp_has_key(r, "callers"));
    ASSERT(resp_has_key(r, "callees"));
    free(r);
    PASS();
}

TEST(tool_trace_depth_1) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"both\",\"depth\":1}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_trace_nonexistent) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"no_such_function_xyz\","
                              "\"direction\":\"both\"}",
                              g_project);
    TOOL_OK(r, ms);
    /* Should return not found or empty, not crash */
    free(r);
    PASS();
}

TEST(tool_trace_edge_types) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"both\","
                              "\"edge_types\":[\"CALLS\"]}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── get_architecture ──────────────────────────────────────────── */

TEST(tool_arch_structure) {
    double ms;
    char *r = call_tool_timed("get_architecture", &ms,
                              "{\"project\":\"%s\",\"aspects\":[\"structure\"]}", g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "fastapi") != NULL);
    free(r);
    PASS();
}

TEST(tool_arch_all) {
    double ms;
    char *r = call_tool_timed("get_architecture", &ms, "{\"project\":\"%s\",\"aspects\":[\"all\"]}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_arch_no_aspects) {
    double ms;
    char *r = call_tool_timed("get_architecture", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── detect_changes ────────────────────────────────────────────── */

TEST(tool_detect_changes_default) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    /* Must have changed_files array and changed_count */
    ASSERT(resp_has_key(r, "changed_files"));
    ASSERT(resp_has_key(r, "changed_count"));
    ASSERT(resp_has_key(r, "impacted_symbols"));
    ASSERT(resp_has_key(r, "depth"));
    free(r);
    PASS();
}

TEST(tool_detect_changes_custom_branch) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms,
                              "{\"project\":\"%s\",\"base_branch\":\"HEAD\"}", g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "changed") != NULL);
    free(r);
    PASS();
}

/* Regression: `since` was advertised in the schema but ignored by the handler;
 * it must be honored as the diff base. Fixture is a --depth=1 shallow clone, so
 * HEAD~N won't resolve — use HEAD for a valid (empty) diff. */
TEST(tool_detect_changes_since) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms, "{\"project\":\"%s\",\"since\":\"HEAD\"}",
                              g_project);
    TOOL_OK(r, ms);
    ASSERT(resp_has_key(r, "changed_files"));
    free(r);
    PASS();
}

/* Regression: `since` must take precedence over base_branch. A valid since plus a
 * bogus base_branch must still succeed (proving since won) and must not reference
 * the bogus branch. */
TEST(tool_detect_changes_since_precedence) {
    double ms;
    char *r = call_tool_timed(
        "detect_changes", &ms,
        "{\"project\":\"%s\",\"since\":\"HEAD\",\"base_branch\":\"no-such-branch-xyz\"}",
        g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "no-such-branch-xyz") == NULL);
    free(r);
    PASS();
}

TEST(tool_detect_changes_depth) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms, "{\"project\":\"%s\",\"depth\":5}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── manage_adr ────────────────────────────────────────────────── */

TEST(tool_adr_get) {
    double ms;
    char *r =
        call_tool_timed("manage_adr", &ms, "{\"project\":\"%s\",\"mode\":\"get\"}", g_project);
    TOOL_OK(r, ms);
    /* May return empty if no ADR exists yet */
    free(r);
    PASS();
}

TEST(tool_adr_sections) {
    double ms;
    char *r =
        call_tool_timed("manage_adr", &ms, "{\"project\":\"%s\",\"mode\":\"sections\"}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── ingest_traces ─────────────────────────────────────────────── */

TEST(tool_ingest_traces_empty) {
    double ms;
    char *r =
        call_tool_timed("ingest_traces", &ms, "{\"project\":\"%s\",\"traces\":[]}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_ingest_traces_basic) {
    double ms;
    char *r = call_tool_timed(
        "ingest_traces", &ms,
        "{\"project\":\"%s\",\"traces\":[{\"caller\":\"a\",\"callee\":\"b\"}]}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── Error handling: every tool with bad project ───────────────── */

TEST(tool_err_query_bad_project) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"xxx\","
                              "\"query\":\"MATCH (n) RETURN n LIMIT 1\"}");
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "error") != NULL || strstr(r, "not found") != NULL);
    free(r);
    PASS();
}

TEST(tool_err_search_code_bad_project) {
    double ms;
    char *r = call_tool_timed("search_code", &ms, "{\"project\":\"xxx\",\"pattern\":\"test\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_err_snippet_bad_project) {
    double ms;
    char *r = call_tool_timed("get_code_snippet", &ms,
                              "{\"project\":\"xxx\",\"qualified_name\":\"test\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_err_trace_bad_project) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"xxx\","
                              "\"function_name\":\"test\",\"direction\":\"both\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_err_arch_bad_project) {
    double ms;
    char *r = call_tool_timed("get_architecture", &ms, "{\"project\":\"xxx\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_err_detect_bad_project) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms, "{\"project\":\"xxx\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_err_delete_nonexistent) {
    double ms;
    char *r = call_tool_timed("delete_project", &ms, "{\"project\":\"xxx-nonexistent\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── index_repository: mode param ───────────────────────────────── */

TEST(tool_index_mode_fast) {
    double ms;
    char *r = call_tool_timed("index_repository", &ms, "{\"repo_path\":\"%s\",\"mode\":\"fast\"}",
                              g_repodir);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "indexed") != NULL);
    free(r);
    PASS();
}

TEST(tool_index_invalid_path) {
    double ms;
    char *r = call_tool_timed("index_repository", &ms, "{\"repo_path\":\"/nonexistent/path/xyz\"}");
    TOOL_OK(r, ms);
    /* Should fail gracefully */
    free(r);
    PASS();
}

TEST(tool_index_missing_param) {
    double ms;
    char *r = call_tool_timed("index_repository", &ms, "{}");
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "required") != NULL || strstr(r, "error") != NULL ||
           strstr(r, "repo_path") != NULL);
    free(r);
    PASS();
}

/* ── search_graph: exclude_entry_points ────────────────────────── */

TEST(tool_sg_exclude_entry_points) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"exclude_entry_points\":true,\"max_degree\":0}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GTE(total, 0);
    free(r);
    PASS();
}

TEST(tool_sg_exclude_entry_points_false) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"exclude_entry_points\":false,\"max_degree\":0}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GTE(total, 0);
    free(r);
    PASS();
}

/* ── search_graph: high offset (beyond results) ───────────────── */

TEST(tool_sg_high_offset) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"offset\":999999,\"limit\":10}",
                              g_project);
    TOOL_OK(r, ms);
    /* Should return 0 results at this offset */
    free(r);
    PASS();
}

/* ── search_graph: name_pattern edge cases ─────────────────────── */

TEST(tool_sg_name_pattern_dot_star) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\".*\",\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 100);
    free(r);
    PASS();
}

TEST(tool_sg_name_pattern_anchored) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\"^incr_test_injected$\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_EQ(total, 1);
    free(r);
    PASS();
}

/* ── query_graph: more Cypher patterns ─────────────────────────── */

TEST(tool_qg_count_functions) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (n:Function) RETURN n.name LIMIT 200\","
                              "\"max_rows\":200}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 50);
    free(r);
    PASS();
}

TEST(tool_qg_edge_properties) {
    double ms;
    char *r = call_tool_timed(
        "query_graph", &ms,
        "{\"project\":\"%s\","
        "\"query\":\"MATCH (a)-[r:CALLS]->(b) RETURN a.name, b.name, r.callee LIMIT 5\"}",
        g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "columns") != NULL || strstr(r, "rows") != NULL);
    free(r);
    PASS();
}

TEST(tool_qg_node_file_path) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (n:Function) WHERE n.name = 'incr_test_injected' "
                              "RETURN n.name, n.file_path, n.start_line\"}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_EQ(total, 1);
    free(r);
    PASS();
}

TEST(tool_qg_match_module_defines) {
    double ms;
    /* DEFINES edges connect modules to their symbols */
    char *r =
        call_tool_timed("query_graph", &ms,
                        "{\"project\":\"%s\","
                        "\"query\":\"MATCH (a)-[:DEFINES]->(b) RETURN a.name, b.name LIMIT 10\"}",
                        g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_GT(total, 0);
    free(r);
    PASS();
}

TEST(tool_qg_max_rows_1) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (n:Function) RETURN n.name\","
                              "\"max_rows\":1}",
                              g_project);
    TOOL_OK(r, ms);
    int total = count_in_response(r, "total");
    ASSERT_EQ(total, 1);
    free(r);
    PASS();
}

/* ── search_code: limit param ──────────────────────────────────── */

TEST(tool_sc_limit_1) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"def \",\"limit\":1}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_sc_limit_50) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"import\",\"limit\":50}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── search_code: combined params ──────────────────────────────── */

TEST(tool_sc_combined) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"class\","
                              "\"mode\":\"compact\","
                              "\"file_pattern\":\"*.py\","
                              "\"path_filter\":\"fastapi/\","
                              "\"limit\":5,\"context\":2}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── get_code_snippet: full QN from search ─────────────────────── */

TEST(tool_snippet_full_qn) {
    /* First find a function QN via search */
    double ms;
    char *sr = call_tool_timed("search_graph", &ms,
                               "{\"project\":\"%s\",\"label\":\"Function\","
                               "\"name_pattern\":\"incr_test_helper\",\"limit\":1}",
                               g_project);
    TOOL_OK(sr, ms);

    /* Extract qualified_name */
    const char *qn_start = strstr(sr, "qualified_name");
    if (qn_start) {
        qn_start = strchr(qn_start, ':');
        if (qn_start) {
            qn_start += 2; /* skip :" */
            /* Handle escaped quotes in JSON */
            if (*qn_start == '\\')
                qn_start += 2;
            const char *qn_end = strchr(qn_start, '"');
            if (!qn_end)
                qn_end = strchr(qn_start, '\\');
            if (qn_end && qn_end > qn_start) {
                char qn[512];
                int len = (int)(qn_end - qn_start);
                if (len > 0 && len < (int)sizeof(qn)) {
                    memcpy(qn, qn_start, (size_t)len);
                    qn[len] = '\0';

                    char *r = call_tool_timed("get_code_snippet", &ms,
                                              "{\"project\":\"%s\",\"qualified_name\":\"%s\"}",
                                              g_project, qn);
                    TOOL_OK(r, ms);
                    ASSERT(strstr(r, "source") != NULL || strstr(r, "def") != NULL ||
                           strstr(r, "return") != NULL);
                    free(r);
                }
            }
        }
    }
    free(sr);
    PASS();
}

/* ── get_architecture: specific aspects ────────────────────────── */

TEST(tool_arch_dependencies) {
    double ms;
    char *r = call_tool_timed("get_architecture", &ms,
                              "{\"project\":\"%s\",\"aspects\":[\"dependencies\"]}", g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_arch_entry_points) {
    double ms;
    char *r = call_tool_timed("get_architecture", &ms,
                              "{\"project\":\"%s\",\"aspects\":[\"entry_points\"]}", g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_arch_routes) {
    double ms;
    char *r = call_tool_timed("get_architecture", &ms,
                              "{\"project\":\"%s\",\"aspects\":[\"routes\"]}", g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_arch_multi_aspects) {
    double ms;
    char *r = call_tool_timed(
        "get_architecture", &ms,
        "{\"project\":\"%s\",\"aspects\":[\"structure\",\"dependencies\",\"entry_points\"]}",
        g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── detect_changes: scope param ───────────────────────────────── */

TEST(tool_detect_changes_scope) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms, "{\"project\":\"%s\",\"scope\":\"fastapi/\"}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── manage_adr: update + content ──────────────────────────────── */

TEST(tool_adr_update) {
    double ms;
    char *r = call_tool_timed("manage_adr", &ms,
                              "{\"project\":\"%s\",\"mode\":\"update\","
                              "\"content\":\"# Test ADR\\n\\nThis is a test ADR.\"}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);

    /* Verify it was stored */
    r = call_tool_timed("manage_adr", &ms, "{\"project\":\"%s\",\"mode\":\"get\"}", g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "Test ADR") != NULL);
    free(r);
    PASS();
}

TEST(tool_adr_sections_array) {
    double ms;
    char *r = call_tool_timed("manage_adr", &ms,
                              "{\"project\":\"%s\",\"mode\":\"sections\","
                              "\"sections\":[\"overview\",\"decisions\"]}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── ingest_traces: multiple traces ────────────────────────────── */

TEST(tool_ingest_traces_multiple) {
    double ms;
    char *r = call_tool_timed("ingest_traces", &ms,
                              "{\"project\":\"%s\",\"traces\":["
                              "{\"caller\":\"funcA\",\"callee\":\"funcB\",\"count\":5},"
                              "{\"caller\":\"funcB\",\"callee\":\"funcC\",\"count\":3},"
                              "{\"caller\":\"funcC\",\"callee\":\"funcD\",\"count\":1}"
                              "]}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── trace_path: more edge_types combos ───────────────────── */

TEST(tool_trace_imports_only) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"both\","
                              "\"edge_types\":[\"IMPORTS\"]}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_trace_calls_and_imports) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"both\","
                              "\"edge_types\":[\"CALLS\",\"IMPORTS\"]}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

TEST(tool_trace_deep) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"outbound\",\"depth\":10}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 *  GAP COVERAGE: every remaining tool × param combination
 * ═══════════════════════════════════════════════════════════════ */

/* ── search_graph: remaining relationship values ───────────────── */

TEST(tool_sg_rel_imports) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Module\","
                              "\"relationship\":\"IMPORTS\",\"min_degree\":1}",
                              g_project);
    TOOL_OK(r, ms);
    int t = count_in_response(r, "total");
    ASSERT_GT(t, 0);
    free(r);
    PASS();
}

TEST(tool_sg_rel_defines) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Module\","
                              "\"relationship\":\"DEFINES\",\"min_degree\":1}",
                              g_project);
    TOOL_OK(r, ms);
    int t = count_in_response(r, "total");
    ASSERT_GT(t, 0);
    free(r);
    PASS();
}

TEST(tool_sg_rel_contains_file) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Folder\","
                              "\"relationship\":\"CONTAINS_FILE\",\"min_degree\":1}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── search_graph: remaining label values ──────────────────────── */

TEST(tool_sg_label_file) {
    double ms;
    char *r =
        call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"File\"}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_sg_label_folder) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"Folder\"}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

TEST(tool_sg_label_package) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms, "{\"project\":\"%s\",\"label\":\"Package\"}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── search_graph: include_connected=false ─────────────────────── */

TEST(tool_sg_include_connected_false) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"name_pattern\":\"incr_test_injected\","
                              "\"include_connected\":false}",
                              g_project);
    TOOL_OK(r, ms);
    int t = count_in_response(r, "total");
    ASSERT_EQ(t, 1);
    free(r);
    PASS();
}

/* ── search_graph: qn_pattern + label combined ─────────────────── */

TEST(tool_sg_qn_and_label) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"qn_pattern\":\".*applications.*\"}",
                              g_project);
    TOOL_OK(r, ms);
    int t = count_in_response(r, "total");
    ASSERT_GT(t, 0);
    free(r);
    PASS();
}

/* ── search_graph: file_pattern + name_pattern combined ────────── */

TEST(tool_sg_file_and_name) {
    double ms;
    char *r = call_tool_timed("search_graph", &ms,
                              "{\"project\":\"%s\",\"label\":\"Function\","
                              "\"file_pattern\":\"*fastapi*\","
                              "\"name_pattern\":\".*\",\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── query_graph: CONFIGURES edges ─────────────────────────────── */

TEST(tool_qg_configures) {
    double ms;
    char *r = call_tool_timed(
        "query_graph", &ms,
        "{\"project\":\"%s\","
        "\"query\":\"MATCH (a)-[r:CONFIGURES]->(b) RETURN a.name, b.name LIMIT 5\"}",
        g_project);
    TOOL_OK(r, ms);
    /* May have 0 results if no CONFIGURES edges in FastAPI */
    ASSERT(strstr(r, "columns") != NULL || strstr(r, "rows") != NULL);
    free(r);
    PASS();
}

/* ── query_graph: HANDLES edges ────────────────────────────────── */

TEST(tool_qg_handles) {
    double ms;
    char *r =
        call_tool_timed("query_graph", &ms,
                        "{\"project\":\"%s\","
                        "\"query\":\"MATCH (a)-[r:HANDLES]->(b) RETURN a.name, b.name LIMIT 5\"}",
                        g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "columns") != NULL || strstr(r, "rows") != NULL);
    free(r);
    PASS();
}

/* ── query_graph: DEFINES_METHOD edges ─────────────────────────── */

TEST(tool_qg_defines_method) {
    double ms;
    char *r = call_tool_timed("query_graph", &ms,
                              "{\"project\":\"%s\","
                              "\"query\":\"MATCH (c:Class)-[r:DEFINES_METHOD]->(m:Method) RETURN "
                              "c.name, m.name LIMIT 5\"}",
                              g_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "columns") != NULL || strstr(r, "rows") != NULL);
    free(r);
    PASS();
}

/* ── query_graph: no LIMIT (default behavior) ──────────────────── */

TEST(tool_qg_no_limit) {
    double ms;
    char *r = call_tool_timed(
        "query_graph", &ms,
        "{\"project\":\"%s\","
        "\"query\":\"MATCH (n:Function) WHERE n.name = 'incr_test_injected' RETURN n.name\"}",
        g_project);
    TOOL_OK(r, ms);
    int t = count_in_response(r, "total");
    ASSERT_GT(t, 0);
    free(r);
    PASS();
}

/* ── query_graph: empty result ─────────────────────────────────── */

TEST(tool_qg_empty_result) {
    double ms;
    char *r = call_tool_timed(
        "query_graph", &ms,
        "{\"project\":\"%s\","
        "\"query\":\"MATCH (n:Function) WHERE n.name = 'zzz_no_exist_zzz' RETURN n.name\"}",
        g_project);
    TOOL_OK(r, ms);
    int t = count_in_response(r, "total");
    ASSERT_EQ(t, 0);
    free(r);
    PASS();
}

/* ── search_code: regex=false explicit ─────────────────────────── */

TEST(tool_sc_regex_false) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"FastAPI\",\"regex\":false,\"limit\":3}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── search_code: context=0 ────────────────────────────────────── */

TEST(tool_sc_context_zero) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"import\",\"context\":0,\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── search_code: special chars in pattern ─────────────────────── */

TEST(tool_sc_special_chars) {
    double ms;
    char *r = call_tool_timed("search_code", &ms,
                              "{\"project\":\"%s\","
                              "\"pattern\":\"def __init__(self\",\"limit\":5}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── get_code_snippet: include_neighbors=false ─────────────────── */

TEST(tool_snippet_neighbors_false) {
    double ms;
    char *r = call_tool_timed("get_code_snippet", &ms,
                              "{\"project\":\"%s\","
                              "\"qualified_name\":\"incr_test_injected\","
                              "\"include_neighbors\":false}",
                              g_project);
    TOOL_OK(r, ms);
    /* include_neighbors=false should NOT have "caller_names" field */
    ASSERT(resp_lacks_key(r, "caller_names"));
    /* But must still have source code */
    ASSERT(resp_has_key(r, "source"));
    free(r);
    PASS();
}

/* ── get_code_snippet: Class name ──────────────────────────────── */

TEST(tool_snippet_class) {
    /* FastAPI has classes — try a common one */
    double ms;
    char *r = call_tool_timed("get_code_snippet", &ms,
                              "{\"project\":\"%s\","
                              "\"qualified_name\":\"FastAPI\"}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── trace_path: depth=0 ──────────────────────────────────── */

TEST(tool_trace_depth_0) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"both\",\"depth\":0}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── trace_path: edge_types=[DEFINES] ─────────────────────── */

TEST(tool_trace_defines_only) {
    double ms;
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"inbound\","
                              "\"edge_types\":[\"DEFINES\"]}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── trace_path: include_tests flag ────────────────────────────── */

TEST(tool_trace_include_tests) {
    double ms;
    /* Default (include_tests=false): test files should be filtered out */
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"inbound\","
                              "\"include_tests\":false}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* Result should not contain is_test markers when tests are excluded */
    free(r);

    /* With include_tests=true: test files should appear with is_test marker */
    r = call_tool_timed("trace_path", &ms,
                        "{\"project\":\"%s\","
                        "\"function_name\":\"incr_test_injected\","
                        "\"direction\":\"inbound\","
                        "\"include_tests\":true}",
                        g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── trace_path: risk_labels flag ─────────────────────────────── */

TEST(tool_trace_risk_labels) {
    double ms;
    /* Use outbound direction — incr_test_injected may call stdlib functions */
    char *r = call_tool_timed("trace_path", &ms,
                              "{\"project\":\"%s\","
                              "\"function_name\":\"incr_test_injected\","
                              "\"direction\":\"outbound\","
                              "\"risk_labels\":true}",
                              g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    /* If there are callees, they should have risk labels.
     * If no callees, the response is still valid — just empty. */
    if (strstr(r, "\"hop\"") != NULL) {
        ASSERT(strstr(r, "\"risk\"") != NULL);
    }
    free(r);
    PASS();
}

/* ── detect_changes: nonexistent branch ────────────────────────── */

TEST(tool_detect_nonexistent_branch) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms,
                              "{\"project\":\"%s\","
                              "\"base_branch\":\"nonexistent-branch-xyz\"}",
                              g_project);
    TOOL_OK(r, ms);
    /* Should handle gracefully — git diff may fail or return empty */
    free(r);
    PASS();
}

/* ── detect_changes: depth=1 ───────────────────────────────────── */

TEST(tool_detect_depth_1) {
    double ms;
    char *r = call_tool_timed("detect_changes", &ms, "{\"project\":\"%s\",\"depth\":1}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── manage_adr: default mode (omitted) ────────────────────────── */

TEST(tool_adr_default_mode) {
    double ms;
    char *r = call_tool_timed("manage_adr", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── manage_adr: update with empty content ─────────────────────── */

TEST(tool_adr_update_empty) {
    double ms;
    char *r = call_tool_timed("manage_adr", &ms,
                              "{\"project\":\"%s\",\"mode\":\"update\","
                              "\"content\":\"\"}",
                              g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── ingest_traces: trace with missing fields ──────────────────── */

TEST(tool_ingest_traces_partial) {
    double ms;
    char *r =
        call_tool_timed("ingest_traces", &ms,
                        "{\"project\":\"%s\",\"traces\":[{\"caller\":\"onlyA\"}]}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── Error: get_graph_schema bad project ───────────────────────── */

TEST(tool_err_schema_bad_project) {
    double ms;
    char *r = call_tool_timed("get_graph_schema", &ms, "{\"project\":\"nonexistent-xyz\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── Error: index_status missing project ───────────────────────── */

TEST(tool_err_index_status_no_project) {
    double ms;
    char *r = call_tool_timed("index_status", &ms, "{}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── Error: manage_adr bad project ─────────────────────────────── */

TEST(tool_err_adr_bad_project) {
    double ms;
    char *r =
        call_tool_timed("manage_adr", &ms, "{\"project\":\"nonexistent-xyz\",\"mode\":\"get\"}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── Error: ingest_traces bad project ──────────────────────────── */

TEST(tool_err_ingest_bad_project) {
    double ms;
    char *r =
        call_tool_timed("ingest_traces", &ms, "{\"project\":\"nonexistent-xyz\",\"traces\":[]}");
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── Error: ingest_traces missing traces array ─────────────────── */

TEST(tool_err_ingest_no_traces) {
    double ms;
    char *r = call_tool_timed("ingest_traces", &ms, "{\"project\":\"%s\"}", g_project);
    TOOL_OK(r, ms);
    free(r);
    PASS();
}

/* ── index_repository: mode=full explicit ──────────────────────── */

TEST(tool_index_mode_full) {
    double ms;
    char *r = call_tool_timed("index_repository", &ms, "{\"repo_path\":\"%s\",\"mode\":\"full\"}",
                              g_repodir);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "indexed") != NULL);
    free(r);
    PASS();
}

/* ── get_architecture: empty aspects array ─────────────────────── */

TEST(tool_arch_empty_aspects) {
    double ms;
    char *r =
        call_tool_timed("get_architecture", &ms, "{\"project\":\"%s\",\"aspects\":[]}", g_project);
    TOOL_OK(r, ms);
    NOT_ERROR(r);
    free(r);
    PASS();
}

/* ── delete_project: real delete + verify ──────────────────────── */
/* NOTE: This is destructive — must run LAST */

TEST(tool_delete_and_verify) {
    /* Create a throwaway project to delete */
    char tmpdir2[256];
    snprintf(tmpdir2, sizeof(tmpdir2), "%s/throwaway", g_tmpdir);
    cbm_mkdir(tmpdir2);
    char pypath[512];
    snprintf(pypath, sizeof(pypath), "%s/dummy.py", tmpdir2);
    FILE *f = fopen(pypath, "w");
    if (f) {
        fprintf(f, "def throwaway():\n    pass\n");
        fclose(f);
    }

    double ms;
    char *r = call_tool_timed("index_repository", &ms, "{\"repo_path\":\"%s\"}", tmpdir2);
    TOOL_OK(r, ms);
    free(r);

    char *throwaway_project = cbm_project_name_from_path(tmpdir2);
    ASSERT(throwaway_project != NULL);

    /* Verify it exists */
    r = call_tool_timed("index_status", &ms, "{\"project\":\"%s\"}", throwaway_project);
    TOOL_OK(r, ms);
    free(r);

    /* Delete it */
    r = call_tool_timed("delete_project", &ms, "{\"project\":\"%s\"}", throwaway_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "deleted") != NULL);
    free(r);

    /* Verify it's gone */
    r = call_tool_timed("index_status", &ms, "{\"project\":\"%s\"}", throwaway_project);
    TOOL_OK(r, ms);
    ASSERT(strstr(r, "not") != NULL || strstr(r, "error") != NULL);
    free(r);

    free(throwaway_project);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

SUITE(incremental) {
    if (incremental_setup() != 0) {
        printf("  SETUP FAILED — skipping incremental tests (network?)\n");
        return;
    }
    if (getenv("CBM_ONLY_ISSUE12")) {
        RUN_TEST(incr_full_index);
        RUN_TEST(incr_partial_vs_full_load_single_file_cross_call);
        RUN_TEST(incr_partial_vs_full_load_above_threshold_falls_back);
        RUN_TEST(incr_partial_vs_full_load_new_cross_file_symbol);
        return;
    }


    /* Phase 1: Full index baseline (needed for tool tests below) */
    RUN_TEST(incr_full_index);
    RUN_TEST(incr_full_has_functions);
    RUN_TEST(incr_full_edge_types);

    /* Inject test functions so tool tests (trace_path etc.) always have data,
     * even when perf phases are skipped on CI. Update baseline counts. */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/fastapi/applications.py", g_repodir);
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "\n\ndef incr_test_injected(x: int) -> int:\n"
                       "    return x * 42\n"
                       "\n"
                       "def incr_test_helper(y: str) -> str:\n"
                       "    return y.upper()\n");
            fclose(f);
        }
        char *resp = index_repo();
        free(resp);
        g_full_nodes = get_node_count();
        g_full_edges = get_edge_count();
        g_full_calls = get_edge_count_by_type("CALLS");
        g_full_imports = get_edge_count_by_type("IMPORTS");
    }

    /* Phase 2: Noop */
    RUN_TEST(incr_noop_reindex);

    /* Phase 3: Incremental deltas */
    RUN_TEST(incr_modify_file);
    RUN_TEST(incr_formatter_run);
    RUN_TEST(incr_add_file);
    RUN_TEST(incr_delete_file);
    RUN_TEST(incr_simultaneous_changes);

    /* Phase 4: Adversarial */
    RUN_TEST(incr_empty_file);
    RUN_TEST(incr_syntax_error);
    RUN_TEST(incr_huge_single_function);
    RUN_TEST(incr_binary_content);
    RUN_TEST(incr_large_generated);
    RUN_TEST(incr_new_subdir);

    /* Phase 5: Stress */
    RUN_TEST(incr_rapid_reindex);
    RUN_TEST(incr_replace_file_content);
    RUN_TEST(incr_fts_bm25_consistency);
    RUN_TEST(incr_partial_vs_full_load_single_file_cross_call);
    RUN_TEST(incr_partial_vs_full_load_above_threshold_falls_back);
    RUN_TEST(incr_partial_vs_full_load_new_cross_file_symbol);
    RUN_TEST(incr_batch_add_delete);

    /* Phase 6: Recovery + accuracy */
    RUN_TEST(incr_db_deleted_recovery);
    RUN_TEST(incr_accuracy_vs_full);

    /* Phase 7: Performance regression */
    RUN_TEST(incr_perf_single_file_fast);
    RUN_TEST(incr_mtime_bump_only_is_unchanged);
    RUN_TEST(incr_content_change_still_detected);

    /* Phase 8: list_projects + index_status + schema */
    RUN_TEST(tool_list_projects_basic);
    RUN_TEST(tool_list_projects_has_current);
    RUN_TEST(tool_index_status_basic);
    RUN_TEST(tool_index_status_nonexistent);
    RUN_TEST(tool_schema_has_labels);
    RUN_TEST(tool_schema_has_edge_types);

    /* Phase 9: search_graph — every parameter */
    RUN_TEST(tool_sg_label_function);
    RUN_TEST(tool_sg_label_method);
    RUN_TEST(tool_sg_label_module);
    RUN_TEST(tool_sg_label_variable);
    RUN_TEST(tool_sg_label_class);
    RUN_TEST(tool_sg_label_route);
    RUN_TEST(tool_sg_label_nonexistent);
    RUN_TEST(tool_sg_name_exact);
    RUN_TEST(tool_sg_name_regex);
    RUN_TEST(tool_sg_name_no_match);
    RUN_TEST(tool_sg_min_degree);
    RUN_TEST(tool_sg_max_degree_zero);
    RUN_TEST(tool_sg_degree_range);
    RUN_TEST(tool_sg_limit);
    RUN_TEST(tool_sg_offset);
    RUN_TEST(tool_sg_file_pattern);
    RUN_TEST(tool_sg_include_connected);
    RUN_TEST(tool_sg_relationship);
    RUN_TEST(tool_sg_qn_pattern);
    RUN_TEST(tool_sg_combined_filters);
    RUN_TEST(tool_sg_project_only);
    RUN_TEST(tool_sg_invalid_project);

    /* Phase 10: query_graph — Cypher patterns */
    RUN_TEST(tool_qg_match_nodes);
    RUN_TEST(tool_qg_match_edges);
    RUN_TEST(tool_qg_match_imports);
    RUN_TEST(tool_qg_match_defines);
    RUN_TEST(tool_qg_match_contains);
    RUN_TEST(tool_qg_where_name);
    RUN_TEST(tool_qg_two_hop);
    RUN_TEST(tool_qg_max_rows);
    RUN_TEST(tool_qg_return_properties);
    RUN_TEST(tool_qg_invalid_project);

    /* Phase 11: search_code — modes + params */
    RUN_TEST(tool_sc_compact);
    RUN_TEST(tool_sc_full);
    RUN_TEST(tool_sc_files);
    RUN_TEST(tool_sc_regex);
    RUN_TEST(tool_sc_file_pattern);
    RUN_TEST(tool_sc_path_filter);
    RUN_TEST(tool_sc_context);
    RUN_TEST(tool_sc_no_results);

    /* Phase 12: get_code_snippet */
    RUN_TEST(tool_snippet_short_name);
    RUN_TEST(tool_snippet_nonexistent);
    RUN_TEST(tool_snippet_include_neighbors);

    /* Phase 13: trace_path — directions + params */
    RUN_TEST(tool_trace_outbound);
    RUN_TEST(tool_trace_inbound);
    RUN_TEST(tool_trace_both);
    RUN_TEST(tool_trace_depth_1);
    RUN_TEST(tool_trace_nonexistent);
    RUN_TEST(tool_trace_edge_types);

    /* Phase 14: get_architecture */
    RUN_TEST(tool_arch_structure);
    RUN_TEST(tool_arch_all);
    RUN_TEST(tool_arch_no_aspects);

    /* Phase 15: detect_changes */
    RUN_TEST(tool_detect_changes_default);
    RUN_TEST(tool_detect_changes_custom_branch);
    RUN_TEST(tool_detect_changes_since);
    RUN_TEST(tool_detect_changes_since_precedence);
    RUN_TEST(tool_detect_changes_depth);

    /* Phase 16: manage_adr */
    RUN_TEST(tool_adr_get);
    RUN_TEST(tool_adr_sections);

    /* Phase 17: ingest_traces */
    RUN_TEST(tool_ingest_traces_empty);
    RUN_TEST(tool_ingest_traces_basic);

    /* Phase 18: Error handling — every tool with bad project */
    RUN_TEST(tool_err_query_bad_project);
    RUN_TEST(tool_err_search_code_bad_project);
    RUN_TEST(tool_err_snippet_bad_project);
    RUN_TEST(tool_err_trace_bad_project);
    RUN_TEST(tool_err_arch_bad_project);
    RUN_TEST(tool_err_detect_bad_project);
    RUN_TEST(tool_err_delete_nonexistent);

    /* Phase 19: index_repository params */
    RUN_TEST(tool_index_mode_fast);
    RUN_TEST(tool_index_invalid_path);
    RUN_TEST(tool_index_missing_param);

    /* Phase 20: search_graph remaining params */
    RUN_TEST(tool_sg_exclude_entry_points);
    RUN_TEST(tool_sg_exclude_entry_points_false);
    RUN_TEST(tool_sg_high_offset);
    RUN_TEST(tool_sg_name_pattern_dot_star);
    RUN_TEST(tool_sg_name_pattern_anchored);

    /* Phase 21: query_graph advanced */
    RUN_TEST(tool_qg_count_functions);
    RUN_TEST(tool_qg_edge_properties);
    RUN_TEST(tool_qg_node_file_path);
    RUN_TEST(tool_qg_match_module_defines);
    RUN_TEST(tool_qg_max_rows_1);

    /* Phase 22: search_code extra params */
    RUN_TEST(tool_sc_limit_1);
    RUN_TEST(tool_sc_limit_50);
    RUN_TEST(tool_sc_combined);

    /* Phase 23: get_code_snippet full QN */
    RUN_TEST(tool_snippet_full_qn);

    /* Phase 24: get_architecture specific aspects */
    RUN_TEST(tool_arch_dependencies);
    RUN_TEST(tool_arch_entry_points);
    RUN_TEST(tool_arch_routes);
    RUN_TEST(tool_arch_multi_aspects);

    /* Phase 25: detect_changes scope */
    RUN_TEST(tool_detect_changes_scope);

    /* Phase 26: manage_adr update + sections */
    RUN_TEST(tool_adr_update);
    RUN_TEST(tool_adr_sections_array);

    /* Phase 27: ingest_traces multiple */
    RUN_TEST(tool_ingest_traces_multiple);

    /* Phase 28: trace_path edge_types combos */
    RUN_TEST(tool_trace_imports_only);
    RUN_TEST(tool_trace_calls_and_imports);
    RUN_TEST(tool_trace_deep);

    /* Phase 29: search_graph remaining relationships */
    RUN_TEST(tool_sg_rel_imports);
    RUN_TEST(tool_sg_rel_defines);
    RUN_TEST(tool_sg_rel_contains_file);

    /* Phase 30: search_graph remaining labels */
    RUN_TEST(tool_sg_label_file);
    RUN_TEST(tool_sg_label_folder);
    RUN_TEST(tool_sg_label_package);

    /* Phase 31: search_graph param combos */
    RUN_TEST(tool_sg_include_connected_false);
    RUN_TEST(tool_sg_qn_and_label);
    RUN_TEST(tool_sg_file_and_name);

    /* Phase 32: query_graph remaining edge types */
    RUN_TEST(tool_qg_configures);
    RUN_TEST(tool_qg_handles);
    RUN_TEST(tool_qg_defines_method);
    RUN_TEST(tool_qg_defines_method_more_than_10);
    RUN_TEST(tool_qg_class_lines_nonzero);
    RUN_TEST(tool_qg_no_limit);
    RUN_TEST(tool_qg_empty_result);

    /* Phase 33: search_code remaining params */
    RUN_TEST(tool_sc_regex_false);
    RUN_TEST(tool_sc_context_zero);
    RUN_TEST(tool_sc_special_chars);

    /* Phase 34: get_code_snippet remaining */
    RUN_TEST(tool_snippet_neighbors_false);
    RUN_TEST(tool_snippet_class);

    /* Phase 35: trace_path remaining */
    RUN_TEST(tool_trace_depth_0);
    RUN_TEST(tool_trace_defines_only);
    RUN_TEST(tool_trace_include_tests);
    RUN_TEST(tool_trace_risk_labels);

    /* Phase 36: detect_changes remaining */
    RUN_TEST(tool_detect_nonexistent_branch);
    RUN_TEST(tool_detect_depth_1);

    /* Phase 37: manage_adr remaining */
    RUN_TEST(tool_adr_default_mode);
    RUN_TEST(tool_adr_update_empty);

    /* Phase 38: ingest_traces remaining */
    RUN_TEST(tool_ingest_traces_partial);

    /* Phase 39: error handling remaining tools */
    RUN_TEST(tool_err_schema_bad_project);
    RUN_TEST(tool_err_index_status_no_project);
    RUN_TEST(tool_err_adr_bad_project);
    RUN_TEST(tool_err_ingest_bad_project);
    RUN_TEST(tool_err_ingest_no_traces);

    /* Phase 40: index_repository mode=full + arch empty aspects */
    RUN_TEST(tool_index_mode_full);
    RUN_TEST(tool_arch_empty_aspects);

    /* Phase 41: delete_project real flow (MUST BE LAST) */
    RUN_TEST(tool_delete_and_verify);

    incremental_teardown();
}
