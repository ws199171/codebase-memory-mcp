/* AOSP Soong/Make build graph extraction. */
#include "aosp/build_graph.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/sha256.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BG_PATH_MAX = 4096,
    BG_MAX_FILE_BYTES = 32 * 1024 * 1024,
    BG_MAX_WALK_DEPTH = 256,
};

typedef struct {
    char **items;
    int count;
    int cap;
} str_vec_t;

typedef struct {
    char *name;
    str_vec_t values;
} bp_var_t;

typedef struct {
    bp_var_t *items;
    int count;
    int cap;
} bp_var_vec_t;

typedef struct {
    char *name;
    char *kind;
    char *inherited_from;
    char *inheritance_path;
    int inheritance_depth;
    str_vec_t variants;
    bool unconditional;
    char *resolved_target_id;
    char *resolution;
    char *target_namespace;
    char *failure_reason;
    char *visibility_rule;
    str_vec_t declared_references;
    str_vec_t output_tags;
    int candidate_count;
} dep_decl_t;

typedef struct {
    char *path;
    char *role;
    char *inherited_from;
    char *inheritance_path;
    int inheritance_depth;
    str_vec_t variants;
    bool unconditional;
} file_decl_t;

typedef struct {
    char *file_path;
    str_vec_t includes;
    str_vec_t unsupported;
    int condition_count;
    int macro_count;
} make_file_decl_t;

typedef struct {
    make_file_decl_t *items;
    int count;
    int cap;
} make_file_vec_t;

typedef struct {
    char *name;
    char *partition;
    char *source_variable;
    char *source_file;
    char *inherited_from;
    char *inheritance_path;
    int inheritance_depth;
    bool included;
    char *resolved_target_id;
    char *resolution;
} product_package_decl_t;

typedef struct {
    char *path;
    char *source_file;
    char *resolved_product_id;
    char *status;
    bool optional;
} product_inherit_decl_t;

typedef struct {
    char *name;
    char *kind;
    char *file_path;
    char *workspace_path;
    char *device;
    char *brand;
    char *model;
    char *manufacturer;
    char *device_owner;
    char *vendor_owner;
    product_package_decl_t *packages;
    int package_count;
    int package_cap;
    product_inherit_decl_t *inherits;
    int inherit_count;
    int inherit_cap;
    str_vec_t partitions;
    int expansion_state;
    bool inheritance_cycle;
} product_decl_t;

typedef struct {
    product_decl_t *items;
    int count;
    int cap;
} product_vec_t;

typedef struct {
    char *name;
    char *value;
} name_value_t;

typedef struct {
    char *file_path;
    char *workspace_path;
    char *device_owner;
    char *vendor_owner;
    name_value_t *variables;
    int variable_count;
    int variable_cap;
    str_vec_t partitions;
} board_config_decl_t;

typedef struct {
    board_config_decl_t *items;
    int count;
    int cap;
} board_config_vec_t;

typedef struct {
    char *label;
    char *configuration;
    char *type;
    char *transition;
    char *status;
    char *source_module_id;
    char *target_module_id;
    int candidate_count;
} bazel_dependency_decl_t;

typedef struct {
    char *label;
    char *configuration;
    char *kind;
    char *module_name;
    char *module_id;
    char *status;
    int candidate_count;
    bazel_dependency_decl_t *dependencies;
    int dependency_count;
    int dependency_cap;
    str_vec_t coverage_gaps;
} bazel_target_decl_t;

typedef struct {
    char *file_path;
    int format_version;
    char *configuration;
    bazel_target_decl_t *targets;
    int target_count;
    int target_cap;
    str_vec_t coverage_gaps;
} bazel_artifact_decl_t;

typedef struct {
    bazel_artifact_decl_t *items;
    int count;
    int cap;
} bazel_artifact_vec_t;

typedef struct {
    char *name;
    char *type;
    char *file_path;
    dep_decl_t *deps;
    int dep_count;
    int dep_cap;
    int defaults_state;
    char *defaults_cycle;
    char *compile_multilib;
    char *package_path;
    char *namespace_path;
    char *visibility_origin;
    char *visibility_source_package;
    char *filegroup_path;
    char *generator_command;
    char *make_build_rule;
    char *make_module_class;
    str_vec_t declared_visibility;
    str_vec_t effective_visibility;
    str_vec_t namespace_imports;
    file_decl_t *files;
    int file_count;
    int file_cap;
    bool blueprint;
} module_decl_t;

typedef struct {
    module_decl_t *items;
    int count;
    int cap;
} module_vec_t;

typedef struct {
    char *path;
    char *file_path;
    str_vec_t values;
} scope_decl_t;

typedef struct {
    scope_decl_t *items;
    int count;
    int cap;
} scope_vec_t;

typedef struct {
    const char *source;
    size_t length;
    size_t pos;
} lexer_t;

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_STRING,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COLON,
    TOK_COMMA,
    TOK_PLUS,
    TOK_EQUAL,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_OTHER,
} token_kind_t;

typedef struct {
    token_kind_t kind;
    char *text;
} token_t;

typedef struct {
    lexer_t lexer;
    token_t current;
} parser_t;

typedef struct {
    const cbm_aosp_workspace_t *workspace;
    const cbm_aosp_repo_t *repo;
    module_vec_t modules;
    scope_vec_t namespaces;
    scope_vec_t packages;
    make_file_vec_t make_files;
    product_vec_t products;
    board_config_vec_t board_configs;
    bazel_artifact_vec_t bazel_artifacts;
    cbm_aosp_build_stats_t stats;
    char *err;
    size_t err_size;
} scan_ctx_t;

static void bg_error(char *err, size_t err_size, const char *message, const char *detail) {
    if (!err || err_size == 0) return;
    if (detail && detail[0]) {
        (void)snprintf(err, err_size, "%s: %s", message, detail);
    } else {
        (void)snprintf(err, err_size, "%s", message);
    }
}

static char *bg_read_file(const char *path, size_t *length_out) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || length > BG_MAX_FILE_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    char *source = malloc((size_t)length + 1);
    if (!source) {
        (void)fclose(file);
        return NULL;
    }
    size_t read = fread(source, 1, (size_t)length, file);
    (void)fclose(file);
    if (read != (size_t)length) {
        free(source);
        return NULL;
    }
    source[read] = '\0';
    if (length_out) *length_out = read;
    return source;
}

static bool str_vec_add(str_vec_t *vec, const char *value) {
    if (!vec || !value || !value[0]) return true;
    if (vec->count == vec->cap) {
        int new_cap = vec->cap ? vec->cap * 2 : 8;
        char **items = realloc(vec->items, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->cap = new_cap;
    }
    vec->items[vec->count] = strdup(value);
    if (!vec->items[vec->count]) return false;
    vec->count++;
    return true;
}

static void str_vec_free(str_vec_t *vec) {
    if (!vec) return;
    for (int i = 0; i < vec->count; i++) free(vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static bool str_vec_extend(str_vec_t *vec, const str_vec_t *values) {
    if (!vec || !values) return false;
    for (int i = 0; i < values->count; i++) {
        if (!str_vec_add(vec, values->items[i])) return false;
    }
    return true;
}

static bool str_vec_add_unique(str_vec_t *vec, const char *value) {
    if (!vec || !value || !value[0]) return true;
    for (int i = 0; i < vec->count; i++) {
        if (strcmp(vec->items[i], value) == 0) return true;
    }
    return str_vec_add(vec, value);
}

static bool str_vec_copy(str_vec_t *dest, const str_vec_t *source) {
    if (!dest || !source) return false;
    str_vec_free(dest);
    return str_vec_extend(dest, source);
}

static scope_decl_t *scope_vec_find(scope_vec_t *vec, const char *path) {
    if (!vec || !path) return NULL;
    for (int i = 0; i < vec->count; i++) {
        if (strcmp(vec->items[i].path, path) == 0) return &vec->items[i];
    }
    return NULL;
}

static scope_decl_t *scope_vec_ensure(scope_vec_t *vec, const char *path,
                                      const char *file_path) {
    if (!vec || !path || !file_path) return NULL;
    scope_decl_t *existing = scope_vec_find(vec, path);
    if (existing) return existing;
    if (vec->count == vec->cap) {
        int new_cap = vec->cap ? vec->cap * 2 : 16;
        scope_decl_t *items = realloc(vec->items, (size_t)new_cap * sizeof(*items));
        if (!items) return NULL;
        vec->items = items;
        vec->cap = new_cap;
    }
    scope_decl_t *decl = &vec->items[vec->count];
    memset(decl, 0, sizeof(*decl));
    decl->path = strdup(path);
    decl->file_path = strdup(file_path);
    if (!decl->path || !decl->file_path) {
        free(decl->path);
        free(decl->file_path);
        memset(decl, 0, sizeof(*decl));
        return NULL;
    }
    vec->count++;
    return decl;
}

static void scope_vec_free(scope_vec_t *vec) {
    if (!vec) return;
    for (int i = 0; i < vec->count; i++) {
        free(vec->items[i].path);
        free(vec->items[i].file_path);
        str_vec_free(&vec->items[i].values);
    }
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static make_file_decl_t *make_file_vec_add(make_file_vec_t *vec,
                                           const char *file_path) {
    if (!vec || !file_path) return NULL;
    if (vec->count == vec->cap) {
        int new_cap = vec->cap ? vec->cap * 2 : 8;
        make_file_decl_t *items =
            realloc(vec->items, (size_t)new_cap * sizeof(*items));
        if (!items) return NULL;
        vec->items = items;
        vec->cap = new_cap;
    }
    make_file_decl_t *decl = &vec->items[vec->count];
    memset(decl, 0, sizeof(*decl));
    decl->file_path = strdup(file_path);
    if (!decl->file_path) return NULL;
    vec->count++;
    return decl;
}

static void make_file_vec_free(make_file_vec_t *vec) {
    if (!vec) return;
    for (int i = 0; i < vec->count; i++) {
        free(vec->items[i].file_path);
        str_vec_free(&vec->items[i].includes);
        str_vec_free(&vec->items[i].unsupported);
    }
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void product_package_free(product_package_decl_t *package) {
    if (!package) return;
    free(package->name);
    free(package->partition);
    free(package->source_variable);
    free(package->source_file);
    free(package->inherited_from);
    free(package->inheritance_path);
    free(package->resolved_target_id);
    free(package->resolution);
    memset(package, 0, sizeof(*package));
}

static void product_free(product_decl_t *product) {
    if (!product) return;
    free(product->name);
    free(product->kind);
    free(product->file_path);
    free(product->workspace_path);
    free(product->device);
    free(product->brand);
    free(product->model);
    free(product->manufacturer);
    free(product->device_owner);
    free(product->vendor_owner);
    for (int i = 0; i < product->package_count; i++) {
        product_package_free(&product->packages[i]);
    }
    free(product->packages);
    for (int i = 0; i < product->inherit_count; i++) {
        free(product->inherits[i].path);
        free(product->inherits[i].source_file);
        free(product->inherits[i].resolved_product_id);
        free(product->inherits[i].status);
    }
    free(product->inherits);
    str_vec_free(&product->partitions);
    memset(product, 0, sizeof(*product));
}

static void product_vec_free(product_vec_t *vec) {
    if (!vec) return;
    for (int i = 0; i < vec->count; i++) product_free(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void board_config_free(board_config_decl_t *board) {
    if (!board) return;
    free(board->file_path);
    free(board->workspace_path);
    free(board->device_owner);
    free(board->vendor_owner);
    for (int i = 0; i < board->variable_count; i++) {
        free(board->variables[i].name);
        free(board->variables[i].value);
    }
    free(board->variables);
    str_vec_free(&board->partitions);
    memset(board, 0, sizeof(*board));
}

static void board_config_vec_free(board_config_vec_t *vec) {
    if (!vec) return;
    for (int i = 0; i < vec->count; i++) board_config_free(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void bazel_dependency_free(bazel_dependency_decl_t *dependency) {
    if (!dependency) return;
    free(dependency->label);
    free(dependency->configuration);
    free(dependency->type);
    free(dependency->transition);
    free(dependency->status);
    free(dependency->source_module_id);
    free(dependency->target_module_id);
    memset(dependency, 0, sizeof(*dependency));
}

static void bazel_target_free(bazel_target_decl_t *target) {
    if (!target) return;
    free(target->label);
    free(target->configuration);
    free(target->kind);
    free(target->module_name);
    free(target->module_id);
    free(target->status);
    for (int i = 0; i < target->dependency_count; i++) {
        bazel_dependency_free(&target->dependencies[i]);
    }
    free(target->dependencies);
    str_vec_free(&target->coverage_gaps);
    memset(target, 0, sizeof(*target));
}

static void bazel_artifact_vec_free(bazel_artifact_vec_t *vec) {
    if (!vec) return;
    for (int i = 0; i < vec->count; i++) {
        bazel_artifact_decl_t *artifact = &vec->items[i];
        free(artifact->file_path);
        free(artifact->configuration);
        for (int t = 0; t < artifact->target_count; t++) {
            bazel_target_free(&artifact->targets[t]);
        }
        free(artifact->targets);
        str_vec_free(&artifact->coverage_gaps);
    }
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static char *canonical_bazel_label(const char *label, const char *source_label) {
    if (!label || !label[0] || strstr(label, "..") || strpbrk(label, " \t\r\n")) return NULL;
    char *expanded = NULL;
    if (label[0] == ':' && source_label && strncmp(source_label, "//", 2) == 0) {
        const char *colon = strchr(source_label + 2, ':');
        if (!colon) return NULL;
        size_t package_length = (size_t)(colon - source_label);
        size_t size = package_length + strlen(label) + 1;
        expanded = malloc(size);
        if (!expanded) return NULL;
        (void)snprintf(expanded, size, "%.*s%s", (int)package_length, source_label, label);
        label = expanded;
    }
    if (strncmp(label, "//", 2) != 0 && label[0] != '@') {
        free(expanded);
        return NULL;
    }
    const char *workspace_label = label[0] == '@' ? strstr(label, "//") : label;
    if (!workspace_label || !workspace_label[2]) {
        free(expanded);
        return NULL;
    }
    if (strchr(workspace_label + 2, ':')) {
        char *result = strdup(label);
        free(expanded);
        return result;
    }
    const char *package = workspace_label + 2;
    const char *leaf = strrchr(package, '/');
    leaf = leaf ? leaf + 1 : package;
    if (!leaf[0]) {
        free(expanded);
        return NULL;
    }
    size_t size = strlen(label) + strlen(leaf) + 2;
    char *result = malloc(size);
    if (result) (void)snprintf(result, size, "%s:%s", label, leaf);
    free(expanded);
    return result;
}

static bazel_artifact_decl_t *bazel_artifact_vec_add(bazel_artifact_vec_t *vec,
                                                      const char *file_path) {
    if (vec->count == vec->cap) {
        int new_cap = vec->cap ? vec->cap * 2 : 4;
        bazel_artifact_decl_t *items =
            realloc(vec->items, (size_t)new_cap * sizeof(*items));
        if (!items) return NULL;
        vec->items = items;
        vec->cap = new_cap;
    }
    bazel_artifact_decl_t *artifact = &vec->items[vec->count];
    memset(artifact, 0, sizeof(*artifact));
    artifact->file_path = strdup(file_path);
    if (!artifact->file_path) return NULL;
    vec->count++;
    return artifact;
}

static bazel_target_decl_t *bazel_artifact_add_target(bazel_artifact_decl_t *artifact) {
    if (artifact->target_count == artifact->target_cap) {
        int new_cap = artifact->target_cap ? artifact->target_cap * 2 : 16;
        bazel_target_decl_t *targets =
            realloc(artifact->targets, (size_t)new_cap * sizeof(*targets));
        if (!targets) return NULL;
        artifact->targets = targets;
        artifact->target_cap = new_cap;
    }
    bazel_target_decl_t *target = &artifact->targets[artifact->target_count++];
    memset(target, 0, sizeof(*target));
    return target;
}

static bazel_dependency_decl_t *bazel_target_add_dependency(bazel_target_decl_t *target) {
    if (target->dependency_count == target->dependency_cap) {
        int new_cap = target->dependency_cap ? target->dependency_cap * 2 : 8;
        bazel_dependency_decl_t *dependencies =
            realloc(target->dependencies, (size_t)new_cap * sizeof(*dependencies));
        if (!dependencies) return NULL;
        target->dependencies = dependencies;
        target->dependency_cap = new_cap;
    }
    bazel_dependency_decl_t *dependency = &target->dependencies[target->dependency_count++];
    memset(dependency, 0, sizeof(*dependency));
    return dependency;
}

static const char *json_optional_string(yyjson_val *object, const char *key,
                                        const char *fallback) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return value && yyjson_is_str(value) ? yyjson_get_str(value) : fallback;
}

static bool add_json_string_gaps(str_vec_t *gaps, yyjson_val *values) {
    if (!values) return true;
    if (!yyjson_is_arr(values)) return str_vec_add(gaps, "invalid_unsupported_metadata");
    size_t index, max;
    yyjson_val *value;
    yyjson_arr_foreach(values, index, max, value) {
        if (yyjson_is_str(value)) {
            if (!str_vec_add(gaps, yyjson_get_str(value))) return false;
        } else if (!str_vec_add(gaps, "invalid_unsupported_metadata_entry")) {
            return false;
        }
    }
    return true;
}

static bool parse_bazel_metadata(scan_ctx_t *ctx, const char *source, size_t length,
                                 const char *file_path) {
    yyjson_doc *doc = yyjson_read(source, length, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *schema = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "schema") : NULL;
    yyjson_val *version = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "version") : NULL;
    yyjson_val *targets = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "targets") : NULL;
    if (!root || !yyjson_is_obj(root) || !schema || !yyjson_is_str(schema) ||
        strcmp(yyjson_get_str(schema), "aosp_mixed_build_metadata") != 0 ||
        !version || !yyjson_is_int(version) || !targets || !yyjson_is_arr(targets)) {
        yyjson_doc_free(doc);
        return false;
    }
    bazel_artifact_decl_t *artifact = bazel_artifact_vec_add(&ctx->bazel_artifacts, file_path);
    if (!artifact) {
        yyjson_doc_free(doc);
        return false;
    }
    artifact->format_version = (int)yyjson_get_int(version);
    artifact->configuration = strdup(json_optional_string(root, "configuration", ""));
    if (!artifact->configuration ||
        !add_json_string_gaps(&artifact->coverage_gaps, yyjson_obj_get(root, "unsupported"))) {
        yyjson_doc_free(doc);
        return false;
    }
    if (artifact->format_version != 1) {
        char gap[64];
        (void)snprintf(gap, sizeof(gap), "unsupported_format_version:%d",
                       artifact->format_version);
        bool ok = str_vec_add(&artifact->coverage_gaps, gap);
        yyjson_doc_free(doc);
        return ok;
    }
    size_t target_index, target_max;
    yyjson_val *target_value;
    yyjson_arr_foreach(targets, target_index, target_max, target_value) {
        yyjson_val *label_value = yyjson_is_obj(target_value)
                                      ? yyjson_obj_get(target_value, "label") : NULL;
        yyjson_val *module_value = yyjson_is_obj(target_value)
                                       ? yyjson_obj_get(target_value, "module_name") : NULL;
        if (!label_value || !yyjson_is_str(label_value) ||
            !module_value || !yyjson_is_str(module_value) ||
            !yyjson_get_str(module_value)[0]) {
            char gap[64];
            (void)snprintf(gap, sizeof(gap), "invalid_target:%llu",
                           (unsigned long long)target_index);
            if (!str_vec_add(&artifact->coverage_gaps, gap)) {
                yyjson_doc_free(doc);
                return false;
            }
            continue;
        }
        char *label = canonical_bazel_label(yyjson_get_str(label_value), NULL);
        if (!label) {
            char gap[96];
            (void)snprintf(gap, sizeof(gap), "invalid_target_label:%llu",
                           (unsigned long long)target_index);
            if (!str_vec_add(&artifact->coverage_gaps, gap)) {
                yyjson_doc_free(doc);
                return false;
            }
            continue;
        }
        const char *configuration = json_optional_string(
            target_value, "configuration", artifact->configuration);
        bool duplicate = false;
        for (int i = 0; i < artifact->target_count; i++) {
            if (strcmp(artifact->targets[i].label, label) == 0 &&
                strcmp(artifact->targets[i].configuration, configuration) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            size_t size = strlen(label) + strlen(configuration) + 32;
            char *gap = malloc(size);
            if (gap) (void)snprintf(gap, size, "duplicate_target:%s[%s]", label, configuration);
            bool ok = gap && str_vec_add(&artifact->coverage_gaps, gap);
            free(gap);
            free(label);
            if (!ok) {
                yyjson_doc_free(doc);
                return false;
            }
            continue;
        }
        bazel_target_decl_t *target = bazel_artifact_add_target(artifact);
        if (!target) {
            free(label);
            yyjson_doc_free(doc);
            return false;
        }
        target->label = label;
        target->configuration = strdup(configuration);
        target->kind = strdup(json_optional_string(target_value, "kind", "unknown"));
        target->module_name = strdup(yyjson_get_str(module_value));
        if (!target->configuration || !target->kind || !target->module_name ||
            !add_json_string_gaps(&target->coverage_gaps,
                                  yyjson_obj_get(target_value, "unsupported"))) {
            yyjson_doc_free(doc);
            return false;
        }
        yyjson_val *dependencies = yyjson_obj_get(target_value, "dependencies");
        if (!dependencies) continue;
        if (!yyjson_is_arr(dependencies)) {
            if (!str_vec_add(&target->coverage_gaps, "invalid_dependencies")) {
                yyjson_doc_free(doc);
                return false;
            }
            continue;
        }
        size_t dep_index, dep_max;
        yyjson_val *dep_value;
        yyjson_arr_foreach(dependencies, dep_index, dep_max, dep_value) {
            const char *dep_label = yyjson_is_str(dep_value) ? yyjson_get_str(dep_value) :
                (yyjson_is_obj(dep_value) ? json_optional_string(dep_value, "label", NULL) : NULL);
            char *normalized = canonical_bazel_label(dep_label, target->label);
            if (!normalized) {
                char gap[64];
                (void)snprintf(gap, sizeof(gap), "invalid_dependency:%llu",
                               (unsigned long long)dep_index);
                if (!str_vec_add(&target->coverage_gaps, gap)) {
                    yyjson_doc_free(doc);
                    return false;
                }
                continue;
            }
            bazel_dependency_decl_t *dependency = bazel_target_add_dependency(target);
            if (!dependency) {
                free(normalized);
                yyjson_doc_free(doc);
                return false;
            }
            dependency->label = normalized;
            dependency->configuration = strdup(yyjson_is_obj(dep_value)
                ? json_optional_string(dep_value, "configuration", "") : "");
            dependency->type = strdup(yyjson_is_obj(dep_value)
                ? json_optional_string(dep_value, "type", "BAZEL_DEPENDENCY")
                : "BAZEL_DEPENDENCY");
            dependency->transition = strdup(yyjson_is_obj(dep_value)
                ? json_optional_string(dep_value, "transition", "") : "");
            if (!dependency->configuration || !dependency->type || !dependency->transition) {
                yyjson_doc_free(doc);
                return false;
            }
        }
    }
    yyjson_doc_free(doc);
    return true;
}

static bp_var_t *bp_var_find(const bp_var_vec_t *vars, const char *name) {
    if (!vars || !name) return NULL;
    for (int i = 0; i < vars->count; i++) {
        if (strcmp(vars->items[i].name, name) == 0) return &vars->items[i];
    }
    return NULL;
}

static bool bp_var_assign(bp_var_vec_t *vars, const char *name, const str_vec_t *values,
                          bool append) {
    if (!vars || !name || !values) return false;
    bp_var_t *var = bp_var_find(vars, name);
    if (!var) {
        if (vars->count == vars->cap) {
            int new_cap = vars->cap ? vars->cap * 2 : 16;
            bp_var_t *items = realloc(vars->items, (size_t)new_cap * sizeof(*items));
            if (!items) return false;
            vars->items = items;
            vars->cap = new_cap;
        }
        var = &vars->items[vars->count++];
        memset(var, 0, sizeof(*var));
        var->name = strdup(name);
        if (!var->name) return false;
    } else if (!append) {
        str_vec_free(&var->values);
    }
    return str_vec_extend(&var->values, values);
}

static void bp_var_vec_free(bp_var_vec_t *vars) {
    if (!vars) return;
    for (int i = 0; i < vars->count; i++) {
        free(vars->items[i].name);
        str_vec_free(&vars->items[i].values);
    }
    free(vars->items);
    memset(vars, 0, sizeof(*vars));
}

static bool normalize_module_reference(const char *reference, char **name_out,
                                       char **tag_out) {
    if (!reference || !name_out || !tag_out) return false;
    *name_out = NULL;
    *tag_out = NULL;
    const char *name = reference[0] == ':' ? reference + 1 : reference;
    size_t name_length = strlen(name);
    if (name_length > 2 && name[name_length - 1] == '}') {
        const char *brace = strrchr(name, '{');
        if (brace && brace > name && brace[1] != '}') {
            *tag_out = cbm_strndup(brace + 1,
                                   (size_t)((name + name_length - 1) - (brace + 1)));
            if (!*tag_out) return false;
            name_length = (size_t)(brace - name);
        }
    }
    *name_out = cbm_strndup(name, name_length);
    if (!*name_out || !(*name_out)[0]) {
        free(*name_out);
        free(*tag_out);
        *name_out = NULL;
        *tag_out = NULL;
        return false;
    }
    return true;
}

static file_decl_t *module_find_file(module_decl_t *module, const char *path,
                                     const char *role) {
    for (int i = 0; module && i < module->file_count; i++) {
        if (strcmp(module->files[i].path, path) == 0 &&
            strcmp(module->files[i].role, role) == 0) {
            return &module->files[i];
        }
    }
    return NULL;
}

static file_decl_t *module_ensure_file(module_decl_t *module, const char *path,
                                       const char *role) {
    file_decl_t *existing = module_find_file(module, path, role);
    if (existing) return existing;
    if (module->file_count == module->file_cap) {
        int new_cap = module->file_cap ? module->file_cap * 2 : 8;
        file_decl_t *files = realloc(module->files, (size_t)new_cap * sizeof(*files));
        if (!files) return NULL;
        module->files = files;
        module->file_cap = new_cap;
    }
    file_decl_t *file = &module->files[module->file_count];
    memset(file, 0, sizeof(*file));
    file->path = strdup(path);
    file->role = strdup(role);
    if (!file->path || !file->role) {
        free(file->path);
        free(file->role);
        memset(file, 0, sizeof(*file));
        return NULL;
    }
    module->file_count++;
    return file;
}

static bool module_add_file_variant(module_decl_t *module, const char *path,
                                    const char *role, const char *variant) {
    if (!module || !path || !path[0] || !role) return true;
    file_decl_t *file = module_ensure_file(module, path, role);
    if (!file) return false;
    if (!variant || !variant[0]) {
        file->unconditional = true;
        return true;
    }
    return str_vec_add_unique(&file->variants, variant);
}

static dep_decl_t *module_find_dep(module_decl_t *module, const char *name, const char *kind) {
    if (!module || !name || !kind) return NULL;
    for (int i = 0; i < module->dep_count; i++) {
        if (strcmp(module->deps[i].name, name) == 0 && strcmp(module->deps[i].kind, kind) == 0) {
            return &module->deps[i];
        }
    }
    return NULL;
}

static bool dep_add_variant(dep_decl_t *dep, const char *variant) {
    if (!dep) return false;
    if (!variant || !variant[0]) {
        dep->unconditional = true;
        return true;
    }
    return str_vec_add_unique(&dep->variants, variant);
}

static dep_decl_t *module_ensure_dep(module_decl_t *module, const char *name, const char *kind) {
    dep_decl_t *existing = module_find_dep(module, name, kind);
    if (existing) return existing;
    if (module->dep_count == module->dep_cap) {
        int new_cap = module->dep_cap ? module->dep_cap * 2 : 8;
        dep_decl_t *deps = realloc(module->deps, (size_t)new_cap * sizeof(*deps));
        if (!deps) return NULL;
        module->deps = deps;
        module->dep_cap = new_cap;
    }
    dep_decl_t *dep = &module->deps[module->dep_count];
    memset(dep, 0, sizeof(*dep));
    dep->name = strdup(name);
    dep->kind = strdup(kind);
    if (!dep->name || !dep->kind) {
        free(dep->name);
        free(dep->kind);
        return NULL;
    }
    module->dep_count++;
    return dep;
}

static bool module_add_dep_variant(module_decl_t *module, const char *name, const char *kind,
                                   const char *variant) {
    if (!module || !name || !name[0] || !kind) return true;
    if (strstr(name, "$(") || strstr(name, "${")) return true;
    char *normalized = NULL;
    char *tag = NULL;
    if (!normalize_module_reference(name, &normalized, &tag)) return false;
    dep_decl_t *dep = module_ensure_dep(module, normalized, kind);
    bool ok = dep && str_vec_add_unique(&dep->declared_references, name) &&
              (!tag || str_vec_add_unique(&dep->output_tags, tag)) &&
              dep_add_variant(dep, variant);
    free(normalized);
    free(tag);
    return ok;
}

static bool module_add_dep(module_decl_t *module, const char *name, const char *kind) {
    return module_add_dep_variant(module, name, kind, NULL);
}

static bool dep_add_combined_variant(dep_decl_t *dep, const char *left, const char *right) {
    if (!left || !left[0]) return dep_add_variant(dep, right);
    if (!right || !right[0]) return dep_add_variant(dep, left);
    size_t size = strlen(left) + strlen(right) + 2;
    char *combined = malloc(size);
    if (!combined) return false;
    (void)snprintf(combined, size, "%s&%s", left, right);
    bool ok = dep_add_variant(dep, combined);
    free(combined);
    return ok;
}

static bool file_add_variant(file_decl_t *file, const char *variant) {
    if (!variant || !variant[0]) {
        file->unconditional = true;
        return true;
    }
    return str_vec_add_unique(&file->variants, variant);
}

static bool file_add_combined_variant(file_decl_t *file, const char *left,
                                      const char *right) {
    if (!left || !left[0]) return file_add_variant(file, right);
    if (!right || !right[0]) return file_add_variant(file, left);
    size_t size = strlen(left) + strlen(right) + 2;
    char *combined = malloc(size);
    if (!combined) return false;
    (void)snprintf(combined, size, "%s&%s", left, right);
    bool ok = file_add_variant(file, combined);
    free(combined);
    return ok;
}

static bool dep_merge_inherited_variants(dep_decl_t *dep, const dep_decl_t *defaults_dep,
                                         const dep_decl_t *source) {
    if (defaults_dep->unconditional && source->unconditional) dep->unconditional = true;
    if (defaults_dep->unconditional) {
        for (int i = 0; i < source->variants.count; i++) {
            if (!dep_add_variant(dep, source->variants.items[i])) return false;
        }
    }
    if (source->unconditional) {
        for (int i = 0; i < defaults_dep->variants.count; i++) {
            if (!dep_add_variant(dep, defaults_dep->variants.items[i])) return false;
        }
    }
    for (int i = 0; i < defaults_dep->variants.count; i++) {
        for (int j = 0; j < source->variants.count; j++) {
            if (!dep_add_combined_variant(dep, defaults_dep->variants.items[i],
                                          source->variants.items[j])) return false;
        }
    }
    return true;
}

static bool file_merge_inherited_variants(file_decl_t *file,
                                          const dep_decl_t *defaults_dep,
                                          const file_decl_t *source) {
    if (defaults_dep->unconditional && source->unconditional) file->unconditional = true;
    if (defaults_dep->unconditional) {
        for (int i = 0; i < source->variants.count; i++) {
            if (!file_add_variant(file, source->variants.items[i])) return false;
        }
    }
    if (source->unconditional) {
        for (int i = 0; i < defaults_dep->variants.count; i++) {
            if (!file_add_variant(file, defaults_dep->variants.items[i])) return false;
        }
    }
    for (int i = 0; i < defaults_dep->variants.count; i++) {
        for (int j = 0; j < source->variants.count; j++) {
            if (!file_add_combined_variant(file, defaults_dep->variants.items[i],
                                           source->variants.items[j])) {
                return false;
            }
        }
    }
    return true;
}

static bool module_add_inherited_dep(module_decl_t *module, const dep_decl_t *source,
                                     const dep_decl_t *defaults_dep,
                                     const char *defaults_name) {
    if (!module || !source || !defaults_dep || !defaults_name) return false;
    dep_decl_t *dep = module_find_dep(module, source->name, source->kind);
    bool created = dep == NULL;
    if (!dep) dep = module_ensure_dep(module, source->name, source->kind);
    if (!dep || !dep_merge_inherited_variants(dep, defaults_dep, source)) return false;
    for (int i = 0; i < source->declared_references.count; i++) {
        if (!str_vec_add_unique(&dep->declared_references,
                                source->declared_references.items[i])) return false;
    }
    for (int i = 0; i < source->output_tags.count; i++) {
        if (!str_vec_add_unique(&dep->output_tags, source->output_tags.items[i])) return false;
    }
    if (!created) return true;
    const char *origin = source->inherited_from ? source->inherited_from : defaults_name;
    dep->inherited_from = strdup(origin);
    size_t path_size = strlen(defaults_name) + 1;
    if (source->inheritance_path) path_size += strlen(source->inheritance_path) + 1;
    dep->inheritance_path = malloc(path_size);
    if (dep->inheritance_path) {
        if (source->inheritance_path) {
            (void)snprintf(dep->inheritance_path, path_size, "%s>%s", defaults_name,
                           source->inheritance_path);
        } else {
            (void)snprintf(dep->inheritance_path, path_size, "%s", defaults_name);
        }
    }
    dep->inheritance_depth = source->inheritance_depth + 1;
    return dep->inherited_from && dep->inheritance_path;
}

static bool module_add_inherited_file(module_decl_t *module, const file_decl_t *source,
                                      const dep_decl_t *defaults_dep,
                                      const char *defaults_name) {
    file_decl_t *file = module_find_file(module, source->path, source->role);
    bool created = file == NULL;
    if (!file) file = module_ensure_file(module, source->path, source->role);
    if (!file || !file_merge_inherited_variants(file, defaults_dep, source)) return false;
    if (!created) return true;
    const char *origin = source->inherited_from ? source->inherited_from : defaults_name;
    file->inherited_from = strdup(origin);
    size_t path_size = strlen(defaults_name) + 1;
    if (source->inheritance_path) path_size += strlen(source->inheritance_path) + 1;
    file->inheritance_path = malloc(path_size);
    if (file->inheritance_path) {
        if (source->inheritance_path) {
            (void)snprintf(file->inheritance_path, path_size, "%s>%s", defaults_name,
                           source->inheritance_path);
        } else {
            (void)snprintf(file->inheritance_path, path_size, "%s", defaults_name);
        }
    }
    file->inheritance_depth = source->inheritance_depth + 1;
    return file->inherited_from && file->inheritance_path;
}

static void module_free(module_decl_t *module) {
    if (!module) return;
    free(module->name);
    free(module->type);
    free(module->file_path);
    free(module->defaults_cycle);
    free(module->compile_multilib);
    free(module->package_path);
    free(module->namespace_path);
    free(module->visibility_origin);
    free(module->visibility_source_package);
    free(module->filegroup_path);
    free(module->generator_command);
    free(module->make_build_rule);
    free(module->make_module_class);
    str_vec_free(&module->declared_visibility);
    str_vec_free(&module->effective_visibility);
    str_vec_free(&module->namespace_imports);
    for (int i = 0; i < module->dep_count; i++) {
        free(module->deps[i].name);
        free(module->deps[i].kind);
        free(module->deps[i].inherited_from);
        free(module->deps[i].inheritance_path);
        free(module->deps[i].resolved_target_id);
        free(module->deps[i].resolution);
        free(module->deps[i].target_namespace);
        free(module->deps[i].failure_reason);
        free(module->deps[i].visibility_rule);
        str_vec_free(&module->deps[i].declared_references);
        str_vec_free(&module->deps[i].output_tags);
        str_vec_free(&module->deps[i].variants);
    }
    free(module->deps);
    for (int i = 0; i < module->file_count; i++) {
        free(module->files[i].path);
        free(module->files[i].role);
        free(module->files[i].inherited_from);
        free(module->files[i].inheritance_path);
        str_vec_free(&module->files[i].variants);
    }
    free(module->files);
    memset(module, 0, sizeof(*module));
}

static bool module_vec_add(module_vec_t *vec, module_decl_t *module) {
    if (!module->name || !module->name[0] || strstr(module->name, "$(") ||
        strstr(module->name, "${")) {
        module_free(module);
        return true;
    }
    if (vec->count == vec->cap) {
        int new_cap = vec->cap ? vec->cap * 2 : 64;
        module_decl_t *items = realloc(vec->items, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->cap = new_cap;
    }
    vec->items[vec->count++] = *module;
    memset(module, 0, sizeof(*module));
    return true;
}

static void module_vec_free(module_vec_t *vec) {
    if (!vec) return;
    for (int i = 0; i < vec->count; i++) module_free(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static void token_free(token_t *token) {
    if (!token) return;
    free(token->text);
    token->text = NULL;
}

static void lexer_skip(lexer_t *lexer) {
    while (lexer->pos < lexer->length) {
        unsigned char c = (unsigned char)lexer->source[lexer->pos];
        if (isspace(c)) {
            lexer->pos++;
            continue;
        }
        if (c == '/' && lexer->pos + 1 < lexer->length && lexer->source[lexer->pos + 1] == '/') {
            lexer->pos += 2;
            while (lexer->pos < lexer->length && lexer->source[lexer->pos] != '\n') lexer->pos++;
            continue;
        }
        if (c == '/' && lexer->pos + 1 < lexer->length && lexer->source[lexer->pos + 1] == '*') {
            lexer->pos += 2;
            while (lexer->pos + 1 < lexer->length &&
                   !(lexer->source[lexer->pos] == '*' && lexer->source[lexer->pos + 1] == '/')) {
                lexer->pos++;
            }
            if (lexer->pos + 1 < lexer->length) lexer->pos += 2;
            continue;
        }
        break;
    }
}

static token_t lexer_next(lexer_t *lexer) {
    lexer_skip(lexer);
    token_t token = {0};
    if (lexer->pos >= lexer->length) {
        token.kind = TOK_EOF;
        return token;
    }
    char c = lexer->source[lexer->pos++];
    switch (c) {
        case '{': token.kind = TOK_LBRACE; return token;
        case '}': token.kind = TOK_RBRACE; return token;
        case '[': token.kind = TOK_LBRACKET; return token;
        case ']': token.kind = TOK_RBRACKET; return token;
        case ':': token.kind = TOK_COLON; return token;
        case ',': token.kind = TOK_COMMA; return token;
        case '+': token.kind = TOK_PLUS; return token;
        case '=': token.kind = TOK_EQUAL; return token;
        case '(': token.kind = TOK_LPAREN; return token;
        case ')': token.kind = TOK_RPAREN; return token;
        case '"': {
            token.kind = TOK_STRING;
            size_t cap = 32;
            size_t count = 0;
            token.text = malloc(cap);
            if (!token.text) return token;
            while (lexer->pos < lexer->length) {
                char ch = lexer->source[lexer->pos++];
                if (ch == '"') break;
                if (ch == '\\' && lexer->pos < lexer->length) {
                    char escaped = lexer->source[lexer->pos++];
                    ch = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
                }
                if (count + 1 >= cap) {
                    cap *= 2;
                    char *grown = realloc(token.text, cap);
                    if (!grown) {
                        token_free(&token);
                        return token;
                    }
                    token.text = grown;
                }
                token.text[count++] = ch;
            }
            token.text[count] = '\0';
            return token;
        }
        default: break;
    }
    if (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-') {
        size_t start = lexer->pos - 1;
        while (lexer->pos < lexer->length) {
            unsigned char ch = (unsigned char)lexer->source[lexer->pos];
            if (!isalnum(ch) && ch != '_' && ch != '.' && ch != '-') break;
            lexer->pos++;
        }
        size_t length = lexer->pos - start;
        token.kind = TOK_IDENT;
        token.text = malloc(length + 1);
        if (token.text) {
            memcpy(token.text, lexer->source + start, length);
            token.text[length] = '\0';
        }
        return token;
    }
    token.kind = TOK_OTHER;
    return token;
}

static void parser_advance(parser_t *parser) {
    token_free(&parser->current);
    parser->current = lexer_next(&parser->lexer);
}

static const char *dependency_kind(const char *key) {
    if (!key) return NULL;
    if (strcmp(key, "shared_libs") == 0 || strcmp(key, "runtime_libs") == 0) return "SHARED_LIB";
    if (strcmp(key, "static_libs") == 0 || strcmp(key, "whole_static_libs") == 0) return "STATIC_LIB";
    if (strcmp(key, "header_libs") == 0 || strcmp(key, "export_header_lib_headers") == 0) return "HEADER_LIB";
    if (strcmp(key, "defaults") == 0) return "DEFAULTS";
    if (strcmp(key, "libs") == 0 || strcmp(key, "java_libs") == 0) return "LIB";
    if (strcmp(key, "required") == 0 || strcmp(key, "host_required") == 0 ||
        strcmp(key, "target_required") == 0) return "REQUIRED";
    if (strcmp(key, "tools") == 0) return "TOOL";
    if (strcmp(key, "plugins") == 0) return "PLUGIN";
    if (strcmp(key, "aidl_libs") == 0 || strcmp(key, "imports") == 0) return "AIDL_IMPORT";
    if (strcmp(key, "generated_sources") == 0) return "GENERATED_SOURCE";
    if (strcmp(key, "generated_headers") == 0) return "GENERATED_HEADER";
    if (strcmp(key, "export_generated_headers") == 0) return "EXPORTED_GENERATED_HEADER";
    return NULL;
}

static bool is_variant_scope(const char *scope) {
    static const char *prefixes[] = {
        "target", "arch", "multilib", "product_variables", "soong_config_variables", "select",
    };
    if (!scope || !scope[0]) return false;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t length = strlen(prefixes[i]);
        if (strncmp(scope, prefixes[i], length) == 0 &&
            (scope[length] == '\0' || scope[length] == '.')) return true;
    }
    return false;
}

static char *bp_scope_child(const char *scope, const char *key) {
    size_t scope_length = scope ? strlen(scope) : 0;
    size_t key_length = key ? strlen(key) : 0;
    size_t size = scope_length + key_length + (scope_length ? 2 : 1);
    char *child = malloc(size);
    if (!child) return NULL;
    if (scope_length) {
        (void)snprintf(child, size, "%s.%s", scope, key ? key : "");
    } else {
        (void)snprintf(child, size, "%s", key ? key : "");
    }
    return child;
}

static bool parse_bp_value(parser_t *parser, module_decl_t *module, const bp_var_vec_t *vars,
                           const char *key, const char *scope, int depth);

static bool parse_bp_object(parser_t *parser, module_decl_t *module, const bp_var_vec_t *vars,
                            const char *scope, int depth) {
    while (parser->current.kind != TOK_EOF && parser->current.kind != TOK_RBRACE) {
        if (parser->current.kind != TOK_IDENT) {
            parser_advance(parser);
            continue;
        }
        char *key = parser->current.text ? strdup(parser->current.text) : NULL;
        parser_advance(parser);
        if (parser->current.kind != TOK_COLON) {
            free(key);
            continue;
        }
        parser_advance(parser);
        bool ok = parse_bp_value(parser, module, vars, key, scope, depth);
        free(key);
        if (!ok) return false;
        if (parser->current.kind == TOK_COMMA) parser_advance(parser);
    }
    if (parser->current.kind == TOK_RBRACE) parser_advance(parser);
    return true;
}

static bool record_bp_string(module_decl_t *module, const char *key, const char *scope,
                             int depth, const char *value) {
    if (depth == 1 && key && strcmp(key, "name") == 0 && !module->name) {
        module->name = strdup(value);
        return module->name != NULL;
    }
    if (depth == 1 && key && strcmp(key, "compile_multilib") == 0) {
        free(module->compile_multilib);
        module->compile_multilib = strdup(value);
        return module->compile_multilib != NULL;
    }
    if (depth == 1 && key && strcmp(key, "path") == 0 &&
        strcmp(module->type, "filegroup") == 0) {
        free(module->filegroup_path);
        module->filegroup_path = strdup(value);
        return module->filegroup_path != NULL;
    }
    if (depth == 1 && key && strcmp(key, "cmd") == 0 &&
        strcmp(module->type, "genrule") == 0) {
        free(module->generator_command);
        module->generator_command = strdup(value);
        return module->generator_command != NULL;
    }
    if (depth == 1 && key &&
        (strcmp(key, "visibility") == 0 || strcmp(key, "defaults_visibility") == 0)) {
        return str_vec_add_unique(&module->declared_visibility, value);
    }
    if (depth == 1 && key && strcmp(module->type, "package") == 0 &&
        strcmp(key, "default_visibility") == 0) {
        return str_vec_add_unique(&module->declared_visibility, value);
    }
    if (depth == 1 && key && strcmp(module->type, "soong_namespace") == 0 &&
        strcmp(key, "imports") == 0) {
        return str_vec_add_unique(&module->namespace_imports, value);
    }
    const char *variant = is_variant_scope(scope) ? scope : NULL;
    if (key && strcmp(key, "srcs") == 0) {
        if (value[0] == ':' || strncmp(value, "//", 2) == 0) {
            const char *kind = strcmp(module->type, "genrule") == 0
                                   ? "GENRULE_INPUT"
                                   : strcmp(module->type, "filegroup") == 0
                                         ? "FILEGROUP_INPUT"
                                         : "SOURCE";
            return module_add_dep_variant(module, value, kind, variant);
        }
        return module_add_file_variant(module, value, "SOURCE", variant);
    }
    if (key && strcmp(key, "tool_files") == 0) {
        if (value[0] == ':' || strncmp(value, "//", 2) == 0) {
            return module_add_dep_variant(module, value, "TOOL_FILE", variant);
        }
        return module_add_file_variant(module, value, "TOOL_FILE", variant);
    }
    if (key && (strcmp(key, "out") == 0 || strcmp(key, "outs") == 0)) {
        return module_add_file_variant(module, value, "OUTPUT", variant);
    }
    const char *kind = dependency_kind(key);
    if (kind && strcmp(key, "imports") == 0 &&
        (!module->type || strcmp(module->type, "aidl_interface") != 0)) {
        kind = NULL;
    }
    return !kind || module_add_dep_variant(module, value, kind, variant);
}

static bool parse_bp_term(parser_t *parser, const bp_var_vec_t *vars, str_vec_t *values) {
    if (parser->current.kind == TOK_STRING) {
        bool ok = str_vec_add(values, parser->current.text ? parser->current.text : "");
        parser_advance(parser);
        return ok;
    }
    if (parser->current.kind == TOK_IDENT) {
        bp_var_t *var = bp_var_find(vars, parser->current.text);
        bool ok = !var || str_vec_extend(values, &var->values);
        parser_advance(parser);
        return ok;
    }
    if (parser->current.kind != TOK_LBRACKET) return true;
    parser_advance(parser);
    while (parser->current.kind != TOK_EOF && parser->current.kind != TOK_RBRACKET) {
        if (parser->current.kind == TOK_COMMA || parser->current.kind == TOK_PLUS) {
            parser_advance(parser);
            continue;
        }
        if (parser->current.kind == TOK_STRING || parser->current.kind == TOK_IDENT ||
            parser->current.kind == TOK_LBRACKET) {
            if (!parse_bp_term(parser, vars, values)) return false;
        } else {
            parser_advance(parser);
        }
    }
    if (parser->current.kind == TOK_RBRACKET) parser_advance(parser);
    return true;
}

static bool parse_bp_expression(parser_t *parser, const bp_var_vec_t *vars, str_vec_t *values) {
    if (!parse_bp_term(parser, vars, values)) return false;
    while (parser->current.kind == TOK_PLUS) {
        parser_advance(parser);
        if (!parse_bp_term(parser, vars, values)) return false;
    }
    return true;
}

static char *bp_join_path(const str_vec_t *parts) {
    size_t size = 1;
    for (int i = 0; parts && i < parts->count; i++) size += strlen(parts->items[i]) + 1;
    char *path = malloc(size);
    if (!path) return NULL;
    path[0] = '\0';
    for (int i = 0; parts && i < parts->count; i++) {
        if (path[0]) (void)strcat(path, ".");
        (void)strcat(path, parts->items[i]);
    }
    return path;
}

static bool parse_bp_select(parser_t *parser, module_decl_t *module,
                            const bp_var_vec_t *vars, const char *key,
                            const char *scope, int depth) {
    parser_advance(parser);
    if (parser->current.kind != TOK_LPAREN) return true;
    parser_advance(parser);
    str_vec_t condition_parts = {0};
    int nested_parens = 0;
    while (parser->current.kind != TOK_EOF) {
        if (parser->current.kind == TOK_COMMA && nested_parens == 0) break;
        if (parser->current.kind == TOK_LPAREN) {
            nested_parens++;
        } else if (parser->current.kind == TOK_RPAREN) {
            if (nested_parens == 0) break;
            nested_parens--;
        } else if ((parser->current.kind == TOK_IDENT || parser->current.kind == TOK_STRING) &&
                   parser->current.text &&
                   !str_vec_add(&condition_parts, parser->current.text)) {
            str_vec_free(&condition_parts);
            return false;
        }
        parser_advance(parser);
    }
    char *condition = bp_join_path(&condition_parts);
    str_vec_free(&condition_parts);
    if (!condition) return false;
    if (parser->current.kind == TOK_COMMA) parser_advance(parser);
    if (parser->current.kind != TOK_LBRACE) {
        free(condition);
        return true;
    }
    parser_advance(parser);
    while (parser->current.kind != TOK_EOF && parser->current.kind != TOK_RBRACE) {
        if (parser->current.kind != TOK_IDENT && parser->current.kind != TOK_STRING) {
            parser_advance(parser);
            continue;
        }
        char *branch = parser->current.text ? strdup(parser->current.text) : NULL;
        parser_advance(parser);
        if (!branch) {
            free(condition);
            return false;
        }
        if (parser->current.kind != TOK_COLON) {
            free(branch);
            continue;
        }
        parser_advance(parser);
        str_vec_t values = {0};
        if (!parse_bp_expression(parser, vars, &values)) {
            free(branch);
            free(condition);
            str_vec_free(&values);
            return false;
        }
        const char *condition_name = condition[0] ? condition : "condition";
        size_t select_size = strlen(condition_name) + strlen(branch) + 9;
        char *select_scope = malloc(select_size);
        if (!select_scope) {
            free(branch);
            free(condition);
            str_vec_free(&values);
            return false;
        }
        (void)snprintf(select_scope, select_size, "select.%s.%s", condition_name, branch);
        char *effective_scope = select_scope;
        if (is_variant_scope(scope)) {
            size_t effective_size = strlen(scope) + strlen(select_scope) + 2;
            effective_scope = malloc(effective_size);
            if (effective_scope) {
                (void)snprintf(effective_scope, effective_size, "%s&%s", scope, select_scope);
            }
        }
        bool ok = effective_scope != NULL;
        for (int i = 0; ok && i < values.count; i++) {
            ok = record_bp_string(module, key, effective_scope, depth, values.items[i]);
        }
        if (effective_scope != select_scope) free(effective_scope);
        free(select_scope);
        free(branch);
        str_vec_free(&values);
        if (!ok) {
            free(condition);
            return false;
        }
        if (parser->current.kind == TOK_COMMA) parser_advance(parser);
    }
    free(condition);
    if (parser->current.kind == TOK_RBRACE) parser_advance(parser);
    if (parser->current.kind == TOK_RPAREN) parser_advance(parser);
    return true;
}

static bool parse_bp_value(parser_t *parser, module_decl_t *module, const bp_var_vec_t *vars,
                           const char *key, const char *scope, int depth) {
    if (parser->current.kind == TOK_IDENT && parser->current.text &&
        strcmp(parser->current.text, "select") == 0) {
        return parse_bp_select(parser, module, vars, key, scope, depth);
    }
    if (parser->current.kind == TOK_LBRACE) {
        char *child_scope = bp_scope_child(scope, key);
        if (!child_scope) return false;
        parser_advance(parser);
        bool ok = parse_bp_object(parser, module, vars, child_scope, depth + 1);
        free(child_scope);
        return ok;
    }
    str_vec_t values = {0};
    if (!parse_bp_expression(parser, vars, &values)) {
        str_vec_free(&values);
        return false;
    }
    for (int i = 0; i < values.count; i++) {
        if (!record_bp_string(module, key, scope, depth, values.items[i])) {
            str_vec_free(&values);
            return false;
        }
    }
    str_vec_free(&values);
    return true;
}

static char *workspace_dir_path(const cbm_aosp_repo_t *repo, const char *file_path) {
    if (!repo || !repo->path || !file_path) return NULL;
    const char *slash = strrchr(file_path, '/');
    size_t dir_length = slash ? (size_t)(slash - file_path) : 0;
    const char *repo_path = strcmp(repo->path, ".") == 0 ? "" : repo->path;
    size_t repo_length = strlen(repo_path);
    size_t size = repo_length + dir_length + (repo_length && dir_length ? 2 : 1);
    char *path = malloc(size);
    if (!path) return NULL;
    if (repo_length && dir_length) {
        (void)snprintf(path, size, "%s/%.*s", repo_path, (int)dir_length, file_path);
    } else if (repo_length) {
        (void)snprintf(path, size, "%s", repo_path);
    } else if (dir_length) {
        (void)snprintf(path, size, "%.*s", (int)dir_length, file_path);
    } else {
        path[0] = '\0';
    }
    return path;
}

static char *workspace_file_path(const cbm_aosp_repo_t *repo, const char *file_path) {
    if (!repo || !repo->path || !file_path) return NULL;
    const char *repo_path = strcmp(repo->path, ".") == 0 ? "" : repo->path;
    size_t size = strlen(repo_path) + strlen(file_path) + (repo_path[0] ? 2 : 1);
    char *path = malloc(size);
    if (!path) return NULL;
    if (repo_path[0]) {
        (void)snprintf(path, size, "%s/%s", repo_path, file_path);
    } else {
        (void)snprintf(path, size, "%s", file_path);
    }
    for (char *p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    while (strncmp(path, "./", 2) == 0) memmove(path, path + 2, strlen(path + 2) + 1);
    return path;
}

static char *workspace_owner_path(const char *workspace_path, const char *prefix) {
    if (!workspace_path || !prefix) return NULL;
    size_t prefix_length = strlen(prefix);
    if (strncmp(workspace_path, prefix, prefix_length) != 0) return NULL;
    const char *slash = strrchr(workspace_path, '/');
    if (!slash || slash <= workspace_path + prefix_length) return NULL;
    return cbm_strndup(workspace_path, (size_t)(slash - workspace_path));
}

static bool parse_blueprint(scan_ctx_t *ctx, const char *source, size_t length,
                            const char *file_path) {
    char *package_path = workspace_dir_path(ctx->repo, file_path);
    if (!package_path) return false;
    scope_decl_t *package = scope_vec_ensure(&ctx->packages, package_path, file_path);
    if (!package) {
        free(package_path);
        return false;
    }
    parser_t parser = {.lexer = {.source = source, .length = length}};
    bp_var_vec_t vars = {0};
    parser.current = lexer_next(&parser.lexer);
    while (parser.current.kind != TOK_EOF) {
        if (parser.current.kind != TOK_IDENT) {
            parser_advance(&parser);
            continue;
        }
        char *type = parser.current.text ? strdup(parser.current.text) : NULL;
        parser_advance(&parser);
        bool append = false;
        if (parser.current.kind == TOK_PLUS) {
            append = true;
            parser_advance(&parser);
        }
        if (parser.current.kind == TOK_EQUAL) {
            parser_advance(&parser);
            str_vec_t values = {0};
            bool ok = parse_bp_expression(&parser, &vars, &values) &&
                      bp_var_assign(&vars, type, &values, append);
            str_vec_free(&values);
            free(type);
            if (!ok) {
                token_free(&parser.current);
                bp_var_vec_free(&vars);
                free(package_path);
                return false;
            }
            continue;
        }
        if (parser.current.kind != TOK_LBRACE) {
            free(type);
            continue;
        }
        parser_advance(&parser);
        module_decl_t module = {
            .type = type,
            .file_path = strdup(file_path),
            .package_path = strdup(package_path),
            .blueprint = true,
        };
        if (!module.type || !module.file_path ||
            !module.package_path || !parse_bp_object(&parser, &module, &vars, "", 1)) {
            module_free(&module);
            token_free(&parser.current);
            bp_var_vec_free(&vars);
            free(package_path);
            return false;
        }
        bool ok = true;
        if (strcmp(module.type, "package") == 0) {
            ok = str_vec_copy(&package->values, &module.declared_visibility);
            module_free(&module);
        } else if (strcmp(module.type, "soong_namespace") == 0) {
            scope_decl_t *namespace_decl =
                scope_vec_ensure(&ctx->namespaces, package_path, file_path);
            ok = namespace_decl &&
                 str_vec_copy(&namespace_decl->values, &module.namespace_imports);
            module_free(&module);
        } else {
            ok = module_vec_add(&ctx->modules, &module);
        }
        if (!ok) {
            module_free(&module);
            token_free(&parser.current);
            bp_var_vec_free(&vars);
            free(package_path);
            return false;
        }
    }
    token_free(&parser.current);
    bp_var_vec_free(&vars);
    free(package_path);
    return true;
}

static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static product_decl_t *product_vec_add(scan_ctx_t *ctx, const char *file_path) {
    product_vec_t *vec = &ctx->products;
    if (vec->count == vec->cap) {
        int new_cap = vec->cap ? vec->cap * 2 : 16;
        product_decl_t *items = realloc(vec->items, (size_t)new_cap * sizeof(*items));
        if (!items) return NULL;
        vec->items = items;
        vec->cap = new_cap;
    }
    product_decl_t *product = &vec->items[vec->count];
    memset(product, 0, sizeof(*product));
    product->file_path = strdup(file_path);
    product->workspace_path = workspace_file_path(ctx->repo, file_path);
    if (product->workspace_path) {
        product->device_owner = workspace_owner_path(product->workspace_path, "device/");
        product->vendor_owner = workspace_owner_path(product->workspace_path, "vendor/");
    }
    if (!product->file_path || !product->workspace_path) {
        product_free(product);
        return NULL;
    }
    vec->count++;
    return product;
}

static bool product_add_inherit(product_decl_t *product, const char *path,
                                const char *source_file, bool optional,
                                const char *status) {
    if (!product || !path || !path[0]) return true;
    if (product->inherit_count == product->inherit_cap) {
        int new_cap = product->inherit_cap ? product->inherit_cap * 2 : 8;
        product_inherit_decl_t *items =
            realloc(product->inherits, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        product->inherits = items;
        product->inherit_cap = new_cap;
    }
    product_inherit_decl_t *inherit = &product->inherits[product->inherit_count];
    memset(inherit, 0, sizeof(*inherit));
    inherit->path = strdup(path);
    inherit->source_file = strdup(source_file ? source_file : product->file_path);
    inherit->status = strdup(status ? status : "pending");
    inherit->optional = optional;
    if (!inherit->path || !inherit->source_file || !inherit->status) {
        free(inherit->path);
        free(inherit->source_file);
        free(inherit->status);
        memset(inherit, 0, sizeof(*inherit));
        return false;
    }
    product->inherit_count++;
    return true;
}

static product_package_decl_t *product_find_package(product_decl_t *product,
                                                    const char *name,
                                                    const char *partition) {
    for (int i = 0; product && i < product->package_count; i++) {
        product_package_decl_t *package = &product->packages[i];
        if (strcmp(package->name, name) == 0 &&
            (strcmp(package->partition, partition) == 0 || !package->included)) {
            return package;
        }
    }
    return NULL;
}

static bool product_add_package(product_decl_t *product, const char *name,
                                const char *partition, const char *source_variable,
                                const char *source_file, bool included,
                                const product_package_decl_t *inherited,
                                const char *inherit_source_path) {
    if (!product || !name || !name[0] || !partition) return true;
    product_package_decl_t *existing = product_find_package(product, name, partition);
    if (existing) return true;
    if (product->package_count == product->package_cap) {
        int new_cap = product->package_cap ? product->package_cap * 2 : 32;
        product_package_decl_t *items =
            realloc(product->packages, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        product->packages = items;
        product->package_cap = new_cap;
    }
    product_package_decl_t *package = &product->packages[product->package_count];
    memset(package, 0, sizeof(*package));
    package->name = strdup(name);
    package->partition = strdup(partition);
    package->source_variable = strdup(source_variable ? source_variable : "PRODUCT_PACKAGES");
    package->source_file = strdup(source_file ? source_file : product->file_path);
    package->included = included;
    if (inherited) {
        package->inherited_from = strdup(inherit_source_path);
        size_t path_size = strlen(inherited->inheritance_path ? inherited->inheritance_path : "") +
                           strlen(inherit_source_path) + 3;
        package->inheritance_path = malloc(path_size);
        if (package->inheritance_path) {
            if (inherited->inheritance_path && inherited->inheritance_path[0]) {
                (void)snprintf(package->inheritance_path, path_size, "%s>%s",
                               inherit_source_path, inherited->inheritance_path);
            } else {
                (void)snprintf(package->inheritance_path, path_size, "%s",
                               inherit_source_path);
            }
        }
        package->inheritance_depth = inherited->inheritance_depth + 1;
    }
    if (!package->name || !package->partition || !package->source_variable ||
        !package->source_file || (inherited && (!package->inherited_from ||
                                                !package->inheritance_path))) {
        product_package_free(package);
        return false;
    }
    product->package_count++;
    if (included && strcmp(partition, "unspecified") != 0) {
        return str_vec_add_unique(&product->partitions, partition);
    }
    return true;
}

static board_config_decl_t *board_config_vec_add(scan_ctx_t *ctx,
                                                 const char *file_path) {
    board_config_vec_t *vec = &ctx->board_configs;
    if (vec->count == vec->cap) {
        int new_cap = vec->cap ? vec->cap * 2 : 8;
        board_config_decl_t *items =
            realloc(vec->items, (size_t)new_cap * sizeof(*items));
        if (!items) return NULL;
        vec->items = items;
        vec->cap = new_cap;
    }
    board_config_decl_t *board = &vec->items[vec->count];
    memset(board, 0, sizeof(*board));
    board->file_path = strdup(file_path);
    board->workspace_path = workspace_file_path(ctx->repo, file_path);
    if (board->workspace_path) {
        board->device_owner = workspace_owner_path(board->workspace_path, "device/");
        board->vendor_owner = workspace_owner_path(board->workspace_path, "vendor/");
    }
    if (!board->file_path || !board->workspace_path) {
        board_config_free(board);
        return NULL;
    }
    vec->count++;
    return board;
}

static bool board_add_variable(board_config_decl_t *board, const char *name,
                               const char *value) {
    if (board->variable_count == board->variable_cap) {
        int new_cap = board->variable_cap ? board->variable_cap * 2 : 32;
        name_value_t *items = realloc(board->variables, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        board->variables = items;
        board->variable_cap = new_cap;
    }
    name_value_t *variable = &board->variables[board->variable_count];
    variable->name = strdup(name);
    variable->value = strdup(value ? value : "");
    if (!variable->name || !variable->value) {
        free(variable->name);
        free(variable->value);
        return false;
    }
    board->variable_count++;
    return true;
}

typedef struct {
    char *name;
    char *value;
    bool recursive;
} make_var_t;

typedef struct {
    make_var_t *items;
    int count;
    int cap;
} make_var_vec_t;

typedef struct {
    bool parent_active;
    bool active;
    bool matched;
    bool known;
    bool else_seen;
} make_condition_t;

typedef struct {
    scan_ctx_t *ctx;
    make_file_decl_t *coverage;
    product_decl_t *product;
    make_var_vec_t vars;
    str_vec_t include_stack;
    const char *current_file;
    make_condition_t conditions[64];
    int condition_depth;
} make_eval_t;

static make_var_t *make_var_find(make_var_vec_t *vars, const char *name) {
    for (int i = 0; vars && i < vars->count; i++) {
        if (strcmp(vars->items[i].name, name) == 0) return &vars->items[i];
    }
    return NULL;
}

static bool make_var_set(make_var_vec_t *vars, const char *name, const char *value,
                         bool recursive, bool append, bool only_if_missing) {
    make_var_t *var = make_var_find(vars, name);
    if (var && only_if_missing) return true;
    if (!var) {
        if (vars->count == vars->cap) {
            int new_cap = vars->cap ? vars->cap * 2 : 32;
            make_var_t *items = realloc(vars->items, (size_t)new_cap * sizeof(*items));
            if (!items) return false;
            vars->items = items;
            vars->cap = new_cap;
        }
        var = &vars->items[vars->count++];
        memset(var, 0, sizeof(*var));
        var->name = strdup(name);
        if (!var->name) return false;
        var->recursive = recursive;
    }
    if (append && var->value && var->value[0]) {
        size_t length = strlen(var->value) + strlen(value) + 2;
        char *combined = malloc(length);
        if (!combined) return false;
        (void)snprintf(combined, length, "%s %s", var->value, value);
        free(var->value);
        var->value = combined;
        return true;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) return false;
    free(var->value);
    var->value = copy;
    var->recursive = recursive;
    return true;
}

static void make_vars_clear_local(make_var_vec_t *vars) {
    for (int i = 0; vars && i < vars->count;) {
        if (strncmp(vars->items[i].name, "LOCAL_", 6) != 0) {
            i++;
            continue;
        }
        free(vars->items[i].name);
        free(vars->items[i].value);
        memmove(&vars->items[i], &vars->items[i + 1],
                (size_t)(vars->count - i - 1) * sizeof(*vars->items));
        vars->count--;
    }
}

static void make_var_vec_free(make_var_vec_t *vars) {
    if (!vars) return;
    for (int i = 0; i < vars->count; i++) {
        free(vars->items[i].name);
        free(vars->items[i].value);
    }
    free(vars->items);
    memset(vars, 0, sizeof(*vars));
}

static bool make_append(char **buffer, size_t *length, size_t *capacity,
                        const char *text, size_t text_length) {
    if (*length + text_length + 1 > *capacity) {
        size_t new_cap = *capacity ? *capacity : 64;
        while (new_cap < *length + text_length + 1) new_cap *= 2;
        char *grown = realloc(*buffer, new_cap);
        if (!grown) return false;
        *buffer = grown;
        *capacity = new_cap;
    }
    memcpy(*buffer + *length, text, text_length);
    *length += text_length;
    (*buffer)[*length] = '\0';
    return true;
}

static bool make_record_unsupported(make_eval_t *eval, const char *expression) {
    return !eval || !eval->coverage ||
           str_vec_add_unique(&eval->coverage->unsupported, expression);
}

static char *make_expand_text(make_eval_t *eval, const char *input, int depth,
                              bool *complete);

static bool make_split_args(const char *text, str_vec_t *args) {
    int nesting = 0;
    const char *start = text;
    for (const char *p = text;; p++) {
        if (*p == '(' || *p == '{') nesting++;
        if ((*p == ')' || *p == '}') && nesting > 0) nesting--;
        if ((*p == ',' && nesting == 0) || *p == '\0') {
            char *part = cbm_strndup(start, (size_t)(p - start));
            if (!part) return false;
            char *clean = trim(part);
            bool ok = str_vec_add(args, clean);
            free(part);
            if (!ok) return false;
            if (!*p) break;
            start = p + 1;
        }
    }
    return true;
}

static char *make_normalize_words(const char *text) {
    char *copy = strdup(text ? text : "");
    if (!copy) return NULL;
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;
    char *save = NULL;
    for (char *word = strtok_r(copy, " \t\r\n", &save); word;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        if (length && !make_append(&result, &length, &capacity, " ", 1)) goto fail;
        if (!make_append(&result, &length, &capacity, word, strlen(word))) goto fail;
    }
    free(copy);
    if (!result) result = strdup("");
    return result;
fail:
    free(copy);
    free(result);
    return NULL;
}

static char *make_replace_all(const char *text, const char *needle,
                              const char *replacement) {
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t needle_length = strlen(needle);
    const char *cursor = text;
    while (needle_length && strstr(cursor, needle)) {
        const char *match = strstr(cursor, needle);
        if (!make_append(&result, &length, &capacity, cursor,
                         (size_t)(match - cursor)) ||
            !make_append(&result, &length, &capacity, replacement,
                         strlen(replacement))) {
            free(result);
            return NULL;
        }
        cursor = match + needle_length;
    }
    if (!make_append(&result, &length, &capacity, cursor, strlen(cursor))) {
        free(result);
        return NULL;
    }
    return result;
}

static char *make_expand_call(make_eval_t *eval, const str_vec_t *args, int depth,
                              bool *complete) {
    if (args->count == 0) {
        *complete = false;
        return strdup("");
    }
    bool name_complete = true;
    char *name = make_expand_text(eval, args->items[0], depth + 1, &name_complete);
    if (!name) return NULL;
    char *clean_name = trim(name);
    if (strcmp(clean_name, "my-dir") == 0) {
        const char *slash = eval->current_file ? strrchr(eval->current_file, '/') : NULL;
        char *directory = slash ? cbm_strndup(eval->current_file,
                                               (size_t)(slash - eval->current_file))
                                : strdup(".");
        free(name);
        *complete = name_complete;
        return directory;
    }
    make_var_t *macro = make_var_find(&eval->vars, clean_name);
    if (!macro || !macro->value) {
        free(name);
        *complete = false;
        return strdup("");
    }
    char *expanded_macro = strdup(macro->value);
    if (!expanded_macro) {
        free(name);
        return NULL;
    }
    for (int i = 0; i < args->count && i < 10; i++) {
        char marker[8];
        (void)snprintf(marker, sizeof(marker), "$(%d)", i);
        bool arg_complete = true;
        char *argument = i == 0 ? strdup(clean_name)
                                : make_expand_text(eval, args->items[i], depth + 1,
                                                   &arg_complete);
        if (!argument) {
            free(expanded_macro);
            free(name);
            return NULL;
        }
        char *replaced = make_replace_all(expanded_macro, marker, argument);
        free(argument);
        free(expanded_macro);
        if (!replaced) {
            free(name);
            return NULL;
        }
        expanded_macro = replaced;
        name_complete = name_complete && arg_complete;
    }
    char *result = make_expand_text(eval, expanded_macro, depth + 1, complete);
    *complete = *complete && name_complete;
    free(expanded_macro);
    free(name);
    return result;
}

static char *make_expand_function(make_eval_t *eval, const char *expression,
                                  int depth, bool *complete) {
    const char *separator = expression;
    while (*separator && !isspace((unsigned char)*separator)) separator++;
    if (!*separator) {
        char *name = strdup(expression);
        if (!name) return NULL;
        char *clean = trim(name);
        make_var_t *var = make_var_find(&eval->vars, clean);
        if (!var || !var->value) {
            *complete = false;
            free(name);
            return NULL;
        }
        char *result = var->recursive
                           ? make_expand_text(eval, var->value, depth + 1, complete)
                           : strdup(var->value);
        free(name);
        return result;
    }
    char *function = cbm_strndup(expression, (size_t)(separator - expression));
    if (!function) return NULL;
    while (*separator && isspace((unsigned char)*separator)) separator++;
    str_vec_t args = {0};
    if (!make_split_args(separator, &args)) {
        free(function);
        str_vec_free(&args);
        return NULL;
    }
    char *result = NULL;
    if (strcmp(function, "call") == 0) {
        eval->coverage->macro_count++;
        result = make_expand_call(eval, &args, depth, complete);
    } else if (strcmp(function, "strip") == 0 && args.count == 1) {
        char *expanded = make_expand_text(eval, args.items[0], depth + 1, complete);
        result = expanded ? make_normalize_words(expanded) : NULL;
        free(expanded);
    } else if ((strcmp(function, "addprefix") == 0 ||
                strcmp(function, "addsuffix") == 0) && args.count == 2) {
        bool left_complete = true;
        bool right_complete = true;
        char *affix = make_expand_text(eval, args.items[0], depth + 1, &left_complete);
        char *words = make_expand_text(eval, args.items[1], depth + 1, &right_complete);
        char *copy = words ? strdup(words) : NULL;
        size_t length = 0;
        size_t capacity = 0;
        char *save = NULL;
        for (char *word = copy ? strtok_r(copy, " \t\r\n", &save) : NULL; word;
             word = strtok_r(NULL, " \t\r\n", &save)) {
            if (length) (void)make_append(&result, &length, &capacity, " ", 1);
            if (strcmp(function, "addprefix") == 0) {
                (void)make_append(&result, &length, &capacity, affix, strlen(affix));
            }
            (void)make_append(&result, &length, &capacity, word, strlen(word));
            if (strcmp(function, "addsuffix") == 0) {
                (void)make_append(&result, &length, &capacity, affix, strlen(affix));
            }
        }
        if (!result) result = strdup("");
        *complete = left_complete && right_complete;
        free(copy);
        free(words);
        free(affix);
    } else if (strcmp(function, "subst") == 0 && args.count == 3) {
        bool from_complete = true;
        bool to_complete = true;
        bool text_complete = true;
        char *from = make_expand_text(eval, args.items[0], depth + 1, &from_complete);
        char *to = make_expand_text(eval, args.items[1], depth + 1, &to_complete);
        char *text_value = make_expand_text(eval, args.items[2], depth + 1, &text_complete);
        result = from && to && text_value ? make_replace_all(text_value, from, to) : NULL;
        *complete = from_complete && to_complete && text_complete;
        free(from);
        free(to);
        free(text_value);
    } else if (strcmp(function, "if") == 0 && args.count >= 2) {
        bool condition_complete = true;
        char *condition = make_expand_text(eval, args.items[0], depth + 1,
                                           &condition_complete);
        int selected = condition && trim(condition)[0] ? 1 : 2;
        if (selected < args.count) {
            result = make_expand_text(eval, args.items[selected], depth + 1, complete);
        } else {
            result = strdup("");
            *complete = true;
        }
        *complete = *complete && condition_complete;
        free(condition);
    } else {
        *complete = false;
    }
    if (!result && !*complete) {
        size_t size = strlen(expression) + 4;
        result = malloc(size);
        if (result) (void)snprintf(result, size, "$(%s)", expression);
    }
    free(function);
    str_vec_free(&args);
    return result;
}

static char *make_expand_text(make_eval_t *eval, const char *input, int depth,
                              bool *complete) {
    if (complete) *complete = true;
    if (!input) return strdup("");
    if (depth > 32) {
        if (complete) *complete = false;
        return strdup(input);
    }
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;
    bool all_complete = true;
    for (size_t i = 0; input[i];) {
        if (input[i] != '$' || (input[i + 1] != '(' && input[i + 1] != '{')) {
            if (!make_append(&result, &length, &capacity, input + i, 1)) goto fail;
            i++;
            continue;
        }
        char open = input[i + 1];
        char close = open == '(' ? ')' : '}';
        int nesting = 1;
        size_t end = i + 2;
        while (input[end] && nesting > 0) {
            if (input[end] == open) nesting++;
            if (input[end] == close) nesting--;
            if (nesting > 0) end++;
        }
        if (nesting != 0) {
            all_complete = false;
            if (!make_append(&result, &length, &capacity, input + i, strlen(input + i)))
                goto fail;
            break;
        }
        char *expression = cbm_strndup(input + i + 2, end - (i + 2));
        if (!expression) goto fail;
        bool expression_complete = true;
        char *expanded = make_expand_function(eval, expression, depth,
                                              &expression_complete);
        if (!expanded) {
            size_t raw_length = end - i + 1;
            expanded = cbm_strndup(input + i, raw_length);
            expression_complete = false;
        }
        if (!expanded || !make_append(&result, &length, &capacity, expanded,
                                      strlen(expanded))) {
            free(expression);
            free(expanded);
            goto fail;
        }
        if (!expression_complete) all_complete = false;
        free(expression);
        free(expanded);
        i = end + 1;
    }
    if (!result) result = strdup("");
    if (complete) *complete = all_complete;
    return result;
fail:
    free(result);
    return NULL;
}

static bool make_handle_product_inherit(make_eval_t *eval, const char *line,
                                        bool *handled) {
    *handled = false;
    if (!eval->product || strncmp(line, "$(call ", 7) != 0) return true;
    size_t length = strlen(line);
    if (length < 9 || line[length - 1] != ')') return true;
    char *expression = cbm_strndup(line + 7, length - 8);
    if (!expression) return false;
    str_vec_t args = {0};
    bool ok = make_split_args(expression, &args);
    if (!ok || args.count != 2 ||
        (strcmp(args.items[0], "inherit-product") != 0 &&
         strcmp(args.items[0], "inherit-product-if-exists") != 0)) {
        str_vec_free(&args);
        free(expression);
        return ok;
    }
    *handled = true;
    bool complete = true;
    char *expanded = make_expand_text(eval, args.items[1], 0, &complete);
    char *normalized = expanded ? trim(expanded) : NULL;
    while (normalized && strncmp(normalized, "./", 2) == 0) normalized += 2;
    if (!expanded || !normalized[0]) {
        ok = false;
    } else {
        for (char *p = normalized; *p; p++) {
            if (*p == '\\') *p = '/';
        }
        const char *status = complete ? "pending" : "unsupported_expression";
        ok = product_add_inherit(eval->product, normalized, eval->current_file,
                                 strcmp(args.items[0], "inherit-product-if-exists") == 0,
                                 status);
        if (!complete) ok = make_record_unsupported(eval, line) && ok;
    }
    eval->coverage->macro_count++;
    free(expanded);
    str_vec_free(&args);
    free(expression);
    return ok;
}

static bool make_add_words(module_decl_t *module, const char *value,
                           const char *kind) {
    char *copy = strdup(value ? value : "");
    if (!copy) return false;
    bool ok = true;
    char *save = NULL;
    for (char *word = strtok_r(copy, " \t\r\n", &save); word && ok;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        ok = module_add_dep(module, word, kind);
    }
    free(copy);
    return ok;
}

static const char *make_dependency_kind(const char *key) {
    if (strcmp(key, "LOCAL_SHARED_LIBRARIES") == 0) return "SHARED_LIB";
    if (strcmp(key, "LOCAL_STATIC_LIBRARIES") == 0 || strcmp(key, "LOCAL_WHOLE_STATIC_LIBRARIES") == 0) return "STATIC_LIB";
    if (strcmp(key, "LOCAL_HEADER_LIBRARIES") == 0) return "HEADER_LIB";
    if (strcmp(key, "LOCAL_JAVA_LIBRARIES") == 0 || strcmp(key, "LOCAL_STATIC_JAVA_LIBRARIES") == 0) return "LIB";
    if (strcmp(key, "LOCAL_JNI_SHARED_LIBRARIES") == 0) return "JNI_LIB";
    if (strcmp(key, "LOCAL_RUNTIME_LIBRARIES") == 0) return "RUNTIME_LIB";
    if (strcmp(key, "LOCAL_USES_LIBRARIES") == 0) return "USES_LIB";
    if (strcmp(key, "LOCAL_OPTIONAL_USES_LIBRARIES") == 0) return "OPTIONAL_USES_LIB";
    if (strcmp(key, "LOCAL_REQUIRED_MODULES") == 0 ||
        strcmp(key, "LOCAL_HOST_REQUIRED_MODULES") == 0 ||
        strcmp(key, "LOCAL_TARGET_REQUIRED_MODULES") == 0) return "REQUIRED";
    return NULL;
}

static const char *make_semantic_type(const char *rule, const char *module_class) {
    if (strcmp(rule, "BUILD_SHARED_LIBRARY") == 0) return "cc_library_shared";
    if (strcmp(rule, "BUILD_STATIC_LIBRARY") == 0) return "cc_library_static";
    if (strcmp(rule, "BUILD_EXECUTABLE") == 0) return "cc_binary";
    if (strcmp(rule, "BUILD_HOST_EXECUTABLE") == 0) return "cc_binary_host";
    if (strcmp(rule, "BUILD_NATIVE_TEST") == 0) return "cc_test";
    if (strcmp(rule, "BUILD_JAVA_LIBRARY") == 0) return "java_library";
    if (strcmp(rule, "BUILD_STATIC_JAVA_LIBRARY") == 0) return "java_library_static";
    if (strcmp(rule, "BUILD_HOST_JAVA_LIBRARY") == 0) return "java_library_host";
    if (strcmp(rule, "BUILD_PACKAGE") == 0) return "android_app";
    if (strcmp(rule, "BUILD_RRO_PACKAGE") == 0) return "runtime_resource_overlay";
    if (strcmp(rule, "BUILD_PREBUILT") == 0) {
        if (module_class && strcmp(module_class, "APPS") == 0) return "android_app_import";
        if (module_class && strcmp(module_class, "JAVA_LIBRARIES") == 0) return "java_import";
        if (module_class && strcmp(module_class, "SHARED_LIBRARIES") == 0)
            return "cc_prebuilt_library_shared";
        if (module_class && strcmp(module_class, "STATIC_LIBRARIES") == 0)
            return "cc_prebuilt_library_static";
        if (module_class && strcmp(module_class, "EXECUTABLES") == 0)
            return "cc_prebuilt_binary";
        if (module_class && strcmp(module_class, "ETC") == 0) return "prebuilt_etc";
        return "prebuilt";
    }
    return "android_make";
}

static const char *make_default_class(const char *rule) {
    if (strcmp(rule, "BUILD_SHARED_LIBRARY") == 0) return "SHARED_LIBRARIES";
    if (strcmp(rule, "BUILD_STATIC_LIBRARY") == 0) return "STATIC_LIBRARIES";
    if (strcmp(rule, "BUILD_EXECUTABLE") == 0 ||
        strcmp(rule, "BUILD_HOST_EXECUTABLE") == 0) return "EXECUTABLES";
    if (strcmp(rule, "BUILD_NATIVE_TEST") == 0) return "NATIVE_TESTS";
    if (strstr(rule, "JAVA_LIBRARY")) return "JAVA_LIBRARIES";
    if (strcmp(rule, "BUILD_PACKAGE") == 0 || strcmp(rule, "BUILD_RRO_PACKAGE") == 0)
        return "APPS";
    return NULL;
}

static char *make_value(make_eval_t *eval, const char *name, bool *complete) {
    make_var_t *var = make_var_find(&eval->vars, name);
    if (!var || !var->value) {
        *complete = false;
        return strdup("");
    }
    if (!var->recursive) {
        *complete = true;
        return strdup(var->value);
    }
    return make_expand_text(eval, var->value, 0, complete);
}

static bool make_commit_module(make_eval_t *eval, const char *rule) {
    bool name_complete = true;
    char *name = make_value(eval, "LOCAL_MODULE", &name_complete);
    if (!name) return false;
    char *clean_name = trim(name);
    if (!name_complete || !clean_name[0]) {
        bool ok = make_record_unsupported(eval, rule);
        free(name);
        return ok;
    }
    bool class_complete = true;
    char *module_class = make_value(eval, "LOCAL_MODULE_CLASS", &class_complete);
    if (!module_class) {
        free(name);
        return false;
    }
    char *clean_class = trim(module_class);
    const char *effective_class = clean_class[0] ? clean_class : make_default_class(rule);
    module_decl_t module = {
        .name = strdup(clean_name),
        .type = strdup(make_semantic_type(rule, effective_class)),
        .file_path = strdup(eval->current_file),
        .make_build_rule = strdup(rule),
        .make_module_class = effective_class ? strdup(effective_class) : NULL,
    };
    free(name);
    free(module_class);
    if (!module.name || !module.type || !module.file_path || !module.make_build_rule ||
        (effective_class && !module.make_module_class)) {
        module_free(&module);
        return false;
    }
    static const char *dependency_vars[] = {
        "LOCAL_SHARED_LIBRARIES", "LOCAL_STATIC_LIBRARIES",
        "LOCAL_WHOLE_STATIC_LIBRARIES", "LOCAL_HEADER_LIBRARIES",
        "LOCAL_JAVA_LIBRARIES", "LOCAL_STATIC_JAVA_LIBRARIES",
        "LOCAL_JNI_SHARED_LIBRARIES", "LOCAL_RUNTIME_LIBRARIES",
        "LOCAL_USES_LIBRARIES", "LOCAL_OPTIONAL_USES_LIBRARIES",
        "LOCAL_REQUIRED_MODULES", "LOCAL_HOST_REQUIRED_MODULES",
        "LOCAL_TARGET_REQUIRED_MODULES",
    };
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(dependency_vars) / sizeof(dependency_vars[0]); i++) {
        make_var_t *var = make_var_find(&eval->vars, dependency_vars[i]);
        if (!var) continue;
        bool value_complete = true;
        char *value = make_value(eval, dependency_vars[i], &value_complete);
        const char *kind = make_dependency_kind(dependency_vars[i]);
        ok = value && make_add_words(&module, value, kind);
        if (!value_complete) ok = ok && make_record_unsupported(eval, var->value);
        free(value);
    }
    static const char *file_vars[] = {
        "LOCAL_SRC_FILES", "LOCAL_GENERATED_SOURCES", "LOCAL_PREBUILT_MODULE_FILE",
    };
    for (size_t i = 0; ok && i < sizeof(file_vars) / sizeof(file_vars[0]); i++) {
        make_var_t *var = make_var_find(&eval->vars, file_vars[i]);
        if (!var) continue;
        bool value_complete = true;
        char *value = make_value(eval, file_vars[i], &value_complete);
        char *save = NULL;
        for (char *word = value ? strtok_r(value, " \t\r\n", &save) : NULL;
             word && ok; word = strtok_r(NULL, " \t\r\n", &save)) {
            if (!strstr(word, "$(") && !strstr(word, "${")) {
                ok = module_add_file_variant(&module, word, "SOURCE", NULL);
            }
        }
        if (!value_complete) ok = ok && make_record_unsupported(eval, var->value);
        free(value);
    }
    if (ok) ok = module_vec_add(&eval->ctx->modules, &module);
    module_free(&module);
    return ok;
}

static bool make_is_active(const make_eval_t *eval) {
    return eval->condition_depth == 0 ||
           eval->conditions[eval->condition_depth - 1].active;
}

static bool make_parse_condition_args(const char *text, char **left, char **right) {
    *left = NULL;
    *right = NULL;
    char *copy = strdup(text);
    if (!copy) return false;
    char *value = trim(copy);
    size_t length = strlen(value);
    if (length >= 2 && value[0] == '(' && value[length - 1] == ')') {
        value[length - 1] = '\0';
        str_vec_t args = {0};
        bool ok = make_split_args(value + 1, &args) && args.count == 2;
        if (ok) {
            *left = strdup(args.items[0]);
            *right = strdup(args.items[1]);
            ok = *left && *right;
        }
        str_vec_free(&args);
        free(copy);
        return ok;
    }
    if (value[0] == '\'' || value[0] == '"') {
        char quote = value[0];
        char *left_end = strchr(value + 1, quote);
        char *right_start = left_end ? trim(left_end + 1) : NULL;
        if (left_end && right_start && right_start[0] == quote) {
            char *right_end = strchr(right_start + 1, quote);
            if (right_end) {
                *left = cbm_strndup(value + 1, (size_t)(left_end - (value + 1)));
                *right = cbm_strndup(right_start + 1,
                                     (size_t)(right_end - (right_start + 1)));
            }
        }
    }
    free(copy);
    return *left && *right;
}

static bool make_begin_condition(make_eval_t *eval, const char *line,
                                 const char *directive) {
    if (eval->condition_depth >= (int)(sizeof(eval->conditions) /
                                       sizeof(eval->conditions[0]))) {
        return make_record_unsupported(eval, line);
    }
    bool parent_active = make_is_active(eval);
    bool known = true;
    bool matched = false;
    const char *arguments = trim((char *)line + strlen(directive));
    if (strcmp(directive, "ifdef") == 0 || strcmp(directive, "ifndef") == 0) {
        make_var_t *var = make_var_find(&eval->vars, arguments);
        if (!var) {
            known = false;
        } else {
            matched = var->value && var->value[0];
            if (strcmp(directive, "ifndef") == 0) matched = !matched;
        }
    } else {
        char *left = NULL;
        char *right = NULL;
        if (!make_parse_condition_args(arguments, &left, &right)) {
            known = false;
        } else {
            bool left_complete = true;
            bool right_complete = true;
            char *expanded_left = make_expand_text(eval, left, 0, &left_complete);
            char *expanded_right = make_expand_text(eval, right, 0, &right_complete);
            known = expanded_left && expanded_right && left_complete && right_complete;
            if (known) {
                matched = strcmp(trim(expanded_left), trim(expanded_right)) == 0;
                if (strcmp(directive, "ifneq") == 0) matched = !matched;
            }
            free(expanded_left);
            free(expanded_right);
        }
        free(left);
        free(right);
    }
    eval->coverage->condition_count++;
    if (!known && !make_record_unsupported(eval, line)) return false;
    make_condition_t *condition = &eval->conditions[eval->condition_depth++];
    *condition = (make_condition_t){
        .parent_active = parent_active,
        .active = parent_active && known && matched,
        .matched = matched,
        .known = known,
    };
    return true;
}

static bool make_handle_condition(make_eval_t *eval, char *line, bool *handled) {
    *handled = true;
    if (strncmp(line, "ifeq", 4) == 0 && isspace((unsigned char)line[4]))
        return make_begin_condition(eval, line, "ifeq");
    if (strncmp(line, "ifneq", 5) == 0 && isspace((unsigned char)line[5]))
        return make_begin_condition(eval, line, "ifneq");
    if (strncmp(line, "ifdef", 5) == 0 && isspace((unsigned char)line[5]))
        return make_begin_condition(eval, line, "ifdef");
    if (strncmp(line, "ifndef", 6) == 0 && isspace((unsigned char)line[6]))
        return make_begin_condition(eval, line, "ifndef");
    if (strcmp(line, "else") == 0) {
        if (eval->condition_depth == 0) return make_record_unsupported(eval, line);
        make_condition_t *condition = &eval->conditions[eval->condition_depth - 1];
        if (condition->else_seen) return make_record_unsupported(eval, line);
        condition->else_seen = true;
        condition->active = condition->parent_active && condition->known &&
                            !condition->matched;
        return true;
    }
    if (strncmp(line, "else ", 5) == 0) {
        if (eval->condition_depth > 0) {
            make_condition_t *condition =
                &eval->conditions[eval->condition_depth - 1];
            condition->active = false;
            condition->else_seen = true;
        }
        return make_record_unsupported(eval, line);
    }
    if (strcmp(line, "endif") == 0) {
        if (eval->condition_depth == 0) return make_record_unsupported(eval, line);
        eval->condition_depth--;
        return true;
    }
    *handled = false;
    return true;
}

static bool make_normalize_include(const char *path, char **normalized) {
    *normalized = NULL;
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\' ||
        (isalpha((unsigned char)path[0]) && path[1] == ':') || strstr(path, "..")) {
        return false;
    }
    char *copy = strdup(path);
    if (!copy) return false;
    for (char *p = copy; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    while (strncmp(copy, "./", 2) == 0) memmove(copy, copy + 2, strlen(copy + 2) + 1);
    *normalized = copy;
    return true;
}

static bool make_parse_source(make_eval_t *eval, char *source, const char *file_path);

static bool make_include_file(make_eval_t *eval, const char *file_path, bool optional) {
    char *normalized = NULL;
    if (!make_normalize_include(file_path, &normalized))
        return make_record_unsupported(eval, file_path);
    for (int i = 0; i < eval->include_stack.count; i++) {
        if (strcmp(eval->include_stack.items[i], normalized) == 0) {
            bool ok = make_record_unsupported(eval, normalized);
            free(normalized);
            return ok;
        }
    }
    char absolute[BG_PATH_MAX];
    (void)snprintf(absolute, sizeof(absolute), "%s/%s", eval->ctx->repo->abs_path,
                   normalized);
    size_t length = 0;
    char *source = bg_read_file(absolute, &length);
    if (!source && eval->ctx->workspace && eval->ctx->workspace->root) {
        (void)snprintf(absolute, sizeof(absolute), "%s/%s",
                       eval->ctx->workspace->root, normalized);
        source = bg_read_file(absolute, &length);
    }
    if (!source) {
        bool ok = optional || make_record_unsupported(eval, normalized);
        free(normalized);
        return ok;
    }
    bool ok = str_vec_add_unique(&eval->coverage->includes, normalized) &&
              str_vec_add(&eval->include_stack, normalized);
    const char *previous_file = eval->current_file;
    if (ok) {
        eval->current_file = normalized;
        ok = make_parse_source(eval, source, normalized);
        eval->current_file = previous_file;
    }
    if (eval->include_stack.count > 0) {
        free(eval->include_stack.items[--eval->include_stack.count]);
    }
    free(source);
    free(normalized);
    return ok;
}

static bool make_handle_include(make_eval_t *eval, char *line) {
    bool optional = false;
    char *argument = NULL;
    if (strncmp(line, "include ", 8) == 0) {
        argument = trim(line + 8);
    } else if (strncmp(line, "-include ", 9) == 0) {
        optional = true;
        argument = trim(line + 9);
    } else if (strncmp(line, "sinclude ", 9) == 0) {
        optional = true;
        argument = trim(line + 9);
    } else {
        return false;
    }
    if (strcmp(argument, "$(CLEAR_VARS)") == 0 ||
        strcmp(argument, "${CLEAR_VARS}") == 0) {
        make_vars_clear_local(&eval->vars);
        return true;
    }
    size_t length = strlen(argument);
    if (length > 4 && argument[0] == '$' &&
        (argument[1] == '(' || argument[1] == '{')) {
        char close = argument[1] == '(' ? ')' : '}';
        if (argument[length - 1] == close) {
            char *rule = cbm_strndup(argument + 2, length - 3);
            if (!rule) return true;
            if (strncmp(rule, "BUILD_", 6) == 0) {
                bool ok = make_commit_module(eval, rule);
                free(rule);
                return ok;
            }
            free(rule);
        }
    }
    bool complete = true;
    char *expanded = make_expand_text(eval, argument, 0, &complete);
    if (!expanded) return true;
    if (!complete) {
        bool ok = make_record_unsupported(eval, argument);
        free(expanded);
        return ok;
    }
    bool ok = true;
    char *save = NULL;
    for (char *path = strtok_r(expanded, " \t\r\n", &save); path && ok;
         path = strtok_r(NULL, " \t\r\n", &save)) {
        ok = make_include_file(eval, path, optional);
    }
    free(expanded);
    return ok;
}

static bool make_handle_assignment(make_eval_t *eval, char *line) {
    char *assign = strstr(line, ":=");
    const char *op = ":=";
    if (!assign) {
        assign = strstr(line, "+=");
        op = "+=";
    }
    if (!assign) {
        assign = strstr(line, "?=");
        op = "?=";
    }
    if (!assign) {
        assign = strchr(line, '=');
        op = "=";
    }
    if (!assign) return make_record_unsupported(eval, line);
    *assign = '\0';
    char *name = trim(line);
    while (strncmp(name, "override ", 9) == 0 || strncmp(name, "export ", 7) == 0 ||
           strncmp(name, "private ", 8) == 0) {
        char *space = strchr(name, ' ');
        name = trim(space + 1);
    }
    char *value = trim(assign + 2 - (strcmp(op, "=") == 0));
    bool append = strcmp(op, "+=") == 0;
    bool only_if_missing = strcmp(op, "?=") == 0;
    make_var_t *existing = make_var_find(&eval->vars, name);
    if (only_if_missing && existing) return true;
    bool recursive = strcmp(op, "=") == 0 || only_if_missing ||
                     (append && (!existing || existing->recursive));
    char *stored = NULL;
    bool complete = true;
    if (recursive) {
        stored = strdup(value);
    } else {
        stored = make_expand_text(eval, value, 0, &complete);
    }
    if (!stored) return false;
    bool ok = make_var_set(&eval->vars, name, stored, recursive, append,
                           only_if_missing);
    if (!complete) ok = ok && make_record_unsupported(eval, value);
    free(stored);
    return ok;
}

static bool make_logical_lines(char *source, str_vec_t *logical) {
    char *save = NULL;
    char *pending = NULL;
    size_t pending_len = 0;
    for (char *line = strtok_r(source, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *part = trim(line);
        size_t len = strlen(part);
        bool continued = len > 0 && part[len - 1] == '\\';
        if (continued) part[--len] = '\0';
        char *grown = realloc(pending, pending_len + len + 2);
        if (!grown) {
            free(pending);
            str_vec_free(logical);
            return false;
        }
        pending = grown;
        if (pending_len) pending[pending_len++] = ' ';
        memcpy(pending + pending_len, part, len + 1);
        pending_len += len;
        if (continued) continue;
        if (!str_vec_add(logical, pending)) {
            free(pending);
            str_vec_free(logical);
            return false;
        }
        free(pending);
        pending = NULL;
        pending_len = 0;
    }
    free(pending);
    return true;
}

static bool make_parse_source(make_eval_t *eval, char *source, const char *file_path) {
    str_vec_t logical = {0};
    if (!make_logical_lines(source, &logical)) return false;
    bool ok = true;
    for (int i = 0; i < logical.count && ok; i++) {
        char *line = trim(logical.items[i]);
        if (!line[0]) continue;
        bool handled = false;
        ok = make_handle_condition(eval, line, &handled);
        if (!ok || handled) continue;
        if (!make_is_active(eval)) continue;
        if (strncmp(line, "define ", 7) == 0) {
            char *name = trim(line + 7);
            char *body = NULL;
            size_t body_length = 0;
            size_t body_capacity = 0;
            while (++i < logical.count && strcmp(trim(logical.items[i]), "endef") != 0) {
                const char *part = logical.items[i];
                if (body_length && !make_append(&body, &body_length, &body_capacity, "\n", 1)) {
                    ok = false;
                    break;
                }
                if (!make_append(&body, &body_length, &body_capacity, part, strlen(part))) {
                    ok = false;
                    break;
                }
            }
            if (i >= logical.count) ok = make_record_unsupported(eval, line);
            if (ok) ok = make_var_set(&eval->vars, name, body ? body : "", true, false, false);
            free(body);
            continue;
        }
        if (strcmp(line, "endef") == 0) {
            ok = make_record_unsupported(eval, line);
            continue;
        }
        bool product_inherit = false;
        ok = make_handle_product_inherit(eval, line, &product_inherit);
        if (!ok || product_inherit) continue;
        if (strncmp(line, "include ", 8) == 0 ||
            strncmp(line, "-include ", 9) == 0 ||
            strncmp(line, "sinclude ", 9) == 0) {
            ok = make_handle_include(eval, line);
            continue;
        }
        if (strchr(line, '=')) {
            ok = make_handle_assignment(eval, line);
            continue;
        }
        ok = make_record_unsupported(eval, line);
    }
    str_vec_free(&logical);
    (void)file_path;
    return ok;
}

static bool parse_android_mk(scan_ctx_t *ctx, char *source, const char *file_path) {
    make_file_decl_t *coverage = make_file_vec_add(&ctx->make_files, file_path);
    if (!coverage) return false;
    make_eval_t eval = {.ctx = ctx, .coverage = coverage, .current_file = file_path};
    if (!str_vec_add(&eval.include_stack, file_path)) return false;
    bool ok = make_parse_source(&eval, source, file_path);
    if (eval.condition_depth != 0) {
        ok = make_record_unsupported(&eval, "unterminated conditional") && ok;
    }
    make_var_vec_free(&eval.vars);
    str_vec_free(&eval.include_stack);
    return ok;
}

static bool make_seed_product_vars(make_eval_t *eval) {
    char *local_path = workspace_dir_path(eval->ctx->repo, eval->current_file);
    if (!local_path) return false;
    bool ok = make_var_set(&eval->vars, "LOCAL_PATH", local_path, false, false, false) &&
              make_var_set(&eval->vars, "SRC_TARGET_DIR", "build/make/target",
                           false, false, false) &&
              make_var_set(&eval->vars, "BUILD_SYSTEM", "build/make/core",
                           false, false, false);
    free(local_path);
    return ok;
}

static bool make_copy_named_value(make_eval_t *eval, const char *name, char **target) {
    make_var_t *var = make_var_find(&eval->vars, name);
    if (!var) return true;
    bool complete = true;
    char *value = make_value(eval, name, &complete);
    if (!value) return false;
    char *clean = trim(value);
    char *copy = strdup(clean);
    bool ok = copy != NULL;
    if (!complete) ok = make_record_unsupported(eval, var->value) && ok;
    if (ok) {
        free(*target);
        *target = copy;
    } else {
        free(copy);
    }
    free(value);
    return ok;
}

static const char *product_package_partition(const char *variable) {
    if (strcmp(variable, "PRODUCT_PACKAGES_VENDOR") == 0) return "vendor";
    if (strcmp(variable, "PRODUCT_PACKAGES_PRODUCT") == 0) return "product";
    if (strcmp(variable, "PRODUCT_PACKAGES_SYSTEM_EXT") == 0) return "system_ext";
    if (strcmp(variable, "PRODUCT_PACKAGES_ODM") == 0) return "odm";
    if (strcmp(variable, "PRODUCT_PACKAGES_SYSTEM") == 0) return "system";
    return "unspecified";
}

static const char *copy_target_partition(const char *target) {
    static const char *partitions[] = {
        "system_ext", "vendor_boot", "init_boot", "system", "vendor", "product",
        "odm", "oem", "recovery", "boot", "data",
    };
    for (size_t i = 0; i < sizeof(partitions) / sizeof(partitions[0]); i++) {
        size_t length = strlen(partitions[i]);
        if (strncmp(target, partitions[i], length) == 0 &&
            (target[length] == '/' || target[length] == '\0')) {
            return partitions[i];
        }
    }
    return NULL;
}

static bool collect_product_copy_partitions(make_eval_t *eval,
                                            product_decl_t *product) {
    make_var_t *var = make_var_find(&eval->vars, "PRODUCT_COPY_FILES");
    if (!var) return true;
    bool complete = true;
    char *value = make_value(eval, "PRODUCT_COPY_FILES", &complete);
    if (!value) return false;
    bool ok = true;
    char *save = NULL;
    for (char *entry = strtok_r(value, " \t\r\n", &save); entry && ok;
         entry = strtok_r(NULL, " \t\r\n", &save)) {
        char *separator = strchr(entry, ':');
        if (!separator || !separator[1]) {
            ok = make_record_unsupported(eval, entry);
            continue;
        }
        char *target = separator + 1;
        char *owner = strchr(target, ':');
        if (owner) *owner = '\0';
        const char *partition = copy_target_partition(target);
        if (partition) ok = str_vec_add_unique(&product->partitions, partition);
    }
    if (!complete) ok = make_record_unsupported(eval, var->value) && ok;
    free(value);
    return ok;
}

static bool collect_product_packages(make_eval_t *eval, product_decl_t *product) {
    static const char prefix[] = "PRODUCT_PACKAGES";
    bool ok = true;
    for (int i = 0; i < eval->vars.count && ok; i++) {
        make_var_t *var = &eval->vars.items[i];
        if (strncmp(var->name, prefix, sizeof(prefix) - 1) != 0 ||
            (var->name[sizeof(prefix) - 1] != '\0' &&
             var->name[sizeof(prefix) - 1] != '_')) {
            continue;
        }
        bool complete = true;
        char *value = make_value(eval, var->name, &complete);
        if (!value) return false;
        char *save = NULL;
        for (char *word = strtok_r(value, " \t\r\n", &save); word && ok;
             word = strtok_r(NULL, " \t\r\n", &save)) {
            bool included = word[0] != '-';
            const char *name = included ? word : word + 1;
            if (!name[0] || strstr(name, "$(") || strstr(name, "${")) {
                ok = make_record_unsupported(eval, word);
                continue;
            }
            ok = product_add_package(product, name,
                                     product_package_partition(var->name), var->name,
                                     eval->current_file, included, NULL, NULL);
        }
        if (!complete) ok = make_record_unsupported(eval, var->value) && ok;
        free(value);
    }
    return ok;
}

static char *product_fallback_name(const char *workspace_path) {
    const char *base = strrchr(workspace_path, '/');
    base = base ? base + 1 : workspace_path;
    size_t length = strlen(base);
    if (length > 3 && strcmp(base + length - 3, ".mk") == 0) length -= 3;
    return cbm_strndup(base, length);
}

static bool parse_product_make(scan_ctx_t *ctx, char *source, const char *file_path) {
    make_file_decl_t *coverage = make_file_vec_add(&ctx->make_files, file_path);
    product_decl_t *product = product_vec_add(ctx, file_path);
    if (!coverage || !product) return false;
    make_eval_t eval = {
        .ctx = ctx, .coverage = coverage, .product = product, .current_file = file_path,
    };
    bool ok = str_vec_add(&eval.include_stack, file_path) && make_seed_product_vars(&eval) &&
              make_parse_source(&eval, source, file_path);
    if (eval.condition_depth != 0) {
        ok = make_record_unsupported(&eval, "unterminated conditional") && ok;
    }
    if (ok) ok = make_copy_named_value(&eval, "PRODUCT_NAME", &product->name);
    if (ok) ok = make_copy_named_value(&eval, "PRODUCT_DEVICE", &product->device);
    if (ok) ok = make_copy_named_value(&eval, "PRODUCT_BRAND", &product->brand);
    if (ok) ok = make_copy_named_value(&eval, "PRODUCT_MODEL", &product->model);
    if (ok) ok = make_copy_named_value(&eval, "PRODUCT_MANUFACTURER", &product->manufacturer);
    if (ok) ok = collect_product_packages(&eval, product);
    if (ok) ok = collect_product_copy_partitions(&eval, product);
    product->kind = strdup(product->name && product->name[0] ? "product" : "fragment");
    if (!product->name || !product->name[0]) {
        free(product->name);
        product->name = product_fallback_name(product->workspace_path);
    }
    if (!product->kind || !product->name) ok = false;
    make_var_vec_free(&eval.vars);
    str_vec_free(&eval.include_stack);
    return ok;
}

static const char *board_partition_for_variable(const char *name) {
    if (strstr(name, "SYSTEM_EXT")) return "system_ext";
    if (strstr(name, "VENDOR_BOOT")) return "vendor_boot";
    if (strstr(name, "INIT_BOOT")) return "init_boot";
    if (strstr(name, "VENDORIMAGE")) return "vendor";
    if (strstr(name, "PRODUCTIMAGE")) return "product";
    if (strstr(name, "ODMIMAGE")) return "odm";
    if (strstr(name, "SYSTEMIMAGE")) return "system";
    if (strstr(name, "RECOVERYIMAGE")) return "recovery";
    if (strstr(name, "USERDATAIMAGE")) return "userdata";
    if (strstr(name, "CACHEIMAGE")) return "cache";
    if (strstr(name, "DTBOIMG")) return "dtbo";
    if (strstr(name, "BOOTIMAGE")) return "boot";
    if (strstr(name, "SUPER_PARTITION")) return "super";
    return NULL;
}

static bool board_variable_supported(const char *name) {
    return strncmp(name, "BOARD_", 6) == 0 ||
           strncmp(name, "TARGET_BOARD_", 13) == 0 ||
           strncmp(name, "TARGET_BOOTLOADER_", 18) == 0 ||
           strcmp(name, "DEVICE_MANIFEST_FILE") == 0 ||
           strcmp(name, "DEVICE_MATRIX_FILE") == 0 ||
           strcmp(name, "ODM_MANIFEST_FILES") == 0;
}

static bool parse_board_config(scan_ctx_t *ctx, char *source, const char *file_path) {
    make_file_decl_t *coverage = make_file_vec_add(&ctx->make_files, file_path);
    board_config_decl_t *board = board_config_vec_add(ctx, file_path);
    if (!coverage || !board) return false;
    make_eval_t eval = {.ctx = ctx, .coverage = coverage, .current_file = file_path};
    bool ok = str_vec_add(&eval.include_stack, file_path) && make_seed_product_vars(&eval) &&
              make_parse_source(&eval, source, file_path);
    if (eval.condition_depth != 0) {
        ok = make_record_unsupported(&eval, "unterminated conditional") && ok;
    }
    for (int i = 0; i < eval.vars.count && ok; i++) {
        make_var_t *var = &eval.vars.items[i];
        if (!board_variable_supported(var->name)) continue;
        bool complete = true;
        char *value = make_value(&eval, var->name, &complete);
        if (!value) {
            ok = false;
            break;
        }
        ok = board_add_variable(board, var->name, trim(value));
        const char *partition = board_partition_for_variable(var->name);
        if (partition) ok = str_vec_add_unique(&board->partitions, partition) && ok;
        if (!complete) ok = make_record_unsupported(&eval, var->value) && ok;
        free(value);
    }
    make_var_vec_free(&eval.vars);
    str_vec_free(&eval.include_stack);
    return ok;
}

static bool parse_aidl(const char *source, size_t length, const char *file_path,
                       module_vec_t *modules) {
    lexer_t lexer = {.source = source, .length = length};
    token_t token = lexer_next(&lexer);
    char *package_name = NULL;
    char *decl_name = NULL;
    while (token.kind != TOK_EOF) {
        if (token.kind == TOK_IDENT && token.text && strcmp(token.text, "package") == 0) {
            token_free(&token);
            token = lexer_next(&lexer);
            if (token.kind == TOK_IDENT && token.text) package_name = strdup(token.text);
        } else if (token.kind == TOK_IDENT && token.text &&
                   (strcmp(token.text, "interface") == 0 || strcmp(token.text, "parcelable") == 0 ||
                    strcmp(token.text, "union") == 0 || strcmp(token.text, "enum") == 0)) {
            token_free(&token);
            token = lexer_next(&lexer);
            if (token.kind == TOK_IDENT && token.text) {
                decl_name = strdup(token.text);
                break;
            }
        }
        token_free(&token);
        token = lexer_next(&lexer);
    }
    token_free(&token);
    if (!decl_name) {
        free(package_name);
        return true;
    }
    size_t needed = strlen(decl_name) + (package_name ? strlen(package_name) + 1 : 0) + 1;
    char *qualified = malloc(needed);
    if (!qualified) {
        free(package_name);
        free(decl_name);
        return false;
    }
    if (package_name) {
        (void)snprintf(qualified, needed, "%s.%s", package_name, decl_name);
    } else {
        (void)snprintf(qualified, needed, "%s", decl_name);
    }
    module_decl_t module = {.name = qualified, .type = strdup("aidl_decl"),
                            .file_path = strdup(file_path)};
    free(package_name);
    free(decl_name);
    if (!module.type || !module.file_path || !module_vec_add(modules, &module)) {
        module_free(&module);
        return false;
    }
    return true;
}

static bool skip_dir(const char *name) {
    return strcmp(name, ".git") == 0 || strcmp(name, ".repo") == 0 || strcmp(name, "out") == 0 ||
           strcmp(name, "node_modules") == 0 || strncmp(name, "bazel-", 6) == 0;
}

static bool is_nested_repo_boundary(const scan_ctx_t *ctx, const char *abs_path) {
    for (int i = 0; i < ctx->workspace->repo_count; i++) {
        const cbm_aosp_repo_t *candidate = &ctx->workspace->repos[i];
        if (candidate != ctx->repo && candidate->abs_path &&
            strcmp(candidate->abs_path, abs_path) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct {
    module_decl_t **items;
    int count;
    int cap;
} module_stack_t;

static void module_id(const cbm_aosp_repo_t *repo, const module_decl_t *module, char out[65]);
static void product_id(const cbm_aosp_workspace_t *workspace,
                       const product_decl_t *product, char out[65]);

static bool module_stack_push(module_stack_t *stack, module_decl_t *module) {
    if (stack->count == stack->cap) {
        int new_cap = stack->cap ? stack->cap * 2 : 16;
        module_decl_t **items = realloc(stack->items, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        stack->items = items;
        stack->cap = new_cap;
    }
    stack->items[stack->count++] = module;
    return true;
}

static bool path_in_scope(const char *path, const char *scope) {
    if (!path || !scope) return false;
    if (!scope[0]) return true;
    size_t length = strlen(scope);
    return strncmp(path, scope, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static char *canonical_scope_path(const char *path) {
    if (!path) return strdup("");
    while (*path == '/') path++;
    char *normalized = strdup(path);
    if (!normalized) return NULL;
    for (char *p = normalized; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    size_t length = strlen(normalized);
    while (length && normalized[length - 1] == '/') normalized[--length] = '\0';
    return normalized;
}

static scope_decl_t *find_nearest_scope(scan_ctx_t *contexts, int context_count,
                                        bool namespaces, const char *path,
                                        bool require_values) {
    scope_decl_t *best = NULL;
    size_t best_length = 0;
    for (int c = 0; c < context_count; c++) {
        scope_vec_t *vec = namespaces ? &contexts[c].namespaces : &contexts[c].packages;
        for (int i = 0; i < vec->count; i++) {
            scope_decl_t *candidate = &vec->items[i];
            size_t length = strlen(candidate->path);
            if ((!require_values || candidate->values.count > 0) &&
                path_in_scope(path, candidate->path) && (!best || length > best_length)) {
                best = candidate;
                best_length = length;
            }
        }
    }
    return best;
}

static bool copy_effective_visibility(module_decl_t *module, const str_vec_t *source) {
    for (int i = 0; source && i < source->count; i++) {
        if (strcmp(source->items[i], "//visibility:override") != 0 &&
            !str_vec_add_unique(&module->effective_visibility, source->items[i])) {
            return false;
        }
    }
    return true;
}

static bool finalize_module_scopes(scan_ctx_t *contexts, int context_count) {
    for (int c = 0; c < context_count; c++) {
        for (int i = 0; i < contexts[c].modules.count; i++) {
            module_decl_t *module = &contexts[c].modules.items[i];
            if (!module->package_path) {
                module->package_path = workspace_dir_path(contexts[c].repo, module->file_path);
                if (!module->package_path) return false;
            }
            scope_decl_t *namespace_decl = NULL;
            if (module->blueprint) {
                namespace_decl = find_nearest_scope(contexts, context_count, true,
                                                    module->package_path, false);
            }
            module->namespace_path = strdup(namespace_decl ? namespace_decl->path : "");
            if (!module->namespace_path ||
                (namespace_decl && !str_vec_copy(&module->namespace_imports,
                                                 &namespace_decl->values))) {
                return false;
            }
            if (module->declared_visibility.count > 0) {
                module->visibility_origin = strdup("module");
                module->visibility_source_package = strdup(module->package_path);
                if (!copy_effective_visibility(module, &module->declared_visibility)) return false;
            } else {
                scope_decl_t *package_decl =
                    find_nearest_scope(contexts, context_count, false,
                                       module->package_path, true);
                if (package_decl) {
                    module->visibility_origin = strdup("package_default");
                    module->visibility_source_package = strdup(package_decl->path);
                    if (!copy_effective_visibility(module, &package_decl->values)) return false;
                } else {
                    module->visibility_origin = strdup("legacy_public");
                    module->visibility_source_package = strdup("");
                    if (!str_vec_add(&module->effective_visibility,
                                     "//visibility:legacy_public")) return false;
                }
            }
            if (!module->visibility_origin || !module->visibility_source_package) return false;
            if (module->effective_visibility.count == 0 &&
                !str_vec_add(&module->effective_visibility, "//visibility:private")) {
                return false;
            }
        }
    }
    return true;
}

typedef enum {
    MODULE_RESOLVED,
    MODULE_NOT_FOUND,
    MODULE_AMBIGUOUS,
    MODULE_VISIBILITY_BLOCKED,
    MODULE_UNSUPPORTED_VISIBILITY,
} module_resolution_status_t;

typedef struct {
    module_resolution_status_t status;
    scan_ctx_t *ctx;
    module_decl_t *module;
    const char *tier;
    const char *visibility_rule;
    int candidate_count;
} module_resolution_t;

typedef struct {
    const char *namespace_path;
    const char *name;
    scan_ctx_t *ctx;
    module_decl_t *module;
} module_index_item_t;

typedef struct {
    module_index_item_t *items;
    int count;
} module_index_t;

static int compare_module_index_item(const void *left, const void *right) {
    const module_index_item_t *a = left;
    const module_index_item_t *b = right;
    int cmp = strcmp(a->namespace_path, b->namespace_path);
    return cmp ? cmp : strcmp(a->name, b->name);
}

static bool build_module_index(scan_ctx_t *contexts, int context_count,
                               module_index_t *index) {
    int count = 0;
    for (int c = 0; c < context_count; c++) count += contexts[c].modules.count;
    index->items = calloc((size_t)count, sizeof(*index->items));
    if (count > 0 && !index->items) return false;
    for (int c = 0; c < context_count; c++) {
        for (int i = 0; i < contexts[c].modules.count; i++) {
            module_decl_t *module = &contexts[c].modules.items[i];
            module_index_item_t *item = &index->items[index->count++];
            item->namespace_path = module->namespace_path;
            item->name = module->name;
            item->ctx = &contexts[c];
            item->module = module;
        }
    }
    if (index->count > 1) {
        qsort(index->items, (size_t)index->count, sizeof(*index->items),
              compare_module_index_item);
    }
    return true;
}

static int compare_module_key(const module_index_item_t *item,
                              const char *namespace_path, const char *name) {
    int cmp = strcmp(item->namespace_path, namespace_path);
    return cmp ? cmp : strcmp(item->name, name);
}

static int collect_namespace_candidates(const module_index_t *index,
                                        const char *namespace_path, const char *name,
                                        scan_ctx_t **first_ctx, module_decl_t **first_module) {
    int low = 0;
    int high = index->count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (compare_module_key(&index->items[mid], namespace_path, name) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    int count = 0;
    for (int i = low; i < index->count &&
                      compare_module_key(&index->items[i], namespace_path, name) == 0;
         i++) {
        if (count == 0) {
            *first_ctx = index->items[i].ctx;
            *first_module = index->items[i].module;
        }
        count++;
    }
    return count;
}

static int module_visibility_allows(const module_decl_t *source,
                                    const module_decl_t *target,
                                    const char **matching_rule) {
    bool unsupported = false;
    if (matching_rule) *matching_rule = NULL;
    for (int i = 0; i < target->effective_visibility.count; i++) {
        const char *rule = target->effective_visibility.items[i];
        if (strcmp(rule, "//visibility:public") == 0 ||
            strcmp(rule, "//visibility:legacy_public") == 0) {
            if (matching_rule) *matching_rule = rule;
            return 1;
        }
        if (strcmp(rule, "//visibility:private") == 0 ||
            strcmp(rule, ":__pkg__") == 0) {
            if (strcmp(source->package_path, target->package_path) == 0) {
                if (matching_rule) *matching_rule = rule;
                return 1;
            }
            continue;
        }
        if (strcmp(rule, ":__subpackages__") == 0) {
            if (path_in_scope(source->package_path, target->package_path)) {
                if (matching_rule) *matching_rule = rule;
                return 1;
            }
            continue;
        }
        if (strncmp(rule, "//", 2) == 0) {
            const char *colon = strrchr(rule + 2, ':');
            if (colon &&
                (strcmp(colon, ":__pkg__") == 0 ||
                 strcmp(colon, ":__subpackages__") == 0)) {
                char *scope = cbm_strndup(rule + 2, (size_t)(colon - (rule + 2)));
                if (!scope) return -1;
                bool allowed = strcmp(colon, ":__pkg__") == 0
                                   ? strcmp(source->package_path, scope) == 0
                                   : path_in_scope(source->package_path, scope);
                free(scope);
                if (allowed) {
                    if (matching_rule) *matching_rule = rule;
                    return 1;
                }
                continue;
            }
        }
        unsupported = true;
    }
    return unsupported ? -1 : 0;
}

static module_resolution_t resolve_module_reference(const module_index_t *index,
                                                    const module_decl_t *source,
                                                    const char *reference) {
    module_resolution_t result = {.status = MODULE_NOT_FOUND, .tier = "not_found"};
    const char *name = reference;
    char *explicit_scope = NULL;
    if (strncmp(reference, "//", 2) == 0) {
        const char *colon = strrchr(reference + 2, ':');
        if (!colon || !colon[1]) return result;
        explicit_scope = cbm_strndup(reference + 2,
                                     (size_t)(colon - (reference + 2)));
        if (!explicit_scope) return result;
        name = colon + 1;
        result.tier = "explicit_namespace";
        result.candidate_count = collect_namespace_candidates(
            index, explicit_scope, name, &result.ctx, &result.module);
    } else {
        result.tier = source->namespace_path[0] ? "current_namespace" : "global_namespace";
        result.candidate_count = collect_namespace_candidates(
            index, source->namespace_path, name, &result.ctx, &result.module);
        if (result.candidate_count == 0 && source->namespace_path[0]) {
            result.tier = "imported_namespace";
            for (int i = 0; i < source->namespace_imports.count; i++) {
                char *import_path = canonical_scope_path(source->namespace_imports.items[i]);
                if (!import_path) continue;
                scan_ctx_t *candidate_ctx = NULL;
                module_decl_t *candidate = NULL;
                int count = collect_namespace_candidates(index, import_path, name,
                                                          &candidate_ctx, &candidate);
                if (count > 0 && result.candidate_count == 0) {
                    result.ctx = candidate_ctx;
                    result.module = candidate;
                }
                result.candidate_count += count;
                free(import_path);
            }
            if (result.candidate_count == 0) {
                result.tier = "global_namespace";
                result.candidate_count = collect_namespace_candidates(
                    index, "", name, &result.ctx, &result.module);
            }
        }
    }
    free(explicit_scope);
    if (result.candidate_count == 0) return result;
    if (result.candidate_count > 1) {
        result.status = MODULE_AMBIGUOUS;
        return result;
    }
    int visibility = module_visibility_allows(source, result.module,
                                              &result.visibility_rule);
    if (visibility < 0) {
        result.status = MODULE_UNSUPPORTED_VISIBILITY;
    } else if (visibility == 0) {
        result.status = MODULE_VISIBILITY_BLOCKED;
    } else {
        result.status = MODULE_RESOLVED;
    }
    return result;
}

static bool mark_defaults_cycle(module_stack_t *stack, module_decl_t *target) {
    int start = -1;
    for (int i = 0; i < stack->count; i++) {
        if (stack->items[i] == target) start = i;
    }
    if (start < 0) return true;
    int canonical = start;
    size_t path_size = 1;
    for (int i = start; i < stack->count; i++) {
        path_size += strlen(stack->items[i]->name) + 1;
        if (strcmp(stack->items[i]->name, stack->items[canonical]->name) < 0) canonical = i;
    }
    path_size += strlen(stack->items[canonical]->name);
    char *path = malloc(path_size);
    if (!path) return false;
    path[0] = '\0';
    int cycle_count = stack->count - start;
    for (int offset = 0; offset < cycle_count; offset++) {
        int i = start + ((canonical - start + offset) % cycle_count);
        if (path[0]) (void)strcat(path, ">");
        (void)strcat(path, stack->items[i]->name);
    }
    (void)strcat(path, ">");
    (void)strcat(path, stack->items[canonical]->name);
    for (int i = start; i < stack->count; i++) {
        if (!stack->items[i]->defaults_cycle) {
            stack->items[i]->defaults_cycle = strdup(path);
            if (!stack->items[i]->defaults_cycle) {
                free(path);
                return false;
            }
        }
    }
    free(path);
    return true;
}

static bool expand_module_defaults(const module_index_t *index, module_decl_t *module,
                                   module_stack_t *stack) {
    if (module->defaults_state == 2) return true;
    if (module->defaults_state == 1) return mark_defaults_cycle(stack, module);
    module->defaults_state = 1;
    if (!module_stack_push(stack, module)) return false;
    int direct_dep_count = module->dep_count;
    for (int i = 0; i < direct_dep_count; i++) {
        dep_decl_t *defaults_dep = &module->deps[i];
        if (strcmp(defaults_dep->kind, "DEFAULTS") != 0) continue;
        module_resolution_t resolution =
            resolve_module_reference(index, module, defaults_dep->name);
        module_decl_t *target = resolution.status == MODULE_RESOLVED ? resolution.module : NULL;
        if (!target) continue;
        if (target->defaults_state == 1) {
            if (!mark_defaults_cycle(stack, target)) return false;
            continue;
        }
        if (!expand_module_defaults(index, target, stack)) return false;
        if (target->defaults_cycle && !module->defaults_cycle) {
            module->defaults_cycle = strdup(target->defaults_cycle);
            if (!module->defaults_cycle) return false;
        }
        for (int d = 0; d < target->dep_count; d++) {
            if (strcmp(target->deps[d].kind, "DEFAULTS") != 0 &&
                !module_add_inherited_dep(module, &target->deps[d], defaults_dep,
                                          target->name)) {
                return false;
            }
        }
        for (int f = 0; f < target->file_count; f++) {
            if (!module_add_inherited_file(module, &target->files[f], defaults_dep,
                                           target->name)) {
                return false;
            }
        }
    }
    stack->count--;
    module->defaults_state = 2;
    return true;
}

static bool expand_all_defaults(scan_ctx_t *contexts, int context_count,
                                const module_index_t *index) {
    module_stack_t stack = {0};
    bool ok = true;
    for (int c = 0; c < context_count && ok; c++) {
        for (int i = 0; i < contexts[c].modules.count && ok; i++) {
            ok = expand_module_defaults(index, &contexts[c].modules.items[i], &stack);
        }
    }
    free(stack.items);
    return ok;
}

static const char *resolution_failure(module_resolution_status_t status) {
    switch (status) {
        case MODULE_NOT_FOUND: return "not_found";
        case MODULE_AMBIGUOUS: return "ambiguous";
        case MODULE_VISIBILITY_BLOCKED: return "visibility_blocked";
        case MODULE_UNSUPPORTED_VISIBILITY: return "unsupported_visibility";
        case MODULE_RESOLVED: return NULL;
    }
    return "not_found";
}

static bool resolve_all_dependencies(scan_ctx_t *contexts, int context_count,
                                     const module_index_t *index) {
    for (int c = 0; c < context_count; c++) {
        for (int i = 0; i < contexts[c].modules.count; i++) {
            module_decl_t *module = &contexts[c].modules.items[i];
            for (int d = 0; d < module->dep_count; d++) {
                dep_decl_t *dep = &module->deps[d];
                module_resolution_t result =
                    resolve_module_reference(index, module, dep->name);
                dep->candidate_count = result.candidate_count;
                dep->resolution = strdup(result.tier ? result.tier : "not_found");
                if (!dep->resolution) return false;
                if (result.module && result.module->namespace_path) {
                    dep->target_namespace = strdup(result.module->namespace_path);
                    if (!dep->target_namespace) return false;
                }
                const char *failure = resolution_failure(result.status);
                if (failure) {
                    dep->failure_reason = strdup(failure);
                    if (!dep->failure_reason) return false;
                }
                if (result.visibility_rule) {
                    dep->visibility_rule = strdup(result.visibility_rule);
                    if (!dep->visibility_rule) return false;
                }
                if (result.status == MODULE_RESOLVED) {
                    char id[65];
                    module_id(result.ctx->repo, result.module, id);
                    dep->resolved_target_id = strdup(id);
                    if (!dep->resolved_target_id) return false;
                }
            }
        }
    }
    return true;
}

static product_decl_t *find_product_by_path(scan_ctx_t *contexts, int context_count,
                                            const char *workspace_path) {
    for (int c = 0; c < context_count; c++) {
        for (int i = 0; i < contexts[c].products.count; i++) {
            product_decl_t *product = &contexts[c].products.items[i];
            if (strcmp(product->workspace_path, workspace_path) == 0) return product;
        }
    }
    return NULL;
}

static bool product_copy_value_if_missing(char **target, const char *source) {
    if (*target || !source || !source[0]) return true;
    *target = strdup(source);
    return *target != NULL;
}

static bool expand_product_inheritance(scan_ctx_t *contexts, int context_count,
                                       product_decl_t *product,
                                       const cbm_aosp_workspace_t *workspace) {
    if (product->expansion_state == 2) return true;
    if (product->expansion_state == 1) {
        product->inheritance_cycle = true;
        return true;
    }
    product->expansion_state = 1;
    for (int i = 0; i < product->inherit_count; i++) {
        product_inherit_decl_t *inherit = &product->inherits[i];
        if (strcmp(inherit->status, "unsupported_expression") == 0) continue;
        product_decl_t *target = find_product_by_path(contexts, context_count, inherit->path);
        free(inherit->status);
        inherit->status = NULL;
        if (!target) {
            inherit->status = strdup(inherit->optional ? "optional_missing" : "not_found");
            if (!inherit->status) return false;
            continue;
        }
        if (target->expansion_state == 1) {
            inherit->status = strdup("cycle");
            product->inheritance_cycle = true;
            target->inheritance_cycle = true;
            if (!inherit->status) return false;
            continue;
        }
        if (!expand_product_inheritance(contexts, context_count, target, workspace)) return false;
        inherit->status = strdup("resolved");
        char target_id[65];
        product_id(workspace, target, target_id);
        inherit->resolved_product_id = strdup(target_id);
        if (!inherit->status || !inherit->resolved_product_id ||
            !product_copy_value_if_missing(&product->device, target->device) ||
            !product_copy_value_if_missing(&product->brand, target->brand) ||
            !product_copy_value_if_missing(&product->model, target->model) ||
            !product_copy_value_if_missing(&product->manufacturer, target->manufacturer)) {
            return false;
        }
        for (int p = 0; p < target->package_count; p++) {
            product_package_decl_t *package = &target->packages[p];
            if (!product_add_package(product, package->name, package->partition,
                                     package->source_variable, package->source_file,
                                     package->included, package, target->workspace_path)) {
                return false;
            }
        }
        for (int p = 0; p < target->partitions.count; p++) {
            if (!str_vec_add_unique(&product->partitions, target->partitions.items[p])) return false;
        }
        if (target->inheritance_cycle) product->inheritance_cycle = true;
    }
    product->expansion_state = 2;
    return true;
}

static bool expand_all_products(scan_ctx_t *contexts, int context_count,
                                const cbm_aosp_workspace_t *workspace) {
    for (int c = 0; c < context_count; c++) {
        for (int i = 0; i < contexts[c].products.count; i++) {
            if (!expand_product_inheritance(contexts, context_count,
                                            &contexts[c].products.items[i], workspace)) {
                return false;
            }
        }
    }
    return true;
}

static bool resolve_product_packages(scan_ctx_t *contexts, int context_count,
                                     const module_index_t *index) {
    for (int c = 0; c < context_count; c++) {
        for (int i = 0; i < contexts[c].products.count; i++) {
            product_decl_t *product = &contexts[c].products.items[i];
            for (int p = 0; p < product->package_count; p++) {
                product_package_decl_t *package = &product->packages[p];
                if (!package->included) {
                    package->resolution = strdup("removed");
                    if (!package->resolution) return false;
                    continue;
                }
                module_index_item_t *match = NULL;
                int candidates = 0;
                for (int m = 0; m < index->count; m++) {
                    if (strcmp(index->items[m].name, package->name) == 0) {
                        match = &index->items[m];
                        candidates++;
                    }
                }
                if (candidates == 1) {
                    char id[65];
                    module_id(match->ctx->repo, match->module, id);
                    package->resolved_target_id = strdup(id);
                    package->resolution = strdup("unique_module");
                } else {
                    package->resolution = strdup(candidates ? "ambiguous" : "not_found");
                }
                if (!package->resolution || (candidates == 1 && !package->resolved_target_id)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static int collect_modules_by_name(const module_index_t *index, const char *name,
                                   module_index_item_t **first) {
    int count = 0;
    *first = NULL;
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->items[i].name, name) == 0) {
            if (count == 0) *first = &index->items[i];
            count++;
        }
    }
    return count;
}

static int collect_bazel_targets(scan_ctx_t *contexts, int context_count,
                                 const char *label, const char *configuration,
                                 scan_ctx_t **first_ctx, bazel_target_decl_t **first_target) {
    int count = 0;
    *first_ctx = NULL;
    *first_target = NULL;
    for (int c = 0; c < context_count; c++) {
        for (int a = 0; a < contexts[c].bazel_artifacts.count; a++) {
            bazel_artifact_decl_t *artifact = &contexts[c].bazel_artifacts.items[a];
            for (int t = 0; t < artifact->target_count; t++) {
                bazel_target_decl_t *target = &artifact->targets[t];
                if (strcmp(target->label, label) != 0 ||
                    (configuration && configuration[0] &&
                     strcmp(target->configuration, configuration) != 0)) {
                    continue;
                }
                if (count == 0) {
                    *first_ctx = &contexts[c];
                    *first_target = target;
                }
                count++;
            }
        }
    }
    return count;
}

static bool set_bazel_status(char **target, const char *status) {
    free(*target);
    *target = strdup(status);
    return *target != NULL;
}

static bool resolve_bazel_metadata(scan_ctx_t *contexts, int context_count,
                                   const module_index_t *index) {
    for (int c = 0; c < context_count; c++) {
        for (int a = 0; a < contexts[c].bazel_artifacts.count; a++) {
            bazel_artifact_decl_t *artifact = &contexts[c].bazel_artifacts.items[a];
            for (int t = 0; t < artifact->target_count; t++) {
                bazel_target_decl_t *target = &artifact->targets[t];
                if (target->label[0] == '@') {
                    target->candidate_count = 0;
                    if (!set_bazel_status(&target->status, "external_repository") ||
                        !str_vec_add_unique(&target->coverage_gaps,
                                            "external_repository_target")) {
                        return false;
                    }
                    continue;
                }
                module_index_item_t *module = NULL;
                target->candidate_count = collect_modules_by_name(index, target->module_name,
                                                                   &module);
                const char *status = target->candidate_count == 1 ? "resolved" :
                    (target->candidate_count > 1 ? "module_ambiguous" : "module_not_found");
                if (!set_bazel_status(&target->status, status)) return false;
                if (target->candidate_count == 1) {
                    char id[65];
                    module_id(module->ctx->repo, module->module, id);
                    target->module_id = strdup(id);
                    if (!target->module_id) return false;
                }
            }
        }
    }
    for (int c = 0; c < context_count; c++) {
        for (int a = 0; a < contexts[c].bazel_artifacts.count; a++) {
            bazel_artifact_decl_t *artifact = &contexts[c].bazel_artifacts.items[a];
            for (int t = 0; t < artifact->target_count; t++) {
                bazel_target_decl_t *source = &artifact->targets[t];
                for (int d = 0; d < source->dependency_count; d++) {
                    bazel_dependency_decl_t *dependency = &source->dependencies[d];
                    dependency->source_module_id = source->module_id
                        ? strdup(source->module_id) : NULL;
                    if (source->module_id && !dependency->source_module_id) return false;
                    if (dependency->label[0] == '@') {
                        if (!set_bazel_status(&dependency->status, "external_repository") ||
                            !str_vec_add_unique(&source->coverage_gaps,
                                                "external_repository_dependency")) {
                            return false;
                        }
                        continue;
                    }
                    if (!source->module_id) {
                        const char *status = source->candidate_count > 1
                            ? "source_module_ambiguous" : "source_module_not_found";
                        if (!set_bazel_status(&dependency->status, status)) return false;
                        continue;
                    }
                    const char *configuration = dependency->configuration[0]
                        ? dependency->configuration : source->configuration;
                    scan_ctx_t *target_ctx = NULL;
                    bazel_target_decl_t *target = NULL;
                    dependency->candidate_count = collect_bazel_targets(
                        contexts, context_count, dependency->label, configuration,
                        &target_ctx, &target);
                    (void)target_ctx;
                    if (dependency->candidate_count == 0) {
                        if (!set_bazel_status(&dependency->status, "label_not_found")) return false;
                    } else if (dependency->candidate_count > 1) {
                        if (!set_bazel_status(&dependency->status, "label_ambiguous")) return false;
                    } else if (!target->module_id) {
                        const char *status = target->candidate_count > 1
                            ? "target_module_ambiguous" : "target_module_not_found";
                        if (!set_bazel_status(&dependency->status, status)) return false;
                    } else {
                        dependency->target_module_id = strdup(target->module_id);
                        if (!dependency->target_module_id ||
                            !set_bazel_status(&dependency->status, "resolved")) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

static int scan_tree(scan_ctx_t *ctx, const char *abs_dir, const char *rel_dir, int depth) {
    if (depth > BG_MAX_WALK_DEPTH) {
        bg_error(ctx->err, ctx->err_size, "AOSP build scan depth exceeded", rel_dir);
        return -1;
    }
    cbm_dir_t *dir = cbm_opendir(abs_dir);
    if (!dir) return 0;
    cbm_dirent_t *entry;
    int rc = 0;
    while (rc == 0 && (entry = cbm_readdir(dir)) != NULL) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) continue;
        char abs_path[BG_PATH_MAX];
        char rel_path[BG_PATH_MAX];
        (void)snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, entry->name);
        (void)snprintf(rel_path, sizeof(rel_path), "%s%s%s", rel_dir, rel_dir[0] ? "/" : "",
                       entry->name);
        if (entry->is_dir) {
            if (!skip_dir(entry->name) && !is_nested_repo_boundary(ctx, abs_path)) {
                rc = scan_tree(ctx, abs_path, rel_path, depth + 1);
            }
            continue;
        }
        bool is_bp = strcmp(entry->name, "Android.bp") == 0;
        bool is_mk = strcmp(entry->name, "Android.mk") == 0;
        size_t name_len = strlen(entry->name);
        bool is_any_mk = name_len > 3 && strcmp(entry->name + name_len - 3, ".mk") == 0;
        bool is_board = is_any_mk && strncmp(entry->name, "BoardConfig", 11) == 0;
        bool is_aidl = name_len > 5 && strcmp(entry->name + name_len - 5, ".aidl") == 0;
        bool is_bazel_metadata = strcmp(entry->name, "aosp_bazel_mixed_build.json") == 0;
        if (!is_bp && !is_any_mk && !is_aidl && !is_bazel_metadata) continue;
        size_t length = 0;
        char *source = bg_read_file(abs_path, &length);
        if (!source) {
            bg_error(ctx->err, ctx->err_size, "cannot read AOSP build file", abs_path);
            rc = -1;
            continue;
        }
        bool is_product = is_any_mk && !is_mk && !is_board &&
                          (strstr(source, "PRODUCT_") != NULL ||
                           strstr(source, "inherit-product") != NULL);
        if (is_any_mk && !is_mk && !is_board && !is_product) {
            free(source);
            continue;
        }
        bool ok;
        if (is_bazel_metadata) {
            ctx->stats.bazel_metadata_files++;
            ok = parse_bazel_metadata(ctx, source, length, rel_path);
        } else if (is_bp) {
            ctx->stats.blueprint_files++;
            ok = parse_blueprint(ctx, source, length, rel_path);
        } else if (is_mk) {
            ctx->stats.make_files++;
            ok = parse_android_mk(ctx, source, rel_path);
        } else if (is_board) {
            ctx->stats.board_config_files++;
            ok = parse_board_config(ctx, source, rel_path);
        } else if (is_product) {
            ctx->stats.product_make_files++;
            ok = parse_product_make(ctx, source, rel_path);
        } else {
            ctx->stats.aidl_files++;
            ok = parse_aidl(source, length, rel_path, &ctx->modules);
        }
        free(source);
        if (!ok) {
            bg_error(ctx->err, ctx->err_size, "cannot parse AOSP build file", abs_path);
            rc = -1;
        }
    }
    cbm_closedir(dir);
    return rc;
}

static void module_id(const cbm_aosp_repo_t *repo, const module_decl_t *module, char out[65]) {
    cbm_sha256_ctx ctx;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_init(&ctx);
    cbm_sha256_update(&ctx, repo->repo_id, strlen(repo->repo_id));
    cbm_sha256_update(&ctx, "\0", 1);
    cbm_sha256_update(&ctx, module->file_path, strlen(module->file_path));
    cbm_sha256_update(&ctx, "\0", 1);
    cbm_sha256_update(&ctx, module->type, strlen(module->type));
    cbm_sha256_update(&ctx, "\0", 1);
    cbm_sha256_update(&ctx, module->name, strlen(module->name));
    cbm_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}

static void product_id(const cbm_aosp_workspace_t *workspace,
                       const product_decl_t *product, char out[65]) {
    cbm_sha256_ctx ctx;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_init(&ctx);
    cbm_sha256_update(&ctx, workspace->workspace_id, strlen(workspace->workspace_id));
    cbm_sha256_update(&ctx, "\0product\0", 9);
    cbm_sha256_update(&ctx, product->workspace_path, strlen(product->workspace_path));
    cbm_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}

static char *module_properties_json(const module_decl_t *module) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (module->defaults_cycle) {
        yyjson_mut_obj_add_strcpy(doc, root, "defaults_cycle", module->defaults_cycle);
    }
    if (module->compile_multilib) {
        yyjson_mut_obj_add_strcpy(doc, root, "compile_multilib", module->compile_multilib);
    }
    if (module->filegroup_path) {
        yyjson_mut_obj_add_strcpy(doc, root, "filegroup_path", module->filegroup_path);
    }
    if (module->generator_command) {
        yyjson_mut_obj_add_strcpy(doc, root, "generator_command", module->generator_command);
    }
    if (module->make_build_rule) {
        yyjson_mut_obj_add_strcpy(doc, root, "make_build_rule", module->make_build_rule);
    }
    if (module->make_module_class) {
        yyjson_mut_obj_add_strcpy(doc, root, "make_module_class", module->make_module_class);
    }
    yyjson_mut_obj_add_strcpy(doc, root, "package", module->package_path ? module->package_path : "");
    yyjson_mut_obj_add_strcpy(doc, root, "namespace",
                             module->namespace_path ? module->namespace_path : "");
    yyjson_mut_val *imports = yyjson_mut_arr(doc);
    for (int i = 0; i < module->namespace_imports.count; i++) {
        yyjson_mut_arr_add_strcpy(doc, imports, module->namespace_imports.items[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "namespace_imports", imports);
    yyjson_mut_obj_add_strcpy(doc, root, "visibility_origin",
                             module->visibility_origin ? module->visibility_origin : "");
    yyjson_mut_obj_add_strcpy(doc, root, "visibility_source_package",
                             module->visibility_source_package
                                 ? module->visibility_source_package
                                 : "");
    yyjson_mut_val *visibility = yyjson_mut_arr(doc);
    for (int i = 0; i < module->effective_visibility.count; i++) {
        yyjson_mut_arr_add_strcpy(doc, visibility, module->effective_visibility.items[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "visibility", visibility);
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *dependency_properties_json(const dep_decl_t *dep) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (dep->inherited_from) {
        yyjson_mut_obj_add_strcpy(doc, root, "origin", "defaults");
        yyjson_mut_obj_add_strcpy(doc, root, "inherited_from", dep->inherited_from);
        yyjson_mut_obj_add_strcpy(doc, root, "inheritance_path", dep->inheritance_path);
        yyjson_mut_obj_add_int(doc, root, "inheritance_depth", dep->inheritance_depth);
    } else {
        yyjson_mut_obj_add_strcpy(doc, root, "origin", "direct");
    }
    yyjson_mut_obj_add_bool(doc, root, "unconditional", dep->unconditional);
    yyjson_mut_val *variants = yyjson_mut_arr(doc);
    for (int i = 0; i < dep->variants.count; i++) {
        yyjson_mut_arr_add_strcpy(doc, variants, dep->variants.items[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "variants", variants);
    yyjson_mut_obj_add_strcpy(doc, root, "resolution",
                             dep->resolution ? dep->resolution : "not_found");
    yyjson_mut_obj_add_int(doc, root, "candidate_count", dep->candidate_count);
    if (dep->target_namespace) {
        yyjson_mut_obj_add_strcpy(doc, root, "target_namespace", dep->target_namespace);
    }
    if (dep->failure_reason) {
        yyjson_mut_obj_add_strcpy(doc, root, "failure_reason", dep->failure_reason);
    }
    if (dep->visibility_rule) {
        yyjson_mut_obj_add_strcpy(doc, root, "visibility_rule", dep->visibility_rule);
    }
    yyjson_mut_val *references = yyjson_mut_arr(doc);
    for (int i = 0; i < dep->declared_references.count; i++) {
        yyjson_mut_arr_add_strcpy(doc, references, dep->declared_references.items[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "declared_references", references);
    yyjson_mut_val *output_tags = yyjson_mut_arr(doc);
    for (int i = 0; i < dep->output_tags.count; i++) {
        yyjson_mut_arr_add_strcpy(doc, output_tags, dep->output_tags.items[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "output_tags", output_tags);
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *file_properties_json(const file_decl_t *file) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (file->inherited_from) {
        yyjson_mut_obj_add_strcpy(doc, root, "origin", "defaults");
        yyjson_mut_obj_add_strcpy(doc, root, "inherited_from", file->inherited_from);
        yyjson_mut_obj_add_strcpy(doc, root, "inheritance_path", file->inheritance_path);
        yyjson_mut_obj_add_int(doc, root, "inheritance_depth", file->inheritance_depth);
    } else {
        yyjson_mut_obj_add_strcpy(doc, root, "origin", "direct");
    }
    yyjson_mut_obj_add_bool(doc, root, "unconditional", file->unconditional);
    yyjson_mut_val *variants = yyjson_mut_arr(doc);
    for (int i = 0; i < file->variants.count; i++) {
        yyjson_mut_arr_add_strcpy(doc, variants, file->variants.items[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "variants", variants);
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *string_array_json(const str_vec_t *values) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, array);
    for (int i = 0; values && i < values->count; i++) {
        yyjson_mut_arr_add_strcpy(doc, array, values->items[i]);
    }
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *bazel_target_properties_json(const bazel_target_decl_t *target) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, object);
    yyjson_mut_obj_add_int(doc, object, "candidate_count", target->candidate_count);
    yyjson_mut_val *gaps = yyjson_mut_arr(doc);
    for (int i = 0; i < target->coverage_gaps.count; i++) {
        yyjson_mut_arr_add_strcpy(doc, gaps, target->coverage_gaps.items[i]);
    }
    yyjson_mut_obj_add_val(doc, object, "coverage_gaps", gaps);
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *bazel_dependency_properties_json(const bazel_target_decl_t *source,
                                              const bazel_dependency_decl_t *dependency) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, object);
    yyjson_mut_obj_add_strcpy(doc, object, "origin", "bazel_mixed_build");
    yyjson_mut_obj_add_strcpy(doc, object, "source_label", source->label);
    yyjson_mut_obj_add_strcpy(doc, object, "source_configuration", source->configuration);
    yyjson_mut_obj_add_strcpy(doc, object, "target_label", dependency->label);
    yyjson_mut_obj_add_strcpy(doc, object, "target_configuration",
                             dependency->configuration);
    yyjson_mut_obj_add_strcpy(doc, object, "transition", dependency->transition);
    yyjson_mut_obj_add_strcpy(doc, object, "resolution",
                             dependency->status ? dependency->status : "label_not_found");
    yyjson_mut_obj_add_int(doc, object, "candidate_count", dependency->candidate_count);
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *name_value_object_json(const name_value_t *values, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, object);
    for (int i = 0; i < count; i++) {
        yyjson_mut_obj_add_strcpy(doc, object, values[i].name, values[i].value);
    }
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *product_properties_json(const product_decl_t *product) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, object);
    yyjson_mut_obj_add_bool(doc, object, "inheritance_cycle", product->inheritance_cycle);
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *product_package_properties_json(const product_package_decl_t *package) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, object);
    yyjson_mut_obj_add_strcpy(doc, object, "resolution",
                             package->resolution ? package->resolution : "not_found");
    if (package->inherited_from) {
        yyjson_mut_obj_add_strcpy(doc, object, "origin", "inherit-product");
        yyjson_mut_obj_add_strcpy(doc, object, "inherited_from", package->inherited_from);
        yyjson_mut_obj_add_strcpy(doc, object, "inheritance_path",
                                 package->inheritance_path ? package->inheritance_path : "");
        yyjson_mut_obj_add_int(doc, object, "inheritance_depth",
                               package->inheritance_depth);
    } else {
        yyjson_mut_obj_add_strcpy(doc, object, "origin", "direct");
    }
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static int persist_build_graph(const cbm_aosp_workspace_t *workspace, scan_ctx_t *contexts,
                               int context_count, char *err, size_t err_size) {
    char path[BG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        bg_error(err, err_size, "cannot open AOSP master database", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 10000);
    char *sql_err = NULL;
    sqlite3_stmt *reset = NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_stmt *insert_module = NULL;
    sqlite3_stmt *insert_dep = NULL;
    sqlite3_stmt *insert_file = NULL;
    sqlite3_stmt *insert_namespace = NULL;
    sqlite3_stmt *insert_package = NULL;
    sqlite3_stmt *insert_make = NULL;
    sqlite3_stmt *insert_product = NULL;
    sqlite3_stmt *insert_product_inherit = NULL;
    sqlite3_stmt *insert_product_package = NULL;
    sqlite3_stmt *insert_board = NULL;
    sqlite3_stmt *insert_bazel_artifact = NULL;
    sqlite3_stmt *insert_bazel_target = NULL;
    sqlite3_stmt *insert_bazel_dependency = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &sql_err) != SQLITE_OK) goto fail;
    const char *reset_sql =
        "DELETE FROM module_edges WHERE source_id IN "
        "(SELECT module_id FROM modules WHERE workspace_id=?1) OR target_id IN "
        "(SELECT module_id FROM modules WHERE workspace_id=?1);";
    if (sqlite3_prepare_v2(db, reset_sql, -1, &reset, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(reset, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(reset) != SQLITE_DONE) goto fail;
    sqlite3_finalize(reset);
    reset = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM module_files WHERE source_id IN "
            "(SELECT module_id FROM modules WHERE workspace_id=?1);", -1, &stmt, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM module_dependencies WHERE source_id IN "
            "(SELECT module_id FROM modules WHERE workspace_id=?1);", -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM modules WHERE workspace_id=?1;", -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM build_make_files WHERE workspace_id=?1;", -1, &stmt, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM build_namespaces WHERE workspace_id=?1;", -1, &stmt, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM build_packages WHERE workspace_id=?1;", -1, &stmt, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM build_bazel_dependencies WHERE workspace_id=?1;",
            -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM build_bazel_targets WHERE workspace_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM build_bazel_artifacts WHERE workspace_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM build_product_packages WHERE product_id IN "
            "(SELECT product_id FROM build_products WHERE workspace_id=?1);",
            -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM build_product_inheritance WHERE source_product_id IN "
            "(SELECT product_id FROM build_products WHERE workspace_id=?1);",
            -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM build_products WHERE workspace_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM build_board_configs WHERE workspace_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO modules(module_id,workspace_id,repo_id,name,module_type,file_path,properties)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7);", -1, &insert_module, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO module_dependencies(source_id,target_name,type,target_id,resolved,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1, &insert_dep, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO module_files(source_id,path,role,properties) "
            "VALUES(?1,?2,?3,?4);", -1, &insert_file, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_namespaces(workspace_id,namespace_path,repo_id,file_path,imports) "
            "VALUES(?1,?2,?3,?4,?5);", -1, &insert_namespace, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_packages(workspace_id,package_path,repo_id,file_path,default_visibility) "
            "VALUES(?1,?2,?3,?4,?5);", -1, &insert_package, NULL) != SQLITE_OK)
        goto fail_insert;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_make_files("
            "workspace_id,repo_id,file_path,includes,condition_count,macro_count,unsupported_expressions) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);", -1, &insert_make, NULL) != SQLITE_OK)
        goto fail_insert;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_products("
            "product_id,workspace_id,repo_id,name,kind,file_path,workspace_path,device,brand,model,"
            "manufacturer,device_owner,vendor_owner,partitions,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);",
            -1, &insert_product, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_product_inheritance("
            "source_product_id,inherited_path,target_product_id,status,optional,source_file,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,'{}');", -1, &insert_product_inherit, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_product_packages("
            "product_id,module_name,partition_name,module_id,resolved,included,source_variable,source_file,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);", -1, &insert_product_package, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_board_configs("
            "workspace_id,repo_id,file_path,workspace_path,device_owner,vendor_owner,variables,partitions) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);", -1, &insert_board, NULL) != SQLITE_OK)
        goto fail_insert;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_bazel_artifacts("
            "workspace_id,repo_id,file_path,format_version,configuration,target_count,coverage_gaps) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);", -1, &insert_bazel_artifact, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_bazel_targets("
            "workspace_id,repo_id,artifact_path,label,configuration,kind,module_name,module_id,status,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);", -1,
            &insert_bazel_target, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO build_bazel_dependencies("
            "workspace_id,source_repo_id,artifact_path,source_label,source_configuration,"
            "target_label,target_configuration,dependency_type,transition,source_module_id,"
            "target_module_id,status,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);", -1,
            &insert_bazel_dependency, NULL) != SQLITE_OK)
        goto fail_insert;
    for (int c = 0; c < context_count; c++) {
        for (int a = 0; a < contexts[c].bazel_artifacts.count; a++) {
            bazel_artifact_decl_t *artifact = &contexts[c].bazel_artifacts.items[a];
            char *artifact_gaps = string_array_json(&artifact->coverage_gaps);
            if (!artifact_gaps) goto fail_insert;
            sqlite3_reset(insert_bazel_artifact);
            sqlite3_clear_bindings(insert_bazel_artifact);
            sqlite3_bind_text(insert_bazel_artifact, 1, workspace->workspace_id,
                              -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_bazel_artifact, 2, contexts[c].repo->repo_id,
                              -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_bazel_artifact, 3, artifact->file_path,
                              -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_bazel_artifact, 4, artifact->format_version);
            sqlite3_bind_text(insert_bazel_artifact, 5, artifact->configuration,
                              -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_bazel_artifact, 6, artifact->target_count);
            sqlite3_bind_text(insert_bazel_artifact, 7, artifact_gaps,
                              -1, SQLITE_TRANSIENT);
            int artifact_step = sqlite3_step(insert_bazel_artifact);
            free(artifact_gaps);
            if (artifact_step != SQLITE_DONE) goto fail_insert;
            for (int t = 0; t < artifact->target_count; t++) {
                bazel_target_decl_t *target = &artifact->targets[t];
                char *target_properties = bazel_target_properties_json(target);
                if (!target_properties) goto fail_insert;
                sqlite3_reset(insert_bazel_target);
                sqlite3_clear_bindings(insert_bazel_target);
                sqlite3_bind_text(insert_bazel_target, 1, workspace->workspace_id,
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_bazel_target, 2, contexts[c].repo->repo_id,
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_bazel_target, 3, artifact->file_path,
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_bazel_target, 4, target->label,
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_bazel_target, 5, target->configuration,
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_bazel_target, 6, target->kind,
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_bazel_target, 7, target->module_name,
                                  -1, SQLITE_TRANSIENT);
                if (target->module_id) {
                    sqlite3_bind_text(insert_bazel_target, 8, target->module_id,
                                      -1, SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(insert_bazel_target, 8);
                }
                sqlite3_bind_text(insert_bazel_target, 9,
                                  target->status ? target->status : "module_not_found",
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_bazel_target, 10, target_properties,
                                  -1, SQLITE_TRANSIENT);
                int target_step = sqlite3_step(insert_bazel_target);
                free(target_properties);
                if (target_step != SQLITE_DONE) goto fail_insert;
                for (int d = 0; d < target->dependency_count; d++) {
                    bazel_dependency_decl_t *dependency = &target->dependencies[d];
                    char *dependency_properties =
                        bazel_dependency_properties_json(target, dependency);
                    if (!dependency_properties) goto fail_insert;
                    sqlite3_reset(insert_bazel_dependency);
                    sqlite3_clear_bindings(insert_bazel_dependency);
                    sqlite3_bind_text(insert_bazel_dependency, 1, workspace->workspace_id,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 2, contexts[c].repo->repo_id,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 3, artifact->file_path,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 4, target->label,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 5, target->configuration,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 6, dependency->label,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 7, dependency->configuration,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 8, dependency->type,
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 9, dependency->transition,
                                      -1, SQLITE_TRANSIENT);
                    if (dependency->source_module_id) {
                        sqlite3_bind_text(insert_bazel_dependency, 10,
                                          dependency->source_module_id, -1, SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_null(insert_bazel_dependency, 10);
                    }
                    if (dependency->target_module_id) {
                        sqlite3_bind_text(insert_bazel_dependency, 11,
                                          dependency->target_module_id, -1, SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_null(insert_bazel_dependency, 11);
                    }
                    sqlite3_bind_text(insert_bazel_dependency, 12,
                                      dependency->status ? dependency->status : "label_not_found",
                                      -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_bazel_dependency, 13, dependency_properties,
                                      -1, SQLITE_TRANSIENT);
                    int dependency_step = sqlite3_step(insert_bazel_dependency);
                    if (dependency_step == SQLITE_DONE && dependency->source_module_id) {
                        sqlite3_reset(insert_dep);
                        sqlite3_clear_bindings(insert_dep);
                        sqlite3_bind_text(insert_dep, 1, dependency->source_module_id,
                                          -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insert_dep, 2, dependency->label,
                                          -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insert_dep, 3, dependency->type,
                                          -1, SQLITE_TRANSIENT);
                        if (dependency->target_module_id) {
                            sqlite3_bind_text(insert_dep, 4, dependency->target_module_id,
                                              -1, SQLITE_TRANSIENT);
                        } else {
                            sqlite3_bind_null(insert_dep, 4);
                        }
                        sqlite3_bind_int(insert_dep, 5,
                                         dependency->target_module_id != NULL);
                        sqlite3_bind_text(insert_dep, 6, dependency_properties,
                                          -1, SQLITE_TRANSIENT);
                        dependency_step = sqlite3_step(insert_dep);
                    }
                    free(dependency_properties);
                    if (dependency_step != SQLITE_DONE) goto fail_insert;
                }
            }
        }
        for (int i = 0; i < contexts[c].make_files.count; i++) {
            make_file_decl_t *decl = &contexts[c].make_files.items[i];
            char *includes = string_array_json(&decl->includes);
            char *unsupported = string_array_json(&decl->unsupported);
            if (!includes || !unsupported) {
                free(includes);
                free(unsupported);
                goto fail_insert;
            }
            sqlite3_reset(insert_make);
            sqlite3_clear_bindings(insert_make);
            sqlite3_bind_text(insert_make, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_make, 2, contexts[c].repo->repo_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_make, 3, decl->file_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_make, 4, includes, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_make, 5, decl->condition_count);
            sqlite3_bind_int(insert_make, 6, decl->macro_count);
            sqlite3_bind_text(insert_make, 7, unsupported, -1, SQLITE_TRANSIENT);
            int make_step = sqlite3_step(insert_make);
            free(includes);
            free(unsupported);
            if (make_step != SQLITE_DONE) goto fail_insert;
        }
        for (int i = 0; i < contexts[c].namespaces.count; i++) {
            scope_decl_t *decl = &contexts[c].namespaces.items[i];
            char *imports = string_array_json(&decl->values);
            if (!imports) goto fail_insert;
            sqlite3_reset(insert_namespace);
            sqlite3_clear_bindings(insert_namespace);
            sqlite3_bind_text(insert_namespace, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_namespace, 2, decl->path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_namespace, 3, contexts[c].repo->repo_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_namespace, 4, decl->file_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_namespace, 5, imports, -1, SQLITE_TRANSIENT);
            int namespace_step = sqlite3_step(insert_namespace);
            free(imports);
            if (namespace_step != SQLITE_DONE) goto fail_insert;
        }
        for (int i = 0; i < contexts[c].packages.count; i++) {
            scope_decl_t *decl = &contexts[c].packages.items[i];
            char *visibility = string_array_json(&decl->values);
            if (!visibility) goto fail_insert;
            sqlite3_reset(insert_package);
            sqlite3_clear_bindings(insert_package);
            sqlite3_bind_text(insert_package, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_package, 2, decl->path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_package, 3, contexts[c].repo->repo_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_package, 4, decl->file_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_package, 5, visibility, -1, SQLITE_TRANSIENT);
            int package_step = sqlite3_step(insert_package);
            free(visibility);
            if (package_step != SQLITE_DONE) goto fail_insert;
        }
        for (int i = 0; i < contexts[c].products.count; i++) {
            product_decl_t *product = &contexts[c].products.items[i];
            char id[65];
            product_id(workspace, product, id);
            char *partitions = string_array_json(&product->partitions);
            char *properties = product_properties_json(product);
            if (!partitions || !properties) {
                free(partitions);
                free(properties);
                goto fail_insert;
            }
            sqlite3_reset(insert_product);
            sqlite3_clear_bindings(insert_product);
            sqlite3_bind_text(insert_product, 1, id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 3, contexts[c].repo->repo_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 4, product->name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 5, product->kind, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 6, product->file_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 7, product->workspace_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 8, product->device ? product->device : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 9, product->brand ? product->brand : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 10, product->model ? product->model : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 11,
                              product->manufacturer ? product->manufacturer : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 12,
                              product->device_owner ? product->device_owner : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 13,
                              product->vendor_owner ? product->vendor_owner : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 14, partitions, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_product, 15, properties, -1, SQLITE_TRANSIENT);
            int product_step = sqlite3_step(insert_product);
            free(partitions);
            free(properties);
            if (product_step != SQLITE_DONE) goto fail_insert;
            for (int h = 0; h < product->inherit_count; h++) {
                product_inherit_decl_t *inherit = &product->inherits[h];
                sqlite3_reset(insert_product_inherit);
                sqlite3_clear_bindings(insert_product_inherit);
                sqlite3_bind_text(insert_product_inherit, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_product_inherit, 2, inherit->path, -1, SQLITE_TRANSIENT);
                if (inherit->resolved_product_id) {
                    sqlite3_bind_text(insert_product_inherit, 3, inherit->resolved_product_id,
                                      -1, SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(insert_product_inherit, 3);
                }
                sqlite3_bind_text(insert_product_inherit, 4, inherit->status, -1,
                                  SQLITE_TRANSIENT);
                sqlite3_bind_int(insert_product_inherit, 5, inherit->optional);
                sqlite3_bind_text(insert_product_inherit, 6, inherit->source_file, -1,
                                  SQLITE_TRANSIENT);
                if (sqlite3_step(insert_product_inherit) != SQLITE_DONE) goto fail_insert;
            }
            for (int p = 0; p < product->package_count; p++) {
                product_package_decl_t *package = &product->packages[p];
                char *package_properties = product_package_properties_json(package);
                if (!package_properties) goto fail_insert;
                sqlite3_reset(insert_product_package);
                sqlite3_clear_bindings(insert_product_package);
                sqlite3_bind_text(insert_product_package, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_product_package, 2, package->name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_product_package, 3, package->partition, -1,
                                  SQLITE_TRANSIENT);
                if (package->resolved_target_id) {
                    sqlite3_bind_text(insert_product_package, 4, package->resolved_target_id,
                                      -1, SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(insert_product_package, 4);
                }
                sqlite3_bind_int(insert_product_package, 5,
                                 package->resolved_target_id != NULL);
                sqlite3_bind_int(insert_product_package, 6, package->included);
                sqlite3_bind_text(insert_product_package, 7, package->source_variable, -1,
                                  SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_product_package, 8, package->source_file, -1,
                                  SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_product_package, 9, package_properties, -1,
                                  SQLITE_TRANSIENT);
                int package_step = sqlite3_step(insert_product_package);
                free(package_properties);
                if (package_step != SQLITE_DONE) goto fail_insert;
            }
        }
        for (int i = 0; i < contexts[c].board_configs.count; i++) {
            board_config_decl_t *board = &contexts[c].board_configs.items[i];
            char *variables = name_value_object_json(board->variables, board->variable_count);
            char *partitions = string_array_json(&board->partitions);
            if (!variables || !partitions) {
                free(variables);
                free(partitions);
                goto fail_insert;
            }
            sqlite3_reset(insert_board);
            sqlite3_clear_bindings(insert_board);
            sqlite3_bind_text(insert_board, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_board, 2, contexts[c].repo->repo_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_board, 3, board->file_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_board, 4, board->workspace_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_board, 5,
                              board->device_owner ? board->device_owner : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_board, 6,
                              board->vendor_owner ? board->vendor_owner : "", -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_board, 7, variables, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_board, 8, partitions, -1, SQLITE_TRANSIENT);
            int board_step = sqlite3_step(insert_board);
            free(variables);
            free(partitions);
            if (board_step != SQLITE_DONE) goto fail_insert;
        }
        for (int i = 0; i < contexts[c].modules.count; i++) {
            module_decl_t *module = &contexts[c].modules.items[i];
            char id[65];
            module_id(contexts[c].repo, module, id);
            sqlite3_reset(insert_module);
            sqlite3_clear_bindings(insert_module);
            sqlite3_bind_text(insert_module, 1, id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_module, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_module, 3, contexts[c].repo->repo_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_module, 4, module->name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_module, 5, module->type, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_module, 6, module->file_path, -1, SQLITE_TRANSIENT);
            char *module_properties = module_properties_json(module);
            if (!module_properties) goto fail_insert;
            sqlite3_bind_text(insert_module, 7, module_properties, -1, SQLITE_TRANSIENT);
            int module_step = sqlite3_step(insert_module);
            free(module_properties);
            if (module_step != SQLITE_DONE) goto fail_insert;
            for (int f = 0; f < module->file_count; f++) {
                sqlite3_reset(insert_file);
                sqlite3_clear_bindings(insert_file);
                sqlite3_bind_text(insert_file, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_file, 2, module->files[f].path,
                                  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_file, 3, module->files[f].role,
                                  -1, SQLITE_TRANSIENT);
                char *file_properties = file_properties_json(&module->files[f]);
                if (!file_properties) goto fail_insert;
                sqlite3_bind_text(insert_file, 4, file_properties, -1, SQLITE_TRANSIENT);
                int file_step = sqlite3_step(insert_file);
                free(file_properties);
                if (file_step != SQLITE_DONE) goto fail_insert;
            }
            for (int d = 0; d < module->dep_count; d++) {
                sqlite3_reset(insert_dep);
                sqlite3_clear_bindings(insert_dep);
                sqlite3_bind_text(insert_dep, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_dep, 2, module->deps[d].name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_dep, 3, module->deps[d].kind, -1, SQLITE_TRANSIENT);
                if (module->deps[d].resolved_target_id) {
                    sqlite3_bind_text(insert_dep, 4, module->deps[d].resolved_target_id,
                                      -1, SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(insert_dep, 4);
                }
                sqlite3_bind_int(insert_dep, 5,
                                 module->deps[d].resolved_target_id != NULL);
                char *dep_properties = dependency_properties_json(&module->deps[d]);
                if (!dep_properties) goto fail_insert;
                sqlite3_bind_text(insert_dep, 6, dep_properties, -1, SQLITE_TRANSIENT);
                int dep_step = sqlite3_step(insert_dep);
                free(dep_properties);
                if (dep_step != SQLITE_DONE) goto fail_insert;
            }
        }
    }
    sqlite3_finalize(insert_module);
    sqlite3_finalize(insert_dep);
    sqlite3_finalize(insert_file);
    sqlite3_finalize(insert_namespace);
    sqlite3_finalize(insert_package);
    sqlite3_finalize(insert_make);
    sqlite3_finalize(insert_product);
    sqlite3_finalize(insert_product_inherit);
    sqlite3_finalize(insert_product_package);
    sqlite3_finalize(insert_board);
    sqlite3_finalize(insert_bazel_artifact);
    sqlite3_finalize(insert_bazel_target);
    sqlite3_finalize(insert_bazel_dependency);
    insert_module = NULL;
    insert_dep = NULL;
    insert_file = NULL;
    insert_namespace = NULL;
    insert_package = NULL;
    insert_make = NULL;
    insert_product = NULL;
    insert_product_inherit = NULL;
    insert_product_package = NULL;
    insert_board = NULL;
    insert_bazel_artifact = NULL;
    insert_bazel_target = NULL;
    insert_bazel_dependency = NULL;

    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO module_edges(source_id,target_id,type,properties) "
            "SELECT source_id,target_id,type,properties FROM module_dependencies WHERE target_id IS NOT NULL "
            "AND source_id IN (SELECT module_id FROM modules WHERE workspace_id=?1);",
            -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &sql_err) != SQLITE_OK) goto fail;
    sqlite3_close(db);
    return 0;

fail_insert:
    sqlite3_finalize(insert_module);
    sqlite3_finalize(insert_dep);
    sqlite3_finalize(insert_file);
    sqlite3_finalize(insert_namespace);
    sqlite3_finalize(insert_package);
    sqlite3_finalize(insert_make);
    sqlite3_finalize(insert_product);
    sqlite3_finalize(insert_product_inherit);
    sqlite3_finalize(insert_product_package);
    sqlite3_finalize(insert_board);
    sqlite3_finalize(insert_bazel_artifact);
    sqlite3_finalize(insert_bazel_target);
    sqlite3_finalize(insert_bazel_dependency);
fail:
    sqlite3_finalize(reset);
    sqlite3_finalize(stmt);
    (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    bg_error(err, err_size, "cannot persist AOSP build graph",
             sql_err ? sql_err : sqlite3_errmsg(db));
    sqlite3_free(sql_err);
    sqlite3_close(db);
    return -1;
}

int cbm_aosp_build_stats(const cbm_aosp_workspace_t *workspace, cbm_aosp_build_stats_t *stats,
                         char *err, size_t err_size) {
    if (!workspace || !stats) return -1;
    memset(stats, 0, sizeof(*stats));
    char path[BG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        bg_error(err, err_size, "AOSP workspace is not initialized", path);
        sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT (SELECT count(*) FROM modules WHERE workspace_id=?1),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id WHERE m.workspace_id=?1),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id WHERE m.workspace_id=?1 AND d.resolved=1),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id WHERE m.workspace_id=?1 AND d.resolved=0),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND d.properties LIKE '%\"origin\":\"defaults\"%'),"
        "(SELECT count(*) FROM modules WHERE workspace_id=?1 "
        "AND properties LIKE '%\"defaults_cycle\":%'),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND json_array_length(d.properties,'$.variants')>0),"
        "(SELECT coalesce(sum(json_array_length(d.properties,'$.variants')),0) "
        "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1),"
        "(SELECT count(*) FROM build_namespaces WHERE workspace_id=?1),"
        "(SELECT coalesce(sum(json_array_length(imports)),0) FROM build_namespaces "
        "WHERE workspace_id=?1),"
        "(SELECT count(*) FROM build_packages WHERE workspace_id=?1),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND json_extract(d.properties,'$.failure_reason')='ambiguous'),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND json_extract(d.properties,'$.failure_reason')='visibility_blocked'),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND json_extract(d.properties,'$.failure_reason')='unsupported_visibility'),"
        "(SELECT count(*) FROM modules WHERE workspace_id=?1 AND module_type='filegroup'),"
        "(SELECT count(*) FROM modules WHERE workspace_id=?1 AND module_type='genrule'),"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND d.type IN('GENERATED_SOURCE','GENERATED_HEADER',"
        "'EXPORTED_GENERATED_HEADER')) ,"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND d.type IN('TOOL','TOOL_FILE')) ,"
        "(SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
        "WHERE m.workspace_id=?1 AND json_array_length(d.properties,'$.output_tags')>0),"
        "(SELECT count(*) FROM module_files f JOIN modules m ON m.module_id=f.source_id "
        "WHERE m.workspace_id=?1 AND f.role='SOURCE'),"
        "(SELECT count(*) FROM module_files f JOIN modules m ON m.module_id=f.source_id "
        "WHERE m.workspace_id=?1 AND f.role='OUTPUT'),"
        "(SELECT count(*) FROM module_files f JOIN modules m ON m.module_id=f.source_id "
        "WHERE m.workspace_id=?1 AND f.role='TOOL_FILE'),"
        "(SELECT coalesce(sum(json_array_length(includes)),0) FROM build_make_files "
        "WHERE workspace_id=?1),"
        "(SELECT coalesce(sum(condition_count),0) FROM build_make_files WHERE workspace_id=?1),"
        "(SELECT coalesce(sum(macro_count),0) FROM build_make_files WHERE workspace_id=?1),"
        "(SELECT coalesce(sum(json_array_length(unsupported_expressions)),0) "
        "FROM build_make_files WHERE workspace_id=?1),"
        "(SELECT count(*) FROM build_products WHERE workspace_id=?1 AND kind='product'),"
        "(SELECT count(*) FROM build_products WHERE workspace_id=?1 AND kind='fragment'),"
        "(SELECT count(*) FROM build_product_inheritance i JOIN build_products p "
        "ON p.product_id=i.source_product_id WHERE p.workspace_id=?1),"
        "(SELECT count(*) FROM build_product_inheritance i JOIN build_products p "
        "ON p.product_id=i.source_product_id WHERE p.workspace_id=?1 AND i.status='resolved'),"
        "(SELECT count(*) FROM build_products WHERE workspace_id=?1 "
        "AND json_extract(properties,'$.inheritance_cycle')=1),"
        "(SELECT count(*) FROM build_product_packages pp JOIN build_products p "
        "ON p.product_id=pp.product_id WHERE p.workspace_id=?1 AND p.kind='product' AND pp.included=1),"
        "(SELECT count(*) FROM build_product_packages pp JOIN build_products p "
        "ON p.product_id=pp.product_id WHERE p.workspace_id=?1 AND p.kind='product' "
        "AND pp.included=1 AND pp.resolved=1),"
        "(SELECT count(*) FROM build_product_packages pp JOIN build_products p "
        "ON p.product_id=pp.product_id WHERE p.workspace_id=?1 AND p.kind='product' "
        "AND pp.included=1 AND pp.resolved=0),"
        "(SELECT count(*) FROM build_board_configs WHERE workspace_id=?1),"
        "(SELECT count(DISTINCT value) FROM ("
        "SELECT j.value AS value FROM build_products p,json_each(p.partitions) j "
        "WHERE p.workspace_id=?1 AND p.kind='product' UNION ALL "
        "SELECT j.value AS value FROM build_board_configs b,json_each(b.partitions) j "
        "WHERE b.workspace_id=?1)),"
        "(SELECT count(*) FROM build_bazel_artifacts WHERE workspace_id=?1),"
        "(SELECT count(*) FROM build_bazel_targets WHERE workspace_id=?1),"
        "(SELECT count(*) FROM build_bazel_targets WHERE workspace_id=?1 AND status='resolved'),"
        "(SELECT count(*) FROM build_bazel_dependencies WHERE workspace_id=?1),"
        "(SELECT count(*) FROM build_bazel_dependencies WHERE workspace_id=?1 AND status='resolved'),"
        "((SELECT count(*) FROM build_bazel_targets WHERE workspace_id=?1 AND status LIKE '%ambiguous') + "
        " (SELECT count(*) FROM build_bazel_dependencies WHERE workspace_id=?1 AND status LIKE '%ambiguous')) ,"
        "((SELECT count(*) FROM build_bazel_targets WHERE workspace_id=?1 AND status LIKE '%not_found') + "
        " (SELECT count(*) FROM build_bazel_dependencies WHERE workspace_id=?1 AND status LIKE '%not_found')) ,"
        "((SELECT coalesce(sum(json_array_length(coverage_gaps)),0) FROM build_bazel_artifacts WHERE workspace_id=?1) + "
        " (SELECT coalesce(sum(json_array_length(properties,'$.coverage_gaps')),0) FROM build_bazel_targets WHERE workspace_id=?1) + "
        " (SELECT count(*) FROM build_bazel_targets WHERE workspace_id=?1 AND status!='resolved') + "
        " (SELECT count(*) FROM build_bazel_dependencies WHERE workspace_id=?1 AND status!='resolved'));";
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        bg_error(err, err_size, "cannot read AOSP build graph", sqlite3_errmsg(db));
    } else {
        sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats->module_count = sqlite3_column_int(stmt, 0);
            stats->dependency_count = sqlite3_column_int(stmt, 1);
            stats->resolved_count = sqlite3_column_int(stmt, 2);
            stats->unresolved_count = sqlite3_column_int(stmt, 3);
            stats->inherited_dependency_count = sqlite3_column_int(stmt, 4);
            stats->defaults_cycle_count = sqlite3_column_int(stmt, 5);
            stats->variant_dependency_count = sqlite3_column_int(stmt, 6);
            stats->variant_branch_count = sqlite3_column_int(stmt, 7);
            stats->namespace_count = sqlite3_column_int(stmt, 8);
            stats->namespace_import_count = sqlite3_column_int(stmt, 9);
            stats->package_count = sqlite3_column_int(stmt, 10);
            stats->ambiguous_dependency_count = sqlite3_column_int(stmt, 11);
            stats->visibility_blocked_count = sqlite3_column_int(stmt, 12);
            stats->unsupported_visibility_count = sqlite3_column_int(stmt, 13);
            stats->filegroup_count = sqlite3_column_int(stmt, 14);
            stats->genrule_count = sqlite3_column_int(stmt, 15);
            stats->generated_dependency_count = sqlite3_column_int(stmt, 16);
            stats->tool_dependency_count = sqlite3_column_int(stmt, 17);
            stats->tagged_dependency_count = sqlite3_column_int(stmt, 18);
            stats->source_file_count = sqlite3_column_int(stmt, 19);
            stats->generated_output_count = sqlite3_column_int(stmt, 20);
            stats->tool_file_count = sqlite3_column_int(stmt, 21);
            stats->make_include_count = sqlite3_column_int(stmt, 22);
            stats->make_condition_count = sqlite3_column_int(stmt, 23);
            stats->make_macro_count = sqlite3_column_int(stmt, 24);
            stats->make_unsupported_count = sqlite3_column_int(stmt, 25);
            stats->product_count = sqlite3_column_int(stmt, 26);
            stats->product_fragment_count = sqlite3_column_int(stmt, 27);
            stats->product_inheritance_count = sqlite3_column_int(stmt, 28);
            stats->product_inheritance_resolved_count = sqlite3_column_int(stmt, 29);
            stats->product_inheritance_cycle_count = sqlite3_column_int(stmt, 30);
            stats->product_package_count = sqlite3_column_int(stmt, 31);
            stats->product_package_resolved_count = sqlite3_column_int(stmt, 32);
            stats->product_package_unresolved_count = sqlite3_column_int(stmt, 33);
            stats->board_config_count = sqlite3_column_int(stmt, 34);
            stats->product_partition_count = sqlite3_column_int(stmt, 35);
            stats->bazel_artifact_count = sqlite3_column_int(stmt, 36);
            stats->bazel_target_count = sqlite3_column_int(stmt, 37);
            stats->bazel_target_resolved_count = sqlite3_column_int(stmt, 38);
            stats->bazel_dependency_count = sqlite3_column_int(stmt, 39);
            stats->bazel_dependency_resolved_count = sqlite3_column_int(stmt, 40);
            stats->bazel_ambiguous_count = sqlite3_column_int(stmt, 41);
            stats->bazel_missing_count = sqlite3_column_int(stmt, 42);
            stats->bazel_coverage_gap_count = sqlite3_column_int(stmt, 43);
            rc = 0;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

int cbm_aosp_build_scan(const cbm_aosp_workspace_t *workspace, cbm_aosp_build_stats_t *stats,
                        char *err, size_t err_size) {
    if (!workspace || !stats) return -1;
    if (cbm_aosp_master_sync(workspace, err, err_size) != 0) return -1;
    scan_ctx_t *contexts = calloc((size_t)workspace->repo_count, sizeof(*contexts));
    if (!contexts) return -1;
    int rc = 0;
    cbm_aosp_build_stats_t file_stats = {0};
    module_index_t module_index = {0};
    for (int i = 0; i < workspace->repo_count && rc == 0; i++) {
        contexts[i].workspace = workspace;
        contexts[i].repo = &workspace->repos[i];
        contexts[i].err = err;
        contexts[i].err_size = err_size;
        if (workspace->repos[i].exists) {
            rc = scan_tree(&contexts[i], workspace->repos[i].abs_path, "", 0);
            file_stats.blueprint_files += contexts[i].stats.blueprint_files;
            file_stats.make_files += contexts[i].stats.make_files;
            file_stats.product_make_files += contexts[i].stats.product_make_files;
            file_stats.board_config_files += contexts[i].stats.board_config_files;
            file_stats.bazel_metadata_files += contexts[i].stats.bazel_metadata_files;
            file_stats.aidl_files += contexts[i].stats.aidl_files;
        }
    }
    if (rc == 0 && !finalize_module_scopes(contexts, workspace->repo_count)) {
        bg_error(err, err_size, "cannot finalize AOSP build scopes", "out of memory");
        rc = -1;
    }
    if (rc == 0 && !build_module_index(contexts, workspace->repo_count, &module_index)) {
        bg_error(err, err_size, "cannot index AOSP build modules", "out of memory");
        rc = -1;
    }
    if (rc == 0 && !expand_all_defaults(contexts, workspace->repo_count, &module_index)) {
        bg_error(err, err_size, "cannot expand AOSP defaults", "out of memory");
        rc = -1;
    }
    if (rc == 0 &&
        !resolve_all_dependencies(contexts, workspace->repo_count, &module_index)) {
        bg_error(err, err_size, "cannot resolve AOSP module dependencies", "out of memory");
        rc = -1;
    }
    if (rc == 0 && !expand_all_products(contexts, workspace->repo_count, workspace)) {
        bg_error(err, err_size, "cannot expand AOSP product inheritance", "out of memory");
        rc = -1;
    }
    if (rc == 0 &&
        !resolve_product_packages(contexts, workspace->repo_count, &module_index)) {
        bg_error(err, err_size, "cannot resolve AOSP product packages", "out of memory");
        rc = -1;
    }
    if (rc == 0 &&
        !resolve_bazel_metadata(contexts, workspace->repo_count, &module_index)) {
        bg_error(err, err_size, "cannot resolve AOSP Bazel mixed-build metadata",
                 "out of memory");
        rc = -1;
    }
    if (rc == 0) rc = persist_build_graph(workspace, contexts, workspace->repo_count, err, err_size);
    if (rc == 0) {
        rc = cbm_aosp_build_stats(workspace, stats, err, err_size);
        stats->blueprint_files = file_stats.blueprint_files;
        stats->make_files = file_stats.make_files;
        stats->product_make_files = file_stats.product_make_files;
        stats->board_config_files = file_stats.board_config_files;
        stats->bazel_metadata_files = file_stats.bazel_metadata_files;
        stats->aidl_files = file_stats.aidl_files;
    }
    for (int i = 0; i < workspace->repo_count; i++) {
        module_vec_free(&contexts[i].modules);
        scope_vec_free(&contexts[i].namespaces);
        scope_vec_free(&contexts[i].packages);
        make_file_vec_free(&contexts[i].make_files);
        product_vec_free(&contexts[i].products);
        board_config_vec_free(&contexts[i].board_configs);
        bazel_artifact_vec_free(&contexts[i].bazel_artifacts);
    }
    free(module_index.items);
    free(contexts);
    return rc;
}

void cbm_aosp_modules_free(cbm_aosp_module_t *results, int count) {
    if (!results) return;
    for (int i = 0; i < count; i++) {
        free(results[i].module_id);
        free(results[i].repo_path);
        free(results[i].name);
        free(results[i].module_type);
        free(results[i].file_path);
    }
    free(results);
}

static char *column_dup(sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    return strdup(text ? (const char *)text : "");
}

int cbm_aosp_search_modules(const cbm_aosp_workspace_t *workspace, const char *query, int limit,
                            cbm_aosp_module_t **results, int *count, char *err, size_t err_size) {
    if (!workspace || !results || !count) return -1;
    *results = NULL;
    *count = 0;
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    char path[BG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        bg_error(err, err_size, "AOSP workspace is not initialized", path);
        sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT m.module_id,r.path,m.name,m.module_type,m.file_path,"
        "(SELECT count(*) FROM module_edges e WHERE e.source_id=m.module_id),"
        "(SELECT count(*) FROM module_edges e WHERE e.target_id=m.module_id) "
        "FROM modules m JOIN repos r ON r.repo_id=m.repo_id WHERE m.workspace_id=?1 "
        "AND (?2='' OR m.name LIKE '%'||?2||'%' OR m.module_type LIKE '%'||?2||'%') "
        "ORDER BY m.name,m.module_id LIMIT ?3;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        bg_error(err, err_size, "cannot prepare AOSP module search", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, query ? query : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    cbm_aosp_module_t *items = calloc((size_t)limit, sizeof(*items));
    if (!items) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    int n = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cbm_aosp_module_t *item = &items[n];
        item->module_id = column_dup(stmt, 0);
        item->repo_path = column_dup(stmt, 1);
        item->name = column_dup(stmt, 2);
        item->module_type = column_dup(stmt, 3);
        item->file_path = column_dup(stmt, 4);
        item->outgoing_dependencies = sqlite3_column_int(stmt, 5);
        item->incoming_dependencies = sqlite3_column_int(stmt, 6);
        if (!item->module_id || !item->repo_path || !item->name || !item->module_type ||
            !item->file_path) {
            cbm_aosp_modules_free(items, n + 1);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return -1;
        }
        n++;
    }
    if (step_rc != SQLITE_DONE) {
        bg_error(err, err_size, "cannot search AOSP modules", sqlite3_errmsg(db));
        cbm_aosp_modules_free(items, n);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    *results = items;
    *count = n;
    return 0;
}
