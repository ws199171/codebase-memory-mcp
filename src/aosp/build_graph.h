#ifndef CBM_AOSP_BUILD_GRAPH_H
#define CBM_AOSP_BUILD_GRAPH_H

#include "aosp/aosp.h"

#include <stddef.h>

typedef struct {
    int module_count;
    int dependency_count;
    int resolved_count;
    int unresolved_count;
    int inherited_dependency_count;
    int defaults_cycle_count;
    int blueprint_files;
    int make_files;
    int aidl_files;
} cbm_aosp_build_stats_t;

typedef struct {
    char *module_id;
    char *repo_path;
    char *name;
    char *module_type;
    char *file_path;
    int outgoing_dependencies;
    int incoming_dependencies;
} cbm_aosp_module_t;

int cbm_aosp_build_scan(const cbm_aosp_workspace_t *workspace, cbm_aosp_build_stats_t *stats,
                        char *err, size_t err_size);
int cbm_aosp_build_stats(const cbm_aosp_workspace_t *workspace, cbm_aosp_build_stats_t *stats,
                         char *err, size_t err_size);
int cbm_aosp_search_modules(const cbm_aosp_workspace_t *workspace, const char *query, int limit,
                            cbm_aosp_module_t **results, int *count, char *err, size_t err_size);
void cbm_aosp_modules_free(cbm_aosp_module_t *results, int count);

#endif
