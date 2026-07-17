/*
 * aosp.h - AOSP workspace discovery and master catalog.
 *
 * The AOSP layer keeps repository graphs sharded. This module owns the
 * workspace-level identity and catalog used to route future cross-repository
 * symbol and architecture queries.
 */
#ifndef CBM_AOSP_H
#define CBM_AOSP_H

#include <stdbool.h>
#include <stddef.h>

#define CBM_AOSP_ID_LEN 32
#define CBM_AOSP_HASH_LEN 64

typedef struct {
    char *name;      /* manifest project name, e.g. platform/frameworks/base */
    char *path;      /* workspace-relative checkout path, e.g. frameworks/base */
    char *abs_path;  /* absolute checkout path */
    char repo_id[CBM_AOSP_ID_LEN + 1];
    bool exists;
} cbm_aosp_repo_t;

typedef struct {
    char *root;
    char workspace_id[CBM_AOSP_ID_LEN + 1];
    char manifest_hash[CBM_AOSP_HASH_LEN + 1];
    cbm_aosp_repo_t *repos;
    int repo_count;
} cbm_aosp_workspace_t;

typedef struct {
    int repo_count;
    int existing_count;
    int missing_count;
    int indexed_count;
    int error_count;
    int cross_edge_count;
    int resolved_edge_count;
    int ambiguous_edge_count;
    int unresolved_edge_count;
    int stale_repo_count;
    int stale_edge_count;
    int refresh_failed_count;
} cbm_aosp_master_stats_t;

typedef struct {
    char *repo_path;
    char *manifest_name;
    char *project_name;
    char *name;
    char *qualified_name;
    char *label;
    char *file_path;
    int start_line;
    int end_line;
} cbm_aosp_symbol_t;

/* Discover an AOSP checkout from .repo/manifest.xml plus local_manifests.
 * Uses the vendored tree-sitter XML grammar and applies include,
 * remove-project, and extend-project directives in document order. */
int cbm_aosp_discover(const char *root, cbm_aosp_workspace_t *out, char *err, size_t err_size);
void cbm_aosp_workspace_free(cbm_aosp_workspace_t *workspace);

/* Resolve the workspace master DB path. create_dirs controls whether the
 * workspace cache directory is created. */
int cbm_aosp_master_path(const cbm_aosp_workspace_t *workspace, char *out, size_t out_size,
                         bool create_dirs);

/* Create/update the workspace catalog without changing per-repository graphs. */
int cbm_aosp_master_sync(const cbm_aosp_workspace_t *workspace, char *err, size_t err_size);
int cbm_aosp_master_stats(const cbm_aosp_workspace_t *workspace, cbm_aosp_master_stats_t *out,
                          char *err, size_t err_size);

/* Index one repository into its stable graph shard and refresh its Master
 * symbol catalog. The response body from the indexing worker is intentionally
 * not exposed; errors are normalized into err. */
int cbm_aosp_index_repo(const cbm_aosp_workspace_t *workspace, const cbm_aosp_repo_t *repo,
                        char *err, size_t err_size);

/* Refresh one repository's Master symbol catalog from an existing CBM shard.
 * This is also the resume/import boundary for future distributed indexers. */
int cbm_aosp_catalog_repo_db(const cbm_aosp_workspace_t *workspace, const cbm_aosp_repo_t *repo,
                             const char *shard_path, char *err, size_t err_size);

/* Query the workspace-level symbol catalog. Results own all strings. */
int cbm_aosp_search_symbols(const cbm_aosp_workspace_t *workspace, const char *query, int limit,
                            cbm_aosp_symbol_t **results, int *count, char *err, size_t err_size);
void cbm_aosp_symbols_free(cbm_aosp_symbol_t *results, int count);

/* `codebase-memory-mcp aosp init|index|build|link|federate|status|repos|search ...`. */
int cbm_cmd_aosp(int argc, char **argv);

#endif /* CBM_AOSP_H */
