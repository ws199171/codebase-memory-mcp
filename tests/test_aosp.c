/* AOSP workspace discovery and master catalog tests. */
#include "test_framework.h"
#include "test_helpers.h"

#include "aosp/aosp.h"
#include "aosp/build_graph.h"
#include "aosp/federated_graph.h"
#include "aosp/protocol_graph.h"
#include "aosp/structural_graph.h"
#include "foundation/compat_fs.h"
#include "mcp/mcp.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int make_dir(const char *root, const char *relative) {
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/%s", root, relative);
    return th_mkdir_p(path);
}

static int write_relative(const char *root, const char *relative, const char *content) {
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/%s", root, relative);
    return th_write_file(path, content);
}

static int create_workspace_fixture(char **root_out) {
    const char *temp_root = th_mktempdir("cbm_aosp");
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo/manifests") != 0 ||
        make_dir(root, ".repo/local_manifests") != 0 ||
        make_dir(root, "frameworks/base") != 0 ||
        make_dir(root, "frameworks/base/media") != 0 ||
        make_dir(root, "vendor/acme/widgets") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    if (write_relative(root, ".repo/manifest.xml",
            "<?xml version=\"1.0\"?><manifest>"
            "<project name=\"platform/frameworks/base\" path=\"frameworks/base\"/>"
            "<include name=\"extra.xml\"/>"
            "</manifest>") != 0 ||
        write_relative(root, ".repo/manifests/extra.xml",
            "<manifest><project name=\"platform/system/core\" path=\"system/core\"/></manifest>") != 0 ||
        write_relative(root, ".repo/local_manifests/local.xml",
            "<manifest>"
            "<remove-project name=\"platform/system/core\"/>"
            "<project name=\"acme/widgets\" path=\"vendor/acme/widgets\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "frameworks/base/Android.bp",
            "cc_library_shared {\n"
            "  name: \"libframework_audio\",\n"
            "  shared_libs: [\"libvendor_audio\", \"libmissing\"],\n"
            "  static_libs: [\"libbase_defaults\"],\n"
            "  target: { android: { shared_libs: [\"libandroid_extra\"] } },\n"
            "}\n"
            "cc_defaults { name: \"libbase_defaults\" }\n"
            "cc_library { name: \"libandroid_extra\" }\n"
            "aidl_interface { name: \"android.media.audio\", imports: [\"vendor.acme.audio\"] }\n") != 0 ||
        write_relative(root, "frameworks/base/media/IAudioService.aidl",
            "package android.media;\ninterface IAudioService { void start(); }\n") != 0 ||
        write_relative(root, "frameworks/base/media/jni.cpp",
            "static void nativeClose() {}\n"
            "static const JNINativeMethod gMethods[] = {\n"
            "  {\"nativeClose\", \"()V\", (void*) nativeClose},\n"
            "};\n"
            "registerNativeMethods(env, \"android/media/AudioSystem\", gMethods, 1);\n") != 0 ||
        write_relative(root, "vendor/acme/widgets/Android.bp",
            "aidl_interface { name: \"vendor.acme.audio\" }\n") != 0 ||
        write_relative(root, "vendor/acme/widgets/Android.mk",
            "include $(CLEAR_VARS)\n"
            "LOCAL_MODULE := libvendor_audio\n"
            "LOCAL_SHARED_LIBRARIES := libbase_defaults\n"
            "include $(BUILD_SHARED_LIBRARY)\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

TEST(aosp_manifest_include_and_local_override) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(workspace.repo_count, 2);
    ASSERT_STR_EQ(workspace.repos[0].path, "frameworks/base");
    ASSERT_STR_EQ(workspace.repos[1].path, "vendor/acme/widgets");
    ASSERT(workspace.repos[0].exists);
    ASSERT(workspace.repos[1].exists);
    ASSERT_EQ((int)strlen(workspace.workspace_id), CBM_AOSP_ID_LEN);
    ASSERT_EQ((int)strlen(workspace.manifest_hash), CBM_AOSP_HASH_LEN);
    ASSERT(strcmp(workspace.repos[0].repo_id, workspace.repos[1].repo_id) != 0);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_manifest_rejects_parent_path) {
    const char *temp_root = th_mktempdir("cbm_aosp_unsafe");
    ASSERT(temp_root != NULL);
    char *root = strdup(temp_root);
    ASSERT(root != NULL);
    ASSERT_EQ(make_dir(root, ".repo"), 0);
    ASSERT_EQ(write_relative(root, ".repo/manifest.xml",
        "<manifest><project name=\"escape\" path=\"../escape\"/></manifest>"), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_NEQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT(strstr(err, "unsafe") != NULL);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_manifest_keeps_duplicate_names_and_extend_path) {
    const char *temp_root = th_mktempdir("cbm_aosp_duplicate");
    ASSERT(temp_root != NULL);
    char *root = strdup(temp_root);
    ASSERT(root != NULL);
    ASSERT_EQ(make_dir(root, ".repo"), 0);
    ASSERT_EQ(make_dir(root, "vendor/one"), 0);
    ASSERT_EQ(make_dir(root, "vendor/two"), 0);
    ASSERT_EQ(write_relative(root, ".repo/manifest.xml",
        "<manifest>"
        "<project name=\"shared/project\" path=\"vendor/one\"/>"
        "<project name=\"shared/project\" path=\"vendor/two\"/>"
        "<extend-project name=\"shared/project\" path=\"vendor/two\" revision=\"refs/heads/x\"/>"
        "<remove-project name=\"shared/project\" path=\"vendor/one\"/>"
        "</manifest>"), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(workspace.repo_count, 1);
    ASSERT_STR_EQ(workspace.repos[0].path, "vendor/two");
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_workspace_ids_are_root_scoped) {
    char *left = NULL;
    char *right = NULL;
    ASSERT_EQ(create_workspace_fixture(&left), 0);
    ASSERT_EQ(create_workspace_fixture(&right), 0);
    cbm_aosp_workspace_t a;
    cbm_aosp_workspace_t b;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(left, &a, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_discover(right, &b, err, sizeof(err)), 0);
    ASSERT(strcmp(a.workspace_id, b.workspace_id) != 0);
    ASSERT_STR_EQ(a.manifest_hash, b.manifest_hash);
    ASSERT(strcmp(a.repos[0].repo_id, b.repos[0].repo_id) != 0);
    cbm_aosp_workspace_free(&a);
    cbm_aosp_workspace_free(&b);
    th_rmtree(left);
    th_rmtree(right);
    free(left);
    free(right);
    PASS();
}

TEST(aosp_master_sync_and_stats) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    cbm_aosp_master_stats_t stats;
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repo_count, 2);
    ASSERT_EQ(stats.existing_count, 2);
    ASSERT_EQ(stats.missing_count, 0);
    ASSERT_EQ(stats.indexed_count, 0);
    ASSERT_EQ(stats.cross_edge_count, 0);
    ASSERT_EQ(stats.stale_repo_count, 0);
    ASSERT_EQ(stats.refresh_failed_count, 0);

    char db_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, db_path, sizeof(db_path), false), 0);
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN"
        "('repos','symbols','modules','cross_symbol_edges','architecture_summaries');",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 5);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_catalog_and_global_symbol_search) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_path[4096];
    (void)snprintf(shard_path, sizeof(shard_path), "%s/test-shard.db", root);
    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(7,'HandleAudio','aosp.test.audio.HandleAudio','Function','media/audio.cpp',42,58,'{}'),"
        "(8,'audio.cpp','aosp.test.audio.__file__','File','media/audio.cpp',1,80,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);

    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], shard_path,
                                       err, sizeof(err)), 0);
    cbm_aosp_symbol_t *results = NULL;
    int count = 0;
    ASSERT_EQ(cbm_aosp_search_symbols(&workspace, "Handle", 10, &results, &count,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(results[0].name, "HandleAudio");
    ASSERT_STR_EQ(results[0].label, "Function");
    ASSERT_STR_EQ(results[0].repo_path, "frameworks/base");
    ASSERT_EQ((int)strlen(results[0].global_id), CBM_AOSP_HASH_LEN);
    ASSERT_STR_EQ(results[0].repo_id, workspace.repos[0].repo_id);
    ASSERT_EQ(results[0].local_node_id, 7);
    ASSERT(strncmp(results[0].project_name, "aosp-", 5) == 0);
    ASSERT_EQ(results[0].start_line, 42);
    cbm_aosp_symbols_free(results, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"Handle\",\"limit\":10}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_search_symbols", args);
    ASSERT(response != NULL);
    ASSERT(strstr(response, "\"isError\":false") != NULL);
    ASSERT(strstr(response, "HandleAudio") != NULL);
    ASSERT(strstr(response, "frameworks/base") != NULL);
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_resolver_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);";
    const char *repo_zero =
        "INSERT INTO nodes VALUES"
        "(1,'Unique','alpha.Unique','Class','alpha/Unique.java',10,30,'{}'),"
        "(2,'run','common.Service.run','Method','common/Service.java',40,50,'{}'),"
        "(3,'AliasExecute','vendor.deep.Service.execute','Method','vendor/Service.java',60,70,'{}'),"
        "(4,'Duplicate','one.Duplicate','Class','one/Duplicate.java',80,90,'{}'),"
        "(5,'ExactDuplicate','shared.ExactDuplicate','Class','shared/Exact.java',100,110,'{}'),"
        "(6,'call','native.api.Dispatch.call','Function','native/dispatch.cpp',120,130,'{}'),"
        "(7,'resolveSymbol','mixed.android::query.WorkspaceResolver.resolveSymbol',"
        "'Method','mixed/resolver.cpp',140,150,'{}');";
    const char *repo_one =
        "INSERT INTO nodes VALUES"
        "(1,'Other','beta.Other','Class','beta/Other.java',10,30,'{}'),"
        "(2,'run','vendor.common.Service.run','Method','vendor/Service.java',40,50,'{}'),"
        "(3,'Duplicate','two.Duplicate','Class','two/Duplicate.java',60,70,'{}'),"
        "(4,'ExactDuplicate','shared.ExactDuplicate','Class','shared/Exact.java',80,90,'{}');";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, repo_index == 0 ? repo_zero : repo_one,
                          NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_stmt *stmt = NULL;
    if (rc == 0 && repo_index == 1 &&
        sqlite3_prepare_v2(db,
            "INSERT INTO nodes VALUES(?1,'Crowded',?2,'Function','crowded.cpp',1,2,'{}');",
            -1, &stmt, NULL) == SQLITE_OK) {
        for (int i = 0; i < 205 && rc == 0; i++) {
            char qualified_name[128];
            (void)snprintf(qualified_name, sizeof(qualified_name),
                           "crowded.%03d.Crowded", i);
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_int(stmt, 1, 100 + i);
            sqlite3_bind_text(stmt, 2, qualified_name, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
        }
    } else if (rc == 0 && repo_index == 1) {
        rc = -1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_workspace_symbol_resolver_tiers_and_ambiguity) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/resolver-%d.db", root, i);
        ASSERT_EQ(create_resolver_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
    }
    char master_path[4096];
    sqlite3 *master = NULL;
    sqlite3_stmt *schema_stmt = NULL;
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(master,
        "UPDATE symbols SET qualified_leaf='' "
        "WHERE qualified_name='vendor.deep.Service.execute';"
        "DELETE FROM schema_versions WHERE version=7;",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(master);
    master = NULL;
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    ASSERT_EQ(sqlite3_open_v2(master_path, &master, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT (SELECT count(*) FROM schema_versions WHERE version=7),"
        "(SELECT count(*) FROM symbols WHERE qualified_name='vendor.deep.Service.execute' "
        "AND qualified_leaf='execute');",
        -1, &schema_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(schema_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(schema_stmt, 0), 1);
    ASSERT_EQ(sqlite3_column_int(schema_stmt, 1), 1);
    sqlite3_finalize(schema_stmt);
    sqlite3_close(master);

    cbm_aosp_symbol_resolution_t resolution;
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Unique", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_NAME);
    ASSERT_EQ(resolution.candidate_count, 1);
    ASSERT_EQ(resolution.total_candidate_count, 1);
    ASSERT_FALSE(resolution.truncated);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "alpha.Unique");
    ASSERT_STR_EQ(resolution.candidates[0].repo_id, workspace.repos[0].repo_id);
    ASSERT_EQ(resolution.candidates[0].local_node_id, 1);
    char *unique_global_id = strdup(resolution.candidates[0].global_id);
    ASSERT_NOT_NULL(unique_global_id);
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, unique_global_id, &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_GLOBAL_ID);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "alpha.Unique");
    cbm_aosp_symbol_resolution_free(&resolution);
    free(unique_global_id);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "common.Service.run", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "common.Service.run");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "native::api::Dispatch::call", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "native.api.Dispatch.call");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(
        &workspace, "android::query::WorkspaceResolver::resolveSymbol", &resolution,
        err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_QUALIFIED_SUFFIX);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name,
                  "mixed.android::query.WorkspaceResolver.resolveSymbol");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Service.execute", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_QUALIFIED_SUFFIX);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "vendor.deep.Service.execute");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Duplicate", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_AMBIGUOUS);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_NAME);
    ASSERT_EQ(resolution.candidate_count, 2);
    ASSERT_EQ(resolution.total_candidate_count, 2);
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "shared.ExactDuplicate", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_AMBIGUOUS);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME);
    ASSERT_EQ(resolution.candidate_count, 2);
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Crowded", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_AMBIGUOUS);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_NAME);
    ASSERT_EQ(resolution.candidate_count, 200);
    ASSERT_EQ(resolution.total_candidate_count, 205);
    ASSERT_TRUE(resolution.truncated);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "crowded.000.Crowded");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "wrong.Duplicate", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_NOT_FOUND);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_NONE);
    ASSERT_EQ(resolution.candidate_count, 0);
    cbm_aosp_symbol_resolution_free(&resolution);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_routing_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "CREATE TABLE edges(id INTEGER PRIMARY KEY,source_id INTEGER,target_id INTEGER,"
        "type TEXT,properties TEXT);";
    const char *repo_zero =
        "INSERT INTO nodes VALUES"
        "(1,'Unique','alpha.Unique','Class','alpha/Unique.java',10,30,'{}'),"
        "(2,'Helper','alpha.Helper','Class','alpha/Helper.java',40,50,'{}'),"
        "(3,'Service','alpha.Service','Class','alpha/Service.java',60,70,'{}');"
        "INSERT INTO edges VALUES"
        "(1,1,2,'CALLS','{}'),"
        "(2,2,3,'USES_TYPE','{\"note\":\"test\"}'),"
        "(3,3,1,'IMPLEMENTS','{}');";
    const char *repo_one =
        "INSERT INTO nodes VALUES"
        "(1,'Other','beta.Other','Class','beta/Other.java',10,30,'{}'),"
        "(2,'Consumer','beta.Consumer','Class','beta/Consumer.java',40,50,'{}');"
        "INSERT INTO edges VALUES"
        "(1,2,1,'CALLS','{}');";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, repo_index == 0 ? repo_zero : repo_one,
                          NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

static int mark_repo_indexed(const cbm_aosp_workspace_t *workspace, const char *repo_id,
                             const char *db_path) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "UPDATE repos SET status='indexed', db_path=?1 "
            "WHERE workspace_id=?2 AND repo_id=?3;",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, db_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, repo_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_shard_routing_routes_symbols_and_reads_nodes_and_edges) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/routing-%d.db", root, i);
        ASSERT_EQ(create_routing_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[i].repo_id, shard_paths[i]), 0);
    }

    /* Resolve a symbol to get its global_id via Q1 */
    cbm_aosp_symbol_resolution_t resolution;
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Unique", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.candidate_count, 1);
    char *unique_global_id = strdup(resolution.candidates[0].global_id);
    ASSERT_NOT_NULL(unique_global_id);
    cbm_aosp_symbol_resolution_free(&resolution);

    /* Q2: Route by global_id to the owning shard */
    cbm_aosp_shard_route_t route;
    ASSERT_EQ(cbm_aosp_shard_route(&workspace, unique_global_id, &route, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(route.shard);
    ASSERT_STR_EQ(route.repo_id, workspace.repos[0].repo_id);
    ASSERT_EQ(route.local_node_id, 1);
    ASSERT_STR_EQ(route.global_id, unique_global_id);
    ASSERT_NOT_NULL(route.shard_path);
    ASSERT_STR_EQ(route.shard_path, shard_paths[0]);

    /* Read the routed node from the shard */
    cbm_aosp_symbol_t *node = calloc(1, sizeof(*node));
    ASSERT_NOT_NULL(node);
    ASSERT_EQ(cbm_aosp_shard_read_node(&route, node, err, sizeof(err)), 0);
    ASSERT_STR_EQ(node->name, "Unique");
    ASSERT_STR_EQ(node->qualified_name, "alpha.Unique");
    ASSERT_STR_EQ(node->label, "Class");
    ASSERT_STR_EQ(node->file_path, "alpha/Unique.java");
    ASSERT_EQ(node->start_line, 10);
    ASSERT_EQ(node->end_line, 30);
    ASSERT_EQ(node->local_node_id, 1);
    ASSERT_STR_EQ(node->repo_id, workspace.repos[0].repo_id);
    ASSERT_STR_EQ(node->global_id, unique_global_id);
    cbm_aosp_symbols_free(node, 1);

    /* Read outgoing edges: Unique -> Helper (CALLS) */
    cbm_aosp_shard_edge_t *out_edges = NULL;
    int out_count = 0;
    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_OUTGOING,
                                        &out_edges, &out_count, err, sizeof(err)), 0);
    ASSERT_EQ(out_count, 1);
    ASSERT_STR_EQ(out_edges[0].type, "CALLS");
    ASSERT_EQ(out_edges[0].source_id, 1);
    ASSERT_EQ(out_edges[0].target_id, 2);
    ASSERT_EQ(out_edges[0].neighbor_id, 2);
    ASSERT_STR_EQ(out_edges[0].neighbor_name, "Helper");
    ASSERT_STR_EQ(out_edges[0].neighbor_qualified_name, "alpha.Helper");
    ASSERT_STR_EQ(out_edges[0].neighbor_label, "Class");
    cbm_aosp_shard_edges_free(out_edges, out_count);

    /* Read incoming edges: Service -> Unique (IMPLEMENTS) */
    cbm_aosp_shard_edge_t *in_edges = NULL;
    int in_count = 0;
    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_INCOMING,
                                        &in_edges, &in_count, err, sizeof(err)), 0);
    ASSERT_EQ(in_count, 1);
    ASSERT_STR_EQ(in_edges[0].type, "IMPLEMENTS");
    ASSERT_EQ(in_edges[0].source_id, 3);
    ASSERT_EQ(in_edges[0].target_id, 1);
    ASSERT_EQ(in_edges[0].neighbor_id, 3);
    ASSERT_STR_EQ(in_edges[0].neighbor_name, "Service");
    ASSERT_STR_EQ(in_edges[0].neighbor_qualified_name, "alpha.Service");
    cbm_aosp_shard_edges_free(in_edges, in_count);
    cbm_aosp_shard_route_close(&route);

    /* Q2: Route by resolved symbol (skips Master global_id lookup) */
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Unique", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_shard_route_symbol(&workspace, resolution.candidates, &route,
                                          err, sizeof(err)), 0);
    ASSERT_NOT_NULL(route.shard);
    ASSERT_EQ(route.local_node_id, 1);
    ASSERT_STR_EQ(route.repo_id, workspace.repos[0].repo_id);
    cbm_aosp_shard_route_close(&route);
    cbm_aosp_symbol_resolution_free(&resolution);

    /* Q2: Route to repo 1 and verify cross-shard isolation */
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "beta.Consumer", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    char *consumer_global_id = strdup(resolution.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_shard_route(&workspace, consumer_global_id, &route, err, sizeof(err)), 0);
    ASSERT_STR_EQ(route.repo_id, workspace.repos[1].repo_id);
    ASSERT_EQ(route.local_node_id, 2);
    ASSERT_STR_EQ(route.shard_path, shard_paths[1]);
    node = calloc(1, sizeof(*node));
    ASSERT_NOT_NULL(node);
    ASSERT_EQ(cbm_aosp_shard_read_node(&route, node, err, sizeof(err)), 0);
    ASSERT_STR_EQ(node->qualified_name, "beta.Consumer");
    cbm_aosp_symbols_free(node, 1);

    /* Consumer has outgoing CALLS to Other, no incoming edges */
    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_OUTGOING,
                                        &out_edges, &out_count, err, sizeof(err)), 0);
    ASSERT_EQ(out_count, 1);
    ASSERT_STR_EQ(out_edges[0].type, "CALLS");
    ASSERT_STR_EQ(out_edges[0].neighbor_qualified_name, "beta.Other");
    cbm_aosp_shard_edges_free(out_edges, out_count);

    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_INCOMING,
                                        &in_edges, &in_count, err, sizeof(err)), 0);
    ASSERT_EQ(in_count, 0);
    cbm_aosp_shard_route_close(&route);
    free(consumer_global_id);

    /* Q2: Missing global_id returns error */
    ASSERT_EQ(cbm_aosp_shard_route(&workspace, "nonexistent-global-id-000000",
                                   &route, err, sizeof(err)), -1);
    ASSERT(strstr(err, "not found") != NULL);

    /* Q2: Routing to a repo without indexed shard fails */
    {
        sqlite3 *master = NULL;
        char master_path[4096];
        ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
        ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
        sqlite3_stmt *ins = NULL;
        ASSERT_EQ(sqlite3_prepare_v2(master,
            "INSERT INTO symbols(global_id,workspace_id,repo_id,local_node_id,name,"
            "qualified_name,label,qualified_leaf,file_path,start_line,end_line,properties) "
            "VALUES('fake-orphan-id',?1,'orphan-repo',1,'Orphan','orphan.Orphan',"
            "'Class','Orphan','orphan.java',1,2,'{}');",
            -1, &ins, NULL), SQLITE_OK);
        sqlite3_bind_text(ins, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
        sqlite3_finalize(ins);
        ASSERT_EQ(sqlite3_prepare_v2(master,
            "INSERT OR IGNORE INTO repos(repo_id,workspace_id,manifest_name,path,abs_path,"
            "status,generation) VALUES('orphan-repo',?1,'orphan','orphan','/orphan',"
            "'discovered','g');",
            -1, &ins, NULL), SQLITE_OK);
        sqlite3_bind_text(ins, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
        sqlite3_finalize(ins);
        sqlite3_close(master);
    }
    ASSERT_EQ(cbm_aosp_shard_route(&workspace, "fake-orphan-id", &route, err, sizeof(err)), -1);
    ASSERT(strstr(err, "not indexed") != NULL);

    free(unique_global_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_trace_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "CREATE TABLE edges(id INTEGER PRIMARY KEY,source_id INTEGER,target_id INTEGER,"
        "type TEXT,properties TEXT);";
    const char *repo_zero =
        "INSERT INTO nodes VALUES"
        "(1,'Start','alpha.Start','Class','alpha/Start.java',10,20,'{}'),"
        "(2,'Mid','alpha.Mid','Class','alpha/Mid.java',30,40,'{}');"
        "INSERT INTO edges VALUES(1,1,2,'CALLS','{}');";
    const char *repo_one =
        "INSERT INTO nodes VALUES"
        "(1,'Target','beta.Target','Class','beta/Target.java',10,20,'{}'),"
        "(2,'Deep','beta.Deep','Class','beta/Deep.java',30,40,'{}');"
        "INSERT INTO edges VALUES(1,1,2,'CALLS','{}');";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, repo_index == 0 ? repo_zero : repo_one,
                          NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

static int insert_cross_edge(const cbm_aosp_workspace_t *workspace,
                             const char *edge_id, const char *source_repo_id,
                             const char *target_repo_id, const char *source_global_id,
                             const char *target_global_id, const char *type,
                             double confidence, const char *evidence) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO cross_symbol_edges(edge_id,workspace_id,source_repo_id,target_repo_id,"
            "source_global_id,target_global_id,target_name,target_leaf,type,status,"
            "confidence,evidence,source_generation,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,'','',?7,'resolved',?8,?9,'','{}');",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source_repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, target_repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, source_global_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, target_global_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 8, confidence);
    sqlite3_bind_text(stmt, 9, evidence, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_trace_path_traverses_local_and_cross_repo_edges) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/trace-%d.db", root, i);
        ASSERT_EQ(create_trace_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[i].repo_id, shard_paths[i]), 0);
    }

    /* Resolve symbols to get global IDs */
    cbm_aosp_symbol_resolution_t res;
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Start", &res, err, sizeof(err)), 0);
    ASSERT_EQ(res.candidate_count, 1);
    char *start_gid = strdup(res.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&res);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Mid", &res, err, sizeof(err)), 0);
    char *mid_gid = strdup(res.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&res);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "beta.Target", &res, err, sizeof(err)), 0);
    char *target_gid = strdup(res.candidates[0].global_id);
    char *target_repo_id = strdup(res.candidates[0].repo_id);
    cbm_aosp_symbol_resolution_free(&res);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "beta.Deep", &res, err, sizeof(err)), 0);
    char *deep_gid = strdup(res.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&res);

    /* Insert cross-repository edges:
     *   alpha.Start --CALLS--> beta.Target (confidence 0.9)
     *   alpha.Mid   --USES_TYPE--> beta.Deep (confidence 0.8) */
    ASSERT_EQ(insert_cross_edge(&workspace, "edge1", workspace.repos[0].repo_id,
                                target_repo_id, start_gid, target_gid, "CALLS", 0.9,
                                "cross_calls"), 0);
    ASSERT_EQ(insert_cross_edge(&workspace, "edge2", workspace.repos[0].repo_id,
                                target_repo_id, mid_gid, deep_gid, "USES_TYPE", 0.8,
                                "cross_uses"), 0);

    /* Test: full outgoing traversal from alpha.Start
     * Graph: Start -> Mid (local), Start -> Target (cross), Mid -> Deep (cross),
     *        Target -> Deep (local, but Deep already visited via cross) */
    cbm_aosp_trace_options_t opts = {0};
    opts.max_depth = -1;
    opts.direction = CBM_AOSP_TRACE_OUTGOING;
    opts.result_budget = 0;
    opts.cancel_flag = NULL;

    cbm_aosp_trace_result_t result;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 4);
    ASSERT_FALSE(result.truncated);
    ASSERT_EQ(result.max_depth_reached, 2);
    /* Start node */
    ASSERT_STR_EQ(result.nodes[0].global_id, start_gid);
    ASSERT_EQ(result.nodes[0].depth, 0);
    ASSERT_FALSE(result.nodes[0].cross_repo);
    ASSERT(result.nodes[0].edge_type == NULL);
    /* Depth 1 nodes: Mid (local) and Target (cross) */
    bool found_mid = false, found_target = false;
    for (int i = 1; i <= 2; i++) {
        if (strcmp(result.nodes[i].global_id, mid_gid) == 0) {
            found_mid = true;
            ASSERT_EQ(result.nodes[i].depth, 1);
            ASSERT_FALSE(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "CALLS");
            ASSERT_EQ(result.nodes[i].confidence, 1.0);
        }
        if (strcmp(result.nodes[i].global_id, target_gid) == 0) {
            found_target = true;
            ASSERT_EQ(result.nodes[i].depth, 1);
            ASSERT(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "CALLS");
            ASSERT_EQ(result.nodes[i].confidence, 0.9);
        }
    }
    ASSERT(found_mid);
    ASSERT(found_target);
    /* Depth 2: Deep (reached via cross from Mid or local from Target) */
    ASSERT_STR_EQ(result.nodes[3].global_id, deep_gid);
    ASSERT_EQ(result.nodes[3].depth, 2);
    cbm_aosp_trace_result_free(&result);

    /* Test: max_depth=0 returns only start */
    opts.max_depth = 0;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 1);
    ASSERT_EQ(result.max_depth_reached, 0);
    cbm_aosp_trace_result_free(&result);

    /* Test: max_depth=1 returns start + depth 1 */
    opts.max_depth = 1;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 3);
    ASSERT_EQ(result.max_depth_reached, 1);
    cbm_aosp_trace_result_free(&result);

    /* Test: result_budget=2 truncates */
    opts.max_depth = -1;
    opts.result_budget = 2;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 2);
    ASSERT(result.truncated);
    cbm_aosp_trace_result_free(&result);

    /* Test: incoming direction from beta.Deep finds:
     *   Depth 0: Deep
     *   Depth 1: Target (local CALLS), Mid (cross USES_TYPE)
     *   Depth 2: Start (via Target cross Start→Target, or Mid local Start→Mid) */
    opts.max_depth = -1;
    opts.direction = CBM_AOSP_TRACE_INCOMING;
    opts.result_budget = 0;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, deep_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 4);
    /* Start: Deep */
    ASSERT_STR_EQ(result.nodes[0].global_id, deep_gid);
    ASSERT_EQ(result.nodes[0].depth, 0);
    /* Depth 1: Target (local CALLS) and Mid (cross USES_TYPE) */
    bool found_target_in = false, found_mid_in = false;
    for (int i = 1; i <= 2; i++) {
        if (strcmp(result.nodes[i].global_id, target_gid) == 0) {
            found_target_in = true;
            ASSERT_FALSE(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "CALLS");
        }
        if (strcmp(result.nodes[i].global_id, mid_gid) == 0) {
            found_mid_in = true;
            ASSERT(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "USES_TYPE");
        }
    }
    ASSERT(found_target_in);
    ASSERT(found_mid_in);
    cbm_aosp_trace_result_free(&result);

    /* Test: missing start symbol returns error */
    opts.direction = CBM_AOSP_TRACE_OUTGOING;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, "nonexistent-id", &opts, &result, err, sizeof(err)),
              -1);
    ASSERT(strstr(err, "not found") != NULL);

    free(start_gid);
    free(mid_gid);
    free(target_gid);
    free(target_repo_id);
    free(deep_gid);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_resolves_cross_repo_modules) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.blueprint_files, 2);
    ASSERT_EQ(stats.make_files, 1);
    ASSERT_EQ(stats.aidl_files, 1);
    ASSERT_EQ(stats.module_count, 7);
    ASSERT_EQ(stats.dependency_count, 6);
    ASSERT_EQ(stats.resolved_count, 5);
    ASSERT_EQ(stats.unresolved_count, 1);

    cbm_aosp_module_t *modules = NULL;
    int count = 0;
    ASSERT_EQ(cbm_aosp_search_modules(&workspace, "libframework_audio", 10, &modules,
                                      &count, err, sizeof(err)), 0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(modules[0].repo_path, "frameworks/base");
    ASSERT_EQ(modules[0].outgoing_dependencies, 3);
    cbm_aosp_modules_free(modules, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"libframework_audio\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT(response != NULL);
    ASSERT(strstr(response, "\"isError\":false") != NULL);
    ASSERT(strstr(response, "libframework_audio") != NULL);
    ASSERT(strstr(response, "dependencies_unresolved") != NULL);
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_protocol_graph_links_binder_and_jni_evidence) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_path[4096];
    (void)snprintf(shard_path, sizeof(shard_path), "%s/protocol-shard.db", root);
    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(1,'start','aosp.media.BnAudioService.start','Method','media/BnAudioService.cpp',1,3,'{}'),"
        "(2,'start','aosp.media.BpAudioService.start','Method','media/BpAudioService.cpp',1,3,'{}'),"
        "(3,'nativeOpen','aosp.android.media.AudioSystem.nativeOpen','Method','media/AudioSystem.java',4,4,'{}'),"
        "(4,'Java_android_media_AudioSystem_nativeOpen','aosp.jni.Java_android_media_AudioSystem_nativeOpen','Function','media/jni.cpp',5,5,'{}'),"
        "(5,'nativeClose','aosp.android.media.AudioSystem.nativeClose','Method','media/AudioSystem.java',6,6,'{}'),"
        "(6,'nativeClose','aosp.jni.nativeClose','Function','media/jni.cpp',1,1,'{}'),"
        "(7,'nativeClose','aosp.other.nativeClose','Function','media/other.cpp',1,1,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], shard_path,
                                       err, sizeof(err)), 0);

    cbm_aosp_protocol_stats_t stats;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.aidl_interfaces, 1);
    ASSERT_EQ(stats.aidl_methods, 1);
    ASSERT_EQ(stats.binder_server_edges, 1);
    ASSERT_EQ(stats.binder_client_edges, 1);
    ASSERT_EQ(stats.jni_static_edges, 1);
    ASSERT_EQ(stats.jni_dynamic_edges, 1);

    cbm_aosp_protocol_node_t *nodes = NULL;
    int count = 0;
    ASSERT_EQ(cbm_aosp_search_protocols(&workspace, "IAudioService", 20, &nodes,
                                        &count, err, sizeof(err)), 0);
    ASSERT(count >= 2);
    cbm_aosp_protocol_nodes_free(nodes, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"IAudioService\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_trace_protocol", args);
    ASSERT(response != NULL);
    ASSERT(strstr(response, "\"isError\":false") != NULL);
    ASSERT(strstr(response, "binder_server_edges") != NULL);
    ASSERT(strstr(response, "aidl_method_generated_owner") != NULL);
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int catalog_single_symbol(const cbm_aosp_workspace_t *workspace,
                                 const cbm_aosp_repo_t *repo, const char *db_path,
                                 const char *name, const char *qualified_name) {
    sqlite3 *shard = NULL;
    if (sqlite3_open(db_path, &shard) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);";
    if (sqlite3_exec(shard, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(shard);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(shard,
            "INSERT INTO nodes VALUES(1,?1,?2,'Function','source.cpp',1,2,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(shard);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, qualified_name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(shard);
    if (rc != 0) return rc;
    char err[512] = {0};
    return cbm_aosp_catalog_repo_db(workspace, repo, db_path, err, sizeof(err));
}

static char *find_symbol_id(const cbm_aosp_workspace_t *workspace, const char *name) {
    cbm_aosp_symbol_t *symbols = NULL;
    int count = 0;
    char err[512] = {0};
    if (cbm_aosp_search_symbols(workspace, name, 10, &symbols, &count,
                                err, sizeof(err)) != 0 || count != 1) {
        cbm_aosp_symbols_free(symbols, count);
        return NULL;
    }
    char master_path[4096];
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    char *global_id = NULL;
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) == 0 &&
        sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
            "SELECT global_id FROM symbols WHERE workspace_id=?1 AND name=?2;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *value = (const char *)sqlite3_column_text(stmt, 0);
            global_id = value ? strdup(value) : NULL;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    cbm_aosp_symbols_free(symbols, count);
    return global_id;
}

TEST(aosp_cross_edges_are_deterministic_and_refresh_per_source_repo) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char source_db[4096];
    char target_db[4096];
    (void)snprintf(source_db, sizeof(source_db), "%s/source-shard.db", root);
    (void)snprintf(target_db, sizeof(target_db), "%s/target-shard.db", root);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[0], source_db,
                                    "CallVendor", "aosp.framework.CallVendor"), 0);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[1], target_db,
                                    "VendorEntry", "aosp.vendor.VendorEntry"), 0);
    char *source_id = find_symbol_id(&workspace, "CallVendor");
    char *target_id = find_symbol_id(&workspace, "VendorEntry");
    ASSERT_NOT_NULL(source_id);
    ASSERT_NOT_NULL(target_id);

    cbm_aosp_cross_edge_candidate_t candidates[] = {
        {.source_global_id = source_id, .target_global_id = target_id,
         .target_name = "VendorEntry", .type = "CALLS",
         .status = CBM_AOSP_CROSS_EDGE_RESOLVED, .confidence = 0.95,
         .evidence = "qualified_call", .properties = "{}"},
        {.source_global_id = source_id, .target_global_id = target_id,
         .target_name = "VendorEntry", .type = "IMPLEMENTS",
         .status = CBM_AOSP_CROSS_EDGE_AMBIGUOUS, .confidence = 0.5,
         .evidence = "multiple_type_candidates", .properties = "{}"},
        {.source_global_id = source_id, .target_name = "MissingType", .type = "USES_TYPE",
         .status = CBM_AOSP_CROSS_EDGE_UNRESOLVED, .confidence = 0.0,
         .evidence = "unresolved_type", .properties = "{}"},
    };
    cbm_aosp_cross_edge_stats_t stats;
    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&workspace, &workspace.repos[0], candidates, 3,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 3);
    ASSERT_EQ(stats.resolved_count, 1);
    ASSERT_EQ(stats.ambiguous_count, 1);
    ASSERT_EQ(stats.unresolved_count, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT group_concat(edge_id,'|') FROM "
        "(SELECT edge_id FROM cross_symbol_edges ORDER BY edge_id);", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *first_ids_text = (const char *)sqlite3_column_text(stmt, 0);
    char *first_ids = first_ids_text ? strdup(first_ids_text) : NULL;
    ASSERT_NOT_NULL(first_ids);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&workspace, &workspace.repos[0], candidates, 3,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 3);
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT group_concat(edge_id,'|') FROM "
        "(SELECT edge_id FROM cross_symbol_edges ORDER BY edge_id);", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), first_ids);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&workspace, &workspace.repos[0], candidates, 1,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 1);
    ASSERT_EQ(stats.resolved_count, 1);
    ASSERT_EQ(stats.unresolved_count, 0);

    free(first_ids);
    free(source_id);
    free(target_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_cross_edges_migrate_v3_schema_without_losing_resolved_edges) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char source_db[4096];
    char target_db[4096];
    (void)snprintf(source_db, sizeof(source_db), "%s/migrate-source.db", root);
    (void)snprintf(target_db, sizeof(target_db), "%s/migrate-target.db", root);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[0], source_db,
                                    "LegacySource", "legacy.Source"), 0);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[1], target_db,
                                    "LegacyTarget", "legacy.Target"), 0);
    char *source_id = find_symbol_id(&workspace, "LegacySource");
    char *target_id = find_symbol_id(&workspace, "LegacyTarget");
    ASSERT_NOT_NULL(source_id);
    ASSERT_NOT_NULL(target_id);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "DROP TABLE cross_symbol_edges;"
        "DELETE FROM schema_versions WHERE version=4;"
        "CREATE TABLE cross_symbol_edges("
        "source_global_id TEXT NOT NULL,target_global_id TEXT NOT NULL,type TEXT NOT NULL,"
        "confidence REAL NOT NULL DEFAULT 0,evidence TEXT DEFAULT '',properties TEXT DEFAULT '{}',"
        "PRIMARY KEY(source_global_id,target_global_id,type));",
        NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO cross_symbol_edges VALUES(?1,?2,'CALLS',0.8,'legacy_test','{}');",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    cbm_aosp_cross_edge_stats_t stats;
    ASSERT_EQ(cbm_aosp_cross_edge_stats(&workspace, &workspace.repos[0], &stats,
                                        err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 1);
    ASSERT_EQ(stats.resolved_count, 1);
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT length(edge_id),status,evidence,target_leaf FROM cross_symbol_edges;",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), CBM_AOSP_EDGE_ID_LEN);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "resolved");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "legacy_test");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "LegacyTarget");
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "DROP TABLE cross_symbol_edges;"
        "DELETE FROM schema_versions WHERE version=5;"
        "CREATE TABLE cross_symbol_edges("
        "edge_id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,source_repo_id TEXT NOT NULL,"
        "target_repo_id TEXT,source_global_id TEXT NOT NULL,target_global_id TEXT,"
        "target_name TEXT NOT NULL DEFAULT '',type TEXT NOT NULL,status TEXT NOT NULL,"
        "confidence REAL NOT NULL DEFAULT 0,evidence TEXT NOT NULL DEFAULT '',"
        "source_generation TEXT NOT NULL DEFAULT '',properties TEXT NOT NULL DEFAULT '{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM pragma_table_info('cross_symbol_edges') WHERE name='target_leaf';",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM schema_versions WHERE version=5;", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    free(source_id);
    free(target_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_cross_edges_enforce_workspace_and_repository_boundaries) {
    char *left_root = NULL;
    char *right_root = NULL;
    ASSERT_EQ(create_workspace_fixture(&left_root), 0);
    ASSERT_EQ(create_workspace_fixture(&right_root), 0);
    cbm_aosp_workspace_t left;
    cbm_aosp_workspace_t right;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(left_root, &left, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_discover(right_root, &right, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&left, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&right, err, sizeof(err)), 0);

    char left_source_db[4096];
    char left_target_db[4096];
    char right_source_db[4096];
    char right_target_db[4096];
    (void)snprintf(left_source_db, sizeof(left_source_db), "%s/source.db", left_root);
    (void)snprintf(left_target_db, sizeof(left_target_db), "%s/target.db", left_root);
    (void)snprintf(right_source_db, sizeof(right_source_db), "%s/source.db", right_root);
    (void)snprintf(right_target_db, sizeof(right_target_db), "%s/target.db", right_root);
    ASSERT_EQ(catalog_single_symbol(&left, &left.repos[0], left_source_db,
                                    "CallVendor", "same.CallVendor"), 0);
    ASSERT_EQ(catalog_single_symbol(&left, &left.repos[1], left_target_db,
                                    "VendorEntry", "same.VendorEntry"), 0);
    ASSERT_EQ(catalog_single_symbol(&right, &right.repos[0], right_source_db,
                                    "CallVendor", "same.CallVendor"), 0);
    ASSERT_EQ(catalog_single_symbol(&right, &right.repos[1], right_target_db,
                                    "VendorEntry", "same.VendorEntry"), 0);
    char *left_source = find_symbol_id(&left, "CallVendor");
    char *left_target = find_symbol_id(&left, "VendorEntry");
    char *right_source = find_symbol_id(&right, "CallVendor");
    char *right_target = find_symbol_id(&right, "VendorEntry");
    ASSERT_NOT_NULL(left_source);
    ASSERT_NOT_NULL(left_target);
    ASSERT_NOT_NULL(right_source);
    ASSERT_NOT_NULL(right_target);

    cbm_aosp_cross_edge_candidate_t left_edge = {
        .source_global_id = left_source, .target_global_id = left_target,
        .target_name = "VendorEntry", .type = "CALLS",
        .status = CBM_AOSP_CROSS_EDGE_RESOLVED, .confidence = 1.0,
        .evidence = "exact_qualified_name", .properties = "{}",
    };
    cbm_aosp_cross_edge_candidate_t right_edge = left_edge;
    right_edge.source_global_id = right_source;
    right_edge.target_global_id = right_target;
    cbm_aosp_cross_edge_stats_t stats;
    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&left, &left.repos[0], &left_edge, 1,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&right, &right.repos[0], &right_edge, 1,
                                           &stats, err, sizeof(err)), 0);

    char left_master[4096];
    char right_master[4096];
    ASSERT_EQ(cbm_aosp_master_path(&left, left_master, sizeof(left_master), false), 0);
    ASSERT_EQ(cbm_aosp_master_path(&right, right_master, sizeof(right_master), false), 0);
    sqlite3 *left_db = NULL;
    sqlite3 *right_db = NULL;
    sqlite3_stmt *left_stmt = NULL;
    sqlite3_stmt *right_stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(left_master, &left_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_open_v2(right_master, &right_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(left_db, "SELECT edge_id FROM cross_symbol_edges;",
                                 -1, &left_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(right_db, "SELECT edge_id FROM cross_symbol_edges;",
                                 -1, &right_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(left_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_step(right_stmt), SQLITE_ROW);
    ASSERT(strcmp((const char *)sqlite3_column_text(left_stmt, 0),
                  (const char *)sqlite3_column_text(right_stmt, 0)) != 0);
    sqlite3_finalize(left_stmt);
    sqlite3_finalize(right_stmt);
    sqlite3_close(left_db);
    sqlite3_close(right_db);

    ASSERT_EQ(sqlite3_open(left_master, &left_db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(left_db,
        "INSERT INTO cross_edge_refresh_queue(workspace_id,source_repo_id,reason,queued_at) "
        "VALUES(?1,?2,'retry_test',strftime('%s','now')) ON CONFLICT DO UPDATE SET "
        "reason=excluded.reason,queued_at=excluded.queued_at;", -1, &left_stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(left_stmt, 1, left.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(left_stmt, 2, left.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(left_stmt), SQLITE_DONE);
    sqlite3_finalize(left_stmt);
    sqlite3_close(left_db);

    left_edge.target_global_id = right_target;
    ASSERT_NEQ(cbm_aosp_cross_edges_refresh(&left, &left.repos[0], &left_edge, 1,
                                            &stats, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_cross_edge_stats(&left, &left.repos[0], &stats,
                                        err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 1);
    ASSERT_EQ(sqlite3_open_v2(left_master, &left_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(left_db,
        "SELECT count(*) FROM cross_edge_refresh_queue WHERE workspace_id=?1 AND source_repo_id=?2;",
        -1, &left_stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(left_stmt, 1, left.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(left_stmt, 2, left.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(left_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(left_stmt, 0), 1);
    sqlite3_finalize(left_stmt);
    sqlite3_close(left_db);

    free(left_source);
    free(left_target);
    free(right_source);
    free(right_target);
    cbm_aosp_workspace_free(&left);
    cbm_aosp_workspace_free(&right);
    th_rmtree(left_root);
    th_rmtree(right_root);
    free(left_root);
    free(right_root);
    PASS();
}

static int insert_shard_node(sqlite3_stmt *stmt, int id, const char *name,
                             const char *qualified_name, const char *label,
                             const char *file_path) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, qualified_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
}

static int create_structural_shard(const char *path, const char *project, bool source) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK ||
        sqlite3_exec(db,
            "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
            "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);",
            NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO nodes VALUES(?1,?2,?3,?4,?5,1,2,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    char qn[1024];
    int rc = 0;
    if (source) {
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.java.__file__", project);
        rc |= insert_shard_node(stmt, 1, "Client.java", qn, "File", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.client.cpp.__file__", project);
        rc |= insert_shard_node(stmt, 2, "client.cpp", qn, "File", "client.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client", project);
        rc |= insert_shard_node(stmt, 3, "Client", qn, "Class", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.convert", project);
        rc |= insert_shard_node(stmt, 4, "convert", qn, "Method", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.LocalBase", project);
        rc |= insert_shard_node(stmt, 5, "LocalBase", qn, "Class", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.LocalChild", project);
        rc |= insert_shard_node(stmt, 6, "LocalChild", qn, "Class", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.client.NativeClient", project);
        rc |= insert_shard_node(stmt, 7, "NativeClient", qn, "Class", "client.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.localOnly", project);
        rc |= insert_shard_node(stmt, 8, "localOnly", qn, "Method", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.client.native_client", project);
        rc |= insert_shard_node(stmt, 9, "native_client", qn, "Function", "client.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.__file__", project);
        rc |= insert_shard_node(stmt, 10, "Client.kt", qn, "File", "src/app/Client.kt");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.kotlin_client", project);
        rc |= insert_shard_node(stmt, 11, "kotlin_client", qn, "Function", "src/app/Client.kt");
        (void)snprintf(qn, sizeof(qn), "%s.src.client.__file__", project);
        rc |= insert_shard_node(stmt, 12, "client.rs", qn, "File", "src/client.rs");
        (void)snprintf(qn, sizeof(qn), "%s.src.client.rust_client", project);
        rc |= insert_shard_node(stmt, 13, "rust_client", qn, "Function", "src/client.rs");
    } else {
        (void)snprintf(qn, sizeof(qn), "%s.include.vendor.api.vendor.h.__file__", project);
        rc |= insert_shard_node(stmt, 1, "vendor.h", qn, "File", "include/vendor/api/vendor.h");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorBase", project);
        rc |= insert_shard_node(stmt, 2, "VendorBase", qn, "Class", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorInterface", project);
        rc |= insert_shard_node(stmt, 3, "VendorInterface", qn, "Interface", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorAnnotation", project);
        rc |= insert_shard_node(stmt, 4, "VendorAnnotation", qn, "Decorator", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorType", project);
        rc |= insert_shard_node(stmt, 5, "VendorType", qn, "Class", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.include.vendor.api.VendorNativeBase", project);
        rc |= insert_shard_node(stmt, 6, "VendorNativeBase", qn, "Class", "include/vendor/api/vendor.h");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorApi.execute", project);
        rc |= insert_shard_node(stmt, 7, "execute", qn, "Method", "src/vendor/api/Calls.java");
        (void)snprintf(qn, sizeof(qn), "%s.native.vendor_run", project);
        rc |= insert_shard_node(stmt, 8, "vendor_run", qn, "Function", "native/calls.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.kotlin.vendorKotlin", project);
        rc |= insert_shard_node(stmt, 9, "vendorKotlin", qn, "Function", "kotlin/Calls.kt");
        (void)snprintf(qn, sizeof(qn), "%s.rust.vendor.rust_run", project);
        rc |= insert_shard_node(stmt, 10, "rust_run", qn, "Function", "rust/calls.rs");
        (void)snprintf(qn, sizeof(qn), "%s.values.sharedValue", project);
        rc |= insert_shard_node(stmt, 11, "sharedValue", qn, "Field", "src/vendor/api/Values.java");
        (void)snprintf(qn, sizeof(qn), "%s.values.vendor_value", project);
        rc |= insert_shard_node(stmt, 12, "vendor_value", qn, "Variable", "native/values.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.values.vendorValue", project);
        rc |= insert_shard_node(stmt, 13, "vendorValue", qn, "Variable", "kotlin/Values.kt");
        (void)snprintf(qn, sizeof(qn), "%s.values.RUST_VALUE", project);
        rc |= insert_shard_node(stmt, 14, "RUST_VALUE", qn, "Variable", "rust/values.rs");
        (void)snprintf(qn, sizeof(qn), "%s.overload.A.ambiguousCall", project);
        rc |= insert_shard_node(stmt, 15, "ambiguousCall", qn, "Method", "src/vendor/api/A.java");
        (void)snprintf(qn, sizeof(qn), "%s.overload.B.ambiguousCall", project);
        rc |= insert_shard_node(stmt, 16, "ambiguousCall", qn, "Method", "src/vendor/api/B.java");
        (void)snprintf(qn, sizeof(qn), "%s.right.qualifierOnly", project);
        rc |= insert_shard_node(stmt, 17, "qualifierOnly", qn, "Function", "src/vendor/api/Only.java");
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == 0 ? 0 : -1;
}

static int set_repo_indexed(const cbm_aosp_workspace_t *workspace,
                            const cbm_aosp_repo_t *repo, const char *db_path) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(master_path, &db) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "UPDATE repos SET status='indexed',db_path=?1 WHERE workspace_id=?2 AND repo_id=?3;",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, db_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, repo->repo_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_structural_federation_collects_cross_repo_candidates_only) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    ASSERT_EQ(make_dir(root, "frameworks/base/src/app"), 0);
    ASSERT_EQ(make_dir(root, "vendor/acme/widgets/include/vendor/api"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/app/Client.java",
        "package app;\n"
        "import vendor.api.VendorAnnotation;\n"
        "import vendor.api.VendorBase;\n"
        "import vendor.api.VendorInterface;\n"
        "import vendor.api.VendorType;\n"
        "@VendorAnnotation\n"
        "class Client extends VendorBase implements VendorInterface {\n"
        "  VendorType convert(VendorType value) {\n"
        "    VendorApi.execute();\n"
        "    localOnly();\n"
        "    ambiguousCall();\n"
        "    wrong.qualifierOnly();\n"
        "    missingJava();\n"
        "    return sharedValue;\n"
        "  }\n"
        "  void localOnly() {}\n"
        "}\n"
        "class LocalBase {}\n"
        "class LocalChild extends LocalBase {}\n"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/client.cpp",
        "#include \"vendor/api/vendor.h\"\n"
        "class NativeClient : public VendorNativeBase {};\n"
        "void native_client() { vendor_run(); missing_cpp(); int x = vendor_value; }\n"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/app/Client.kt",
        "package app\n"
        "fun kotlin_client() { vendorKotlin(); missingKotlin(); val x = vendorValue }\n"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/client.rs",
        "fn rust_client() { vendor::rust_run(); missing_rust(); let x = RUST_VALUE; }\n"), 0);
    ASSERT_EQ(write_relative(root, "vendor/acme/widgets/include/vendor/api/vendor.h",
        "class VendorNativeBase {};\n"), 0);

    cbm_aosp_workspace_t workspace;
    char err[1024] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    char source_path[4096];
    char target_path[4096];
    char source_project[64];
    char target_project[64];
    (void)snprintf(source_path, sizeof(source_path), "%s/struct-source.db", root);
    (void)snprintf(target_path, sizeof(target_path), "%s/struct-target.db", root);
    (void)snprintf(source_project, sizeof(source_project), "aosp-%s", workspace.repos[0].repo_id);
    (void)snprintf(target_project, sizeof(target_project), "aosp-%s", workspace.repos[1].repo_id);
    ASSERT_EQ(create_structural_shard(source_path, source_project, true), 0);
    ASSERT_EQ(create_structural_shard(target_path, target_project, false), 0);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], source_path,
                                       err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[1], target_path,
                                       err, sizeof(err)), 0);
    ASSERT_EQ(set_repo_indexed(&workspace, &workspace.repos[0], source_path), 0);
    ASSERT_EQ(set_repo_indexed(&workspace, &workspace.repos[1], target_path), 0);

    cbm_aosp_structural_stats_t stats;
    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 2);
    ASSERT(stats.files_scanned >= 2);
    ASSERT(stats.edges.resolved_count >= 6);
    ASSERT(stats.local_references_skipped >= 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    const char *types[] = {"IMPORTS", "INCLUDES", "EXTENDS", "IMPLEMENTS", "ANNOTATED_BY", "USES_TYPE"};
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND type=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, types[i], -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT(sqlite3_column_int(stmt, 0) >= 1);
    }
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT confidence,properties FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND type='CALLS' AND target_name=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    const char *scored_targets[] = {"VendorApi.execute", "vendorKotlin"};
    const char *scored_resolutions[] = {"qualified_suffix", "unique_short_name"};
    for (size_t i = 0; i < sizeof(scored_targets) / sizeof(scored_targets[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, scored_targets[i], -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        double confidence = sqlite3_column_double(stmt, 0);
        ASSERT(i == 0 ? confidence >= 0.95 : confidence < 0.80);
        const char *properties = (const char *)sqlite3_column_text(stmt, 1);
        ASSERT_NOT_NULL(properties);
        ASSERT_NOT_NULL(strstr(properties, scored_resolutions[i]));
        ASSERT_NOT_NULL(strstr(properties, "\"candidate_count\":1"));
    }
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND target_name='LocalBase';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    const char *resolved_targets[] = {
        "VendorApi.execute", "vendor_run", "vendorKotlin", "vendor.rust_run",
        "sharedValue", "vendorValue", "RUST_VALUE",
    };
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT status,confidence,evidence FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND target_name=?2 AND type=?3;",
        -1, &stmt, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(resolved_targets) / sizeof(resolved_targets[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, resolved_targets[i], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, i < 4 ? "CALLS" : "USAGE", -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "resolved");
        ASSERT(sqlite3_column_double(stmt, 1) > 0.0);
        ASSERT(sqlite3_column_text(stmt, 2) != NULL);
    }
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND type='CALLS' "
        "AND target_name='localOnly';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*),count(DISTINCT target_global_id),min(status),max(status),"
        "min(confidence),min(properties),max(properties) FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND type='CALLS' AND target_name='ambiguousCall';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    ASSERT_EQ(sqlite3_column_int(stmt, 1), 2);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "ambiguous");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "ambiguous");
    ASSERT(sqlite3_column_double(stmt, 4) < 0.5);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 5),
                  (const char *)sqlite3_column_text(stmt, 6));
    ASSERT_NOT_NULL(strstr((const char *)sqlite3_column_text(stmt, 5),
                           "\"candidate_count\":2"));
    sqlite3_finalize(stmt);

    const char *unresolved_targets[] = {
        "missingJava", "missing_cpp", "missingKotlin", "missing_rust", "wrong.qualifierOnly",
    };
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT status,target_global_id,confidence,evidence,properties FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND type='CALLS' AND target_name=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(unresolved_targets) / sizeof(unresolved_targets[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, unresolved_targets[i], -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "unresolved");
        ASSERT_EQ(sqlite3_column_type(stmt, 1), SQLITE_NULL);
        ASSERT_EQ(sqlite3_column_double(stmt, 2), 0.0);
        ASSERT(sqlite3_column_text(stmt, 3) != NULL);
        const char *properties = (const char *)sqlite3_column_text(stmt, 4);
        ASSERT_NOT_NULL(properties);
        ASSERT_NOT_NULL(strstr(properties, "\"resolution\":\"unresolved\""));
        ASSERT_NOT_NULL(strstr(properties, "\"candidate_count\":0"));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_refresh_shard(const char *path, const char *project, int kind) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK ||
        sqlite3_exec(db,
            "DROP TABLE IF EXISTS nodes;"
            "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
            "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);",
            NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO nodes VALUES(?1,?2,?3,?4,?5,1,3,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    char qn[1024];
    int rc = 0;
    if (kind == 0) {
        (void)snprintf(qn, sizeof(qn), "%s.Client.__file__", project);
        rc |= insert_shard_node(stmt, 1, "Client.java", qn, "File", "Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.Client.run", project);
        rc |= insert_shard_node(stmt, 2, "run", qn, "Method", "Client.java");
    } else if (kind == 1) {
        (void)snprintf(qn, sizeof(qn), "%s.TargetApi.hit", project);
        rc |= insert_shard_node(stmt, 1, "hit", qn, "Method", "TargetApi.java");
    } else if (kind == 2) {
        (void)snprintf(qn, sizeof(qn), "%s.Other.__file__", project);
        rc |= insert_shard_node(stmt, 1, "Other.java", qn, "File", "Other.java");
        (void)snprintf(qn, sizeof(qn), "%s.Other.run", project);
        rc |= insert_shard_node(stmt, 2, "run", qn, "Method", "Other.java");
    } else {
        (void)snprintf(qn, sizeof(qn), "%s.Replacement", project);
        rc |= insert_shard_node(stmt, 1, "Replacement", qn, "Class", "Replacement.java");
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == 0 ? 0 : -1;
}

TEST(aosp_federation_refreshes_only_invalidated_repositories) {
    const char *temp_root = th_mktempdir("cbm_aosp_refresh");
    ASSERT_NOT_NULL(temp_root);
    char *root = strdup(temp_root);
    ASSERT_NOT_NULL(root);
    ASSERT_EQ(make_dir(root, ".repo/manifests"), 0);
    ASSERT_EQ(make_dir(root, "source/app"), 0);
    ASSERT_EQ(make_dir(root, "target/lib"), 0);
    ASSERT_EQ(make_dir(root, "unrelated/tool"), 0);
    ASSERT_EQ(write_relative(root, ".repo/manifest.xml",
        "<manifest>"
        "<project name=\"source/app\" path=\"source/app\"/>"
        "<project name=\"target/lib\" path=\"target/lib\"/>"
        "<project name=\"unrelated/tool\" path=\"unrelated/tool\"/>"
        "</manifest>"), 0);
    ASSERT_EQ(write_relative(root, "source/app/Client.java",
        "class Client { void run() { TargetApi.hit(); } }\n"), 0);
    ASSERT_EQ(write_relative(root, "unrelated/tool/Other.java",
        "class Other { void run() { MissingOther(); } }\n"), 0);

    cbm_aosp_workspace_t workspace;
    char err[1024] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(workspace.repo_count, 3);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    char shard_paths[3][4096];
    char projects[3][64];
    for (int i = 0; i < 3; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/repo-%d.db", root, i);
        (void)snprintf(projects[i], sizeof(projects[i]), "aosp-%s", workspace.repos[i].repo_id);
        ASSERT_EQ(create_refresh_shard(shard_paths[i], projects[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(set_repo_indexed(&workspace, &workspace.repos[i], shard_paths[i]), 0);
    }

    cbm_aosp_structural_stats_t stats;
    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 3);
    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT edge_id FROM cross_symbol_edges WHERE workspace_id=?1 AND source_repo_id=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[2].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    char *unrelated_edge_id = strdup((const char *)sqlite3_column_text(stmt, 0));
    ASSERT_NOT_NULL(unrelated_edge_id);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(create_refresh_shard(shard_paths[1], projects[1], 3), 0);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[1], shard_paths[1],
                                       err, sizeof(err)), 0);
    cbm_aosp_master_stats_t status;
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.cross_edge_count, 2);
    ASSERT_EQ(status.resolved_edge_count, 1);
    ASSERT_EQ(status.unresolved_edge_count, 1);
    ASSERT_EQ(status.stale_repo_count, 2);
    ASSERT_EQ(status.stale_edge_count, 1);
    ASSERT_EQ(status.refresh_failed_count, 0);
    ASSERT_EQ(cbm_aosp_cross_edge_refresh_failed(&workspace, &workspace.repos[0],
                                                  "fixture refresh failure",
                                                  err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.refresh_failed_count, 1);
    char status_args[8192];
    (void)snprintf(status_args, sizeof(status_args),
                   "{\"workspace_root\":\"%s\"}", root);
    char *status_response = cbm_mcp_handle_tool(NULL, "aosp_get_status", status_args);
    ASSERT_NOT_NULL(status_response);
    ASSERT_NOT_NULL(strstr(status_response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(status_response, "stale_repositories"));
    ASSERT_NOT_NULL(strstr(status_response, "stale_edges"));
    ASSERT_NOT_NULL(strstr(status_response, "refresh_failures"));
    free(status_response);
    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 2);
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT edge_id FROM cross_symbol_edges WHERE workspace_id=?1 AND source_repo_id=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[2].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), unrelated_edge_id);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT status FROM cross_symbol_edges WHERE workspace_id=?1 AND source_repo_id=?2 "
        "AND target_name='TargetApi.hit';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "unresolved");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_edge_refresh_queue WHERE workspace_id=?1;",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.cross_edge_count, 2);
    ASSERT_EQ(status.resolved_edge_count, 0);
    ASSERT_EQ(status.ambiguous_edge_count, 0);
    ASSERT_EQ(status.unresolved_edge_count, 2);
    ASSERT_EQ(status.stale_repo_count, 0);
    ASSERT_EQ(status.stale_edge_count, 0);
    ASSERT_EQ(status.refresh_failed_count, 0);

    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 0);
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "UPDATE repos SET db_path='/missing/federation-shard.db' "
        "WHERE workspace_id=?1 AND repo_id=?2;", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO cross_edge_refresh_queue(workspace_id,source_repo_id,reason,queued_at) "
        "VALUES(?1,?2,'failure_probe',strftime('%s','now'));", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    err[0] = '\0';
    ASSERT_NEQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.stale_repo_count, 1);
    ASSERT_EQ(status.refresh_failed_count, 1);
    free(unrelated_edge_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

SUITE(aosp) {
    RUN_TEST(aosp_manifest_include_and_local_override);
    RUN_TEST(aosp_manifest_rejects_parent_path);
    RUN_TEST(aosp_manifest_keeps_duplicate_names_and_extend_path);
    RUN_TEST(aosp_workspace_ids_are_root_scoped);
    RUN_TEST(aosp_master_sync_and_stats);
    RUN_TEST(aosp_catalog_and_global_symbol_search);
    RUN_TEST(aosp_workspace_symbol_resolver_tiers_and_ambiguity);
    RUN_TEST(aosp_shard_routing_routes_symbols_and_reads_nodes_and_edges);
    RUN_TEST(aosp_trace_path_traverses_local_and_cross_repo_edges);
    RUN_TEST(aosp_build_graph_resolves_cross_repo_modules);
    RUN_TEST(aosp_protocol_graph_links_binder_and_jni_evidence);
    RUN_TEST(aosp_cross_edges_are_deterministic_and_refresh_per_source_repo);
    RUN_TEST(aosp_cross_edges_enforce_workspace_and_repository_boundaries);
    RUN_TEST(aosp_cross_edges_migrate_v3_schema_without_losing_resolved_edges);
    RUN_TEST(aosp_structural_federation_collects_cross_repo_candidates_only);
    RUN_TEST(aosp_federation_refreshes_only_invalidated_repositories);
}
