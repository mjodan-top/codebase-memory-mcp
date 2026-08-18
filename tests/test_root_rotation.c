/*
 * test_root_rotation.c — issue #56: alias/worktree rotation must sync the
 * Projects row (root_path + git context) on the INCREMENTAL path.
 *
 * Scenario (mirrors the cbm-wt alias workflow): the same project name is
 * re-indexed from tree1 (full), then from tree2 (rotation). The incremental
 * route — BOTH its no-op fast path (identical content) and its changed path
 * (row-level dump) — must leave Projects.root_path pointing at the CURRENT
 * tree. Before the fix only the full pipeline upserted the Projects row, so
 * a rotation through the incremental path kept tree1's root_path forever.
 *
 * Route pinning: a sentinel file-hash row for an unrelated project is
 * planted in the DB between runs. The full-reindex route unlinks the DB
 * file (sentinel dies); the incremental route only touches the project's
 * own rows (sentinel survives). Asserting the sentinel survives proves the
 * assertions below really exercised the incremental path — without it, a
 * silent fallback to full reindex would turn this regression test into a
 * tautology.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "../src/pipeline/pipeline.h"
#include <mcp/mcp.h>
#include <store/store.h>

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define ROT_PROJECT "rotationprojissuefiftysix"
#define ROT_SENTINEL_PROJECT "rotation_sentinel"

static int rot_write(const char *dir, const char *rel, const char *content) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, rel);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t want = strlen(content);
    size_t wrote = fwrite(content, 1, want, fp);
    int rc = (wrote == want && fclose(fp) == 0) ? 0 : -1;
    if (wrote != want) {
        (void)fclose(fp);
    }
    return rc;
}

/* Copy atime/mtime from src onto dst so the identical-content rotation is
 * classified unchanged by the mtime+size fast check alone — the no-op fast
 * path must not depend on the sha256 fallback for this test's routing. */
static int rot_sync_mtime(const char *src_dir, const char *dst_dir, const char *rel) {
    char src[1024];
    char dst[1024];
    snprintf(src, sizeof(src), "%s/%s", src_dir, rel);
    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, rel);
    struct stat st;
    if (stat(src, &st) != 0) {
        return -1;
    }
#ifdef _WIN32
    struct _utimbuf ut;
    ut.actime = st.st_atime;
    ut.modtime = st.st_mtime;
    return _utime(dst, &ut);
#else
    struct timespec times[2];
#ifdef __APPLE__
    times[0] = st.st_atimespec;
    times[1] = st.st_mtimespec;
#else
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
#endif
    return utimensat(AT_FDCWD, dst, times, 0);
#endif
}

/* Exact rotation assertion: both sides canonicalized, then compared equal —
 * a substring match could pass on an unrelated path that merely embeds the
 * temp dir's random suffix. */
static int rot_root_points_at(const char *root, const char *tree) {
    if (!root || !tree) {
        return 0;
    }
    char croot[4096];
    char ctree[4096];
#ifdef _WIN32
    if (!_fullpath(croot, root, sizeof(croot)) || !_fullpath(ctree, tree, sizeof(ctree))) {
        return 0;
    }
#else
    if (!realpath(root, croot) || !realpath(tree, ctree)) {
        return 0;
    }
#endif
    return strcmp(croot, ctree) == 0;
}

/* Index `repo` into `db_path` under the FIXED project name (simulates the
 * stable-alias rotation: same project, different tree each run). */
static int rot_index(const char *repo, const char *db_path) {
    cbm_pipeline_t *p = cbm_pipeline_new(repo, db_path, CBM_MODE_FAST);
    if (!p) {
        return -1;
    }
    if (!cbm_pipeline_set_project_name(p, ROT_PROJECT)) {
        cbm_pipeline_free(p);
        return -1;
    }
    int rc = cbm_pipeline_run(p);
    cbm_pipeline_free(p);
    return rc;
}

/* Fetch a strdup'd Projects.root_path for ROT_PROJECT (NULL on miss). */
static char *rot_get_root_path(const char *db_path) {
    cbm_store_t *s = cbm_store_open_path(db_path);
    if (!s) {
        return NULL;
    }
    cbm_project_t proj = {0};
    char *out = NULL;
    if (cbm_store_get_project(s, ROT_PROJECT, &proj) == CBM_STORE_OK) {
        out = proj.root_path ? strdup(proj.root_path) : NULL;
        cbm_project_free_fields(&proj);
    }
    cbm_store_close(s);
    return out;
}

/* Count nodes rows for ROT_PROJECT with the given short name. */
static int rot_node_count_by_name(const char *db_path, const char *name) {
    cbm_store_t *s = cbm_store_open_path(db_path);
    if (!s) {
        return -1;
    }
    sqlite3 *db = cbm_store_get_db(s);
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (db && sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM nodes WHERE project=? AND name=?", -1,
                                 &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ROT_PROJECT, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    cbm_store_close(s);
    return count;
}

/* Plant a probe NODE row on app.py inside the project. The incremental
 * row-level dump deletes nodes-by-file for CHANGED files only, so:
 *   - probe survives round 2  ⟺ app.py was classified unchanged (no-op);
 *   - probe dies in round 3   ⟺ the changed path really re-dumped app.py.
 * This distinguishes the no-op fast path from the row-level dump route. */
static int rot_plant_probe_node(const char *db_path) {
    cbm_store_t *s = cbm_store_open_path(db_path);
    if (!s) {
        return -1;
    }
    sqlite3 *db = cbm_store_get_db(s);
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (db && sqlite3_prepare_v2(db,
                                 "INSERT INTO nodes (project,label,name,qualified_name,"
                                 "file_path,start_line,end_line,properties) "
                                 "VALUES (?,'Function','noop_probe_node',"
                                 "'rot.noop_probe_node','app.py',1,1,'{}')",
                                 -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ROT_PROJECT, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            rc = 0;
        }
        sqlite3_finalize(stmt);
    }
    cbm_store_close(s);
    return rc;
}

/* MCP end-to-end: grep-match count reported by the search_code tool, which
 * resolves the grep root from Projects.root_path — the user-visible symptom
 * of issue #56 (stale root ⇒ search served from the OLD tree). */
static int rot_search_code_matches(cbm_mcp_server_t *srv, const char *pattern) {
    char args[512];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"pattern\":\"%s\"}", ROT_PROJECT, pattern);
    char *resp = cbm_mcp_handle_tool(srv, "search_code", args);
    if (!resp) {
        return -1;
    }
    int n = -1;
    const char *k = strstr(resp, "\"total_grep_matches\":");
    if (k) {
        n = atoi(k + strlen("\"total_grep_matches\":"));
    }
    free(resp);
    return n;
}

/* Plant / probe the route-pinning sentinel (see file header): a private
 * marker TABLE in the DB file. The incremental route mutates rows in place
 * (marker survives); the full-reindex route unlinks and recreates the DB
 * file (marker dies). No schema/FK coupling with production tables. */
static int rot_plant_sentinel(const char *db_path) {
    cbm_store_t *s = cbm_store_open_path(db_path);
    if (!s) {
        return -1;
    }
    sqlite3 *db = cbm_store_get_db(s);
    int rc = db ? sqlite3_exec(db, "CREATE TABLE rot_route_sentinel (x INTEGER);", NULL, NULL,
                               NULL)
                : SQLITE_ERROR;
    cbm_store_close(s);
    return (rc == SQLITE_OK) ? 0 : -1;
}

static int rot_sentinel_alive(const char *db_path) {
    cbm_store_t *s = cbm_store_open_path(db_path);
    if (!s) {
        return 0;
    }
    sqlite3 *db = cbm_store_get_db(s);
    sqlite3_stmt *stmt = NULL;
    int alive = 0;
    if (db && sqlite3_prepare_v2(db,
                                 "SELECT 1 FROM sqlite_master WHERE type='table' "
                                 "AND name='rot_route_sentinel'",
                                 -1, &stmt, NULL) == SQLITE_OK) {
        alive = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    cbm_store_close(s);
    return alive;
}

static const char rot_content_v1[] = "def hello_rotation():\n"
                                     "    return 1\n"
                                     "\n"
                                     "def removed_after_rotation():\n"
                                     "    return 2\n";

static const char rot_content_v2[] = "def hello_rotation():\n"
                                     "    return 1\n"
                                     "\n"
                                     "def added_after_rotation():\n"
                                     "    return 3\n";

TEST(root_rotation_incremental_syncs_root_path) {
    char tree1[] = "/tmp/cbm_rot_t1_XXXXXX";
    char tree2[] = "/tmp/cbm_rot_t2_XXXXXX";
    char tree3[] = "/tmp/cbm_rot_t3_XXXXXX";
    char dbdir[] = "/tmp/cbm_rot_db_XXXXXX";
    ASSERT_NOT_NULL(cbm_mkdtemp(tree1));
    ASSERT_NOT_NULL(cbm_mkdtemp(tree2));
    ASSERT_NOT_NULL(cbm_mkdtemp(tree3));
    ASSERT_NOT_NULL(cbm_mkdtemp(dbdir));
    /* Isolated cache dir: CBM_CACHE_DIR points store-by-project resolution
     * (used by the MCP server below) at this test's private tmp dir, so the
     * pipeline's DB and the MCP server's DB are the same file WITHOUT
     * touching the user's real ~/.cache/codebase-memory-mcp. */
#ifdef _WIN32
    _putenv_s("CBM_CACHE_DIR", dbdir);
#else
    setenv("CBM_CACHE_DIR", dbdir, 1);
#endif
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", dbdir, ROT_PROJECT);

    ASSERT_EQ(rot_write(tree1, "app.py", rot_content_v1), 0);
    ASSERT_EQ(rot_write(tree2, "app.py", rot_content_v1), 0); /* identical → no-op route */
    ASSERT_EQ(rot_write(tree3, "app.py", rot_content_v2), 0); /* changed → row-level dump */
    /* Deterministic no-op routing: tree2's mtime matches tree1's exactly. */
    ASSERT_EQ(rot_sync_mtime(tree1, tree2, "app.py"), 0);

    /* Round 1: full index from tree1. */
    ASSERT_EQ(rot_index(tree1, db_path), 0);
    char *root1 = rot_get_root_path(db_path);
    ASSERT_NOT_NULL(root1);
    ASSERT_TRUE(rot_root_points_at(root1, tree1));
    free(root1);
    ASSERT_TRUE(rot_node_count_by_name(db_path, "hello_rotation") > 0);

    ASSERT_EQ(rot_plant_sentinel(db_path), 0);
    ASSERT_EQ(rot_plant_probe_node(db_path), 0);

    /* Round 2: rotation to tree2, byte-identical content + identical mtime →
     * incremental NO-OP fast path. root_path must follow the rotation. */
    ASSERT_EQ(rot_index(tree2, db_path), 0);
    ASSERT_TRUE(rot_sentinel_alive(db_path)); /* incremental route, DB file preserved */
    /* probe survives ⟺ app.py was NOT re-dumped ⟺ no-op fast path taken */
    ASSERT_TRUE(rot_node_count_by_name(db_path, "noop_probe_node") > 0);
    char *root2 = rot_get_root_path(db_path);
    ASSERT_NOT_NULL(root2);
    ASSERT_TRUE(rot_root_points_at(root2, tree2));
    free(root2);

    /* Round 3: rotation to tree3 with a real change → incremental row-level
     * dump. root_path AND graph content must both land on tree3. */
    ASSERT_EQ(rot_index(tree3, db_path), 0);
    ASSERT_TRUE(rot_sentinel_alive(db_path));
    /* probe dies ⟺ app.py went through the changed-path node re-dump */
    ASSERT_EQ(rot_node_count_by_name(db_path, "noop_probe_node"), 0);
    char *root3 = rot_get_root_path(db_path);
    ASSERT_NOT_NULL(root3);
    ASSERT_TRUE(rot_root_points_at(root3, tree3));
    free(root3);
    ASSERT_TRUE(rot_node_count_by_name(db_path, "added_after_rotation") > 0);
    ASSERT_EQ(rot_node_count_by_name(db_path, "removed_after_rotation"), 0);
    ASSERT_TRUE(rot_node_count_by_name(db_path, "hello_rotation") > 0);

    /* End-to-end (user-visible symptom): search_code greps under the
     * project's CURRENT root_path. The new tree's symbol must hit; the old
     * tree's removed symbol must not — a stale root_path fails both. The
     * store is injected by path (isolated tmp DB, no shared cache dir). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(ROT_PROJECT);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_project(srv, ROT_PROJECT);
    ASSERT_TRUE(rot_search_code_matches(srv, "added_after_rotation") > 0);
    ASSERT_EQ(rot_search_code_matches(srv, "removed_after_rotation"), 0);
    cbm_mcp_server_free(srv);

#ifdef _WIN32
    _putenv_s("CBM_CACHE_DIR", "");
#else
    unsetenv("CBM_CACHE_DIR");
#endif
    th_rmtree(tree1);
    th_rmtree(tree2);
    th_rmtree(tree3);
    th_rmtree(dbdir);
    PASS();
}

SUITE(root_rotation) {
    RUN_TEST(root_rotation_incremental_syncs_root_path);
}
