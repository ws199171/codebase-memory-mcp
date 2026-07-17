#ifndef CBM_AOSP_STRUCTURAL_GRAPH_H
#define CBM_AOSP_STRUCTURAL_GRAPH_H

#include "aosp/aosp.h"
#include "aosp/federated_graph.h"

#include <stddef.h>

typedef struct {
    int repos_scanned;
    int files_scanned;
    int references_seen;
    int local_references_skipped;
    int missing_sources;
    cbm_aosp_cross_edge_stats_t edges;
} cbm_aosp_structural_stats_t;

int cbm_aosp_structural_link(const cbm_aosp_workspace_t *workspace,
                             cbm_aosp_structural_stats_t *stats,
                             char *err, size_t err_size);

#endif
