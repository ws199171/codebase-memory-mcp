/* AOSP workspace discovery and master catalog tests. */
#include "test_framework.h"
#include "test_helpers.h"

#include "aosp/aosp.h"
#include "aosp/build_graph.h"
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

SUITE(aosp) {
    RUN_TEST(aosp_manifest_include_and_local_override);
    RUN_TEST(aosp_manifest_rejects_parent_path);
    RUN_TEST(aosp_manifest_keeps_duplicate_names_and_extend_path);
    RUN_TEST(aosp_workspace_ids_are_root_scoped);
    RUN_TEST(aosp_master_sync_and_stats);
    RUN_TEST(aosp_catalog_and_global_symbol_search);
    RUN_TEST(aosp_build_graph_resolves_cross_repo_modules);
    RUN_TEST(aosp_protocol_graph_links_binder_and_jni_evidence);
}
