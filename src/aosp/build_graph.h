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
    int variant_dependency_count;
    int variant_branch_count;
    int namespace_count;
    int namespace_import_count;
    int package_count;
    int ambiguous_dependency_count;
    int visibility_blocked_count;
    int unsupported_visibility_count;
    int filegroup_count;
    int genrule_count;
    int generated_dependency_count;
    int tool_dependency_count;
    int tagged_dependency_count;
    int source_file_count;
    int generated_output_count;
    int tool_file_count;
    int make_include_count;
    int make_condition_count;
    int make_macro_count;
    int make_unsupported_count;
    int product_count;
    int product_fragment_count;
    int product_inheritance_count;
    int product_inheritance_resolved_count;
    int product_inheritance_cycle_count;
    int product_package_count;
    int product_package_resolved_count;
    int product_package_unresolved_count;
    int board_config_count;
    int product_partition_count;
    int bazel_artifact_count;
    int bazel_target_count;
    int bazel_target_resolved_count;
    int bazel_dependency_count;
    int bazel_dependency_resolved_count;
    int bazel_ambiguous_count;
    int bazel_missing_count;
    int bazel_coverage_gap_count;
    int file_link_count;
    int file_link_resolved_count;
    int file_link_unresolved_count;
    int file_link_ambiguous_count;
    int file_link_missing_count;
    int file_link_unindexed_count;
    int generated_file_count;
    int generated_link_count;
    int generated_link_resolved_count;
    int generated_link_unresolved_count;
    int definition_symbol_link_count;
    int blueprint_files;
    int make_files;
    int product_make_files;
    int board_config_files;
    int bazel_metadata_files;
    int aidl_files;
} cbm_aosp_build_stats_t;

typedef struct {
    char *target_name;
    char *dependency_type;
    char *target_module_id;
    char *target_repo_path;
    char *target_module_name;
    char *properties;
    int resolved;
} cbm_aosp_module_dependency_t;

typedef struct {
    char *declared_path;
    char *role;
    char *workspace_path;
    char *status;
    char *file_global_id;
    char *generated_id;
    char *properties;
} cbm_aosp_module_file_t;

typedef struct {
    char *module_id;
    char *repo_path;
    char *name;
    char *module_type;
    char *file_path;
    char *properties;
    int outgoing_dependencies;
    int incoming_dependencies;
    cbm_aosp_module_dependency_t *dependencies;
    int dependency_count;
    cbm_aosp_module_file_t *files;
    int file_count;
    int details_truncated;
} cbm_aosp_module_t;

typedef struct {
    char *kind;
    char *repo_path;
    char *file_path;
    char *subject;
    char *status;
    char *properties;
} cbm_aosp_build_gap_t;

int cbm_aosp_build_scan(const cbm_aosp_workspace_t *workspace, cbm_aosp_build_stats_t *stats,
                        char *err, size_t err_size);
int cbm_aosp_build_stats(const cbm_aosp_workspace_t *workspace, cbm_aosp_build_stats_t *stats,
                         char *err, size_t err_size);
int cbm_aosp_search_modules(const cbm_aosp_workspace_t *workspace, const char *query, int limit,
                            cbm_aosp_module_t **results, int *count, char *err, size_t err_size);
int cbm_aosp_load_module_details(const cbm_aosp_workspace_t *workspace,
                                 cbm_aosp_module_t *modules, int count, int detail_limit,
                                 bool *truncated, char *err, size_t err_size);
void cbm_aosp_modules_free(cbm_aosp_module_t *results, int count);
int cbm_aosp_build_gaps(const cbm_aosp_workspace_t *workspace, int limit,
                        cbm_aosp_build_gap_t **results, int *count, bool *truncated,
                        char *err, size_t err_size);
void cbm_aosp_build_gaps_free(cbm_aosp_build_gap_t *results, int count);

#endif
