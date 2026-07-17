/* AOSP workspace discovery and master catalog tests. */
#include "test_framework.h"
#include "test_helpers.h"

#include "aosp/aosp.h"
#include "aosp/build_graph.h"
#include "aosp/federated_graph.h"
#include "aosp/protocol_graph.h"
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
        "SELECT length(edge_id),status,evidence FROM cross_symbol_edges;",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), CBM_AOSP_EDGE_ID_LEN);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "resolved");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "legacy_test");
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

    left_edge.target_global_id = right_target;
    ASSERT_NEQ(cbm_aosp_cross_edges_refresh(&left, &left.repos[0], &left_edge, 1,
                                            &stats, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_cross_edge_stats(&left, &left.repos[0], &stats,
                                        err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 1);

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

SUITE(aosp) {
    RUN_TEST(aosp_manifest_include_and_local_override);
    RUN_TEST(aosp_manifest_rejects_parent_path);
    RUN_TEST(aosp_manifest_keeps_duplicate_names_and_extend_path);
    RUN_TEST(aosp_workspace_ids_are_root_scoped);
    RUN_TEST(aosp_master_sync_and_stats);
    RUN_TEST(aosp_catalog_and_global_symbol_search);
    RUN_TEST(aosp_build_graph_resolves_cross_repo_modules);
    RUN_TEST(aosp_protocol_graph_links_binder_and_jni_evidence);
    RUN_TEST(aosp_cross_edges_are_deterministic_and_refresh_per_source_repo);
    RUN_TEST(aosp_cross_edges_enforce_workspace_and_repository_boundaries);
    RUN_TEST(aosp_cross_edges_migrate_v3_schema_without_losing_resolved_edges);
}
