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
#include <stdint.h>

#define CBM_AOSP_ID_LEN 32
#define CBM_AOSP_HASH_LEN 64

typedef struct sqlite3 sqlite3;

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
    char *global_id;
    char *repo_id;
    char *repo_path;
    char *manifest_name;
    char *project_name;
    int64_t local_node_id;
    char *name;
    char *qualified_name;
    char *label;
    char *language;
    char *file_path;
    int start_line;
    int end_line;
} cbm_aosp_symbol_t;

typedef enum {
    CBM_AOSP_SYMBOL_NOT_FOUND = 0,
    CBM_AOSP_SYMBOL_RESOLVED,
    CBM_AOSP_SYMBOL_AMBIGUOUS,
} cbm_aosp_symbol_resolution_status_t;

typedef enum {
    CBM_AOSP_SYMBOL_MATCH_NONE = 0,
    CBM_AOSP_SYMBOL_MATCH_GLOBAL_ID,
    CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME,
    CBM_AOSP_SYMBOL_MATCH_QUALIFIED_SUFFIX,
    CBM_AOSP_SYMBOL_MATCH_EXACT_NAME,
} cbm_aosp_symbol_match_kind_t;

typedef struct {
    cbm_aosp_symbol_resolution_status_t status;
    cbm_aosp_symbol_match_kind_t match_kind;
    cbm_aosp_symbol_t *candidates;
    int candidate_count;
    int total_candidate_count;
    bool truncated;
} cbm_aosp_symbol_resolution_t;

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

/* Resolve one Master-catalog symbol by deterministic tiers. A qualified miss
 * never falls back to an unrelated short name. Ambiguous best-tier candidates
 * are returned explicitly and never promoted to a resolved result. */
int cbm_aosp_resolve_symbol(const cbm_aosp_workspace_t *workspace, const char *reference,
                            cbm_aosp_symbol_resolution_t *out, char *err, size_t err_size);
void cbm_aosp_symbol_resolution_free(cbm_aosp_symbol_resolution_t *resolution);

/* Q2: Shard routing from Master symbol IDs to repository databases.
 * A route owns an open read-only shard handle and must be closed with
 * cbm_aosp_shard_route_close. */
typedef struct {
    sqlite3 *shard;        /* open read-only shard database, or NULL */
    char *shard_path;      /* filesystem path of the shard database */
    char *repo_id;         /* repository that owns this shard */
    char *global_id;       /* Master global symbol id */
    int64_t local_node_id; /* node id within the shard */
} cbm_aosp_shard_route_t;

typedef enum {
    CBM_AOSP_SHARD_EDGE_OUTGOING = 0,
    CBM_AOSP_SHARD_EDGE_INCOMING = 1,
} cbm_aosp_shard_edge_direction_t;

typedef struct {
    int64_t edge_id;
    int64_t source_id;
    int64_t target_id;
    char *type;
    char *properties;
    /* The neighbor is the node on the opposite end of the edge:
     * target for outgoing edges, source for incoming edges. */
    int64_t neighbor_id;
    char *neighbor_name;
    char *neighbor_qualified_name;
    char *neighbor_label;
    char *neighbor_file_path;
} cbm_aosp_shard_edge_t;

/* Route a Master global symbol id to its owning repository shard.
 * The shard is opened read-only. Returns 0 on success, -1 on missing
 * symbol, missing shard path, unreadable shard, or database error. */
int cbm_aosp_shard_route(const cbm_aosp_workspace_t *workspace, const char *global_id,
                         cbm_aosp_shard_route_t *out, char *err, size_t err_size);

/* Route a resolved symbol directly, skipping the Master global_id lookup.
 * Useful when the caller already has a Q1 resolution result. */
int cbm_aosp_shard_route_symbol(const cbm_aosp_workspace_t *workspace,
                                const cbm_aosp_symbol_t *symbol,
                                cbm_aosp_shard_route_t *out, char *err, size_t err_size);

void cbm_aosp_shard_route_close(cbm_aosp_shard_route_t *route);

/* Read the routed node from its shard. The returned symbol owns all
 * strings and must be freed with aosp_symbol_clear or cbm_aosp_symbols_free. */
int cbm_aosp_shard_read_node(const cbm_aosp_shard_route_t *route, cbm_aosp_symbol_t *out,
                             char *err, size_t err_size);

/* Read edges from the routed node's shard in the given direction.
 * For outgoing edges, the neighbor is the target; for incoming, the
 * source. Results are owned by the caller and must be freed. */
int cbm_aosp_shard_read_edges(const cbm_aosp_shard_route_t *route,
                              cbm_aosp_shard_edge_direction_t direction,
                              cbm_aosp_shard_edge_t **edges, int *count,
                              char *err, size_t err_size);
void cbm_aosp_shard_edges_free(cbm_aosp_shard_edge_t *edges, int count);

/* Q3: Cross-shard trace_path traversal with depth, direction, cycle,
 * result-budget, and cancellation controls. */

typedef enum {
    CBM_AOSP_TRACE_OUTGOING = 0,
    CBM_AOSP_TRACE_INCOMING = 1,
    CBM_AOSP_TRACE_BOTH = 2,
} cbm_aosp_trace_direction_t;

typedef struct {
    int max_depth;                    /* max hops from start (0 = start only, -1 = unlimited) */
    cbm_aosp_trace_direction_t direction;
    int result_budget;                /* max nodes to return (0 = unlimited) */
    const volatile bool *cancel_flag; /* if non-NULL, traversal stops when *flag is true */
} cbm_aosp_trace_options_t;

typedef struct {
    char *global_id;
    char *repo_id;
    char *qualified_name;
    char *label;
    char *file_path;
    int start_line;
    char *edge_type;        /* edge type that led here, or NULL for start */
    char *edge_evidence;    /* edge evidence, or NULL for start */
    double confidence;      /* edge confidence (0.0 for start) */
    int depth;              /* hop count from start (0 for start) */
    bool cross_repo;        /* true if this hop crossed a repository boundary */
} cbm_aosp_trace_node_t;

typedef struct {
    cbm_aosp_trace_node_t *nodes;
    int node_count;
    bool truncated;         /* true if result budget was hit before exhaustion */
    int max_depth_reached;  /* deepest hop count in the result set */
} cbm_aosp_trace_result_t;

/* Traverse the federated graph starting from a Master global symbol ID.
 * Follows local shard edges and resolved cross-repository edges up to
 * max_depth hops. Cycle detection prevents revisiting nodes. The result
 * budget and cancel flag provide early termination. */
int cbm_aosp_trace_path(const cbm_aosp_workspace_t *workspace,
                        const char *start_global_id,
                        const cbm_aosp_trace_options_t *options,
                        cbm_aosp_trace_result_t *out,
                        char *err, size_t err_size);
void cbm_aosp_trace_result_free(cbm_aosp_trace_result_t *result);

/* `codebase-memory-mcp aosp init|index|build|link|federate|status|repos|search ...`. */
int cbm_cmd_aosp(int argc, char **argv);

#endif /* CBM_AOSP_H */
