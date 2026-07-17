/* AOSP federated cross-repository symbol-edge contract and refresh lifecycle. */
#include "aosp/federated_graph.h"

#include "foundation/sha256.h"

#include <sqlite3.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FG_PATH_MAX = 4096 };

static const char *CROSS_EDGE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS cross_symbol_edges("
    " edge_id TEXT PRIMARY KEY, workspace_id TEXT NOT NULL, source_repo_id TEXT NOT NULL,"
    " target_repo_id TEXT, source_global_id TEXT NOT NULL, target_global_id TEXT,"
    " target_name TEXT NOT NULL DEFAULT '', type TEXT NOT NULL, status TEXT NOT NULL,"
    " confidence REAL NOT NULL DEFAULT 0, evidence TEXT NOT NULL DEFAULT '',"
    " source_generation TEXT NOT NULL DEFAULT '', properties TEXT NOT NULL DEFAULT '{}',"
    " CHECK(status IN('resolved','ambiguous','unresolved')),"
    " CHECK((status IN('resolved','ambiguous') AND target_global_id IS NOT NULL AND target_repo_id IS NOT NULL)"
    " OR (status='unresolved' AND target_global_id IS NULL AND target_repo_id IS NULL)));";

static const char *CROSS_EDGE_INDEX_SQL =
    "CREATE INDEX IF NOT EXISTS idx_aosp_cross_edges_source_repo "
    "ON cross_symbol_edges(workspace_id,source_repo_id);"
    "CREATE INDEX IF NOT EXISTS idx_aosp_cross_edges_source_symbol "
    "ON cross_symbol_edges(workspace_id,source_global_id);"
    "CREATE INDEX IF NOT EXISTS idx_aosp_cross_edges_target_symbol "
    "ON cross_symbol_edges(workspace_id,target_global_id);"
    "CREATE INDEX IF NOT EXISTS idx_aosp_cross_edges_target_name "
    "ON cross_symbol_edges(workspace_id,target_name);"
    "CREATE INDEX IF NOT EXISTS idx_aosp_cross_edges_status "
    "ON cross_symbol_edges(workspace_id,status);";

static void fg_error(char *err, size_t err_size, const char *message, const char *detail) {
    if (!err || err_size == 0) return;
    if (detail && detail[0]) {
        (void)snprintf(err, err_size, "%s: %s", message, detail);
    } else {
        (void)snprintf(err, err_size, "%s", message);
    }
}

static int fg_exec(sqlite3 *db, const char *sql, char *err, size_t err_size) {
    char *sqlite_err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &sqlite_err);
    if (rc != SQLITE_OK) fg_error(err, err_size, "AOSP cross-edge database error", sqlite_err);
    sqlite3_free(sqlite_err);
    return rc == SQLITE_OK ? 0 : -1;
}

static void digest_hex(const uint8_t digest[CBM_SHA256_DIGEST_LEN],
                       char out[CBM_AOSP_EDGE_ID_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[CBM_AOSP_EDGE_ID_LEN] = '\0';
}

static void hash_part(cbm_sha256_ctx *ctx, const char *value) {
    const char *text = value ? value : "";
    cbm_sha256_update(ctx, text, strlen(text));
    cbm_sha256_update(ctx, "\0", 1);
}

static void cross_edge_id(const char *workspace_id, const char *source_global_id,
                          const char *target_reference, const char *type,
                          char out[CBM_AOSP_EDGE_ID_LEN + 1]) {
    cbm_sha256_ctx ctx;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_init(&ctx);
    hash_part(&ctx, workspace_id);
    hash_part(&ctx, source_global_id);
    hash_part(&ctx, target_reference);
    hash_part(&ctx, type);
    cbm_sha256_final(&ctx, digest);
    digest_hex(digest, out);
}

static bool table_has_column(sqlite3 *db, const char *table, const char *column) {
    char sql[256];
    (void)snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    sqlite3_stmt *stmt = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 1);
            if (name && strcmp(name, column) == 0) {
                found = true;
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static int insert_schema_version(sqlite3 *db) {
    return sqlite3_exec(db,
        "INSERT OR IGNORE INTO schema_versions(version,applied_at) "
        "VALUES(4,strftime('%s','now'));", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static int migrate_legacy_edges(sqlite3 *db, char *err, size_t err_size) {
    sqlite3_stmt *read_stmt = NULL;
    sqlite3_stmt *insert_stmt = NULL;
    int rc = -1;
    if (fg_exec(db, "BEGIN IMMEDIATE;ALTER TABLE cross_symbol_edges "
                    "RENAME TO cross_symbol_edges_v3;", err, err_size) != 0 ||
        fg_exec(db, CROSS_EDGE_TABLE_SQL, err, err_size) != 0) {
        goto done;
    }
    const char *read_sql =
        "SELECT e.source_global_id,e.target_global_id,e.type,e.confidence,e.evidence,e.properties,"
        "s.workspace_id,s.repo_id,t.repo_id,t.name,r.generation "
        "FROM cross_symbol_edges_v3 e JOIN symbols s ON s.global_id=e.source_global_id "
        "LEFT JOIN symbols t ON t.global_id=e.target_global_id "
        "LEFT JOIN repos r ON r.repo_id=s.repo_id;";
    const char *insert_sql =
        "INSERT INTO cross_symbol_edges(edge_id,workspace_id,source_repo_id,target_repo_id,"
        "source_global_id,target_global_id,target_name,type,status,confidence,evidence,"
        "source_generation,properties) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);";
    if (sqlite3_prepare_v2(db, read_sql, -1, &read_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, NULL) != SQLITE_OK) {
        fg_error(err, err_size, "cannot migrate AOSP cross edges", sqlite3_errmsg(db));
        goto done;
    }
    int step_rc;
    while ((step_rc = sqlite3_step(read_stmt)) == SQLITE_ROW) {
        const char *source_id = (const char *)sqlite3_column_text(read_stmt, 0);
        const char *legacy_target_id = (const char *)sqlite3_column_text(read_stmt, 1);
        const char *type = (const char *)sqlite3_column_text(read_stmt, 2);
        const char *evidence = (const char *)sqlite3_column_text(read_stmt, 4);
        const char *properties = (const char *)sqlite3_column_text(read_stmt, 5);
        const char *workspace_id = (const char *)sqlite3_column_text(read_stmt, 6);
        const char *source_repo_id = (const char *)sqlite3_column_text(read_stmt, 7);
        const char *target_repo_id = (const char *)sqlite3_column_text(read_stmt, 8);
        const char *target_name = (const char *)sqlite3_column_text(read_stmt, 9);
        const char *generation = (const char *)sqlite3_column_text(read_stmt, 10);
        if (!source_id || !legacy_target_id || !type || !workspace_id || !source_repo_id) continue;
        bool resolved = target_repo_id && target_name;
        char edge_id[CBM_AOSP_EDGE_ID_LEN + 1];
        cross_edge_id(workspace_id, source_id, legacy_target_id, type, edge_id);
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        sqlite3_bind_text(insert_stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 2, workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 3, source_repo_id, -1, SQLITE_TRANSIENT);
        if (resolved) sqlite3_bind_text(insert_stmt, 4, target_repo_id, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_stmt, 4);
        sqlite3_bind_text(insert_stmt, 5, source_id, -1, SQLITE_TRANSIENT);
        if (resolved) sqlite3_bind_text(insert_stmt, 6, legacy_target_id, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_stmt, 6);
        sqlite3_bind_text(insert_stmt, 7, resolved ? target_name : legacy_target_id,
                          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 8, type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 9, resolved ? "resolved" : "unresolved",
                          -1, SQLITE_STATIC);
        sqlite3_bind_double(insert_stmt, 10, sqlite3_column_double(read_stmt, 3));
        sqlite3_bind_text(insert_stmt, 11, evidence ? evidence : "legacy_v3",
                          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 12, generation ? generation : "",
                          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 13, properties ? properties : "{}",
                          -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            fg_error(err, err_size, "cannot migrate AOSP cross edge", sqlite3_errmsg(db));
            goto done;
        }
    }
    if (step_rc != SQLITE_DONE) {
        fg_error(err, err_size, "cannot read legacy AOSP cross edges", sqlite3_errmsg(db));
        goto done;
    }
    sqlite3_finalize(read_stmt);
    read_stmt = NULL;
    sqlite3_finalize(insert_stmt);
    insert_stmt = NULL;
    if (fg_exec(db, "DROP TABLE cross_symbol_edges_v3;", err, err_size) != 0 ||
        fg_exec(db, CROSS_EDGE_INDEX_SQL, err, err_size) != 0 ||
        insert_schema_version(db) != 0 ||
        fg_exec(db, "COMMIT;", err, err_size) != 0) {
        goto done;
    }
    rc = 0;

done:
    sqlite3_finalize(read_stmt);
    sqlite3_finalize(insert_stmt);
    if (rc != 0) (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return rc;
}

int cbm_aosp_cross_edges_ensure_schema(sqlite3 *db, char *err, size_t err_size) {
    if (!db) return -1;
    sqlite3_stmt *stmt = NULL;
    bool exists = false;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='cross_symbol_edges';",
            -1, &stmt, NULL) != SQLITE_OK) {
        fg_error(err, err_size, "cannot inspect AOSP cross-edge schema", sqlite3_errmsg(db));
        return -1;
    }
    exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (exists && !table_has_column(db, "cross_symbol_edges", "edge_id")) {
        return migrate_legacy_edges(db, err, err_size);
    }
    if (fg_exec(db, CROSS_EDGE_TABLE_SQL, err, err_size) != 0 ||
        fg_exec(db, CROSS_EDGE_INDEX_SQL, err, err_size) != 0 ||
        insert_schema_version(db) != 0) {
        if (err && err_size && !err[0]) {
            fg_error(err, err_size, "cannot initialize AOSP cross-edge schema", sqlite3_errmsg(db));
        }
        return -1;
    }
    return 0;
}

static const char *status_name(cbm_aosp_cross_edge_status_t status) {
    switch (status) {
        case CBM_AOSP_CROSS_EDGE_RESOLVED: return "resolved";
        case CBM_AOSP_CROSS_EDGE_AMBIGUOUS: return "ambiguous";
        case CBM_AOSP_CROSS_EDGE_UNRESOLVED: return "unresolved";
    }
    return NULL;
}

static bool edge_type_supported(const char *type) {
    static const char *types[] = {
        "IMPORTS", "INCLUDES", "USES_TYPE", "EXTENDS", "IMPLEMENTS",
        "ANNOTATED_BY", "CALLS", "USAGE",
    };
    if (!type) return false;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcmp(type, types[i]) == 0) return true;
    }
    return false;
}

static int read_stats(sqlite3 *db, const cbm_aosp_workspace_t *workspace,
                      const cbm_aosp_repo_t *source_repo,
                      cbm_aosp_cross_edge_stats_t *stats, char *err, size_t err_size) {
    const char *sql =
        "SELECT count(*),coalesce(sum(status='resolved'),0),"
        "coalesce(sum(status='ambiguous'),0),coalesce(sum(status='unresolved'),0) "
        "FROM cross_symbol_edges WHERE workspace_id=?1 AND (?2='' OR source_repo_id=?2);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fg_error(err, err_size, "cannot prepare AOSP cross-edge stats", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, source_repo ? source_repo->repo_id : "", -1, SQLITE_TRANSIENT);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats->edge_count = sqlite3_column_int(stmt, 0);
        stats->resolved_count = sqlite3_column_int(stmt, 1);
        stats->ambiguous_count = sqlite3_column_int(stmt, 2);
        stats->unresolved_count = sqlite3_column_int(stmt, 3);
        rc = 0;
    } else {
        fg_error(err, err_size, "cannot read AOSP cross-edge stats", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    return rc;
}

int cbm_aosp_cross_edge_stats(const cbm_aosp_workspace_t *workspace,
                              const cbm_aosp_repo_t *source_repo,
                              cbm_aosp_cross_edge_stats_t *stats,
                              char *err, size_t err_size) {
    if (!workspace || !stats) return -1;
    memset(stats, 0, sizeof(*stats));
    char path[FG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fg_error(err, err_size, "AOSP workspace is not initialized", path);
        sqlite3_close(db);
        return -1;
    }
    int rc = read_stats(db, workspace, source_repo, stats, err, err_size);
    sqlite3_close(db);
    return rc;
}

int cbm_aosp_cross_edges_refresh(const cbm_aosp_workspace_t *workspace,
                                 const cbm_aosp_repo_t *source_repo,
                                 const cbm_aosp_cross_edge_candidate_t *candidates,
                                 int candidate_count, cbm_aosp_cross_edge_stats_t *stats,
                                 char *err, size_t err_size) {
    if (!workspace || !source_repo || !stats || candidate_count < 0 ||
        (candidate_count > 0 && !candidates)) {
        fg_error(err, err_size, "invalid AOSP cross-edge refresh", NULL);
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    if (cbm_aosp_master_sync(workspace, err, err_size) != 0) return -1;
    char path[FG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *repo_stmt = NULL;
    sqlite3_stmt *delete_stmt = NULL;
    sqlite3_stmt *source_stmt = NULL;
    sqlite3_stmt *target_stmt = NULL;
    sqlite3_stmt *insert_stmt = NULL;
    char *generation = NULL;
    int rc = -1;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fg_error(err, err_size, "cannot open AOSP Master database", sqlite3_errmsg(db));
        goto done;
    }
    sqlite3_busy_timeout(db, 10000);
    if (fg_exec(db, "BEGIN IMMEDIATE;", err, err_size) != 0) goto done;
    if (sqlite3_prepare_v2(db,
            "SELECT generation FROM repos WHERE workspace_id=?1 AND repo_id=?2;",
            -1, &repo_stmt, NULL) != SQLITE_OK) {
        fg_error(err, err_size, "cannot prepare AOSP source repository lookup", sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_text(repo_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(repo_stmt, 2, source_repo->repo_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(repo_stmt) != SQLITE_ROW) {
        fg_error(err, err_size, "source repository is not in the AOSP workspace", source_repo->path);
        goto rollback;
    }
    const char *generation_text = (const char *)sqlite3_column_text(repo_stmt, 0);
    generation = strdup(generation_text ? generation_text : "");
    if (!generation) {
        fg_error(err, err_size, "out of memory", NULL);
        goto rollback;
    }
    sqlite3_finalize(repo_stmt);
    repo_stmt = NULL;

    if (sqlite3_prepare_v2(db,
            "DELETE FROM cross_symbol_edges WHERE workspace_id=?1 AND source_repo_id=?2;",
            -1, &delete_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "SELECT repo_id,name FROM symbols WHERE workspace_id=?1 AND global_id=?2;",
            -1, &source_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "SELECT repo_id,name FROM symbols WHERE workspace_id=?1 AND global_id=?2;",
            -1, &target_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO cross_symbol_edges(edge_id,workspace_id,source_repo_id,target_repo_id,"
            "source_global_id,target_global_id,target_name,type,status,confidence,evidence,"
            "source_generation,properties) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13) "
            "ON CONFLICT(edge_id) DO UPDATE SET target_repo_id=excluded.target_repo_id,"
            "target_global_id=excluded.target_global_id,target_name=excluded.target_name,"
            "status=excluded.status,confidence=excluded.confidence,evidence=excluded.evidence,"
            "source_generation=excluded.source_generation,properties=excluded.properties;",
            -1, &insert_stmt, NULL) != SQLITE_OK) {
        fg_error(err, err_size, "cannot prepare AOSP cross-edge refresh", sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_text(delete_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_stmt, 2, source_repo->repo_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
        fg_error(err, err_size, "cannot clear stale AOSP cross edges", sqlite3_errmsg(db));
        goto rollback;
    }

    for (int i = 0; i < candidate_count; i++) {
        const cbm_aosp_cross_edge_candidate_t *candidate = &candidates[i];
        const char *status = status_name(candidate->status);
        if (!candidate->source_global_id || !candidate->source_global_id[0] ||
            !edge_type_supported(candidate->type) || !status ||
            !(candidate->confidence >= 0.0 && candidate->confidence <= 1.0) ||
            !candidate->evidence || !candidate->evidence[0]) {
            fg_error(err, err_size, "invalid AOSP cross-edge candidate", candidate->type);
            goto rollback;
        }

        sqlite3_reset(source_stmt);
        sqlite3_clear_bindings(source_stmt);
        sqlite3_bind_text(source_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(source_stmt, 2, candidate->source_global_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(source_stmt) != SQLITE_ROW) {
            fg_error(err, err_size, "AOSP cross-edge source symbol was not cataloged",
                     candidate->source_global_id);
            goto rollback;
        }
        const char *catalog_source_repo = (const char *)sqlite3_column_text(source_stmt, 0);
        if (!catalog_source_repo || strcmp(catalog_source_repo, source_repo->repo_id) != 0) {
            fg_error(err, err_size, "AOSP cross-edge source belongs to another repository",
                     candidate->source_global_id);
            goto rollback;
        }

        const char *target_repo_id = NULL;
        const char *catalog_target_name = NULL;
        if (candidate->status == CBM_AOSP_CROSS_EDGE_UNRESOLVED) {
            if ((candidate->target_global_id && candidate->target_global_id[0]) ||
                !candidate->target_name || !candidate->target_name[0]) {
                fg_error(err, err_size, "unresolved AOSP cross edge requires only target_name",
                         candidate->source_global_id);
                goto rollback;
            }
        } else {
            if (!candidate->target_global_id || !candidate->target_global_id[0]) {
                fg_error(err, err_size, "resolved AOSP cross edge requires target_global_id",
                         candidate->source_global_id);
                goto rollback;
            }
            sqlite3_reset(target_stmt);
            sqlite3_clear_bindings(target_stmt);
            sqlite3_bind_text(target_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(target_stmt, 2, candidate->target_global_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(target_stmt) != SQLITE_ROW) {
                fg_error(err, err_size, "AOSP cross-edge target symbol was not cataloged",
                         candidate->target_global_id);
                goto rollback;
            }
            target_repo_id = (const char *)sqlite3_column_text(target_stmt, 0);
            catalog_target_name = (const char *)sqlite3_column_text(target_stmt, 1);
            if (!target_repo_id || strcmp(target_repo_id, source_repo->repo_id) == 0) {
                fg_error(err, err_size, "AOSP cross-edge target must be in another repository",
                         candidate->target_global_id);
                goto rollback;
            }
        }

        const char *target_name = candidate->target_name && candidate->target_name[0]
                                      ? candidate->target_name
                                      : catalog_target_name;
        const char *target_reference = candidate->target_global_id && candidate->target_global_id[0]
                                           ? candidate->target_global_id
                                           : target_name;
        char edge_id[CBM_AOSP_EDGE_ID_LEN + 1];
        cross_edge_id(workspace->workspace_id, candidate->source_global_id,
                      target_reference, candidate->type, edge_id);
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        sqlite3_bind_text(insert_stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 3, source_repo->repo_id, -1, SQLITE_TRANSIENT);
        if (target_repo_id) sqlite3_bind_text(insert_stmt, 4, target_repo_id, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_stmt, 4);
        sqlite3_bind_text(insert_stmt, 5, candidate->source_global_id, -1, SQLITE_TRANSIENT);
        if (candidate->target_global_id && candidate->target_global_id[0]) {
            sqlite3_bind_text(insert_stmt, 6, candidate->target_global_id, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(insert_stmt, 6);
        }
        sqlite3_bind_text(insert_stmt, 7, target_name ? target_name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 8, candidate->type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 9, status, -1, SQLITE_STATIC);
        sqlite3_bind_double(insert_stmt, 10, candidate->confidence);
        sqlite3_bind_text(insert_stmt, 11, candidate->evidence, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 12, generation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 13,
                          candidate->properties ? candidate->properties : "{}",
                          -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            fg_error(err, err_size, "cannot write AOSP cross edge", sqlite3_errmsg(db));
            goto rollback;
        }
    }
    if (read_stats(db, workspace, source_repo, stats, err, err_size) != 0 ||
        fg_exec(db, "COMMIT;", err, err_size) != 0) {
        goto rollback;
    }
    rc = 0;
    goto done;

rollback:
    (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
done:
    free(generation);
    sqlite3_finalize(repo_stmt);
    sqlite3_finalize(delete_stmt);
    sqlite3_finalize(source_stmt);
    sqlite3_finalize(target_stmt);
    sqlite3_finalize(insert_stmt);
    sqlite3_close(db);
    return rc;
}
