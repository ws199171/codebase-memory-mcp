/* Collect structural cross-repository candidates from indexed AOSP shards. */
#include "aosp/structural_graph.h"

#include "discover/discover.h"
#include "foundation/compat_fs.h"

#include "cbm.h"

#include <sqlite3.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SG_PATH_MAX = 4096,
    SG_REF_MAX = 1024,
    SG_FILE_MAX = 32 * 1024 * 1024,
    SG_PARSE_TIMEOUT_US = 5000000,
};

typedef struct {
    cbm_aosp_cross_edge_candidate_t value;
    char *source_id;
    char *target_id;
    char *target_name;
    char *type;
    char *evidence;
    char *properties;
} owned_candidate_t;

typedef struct {
    owned_candidate_t *items;
    int count;
    int cap;
} candidate_vec_t;

typedef struct {
    char *global_id;
    char *repo_id;
    char *name;
    char *label;
    char *qualified_name;
    char *file_path;
    int score;
} target_match_t;

typedef struct {
    target_match_t *items;
    int count;
    int cap;
} match_vec_t;

typedef struct {
    const cbm_aosp_workspace_t *workspace;
    const cbm_aosp_repo_t *repo;
    sqlite3 *master;
    sqlite3_stmt *source_lookup;
    sqlite3_stmt *target_lookup;
    candidate_vec_t candidates;
    cbm_aosp_structural_stats_t stats;
    char *err;
    size_t err_size;
} collect_ctx_t;

static void sg_error(char *err, size_t err_size, const char *message, const char *detail) {
    if (!err || err_size == 0) return;
    if (detail && detail[0]) (void)snprintf(err, err_size, "%s: %s", message, detail);
    else (void)snprintf(err, err_size, "%s", message);
}

static char *read_source(const char *path, int *length_out) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || length > SG_FILE_MAX || fseek(file, 0, SEEK_SET) != 0) {
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
    *length_out = (int)read;
    return source;
}

static void candidate_vec_free(candidate_vec_t *vec) {
    for (int i = 0; i < vec->count; i++) {
        free(vec->items[i].source_id);
        free(vec->items[i].target_id);
        free(vec->items[i].target_name);
        free(vec->items[i].type);
        free(vec->items[i].evidence);
        free(vec->items[i].properties);
    }
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static bool candidate_vec_add(candidate_vec_t *vec, const char *source_id,
                              const char *target_id, const char *target_name,
                              const char *type, cbm_aosp_cross_edge_status_t status,
                              double confidence, const char *evidence,
                              const char *properties) {
    if (vec->count == vec->cap) {
        int cap = vec->cap ? vec->cap * 2 : 64;
        owned_candidate_t *items = realloc(vec->items, (size_t)cap * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->cap = cap;
    }
    owned_candidate_t *item = &vec->items[vec->count];
    memset(item, 0, sizeof(*item));
    item->source_id = strdup(source_id);
    item->target_id = target_id ? strdup(target_id) : NULL;
    item->target_name = strdup(target_name);
    item->type = strdup(type);
    item->evidence = strdup(evidence);
    item->properties = strdup(properties);
    if (!item->source_id || (target_id && !item->target_id) || !item->target_name ||
        !item->type || !item->evidence || !item->properties) {
        free(item->source_id);
        free(item->target_id);
        free(item->target_name);
        free(item->type);
        free(item->evidence);
        free(item->properties);
        memset(item, 0, sizeof(*item));
        return false;
    }
    item->value.source_global_id = item->source_id;
    item->value.target_global_id = item->target_id;
    item->value.target_name = item->target_name;
    item->value.type = item->type;
    item->value.status = status;
    item->value.confidence = confidence;
    item->value.evidence = item->evidence;
    item->value.properties = item->properties;
    vec->count++;
    return true;
}

static void match_vec_free(match_vec_t *vec) {
    for (int i = 0; i < vec->count; i++) {
        free(vec->items[i].global_id);
        free(vec->items[i].repo_id);
        free(vec->items[i].name);
        free(vec->items[i].label);
        free(vec->items[i].qualified_name);
        free(vec->items[i].file_path);
    }
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static bool match_vec_add(match_vec_t *vec, sqlite3_stmt *stmt, int score) {
    if (vec->count == vec->cap) {
        int cap = vec->cap ? vec->cap * 2 : 16;
        target_match_t *items = realloc(vec->items, (size_t)cap * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->cap = cap;
    }
    target_match_t *item = &vec->items[vec->count];
    memset(item, 0, sizeof(*item));
    item->global_id = strdup((const char *)sqlite3_column_text(stmt, 0));
    item->repo_id = strdup((const char *)sqlite3_column_text(stmt, 1));
    item->name = strdup((const char *)sqlite3_column_text(stmt, 2));
    item->label = strdup((const char *)sqlite3_column_text(stmt, 3));
    item->qualified_name = strdup((const char *)sqlite3_column_text(stmt, 4));
    item->file_path = strdup((const char *)sqlite3_column_text(stmt, 5));
    item->score = score;
    if (!item->global_id || !item->repo_id || !item->name || !item->label ||
        !item->qualified_name || !item->file_path) {
        free(item->global_id);
        free(item->repo_id);
        free(item->name);
        free(item->label);
        free(item->qualified_name);
        free(item->file_path);
        memset(item, 0, sizeof(*item));
        return false;
    }
    vec->count++;
    return true;
}

static void normalize_reference(const char *raw, bool include, char out[SG_REF_MAX]) {
    while (raw && isspace((unsigned char)*raw)) raw++;
    if (!raw) {
        out[0] = '\0';
        return;
    }
    if (*raw == '@' || *raw == '"' || *raw == '\'') raw++;
    size_t n = 0;
    while (*raw && n + 1 < SG_REF_MAX) {
        char c = *raw++;
        if (c == '"' || c == '\'' || c == '(' || c == '<' || c == '[' ||
            (!include && (c == '*' || c == '&' || isspace((unsigned char)c)))) break;
        if (!include && (c == ':' || (c == '-' && *raw == '>'))) {
            if (c == '-' && *raw == '>') raw++;
            while (*raw == ':') raw++;
            if (n > 0 && out[n - 1] != '.') out[n++] = '.';
            continue;
        }
        out[n++] = c == '\\' ? '/' : c;
    }
    while (n > 0 && (out[n - 1] == ';' || out[n - 1] == ':' || isspace((unsigned char)out[n - 1]))) n--;
    out[n] = '\0';
}

static const char *short_reference(const char *reference) {
    const char *short_name = reference;
    for (const char *p = reference; *p; p++) {
        if (*p == '.' || *p == '/' || *p == '\\' || *p == ':' || *p == '>') short_name = p + 1;
    }
    return short_name;
}

static bool type_like(const char *label) {
    static const char *labels[] = {
        "Class", "Interface", "Struct", "Enum", "Trait", "Type", "Annotation", "Decorator",
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (strcmp(label, labels[i]) == 0) return true;
    }
    return false;
}

static bool target_label_allowed(const char *edge_type, const char *label) {
    if (strcmp(edge_type, "INCLUDES") == 0) return strcmp(label, "File") == 0;
    if (strcmp(edge_type, "IMPORTS") == 0) {
        return type_like(label) || strcmp(label, "File") == 0 ||
               strcmp(label, "Module") == 0 || strcmp(label, "Namespace") == 0;
    }
    if (strcmp(edge_type, "CALLS") == 0) {
        return strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0;
    }
    if (strcmp(edge_type, "USAGE") == 0) {
        return strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0 ||
               type_like(label) || strcmp(label, "Variable") == 0 ||
               strcmp(label, "Field") == 0 || strcmp(label, "Macro") == 0;
    }
    return type_like(label);
}

static bool ends_with(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return suffix_len <= text_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

static bool ends_with_boundary(const char *text, const char *suffix, char boundary) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (!ends_with(text, suffix)) return false;
    return text_len == suffix_len || text[text_len - suffix_len - 1] == boundary;
}

static int match_score(const char *edge_type, const char *reference, const char *short_name,
                       const char *name, const char *qualified_name, const char *file_path) {
    if (strcmp(edge_type, "INCLUDES") == 0) {
        if (strcmp(file_path, reference) == 0) return 110;
        if (ends_with_boundary(file_path, reference, '/')) return 100;
        return 0;
    }
    bool qualified_reference = strcmp(reference, short_name) != 0;
    if (strcmp(qualified_name, reference) == 0) return 120;
    if (qualified_reference && ends_with_boundary(qualified_name, reference, '.')) return 110;
    if (!qualified_reference && strcmp(name, reference) == 0) return 80;
    return 0;
}

static const char *resolution_name(const char *edge_type, int score) {
    if (strcmp(edge_type, "INCLUDES") == 0) {
        if (score == 110) return "exact_file_path";
        return "file_path_suffix";
    }
    if (score == 120) return "exact_qualified_name";
    if (score == 110) return "qualified_suffix";
    return "unique_short_name";
}

static double resolved_confidence(int score) {
    if (score >= 120) return 0.99;
    if (score >= 110) return 0.97;
    if (score >= 100) return 0.95;
    if (score >= 80) return 0.75;
    return 0.75;
}

static double ambiguous_confidence(int score) {
    return score >= 100 ? 0.55 : 0.35;
}

static void resolution_properties(char out[192], const char *resolution,
                                  int score, int candidate_count) {
    (void)snprintf(out, 192,
                   "{\"resolution\":\"%s\",\"score\":%d,\"candidate_count\":%d}",
                   resolution, score, candidate_count);
}

static char *lookup_source_id(collect_ctx_t *ctx, const char *qualified_name) {
    sqlite3_reset(ctx->source_lookup);
    sqlite3_clear_bindings(ctx->source_lookup);
    sqlite3_bind_text(ctx->source_lookup, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->source_lookup, 2, ctx->repo->repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->source_lookup, 3, qualified_name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ctx->source_lookup) != SQLITE_ROW) return NULL;
    const char *id = (const char *)sqlite3_column_text(ctx->source_lookup, 0);
    return id ? strdup(id) : NULL;
}

static int collect_reference(collect_ctx_t *ctx, const char *source_qn, const char *raw_reference,
                             const char *requested_type, const char *evidence,
                             double confidence_hint) {
    bool include = strcmp(requested_type, "INCLUDES") == 0;
    char reference[SG_REF_MAX];
    normalize_reference(raw_reference, include, reference);
    if (!source_qn || !reference[0]) return 0;
    ctx->stats.references_seen++;
    char *source_id = lookup_source_id(ctx, source_qn);
    if (!source_id) {
        ctx->stats.missing_sources++;
        return 0;
    }
    const char *short_name = short_reference(reference);
    char qn_pattern[SG_REF_MAX + 2];
    char file_pattern[SG_REF_MAX + 2];
    (void)snprintf(qn_pattern, sizeof(qn_pattern), "%%%s", reference);
    (void)snprintf(file_pattern, sizeof(file_pattern), "%%%s", reference);
    sqlite3_reset(ctx->target_lookup);
    sqlite3_clear_bindings(ctx->target_lookup);
    sqlite3_bind_text(ctx->target_lookup, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->target_lookup, 2, short_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->target_lookup, 3, qn_pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->target_lookup, 4, file_pattern, -1, SQLITE_TRANSIENT);
    match_vec_t matches = {0};
    int best_score = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(ctx->target_lookup)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(ctx->target_lookup, 2);
        const char *label = (const char *)sqlite3_column_text(ctx->target_lookup, 3);
        const char *qualified = (const char *)sqlite3_column_text(ctx->target_lookup, 4);
        const char *file_path = (const char *)sqlite3_column_text(ctx->target_lookup, 5);
        if (!name || !label || !qualified || !file_path ||
            !target_label_allowed(requested_type, label)) continue;
        int score = match_score(requested_type, reference, short_name, name, qualified, file_path);
        if (score <= 0) continue;
        if (!match_vec_add(&matches, ctx->target_lookup, score)) {
            free(source_id);
            match_vec_free(&matches);
            sg_error(ctx->err, ctx->err_size, "out of memory", NULL);
            return -1;
        }
        if (score > best_score) best_score = score;
    }
    if (step_rc != SQLITE_DONE) {
        free(source_id);
        match_vec_free(&matches);
        sg_error(ctx->err, ctx->err_size, "cannot search AOSP structural targets",
                 sqlite3_errmsg(ctx->master));
        return -1;
    }
    int local_best = 0;
    int cross_best = 0;
    for (int i = 0; i < matches.count; i++) {
        if (matches.items[i].score != best_score) continue;
        if (strcmp(matches.items[i].repo_id, ctx->repo->repo_id) == 0) local_best++;
        else cross_best++;
    }
    int rc = 0;
    if (local_best > 0) {
        ctx->stats.local_references_skipped++;
    } else if (cross_best == 0) {
        char properties[192];
        resolution_properties(properties, "unresolved", 0, 0);
        if (!candidate_vec_add(&ctx->candidates, source_id, NULL, reference, requested_type,
                               CBM_AOSP_CROSS_EDGE_UNRESOLVED, 0.0, evidence,
                               properties)) rc = -1;
    } else {
        const char *resolution = resolution_name(requested_type, best_score);
        char properties[192];
        resolution_properties(properties, resolution, best_score, cross_best);
        for (int i = 0; i < matches.count && rc == 0; i++) {
            target_match_t *match = &matches.items[i];
            if (match->score != best_score || strcmp(match->repo_id, ctx->repo->repo_id) == 0) continue;
            const char *edge_type = requested_type;
            if (strcmp(requested_type, "EXTENDS") == 0 &&
                (strcmp(match->label, "Interface") == 0 || strcmp(match->label, "Trait") == 0)) {
                edge_type = "IMPLEMENTS";
            }
            cbm_aosp_cross_edge_status_t status = cross_best == 1
                                                      ? CBM_AOSP_CROSS_EDGE_RESOLVED
                                                      : CBM_AOSP_CROSS_EDGE_AMBIGUOUS;
            double confidence = cross_best == 1 ? resolved_confidence(best_score)
                                                : ambiguous_confidence(best_score);
            if (cross_best == 1 && confidence_hint >= 0.0 && confidence_hint <= 1.0 &&
                confidence_hint < confidence) {
                confidence = confidence_hint;
            }
            if (!candidate_vec_add(&ctx->candidates, source_id, match->global_id, reference,
                                   edge_type, status, confidence, evidence,
                                   properties)) rc = -1;
        }
    }
    if (rc != 0) sg_error(ctx->err, ctx->err_size, "out of memory", NULL);
    free(source_id);
    match_vec_free(&matches);
    return rc;
}

static const char *reference_leaf(const char *reference) {
    const char *leaf = reference;
    if (!reference) return NULL;
    for (const char *p = reference; *p; p++) {
        if (*p == '.' || *p == '/' || *p == '\\' || *p == ':' || *p == '>') leaf = p + 1;
    }
    return leaf;
}

static const CBMResolvedCall *find_resolved_call(const CBMFileResult *result,
                                                 const CBMCall *call) {
    if (!call->enclosing_func_qn || !call->callee_name) return NULL;
    const char *call_leaf = reference_leaf(call->callee_name);
    const CBMResolvedCall *best = NULL;
    for (int i = 0; i < result->resolved_calls.count; i++) {
        const CBMResolvedCall *resolved = &result->resolved_calls.items[i];
        if (!resolved->caller_qn || !resolved->callee_qn || resolved->confidence < 0.6f ||
            strcmp(resolved->caller_qn, call->enclosing_func_qn) != 0) continue;
        const char *resolved_leaf = reference_leaf(resolved->callee_qn);
        const char *original_leaf = reference_leaf(resolved->reason);
        if (strcmp(resolved_leaf, call_leaf) != 0 &&
            (!original_leaf || strcmp(original_leaf, call_leaf) != 0)) continue;
        if (!best || resolved->confidence > best->confidence) best = resolved;
    }
    return best;
}

static bool resolved_call_has_ast_call(const CBMFileResult *result,
                                       const CBMResolvedCall *resolved) {
    if (!resolved->caller_qn || !resolved->callee_qn) return false;
    const char *resolved_leaf = reference_leaf(resolved->callee_qn);
    const char *original_leaf = reference_leaf(resolved->reason);
    for (int i = 0; i < result->calls.count; i++) {
        const CBMCall *call = &result->calls.items[i];
        if (!call->enclosing_func_qn || !call->callee_name ||
            strcmp(call->enclosing_func_qn, resolved->caller_qn) != 0) continue;
        const char *call_leaf = reference_leaf(call->callee_name);
        if (strcmp(call_leaf, resolved_leaf) == 0 ||
            (original_leaf && strcmp(call_leaf, original_leaf) == 0)) return true;
    }
    return false;
}

static const char *find_definition_qn(const CBMFileResult *result, const char *name) {
    for (int i = 0; i < result->defs.count; i++) {
        if (result->defs.items[i].name && strcmp(result->defs.items[i].name, name) == 0) {
            return result->defs.items[i].qualified_name;
        }
    }
    return NULL;
}

static int collect_file_result(collect_ctx_t *ctx, const char *file_qn, CBMLanguage language,
                               const CBMFileResult *result) {
    const char *import_type = (language == CBM_LANG_C || language == CBM_LANG_CPP ||
                               language == CBM_LANG_OBJC || language == CBM_LANG_CUDA)
                                  ? "INCLUDES" : "IMPORTS";
    const char *import_evidence = strcmp(import_type, "INCLUDES") == 0
                                      ? "ast_include_path" : "ast_import_path";
    for (int i = 0; i < result->imports.count; i++) {
        if (collect_reference(ctx, file_qn, result->imports.items[i].module_path,
                              import_type, import_evidence, -1.0) != 0) return -1;
    }
    for (int i = 0; i < result->defs.count; i++) {
        const CBMDefinition *def = &result->defs.items[i];
        if (!def->qualified_name) continue;
        if (def->base_classes) {
            for (int j = 0; def->base_classes[j]; j++) {
                if (collect_reference(ctx, def->qualified_name, def->base_classes[j],
                                      "EXTENDS", "ast_base_class", -1.0) != 0) return -1;
            }
        }
        if (def->decorators) {
            for (int j = 0; def->decorators[j]; j++) {
                if (collect_reference(ctx, def->qualified_name, def->decorators[j],
                                      "ANNOTATED_BY", "ast_annotation", -1.0) != 0) return -1;
            }
        }
    }
    for (int i = 0; i < result->type_refs.count; i++) {
        const CBMTypeRef *ref = &result->type_refs.items[i];
        if (collect_reference(ctx, ref->enclosing_func_qn, ref->type_name,
                              "USES_TYPE", "ast_type_reference", -1.0) != 0) return -1;
    }
    for (int i = 0; i < result->impl_traits.count; i++) {
        const CBMImplTrait *impl = &result->impl_traits.items[i];
        const char *source_qn = find_definition_qn(result, impl->struct_name);
        if (collect_reference(ctx, source_qn, impl->trait_name,
                              "IMPLEMENTS", "ast_impl_trait", -1.0) != 0) return -1;
    }
    for (int i = 0; i < result->calls.count; i++) {
        const CBMCall *call = &result->calls.items[i];
        const CBMResolvedCall *resolved = find_resolved_call(result, call);
        const char *target = resolved ? resolved->callee_qn : call->callee_name;
        const char *evidence = resolved ? "lsp_resolved_call" : "ast_call";
        double confidence = resolved ? resolved->confidence : -1.0;
        if (collect_reference(ctx, call->enclosing_func_qn, target,
                              "CALLS", evidence, confidence) != 0) return -1;
    }
    for (int i = 0; i < result->resolved_calls.count; i++) {
        const CBMResolvedCall *resolved = &result->resolved_calls.items[i];
        if (resolved->confidence < 0.6f || resolved_call_has_ast_call(result, resolved)) continue;
        if (collect_reference(ctx, resolved->caller_qn, resolved->callee_qn,
                              "CALLS", "lsp_resolved_call", resolved->confidence) != 0) return -1;
    }
    for (int i = 0; i < result->usages.count; i++) {
        const CBMUsage *usage = &result->usages.items[i];
        if (collect_reference(ctx, usage->enclosing_func_qn, usage->ref_name,
                              "USAGE", "ast_usage", -1.0) != 0) return -1;
    }
    return 0;
}

static int collect_repo(collect_ctx_t *ctx, const char *shard_path,
                        cbm_aosp_cross_edge_stats_t *edge_stats) {
    sqlite3 *shard = NULL;
    sqlite3_stmt *files = NULL;
    int rc = -1;
    if (sqlite3_open_v2(shard_path, &shard, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(shard,
            "SELECT qualified_name,file_path FROM nodes WHERE label='File' ORDER BY file_path;",
            -1, &files, NULL) != SQLITE_OK) {
        sg_error(ctx->err, ctx->err_size, "cannot read AOSP repository shard", shard_path);
        goto done;
    }
    int step_rc;
    while ((step_rc = sqlite3_step(files)) == SQLITE_ROW) {
        const char *file_qn = (const char *)sqlite3_column_text(files, 0);
        const char *rel_path = (const char *)sqlite3_column_text(files, 1);
        if (!file_qn || !rel_path || !rel_path[0]) continue;
        char abs_path[SG_PATH_MAX];
        int written = snprintf(abs_path, sizeof(abs_path), "%s/%s", ctx->repo->abs_path, rel_path);
        if (written <= 0 || (size_t)written >= sizeof(abs_path)) continue;
        const char *basename = strrchr(rel_path, '/');
        basename = basename ? basename + 1 : rel_path;
        CBMLanguage language = cbm_language_for_filename(basename);
        if (language == CBM_LANG_COUNT) continue;
        int source_len = 0;
        char *source = read_source(abs_path, &source_len);
        if (!source) continue;
        char project[CBM_AOSP_ID_LEN + 6];
        (void)snprintf(project, sizeof(project), "aosp-%s", ctx->repo->repo_id);
        CBMFileResult *result = cbm_extract_file(source, source_len, language, project, rel_path,
                                                 SG_PARSE_TIMEOUT_US, NULL, NULL);
        if (result) {
            ctx->stats.files_scanned++;
            if (collect_file_result(ctx, file_qn, language, result) != 0) {
                cbm_free_result(result);
                free(source);
                goto done;
            }
            cbm_free_result(result);
        }
        free(source);
    }
    if (step_rc != SQLITE_DONE) {
        sg_error(ctx->err, ctx->err_size, "cannot scan AOSP repository files", sqlite3_errmsg(shard));
        goto done;
    }
    cbm_aosp_cross_edge_candidate_t *values = NULL;
    if (ctx->candidates.count > 0) {
        values = calloc((size_t)ctx->candidates.count, sizeof(*values));
        if (!values) {
            sg_error(ctx->err, ctx->err_size, "out of memory", NULL);
            goto done;
        }
        for (int i = 0; i < ctx->candidates.count; i++) values[i] = ctx->candidates.items[i].value;
    }
    rc = cbm_aosp_cross_edges_refresh(ctx->workspace, ctx->repo, values,
                                      ctx->candidates.count, edge_stats,
                                      ctx->err, ctx->err_size);
    free(values);
done:
    sqlite3_finalize(files);
    sqlite3_close(shard);
    return rc;
}

int cbm_aosp_structural_link(const cbm_aosp_workspace_t *workspace,
                             cbm_aosp_structural_stats_t *stats,
                             char *err, size_t err_size) {
    if (!workspace || !stats) return -1;
    memset(stats, 0, sizeof(*stats));
    if (cbm_aosp_master_sync(workspace, err, err_size) != 0) return -1;
    char master_path[SG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *master = NULL;
    sqlite3_stmt *repo_lookup = NULL;
    sqlite3_stmt *refresh_lookup = NULL;
    sqlite3_stmt *source_lookup = NULL;
    sqlite3_stmt *target_lookup = NULL;
    int rc = -1;
    if (sqlite3_open_v2(master_path, &master, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT db_path,status FROM repos WHERE workspace_id=?1 AND repo_id=?2;",
            -1, &repo_lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT EXISTS(SELECT 1 FROM cross_edge_refresh_queue "
            "WHERE workspace_id=?1 AND source_repo_id=?2) OR NOT EXISTS("
            "SELECT 1 FROM cross_edge_refresh_state "
            "WHERE workspace_id=?1 AND source_repo_id=?2);",
            -1, &refresh_lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT global_id FROM symbols WHERE workspace_id=?1 AND repo_id=?2 "
            "AND qualified_name=?3;", -1, &source_lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT global_id,repo_id,name,label,qualified_name,file_path FROM symbols "
            "WHERE workspace_id=?1 AND (name=?2 OR qualified_name LIKE ?3 OR file_path LIKE ?4);",
            -1, &target_lookup, NULL) != SQLITE_OK) {
        sg_error(err, err_size, "cannot prepare AOSP structural collection", sqlite3_errmsg(master));
        goto done;
    }
    cbm_init();
    for (int i = 0; i < workspace->repo_count; i++) {
        const cbm_aosp_repo_t *repo = &workspace->repos[i];
        sqlite3_reset(refresh_lookup);
        sqlite3_clear_bindings(refresh_lookup);
        sqlite3_bind_text(refresh_lookup, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(refresh_lookup, 2, repo->repo_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(refresh_lookup) != SQLITE_ROW ||
            sqlite3_column_int(refresh_lookup, 0) == 0) continue;
        sqlite3_reset(repo_lookup);
        sqlite3_clear_bindings(repo_lookup);
        sqlite3_bind_text(repo_lookup, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(repo_lookup, 2, repo->repo_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(repo_lookup) != SQLITE_ROW) continue;
        const char *db_path_text = (const char *)sqlite3_column_text(repo_lookup, 0);
        const char *status = (const char *)sqlite3_column_text(repo_lookup, 1);
        if (!db_path_text || !db_path_text[0] || !status || strcmp(status, "indexed") != 0) continue;
        char *db_path = strdup(db_path_text);
        if (!db_path) {
            sg_error(err, err_size, "out of memory", NULL);
            goto done;
        }
        collect_ctx_t ctx = {
            .workspace = workspace, .repo = repo, .master = master,
            .source_lookup = source_lookup, .target_lookup = target_lookup,
            .err = err, .err_size = err_size,
        };
        cbm_aosp_cross_edge_stats_t repo_edges;
        int repo_rc = collect_repo(&ctx, db_path, &repo_edges);
        free(db_path);
        if (repo_rc != 0) {
            candidate_vec_free(&ctx.candidates);
            goto done;
        }
        stats->repos_scanned++;
        stats->files_scanned += ctx.stats.files_scanned;
        stats->references_seen += ctx.stats.references_seen;
        stats->local_references_skipped += ctx.stats.local_references_skipped;
        stats->missing_sources += ctx.stats.missing_sources;
        stats->edges.edge_count += repo_edges.edge_count;
        stats->edges.resolved_count += repo_edges.resolved_count;
        stats->edges.ambiguous_count += repo_edges.ambiguous_count;
        stats->edges.unresolved_count += repo_edges.unresolved_count;
        candidate_vec_free(&ctx.candidates);
    }
    rc = 0;
done:
    sqlite3_finalize(repo_lookup);
    sqlite3_finalize(refresh_lookup);
    sqlite3_finalize(source_lookup);
    sqlite3_finalize(target_lookup);
    sqlite3_close(master);
    return rc;
}
