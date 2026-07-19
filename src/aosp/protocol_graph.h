#ifndef CBM_AOSP_PROTOCOL_GRAPH_H
#define CBM_AOSP_PROTOCOL_GRAPH_H

#include "aosp/aosp.h"

#include <stddef.h>

typedef struct {
    int node_count;
    int edge_count;
    int aidl_interfaces;
    int aidl_methods;
    int aidl_parcelables;
    int aidl_unions;
    int aidl_enums;
    int aidl_imports;
    int aidl_callbacks;
    int aidl_oneway_methods;
    int aidl_stable_types;
    int binder_server_edges;
    int binder_client_edges;
    int binder_transaction_constants;
    int binder_on_transact_handlers;
    int binder_transact_calls;
    int binder_implementation_methods;
    int binder_services;
    int binder_service_registrations;
    int binder_service_lookups;
    int binder_service_waits;
    int binder_service_server_links;
    int binder_service_interface_links;
    int jni_static_edges;
    int jni_dynamic_edges;
} cbm_aosp_protocol_stats_t;

typedef struct {
    char *protocol_id;
    char *repo_path;
    char *kind;
    char *name;
    char *qualified_name;
    char *file_path;
    char *properties;
    int outgoing_edges;
    int incoming_edges;
} cbm_aosp_protocol_node_t;

typedef struct {
    char *source_qualified_name;
    char *target_qualified_name;
    char *type;
    double confidence;
    char *evidence;
    char *properties;
} cbm_aosp_protocol_edge_t;

int cbm_aosp_protocol_link(const cbm_aosp_workspace_t *workspace,
                           cbm_aosp_protocol_stats_t *stats, char *err, size_t err_size);
int cbm_aosp_protocol_stats(const cbm_aosp_workspace_t *workspace,
                            cbm_aosp_protocol_stats_t *stats, char *err, size_t err_size);
int cbm_aosp_search_protocols(const cbm_aosp_workspace_t *workspace, const char *query, int limit,
                              cbm_aosp_protocol_node_t **results, int *count,
                              char *err, size_t err_size);
void cbm_aosp_protocol_nodes_free(cbm_aosp_protocol_node_t *results, int count);
int cbm_aosp_search_protocol_edges(const cbm_aosp_workspace_t *workspace, const char *query,
                                   int limit, cbm_aosp_protocol_edge_t **results, int *count,
                                   char *err, size_t err_size);
void cbm_aosp_protocol_edges_free(cbm_aosp_protocol_edge_t *results, int count);

#endif
