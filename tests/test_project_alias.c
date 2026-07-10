/*
 * test_project_alias.c — persisted project alias across git worktrees.
 */
#include "test_framework.h"
#include "../src/git/git_context.h"
#include "../src/pipeline/pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pa_git(const char *dir, const char *args) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" -c user.name=t -c user.email=t@t.io "
             "-c init.defaultBranch=master -c commit.gpgsign=false %s",
             dir, args);
    return system(cmd);
}

static int pa_write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t want = strlen(content);
    size_t wrote = fwrite(content, 1, want, fp);
    int rc = (wrote == want && fclose(fp) == 0) ? 0 : -1;
    if (rc != 0) {
        (void)fclose(fp);
    }
    return rc;
}

TEST(project_alias_roundtrip_and_shared_across_worktree) {
    char tmpdir[] = "/tmp/cbm_alias_roundtrip_XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(tmpdir));

    char mainf[1024];
    snprintf(mainf, sizeof(mainf), "%s/main.rs", tmpdir);
    ASSERT_EQ(pa_write_file(mainf, "fn main(){}\n"), 0);
    ASSERT_EQ(pa_git(tmpdir, "init -q"), 0);
    ASSERT_EQ(pa_git(tmpdir, "add main.rs"), 0);
    ASSERT_EQ(pa_git(tmpdir, "commit -q -m init"), 0);

    cbm_git_context_t ctx = {0};
    ASSERT_EQ(cbm_git_context_resolve(tmpdir, &ctx), 0);
    ASSERT_TRUE(ctx.is_git);
    ASSERT_EQ(cbm_git_context_write_project_alias(&ctx, "runai-active"), 0);
    char *alias = cbm_git_context_read_project_alias(&ctx);
    ASSERT_NOT_NULL(alias);
    ASSERT_STR_EQ(alias, "runai-active");
    free(alias);

    char worktree[1024];
    snprintf(worktree, sizeof(worktree), "%s-wt", tmpdir);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "worktree add --detach \"%s\" HEAD", worktree);
    ASSERT_EQ(pa_git(tmpdir, cmd), 0);

    cbm_git_context_t wt = {0};
    ASSERT_EQ(cbm_git_context_resolve(worktree, &wt), 0);
    ASSERT_TRUE(wt.is_worktree);
    char *wt_alias = cbm_git_context_read_project_alias(&wt);
    ASSERT_NOT_NULL(wt_alias);
    ASSERT_STR_EQ(wt_alias, "runai-active");
    free(wt_alias);

    cbm_git_context_free(&wt);
    cbm_git_context_free(&ctx);
    PASS();
}

TEST(pipeline_project_name_prefers_persisted_alias) {
    char tmpdir[] = "/tmp/cbm_alias_pipeline_XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(tmpdir));

    char mainf[1024];
    snprintf(mainf, sizeof(mainf), "%s/main.rs", tmpdir);
    ASSERT_EQ(pa_write_file(mainf, "fn main(){}\n"), 0);
    ASSERT_EQ(pa_git(tmpdir, "init -q"), 0);
    ASSERT_EQ(pa_git(tmpdir, "add main.rs"), 0);
    ASSERT_EQ(pa_git(tmpdir, "commit -q -m init"), 0);

    cbm_git_context_t ctx = {0};
    ASSERT_EQ(cbm_git_context_resolve(tmpdir, &ctx), 0);
    ASSERT_EQ(cbm_git_context_write_project_alias(&ctx, "solo-active"), 0);
    cbm_git_context_free(&ctx);

    char *pn = cbm_pipeline_project_name_for_path(tmpdir);
    ASSERT_NOT_NULL(pn);
    ASSERT_STR_EQ(pn, "solo-active");
    free(pn);

    cbm_pipeline_t *p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(cbm_pipeline_project_name(p), "solo-active");
    ASSERT_EQ(cbm_pipeline_apply_project_alias(p, "solo-alt"), 0);
    ASSERT_STR_EQ(cbm_pipeline_project_name(p), "solo-alt");
    cbm_pipeline_free(p);
    PASS();
}

/* Issue #1: with NO explicit alias, two worktrees of the SAME repo must
 * derive the SAME project name (auto-normalization via git canonical root),
 * instead of each getting a distinct path-derived name. */
TEST(pipeline_project_name_shared_across_worktrees_without_alias) {
    char tmpdir[] = "/tmp/cbm_wt_shared_XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(tmpdir));

    char mainf[1024];
    snprintf(mainf, sizeof(mainf), "%s/main.rs", tmpdir);
    ASSERT_EQ(pa_write_file(mainf, "fn main(){}\n"), 0);
    ASSERT_EQ(pa_git(tmpdir, "init -q"), 0);
    ASSERT_EQ(pa_git(tmpdir, "add main.rs"), 0);
    ASSERT_EQ(pa_git(tmpdir, "commit -q -m init"), 0);

    /* Second worktree of the same repo, NO alias set anywhere. */
    char worktree[1200];
    snprintf(worktree, sizeof(worktree), "%s-wt", tmpdir);
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "worktree add --detach \"%s\" HEAD", worktree);
    ASSERT_EQ(pa_git(tmpdir, cmd), 0);

    /* No alias persisted: read must be NULL in both trees. */
    cbm_git_context_t ca = {0};
    ASSERT_EQ(cbm_git_context_resolve(tmpdir, &ca), 0);
    char *maybe_alias = cbm_git_context_read_project_alias(&ca);
    ASSERT_TRUE(maybe_alias == NULL);
    cbm_git_context_free(&ca);

    /* Both worktrees resolve to the SAME project name via canonical root. */
    char *name_main = cbm_pipeline_project_name_for_path(tmpdir);
    char *name_wt = cbm_pipeline_project_name_for_path(worktree);
    ASSERT_NOT_NULL(name_main);
    ASSERT_NOT_NULL(name_wt);
    ASSERT_STR_EQ(name_main, name_wt);

    /* And the shared name must NOT be the raw worktree-path-derived name
     * (which would differ between the two trees). Sanity: the linked
     * worktree's own path-derived name differs from the shared name. */
    char *path_derived_wt = cbm_project_name_from_path(worktree);
    ASSERT_NOT_NULL(path_derived_wt);
    ASSERT_TRUE(strcmp(path_derived_wt, name_wt) != 0);

    free(path_derived_wt);
    free(name_main);
    free(name_wt);
    PASS();
}

SUITE(project_alias) {
    RUN_TEST(project_alias_roundtrip_and_shared_across_worktree);
    RUN_TEST(pipeline_project_name_prefers_persisted_alias);
    RUN_TEST(pipeline_project_name_shared_across_worktrees_without_alias);
}
