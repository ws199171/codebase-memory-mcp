#ifndef CBM_AOSP_FEDERATED_GRAPH_H
#define CBM_AOSP_FEDERATED_GRAPH_H

#include "aosp/aosp.h"

#include <stddef.h>

typedef struct sqlite3 sqlite3;

#define CBM_AOSP_EDGE_ID_LEN 64

typedef enum {
    CBM_AOSP_CROSS_EDGE_RESOLVED = 0,
    CBM_AOSP_CROSS_EDGE_AMBIGUOUS = 1,
    CBM_AOSP_CROSS_EDGE_UNRESOLVED = 2,
} cbm_aosp_cross_edge_status_t;

typedef struct {
    const char *source_global_id;
    const char *target_global_id;
    const char *target_name;
    const char *type;
    cbm_aosp_cross_edge_status_t status;
    double confidence;
    const char *evidence;
    const char *properties;
} cbm_aosp_cross_edge_candidate_t;

typedef struct {
    int edge_count;
    int resolved_count;
    int ambiguous_count;
    int unresolved_count;
} cbm_aosp_cross_edge_stats_t;

/* Internal schema boundary used by the AOSP Master initializer. */
int cbm_aosp_cross_edges_ensure_schema(sqlite3 *db, char *err, size_t err_size);

/* Atomically replace every outgoing cross-repository edge for source_repo.
 * Empty input is a valid refresh and removes stale edges. */
int cbm_aosp_cross_edges_refresh(const cbm_aosp_workspace_t *workspace,
                                 const cbm_aosp_repo_t *source_repo,
                                 const cbm_aosp_cross_edge_candidate_t *candidates,
                                 int candidate_count, cbm_aosp_cross_edge_stats_t *stats,
                                 char *err, size_t err_size);

int cbm_aosp_cross_edge_stats(const cbm_aosp_workspace_t *workspace,
                              const cbm_aosp_repo_t *source_repo,
                              cbm_aosp_cross_edge_stats_t *stats,
                              char *err, size_t err_size);

/* Retain a failed per-repository refresh for status reporting and retry. */
int cbm_aosp_cross_edge_refresh_failed(const cbm_aosp_workspace_t *workspace,
                                       const cbm_aosp_repo_t *source_repo,
                                       const char *message, char *err, size_t err_size);

#endif
