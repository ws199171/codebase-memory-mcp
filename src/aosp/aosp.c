/*
 * aosp.c - AOSP federation control-plane foundation.
 *
 * Physical repository graphs remain independent CBM databases. The master DB
 * records stable workspace/repository identities and reserves the schema used
 * by global symbols, build modules, cross-repository edges, and architecture
 * summaries. No source repository is modified.
 */
#include "aosp/aosp.h"
#include "aosp/build_graph.h"
#include "aosp/federated_graph.h"
#include "aosp/protocol_graph.h"
#include "aosp/structural_graph.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "mcp/mcp.h"
#include "tree_sitter/api.h"

#include <sqlite3.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const TSLanguage *tree_sitter_xml(void);

enum {
    AOSP_INITIAL_CAP = 64,
    AOSP_MAX_MANIFEST_BYTES = 16 * 1024 * 1024,
    AOSP_MAX_INCLUDE_DEPTH = 64,
    AOSP_PATH_BUF = 4096,
    AOSP_DIR_MODE = 0755,
    AOSP_SYMBOL_REF_BUF = 1024,
    AOSP_RESOLVE_LIMIT = 200,
    AOSP_MAX_SNIPPET_BYTES = 4 * 1024 * 1024,
};

typedef struct {
    cbm_aosp_workspace_t *workspace;
    const char *repo_meta_root;
    char **visited;
    int visited_count;
    int visited_cap;
    int depth;
    char *err;
    size_t err_size;
} manifest_ctx_t;

static void set_error(char *err, size_t err_size, const char *message, const char *detail) {
    if (!err || err_size == 0) {
        return;
    }
    if (detail && detail[0]) {
        (void)snprintf(err, err_size, "%s: %s", message, detail);
    } else {
        (void)snprintf(err, err_size, "%s", message);
    }
}

static char *read_file(const char *path, size_t *length_out) {
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || size > AOSP_MAX_MANIFEST_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    char *data = malloc((size_t)size + 1);
    if (!data) {
        (void)fclose(f);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    (void)fclose(f);
    if (got != (size_t)size) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    if (length_out) {
        *length_out = got;
    }
    return data;
}

static char *node_text(TSNode node, const char *source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (end < start) {
        return NULL;
    }
    size_t length = (size_t)(end - start);
    char *text = malloc(length + 1);
    if (!text) {
        return NULL;
    }
    memcpy(text, source + start, length);
    text[length] = '\0';
    return text;
}

static char *xml_unquote(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t length = strlen(value);
    size_t begin = 0;
    size_t end = length;
    if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                        (value[0] == '\'' && value[length - 1] == '\''))) {
        begin = 1;
        end--;
    }
    char *out = malloc(end - begin + 1);
    if (!out) {
        return NULL;
    }
    size_t written = 0;
    for (size_t i = begin; i < end;) {
        struct entity {
            const char *encoded;
            char decoded;
        };
        static const struct entity entities[] = {
            {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'},
        };
        bool matched = false;
        for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
            size_t encoded_len = strlen(entities[e].encoded);
            if (i + encoded_len <= end && strncmp(value + i, entities[e].encoded, encoded_len) == 0) {
                out[written++] = entities[e].decoded;
                i += encoded_len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            out[written++] = value[i++];
        }
    }
    out[written] = '\0';
    return out;
}

static char *tag_name(TSNode tag, const char *source) {
    uint32_t count = ts_node_named_child_count(tag);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(tag, i);
        if (strcmp(ts_node_type(child), "Name") == 0) {
            return node_text(child, source);
        }
    }
    return NULL;
}

static char *tag_attribute(TSNode tag, const char *source, const char *wanted) {
    uint32_t count = ts_node_named_child_count(tag);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(tag, i);
        if (strcmp(ts_node_type(child), "Attribute") != 0) {
            continue;
        }
        char *name = NULL;
        char *raw_value = NULL;
        uint32_t attr_count = ts_node_named_child_count(child);
        for (uint32_t j = 0; j < attr_count; j++) {
            TSNode part = ts_node_named_child(child, j);
            const char *kind = ts_node_type(part);
            if (!name && strcmp(kind, "Name") == 0) {
                name = node_text(part, source);
            } else if (!raw_value && strcmp(kind, "AttValue") == 0) {
                raw_value = node_text(part, source);
            }
        }
        bool matches = name && strcmp(name, wanted) == 0;
        free(name);
        if (matches) {
            char *result = xml_unquote(raw_value);
            free(raw_value);
            return result;
        }
        free(raw_value);
    }
    return NULL;
}

static char *normalize_repo_path(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\') || strchr(path, ':')) {
        return NULL;
    }
    size_t length = strlen(path);
    char *out = malloc(length + 1);
    if (!out) {
        return NULL;
    }
    size_t written = 0;
    size_t i = 0;
    while (i < length) {
        while (i < length && path[i] == '/') {
            i++;
        }
        size_t start = i;
        while (i < length && path[i] != '/') {
            if ((unsigned char)path[i] < 0x20) {
                free(out);
                return NULL;
            }
            i++;
        }
        size_t segment_len = i - start;
        if (segment_len == 0 || (segment_len == 1 && path[start] == '.')) {
            continue;
        }
        if (segment_len == 2 && path[start] == '.' && path[start + 1] == '.') {
            free(out);
            return NULL;
        }
        if (written > 0) {
            out[written++] = '/';
        }
        memcpy(out + written, path + start, segment_len);
        written += segment_len;
    }
    if (written == 0) {
        free(out);
        return NULL;
    }
    out[written] = '\0';
    return out;
}

static int ensure_repo_capacity(cbm_aosp_workspace_t *workspace) {
    if (workspace->repo_count % AOSP_INITIAL_CAP != 0) {
        return 0;
    }
    int new_cap = workspace->repo_count + AOSP_INITIAL_CAP;
    cbm_aosp_repo_t *repos = realloc(workspace->repos, (size_t)new_cap * sizeof(*repos));
    if (!repos) {
        return -1;
    }
    memset(repos + workspace->repo_count, 0,
           (size_t)(new_cap - workspace->repo_count) * sizeof(*repos));
    workspace->repos = repos;
    return 0;
}

static int find_repo(const cbm_aosp_workspace_t *workspace, const char *name, const char *path) {
    for (int i = 0; i < workspace->repo_count; i++) {
        bool name_matches = !name || strcmp(workspace->repos[i].name, name) == 0;
        bool path_matches = !path || strcmp(workspace->repos[i].path, path) == 0;
        if (name_matches && path_matches) {
            return i;
        }
    }
    return -1;
}

static int add_or_update_repo(cbm_aosp_workspace_t *workspace, const char *name,
                              const char *raw_path, char *err, size_t err_size) {
    char *path = normalize_repo_path(raw_path && raw_path[0] ? raw_path : name);
    if (!name || !name[0] || !path) {
        set_error(err, err_size, "unsafe or incomplete manifest project", raw_path ? raw_path : name);
        free(path);
        return -1;
    }
    int index = find_repo(workspace, NULL, path);
    if (index >= 0) {
        char *new_name = strdup(name);
        if (!new_name) {
            free(path);
            return -1;
        }
        free(workspace->repos[index].name);
        free(workspace->repos[index].path);
        workspace->repos[index].name = new_name;
        workspace->repos[index].path = path;
        return 0;
    }
    if (ensure_repo_capacity(workspace) != 0) {
        free(path);
        return -1;
    }
    cbm_aosp_repo_t *repo = &workspace->repos[workspace->repo_count++];
    repo->name = strdup(name);
    repo->path = path;
    if (!repo->name) {
        free(repo->path);
        repo->path = NULL;
        workspace->repo_count--;
        return -1;
    }
    return 0;
}

static int remove_repo(cbm_aosp_workspace_t *workspace, const char *name, const char *raw_path,
                       char *err, size_t err_size) {
    char *path = raw_path ? normalize_repo_path(raw_path) : NULL;
    if ((!name || !name[0]) && !raw_path) {
        set_error(err, err_size, "incomplete remove-project", NULL);
        return -1;
    }
    if (raw_path && !path) {
        set_error(err, err_size, "unsafe remove-project path", raw_path);
        return -1;
    }
    for (int i = workspace->repo_count - 1; i >= 0; i--) {
        bool matches = (!name || strcmp(workspace->repos[i].name, name) == 0) &&
                       (!path || strcmp(workspace->repos[i].path, path) == 0);
        if (!matches) {
            continue;
        }
        free(workspace->repos[i].name);
        free(workspace->repos[i].path);
        free(workspace->repos[i].abs_path);
        if (i + 1 < workspace->repo_count) {
            memmove(&workspace->repos[i], &workspace->repos[i + 1],
                    (size_t)(workspace->repo_count - i - 1) * sizeof(workspace->repos[0]));
        }
        workspace->repo_count--;
        memset(&workspace->repos[workspace->repo_count], 0, sizeof(workspace->repos[0]));
    }
    free(path);
    return 0;
}

static bool path_under_root(const char *root, const char *path) {
    size_t root_len = strlen(root);
    return strncmp(root, path, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/' || path[root_len] == '\\');
}

static int parse_manifest_file(manifest_ctx_t *ctx, const char *manifest_path);

static int resolve_include(manifest_ctx_t *ctx, const char *current_file, const char *name,
                           char *resolved, size_t resolved_size) {
    if (!name || !name[0] || name[0] == '/' || strstr(name, "..") || strchr(name, '\\')) {
        set_error(ctx->err, ctx->err_size, "unsafe manifest include", name);
        return -1;
    }
    char current_dir[AOSP_PATH_BUF];
    (void)snprintf(current_dir, sizeof(current_dir), "%s", current_file);
    char *slash = strrchr(current_dir, '/');
    if (slash) {
        *slash = '\0';
    }
    const char *candidates[3] = {current_dir, ctx->repo_meta_root, NULL};
    char repo_dir[AOSP_PATH_BUF];
    (void)snprintf(repo_dir, sizeof(repo_dir), "%s/..", ctx->repo_meta_root);
    candidates[2] = repo_dir;
    for (int i = 0; i < 3; i++) {
        char candidate[AOSP_PATH_BUF];
        (void)snprintf(candidate, sizeof(candidate), "%s/%s", candidates[i], name);
        char canonical[AOSP_PATH_BUF];
        if (cbm_canonical_path(candidate, canonical, sizeof(canonical)) &&
            path_under_root(ctx->workspace->root, canonical)) {
            (void)snprintf(resolved, resolved_size, "%s", canonical);
            return 0;
        }
    }
    set_error(ctx->err, ctx->err_size, "manifest include not found", name);
    return -1;
}

static int apply_tag(manifest_ctx_t *ctx, TSNode tag, const char *source,
                     const char *manifest_path) {
    char *kind = tag_name(tag, source);
    if (!kind) {
        return 0;
    }
    int rc = 0;
    if (strcmp(kind, "project") == 0) {
        char *name = tag_attribute(tag, source, "name");
        char *path = tag_attribute(tag, source, "path");
        rc = add_or_update_repo(ctx->workspace, name, path, ctx->err, ctx->err_size);
        free(name);
        free(path);
    } else if (strcmp(kind, "remove-project") == 0) {
        char *name = tag_attribute(tag, source, "name");
        char *path = tag_attribute(tag, source, "path");
        rc = remove_repo(ctx->workspace, name, path, ctx->err, ctx->err_size);
        free(name);
        free(path);
    } else if (strcmp(kind, "extend-project") == 0) {
        char *name = tag_attribute(tag, source, "name");
        char *path = tag_attribute(tag, source, "path");
        char *normalized = path ? normalize_repo_path(path) : NULL;
        if (!name || !name[0] || (path && !normalized)) {
            set_error(ctx->err, ctx->err_size, "unsafe or incomplete extend-project",
                      path ? path : name);
            rc = -1;
        } else {
            /* `path` narrows which checkout receives revision/group changes;
             * it does not change the checkout path. Those attributes do not
             * affect graph sharding, so discovery only validates the target. */
            (void)find_repo(ctx->workspace, name, normalized);
        }
        free(normalized);
        free(name);
        free(path);
    } else if (strcmp(kind, "include") == 0) {
        char *name = tag_attribute(tag, source, "name");
        char resolved[AOSP_PATH_BUF];
        if (resolve_include(ctx, manifest_path, name, resolved, sizeof(resolved)) != 0) {
            rc = -1;
        } else {
            rc = parse_manifest_file(ctx, resolved);
        }
        free(name);
    }
    free(kind);
    return rc;
}

static int walk_manifest(manifest_ctx_t *ctx, TSNode node, const char *source,
                         const char *manifest_path) {
    const char *kind = ts_node_type(node);
    if (strcmp(kind, "STag") == 0 || strcmp(kind, "EmptyElemTag") == 0) {
        if (apply_tag(ctx, node, source, manifest_path) != 0) {
            return -1;
        }
    }
    uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; i++) {
        if (walk_manifest(ctx, ts_node_named_child(node, i), source, manifest_path) != 0) {
            return -1;
        }
    }
    return 0;
}

static bool visited_manifest(manifest_ctx_t *ctx, const char *canonical) {
    for (int i = 0; i < ctx->visited_count; i++) {
        if (strcmp(ctx->visited[i], canonical) == 0) {
            return true;
        }
    }
    if (ctx->visited_count == ctx->visited_cap) {
        int new_cap = ctx->visited_cap ? ctx->visited_cap * 2 : 16;
        char **visited = realloc(ctx->visited, (size_t)new_cap * sizeof(*visited));
        if (!visited) {
            return true;
        }
        ctx->visited = visited;
        ctx->visited_cap = new_cap;
    }
    ctx->visited[ctx->visited_count++] = strdup(canonical);
    return false;
}

static int parse_manifest_file(manifest_ctx_t *ctx, const char *manifest_path) {
    if (++ctx->depth > AOSP_MAX_INCLUDE_DEPTH) {
        set_error(ctx->err, ctx->err_size, "manifest include depth exceeded", manifest_path);
        ctx->depth--;
        return -1;
    }
    char canonical[AOSP_PATH_BUF];
    if (!cbm_canonical_path(manifest_path, canonical, sizeof(canonical)) ||
        !path_under_root(ctx->workspace->root, canonical)) {
        set_error(ctx->err, ctx->err_size, "manifest path is outside workspace", manifest_path);
        ctx->depth--;
        return -1;
    }
    if (visited_manifest(ctx, canonical)) {
        ctx->depth--;
        return 0;
    }

    size_t source_len = 0;
    char *source = read_file(canonical, &source_len);
    if (!source) {
        set_error(ctx->err, ctx->err_size, "cannot read AOSP manifest", canonical);
        ctx->depth--;
        return -1;
    }
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tree_sitter_xml())) {
        set_error(ctx->err, ctx->err_size, "cannot initialize XML parser", canonical);
        ts_parser_delete(parser);
        free(source);
        ctx->depth--;
        return -1;
    }
    TSTree *tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)source_len);
    int rc = -1;
    if (!tree || ts_node_has_error(ts_tree_root_node(tree))) {
        set_error(ctx->err, ctx->err_size, "invalid AOSP manifest XML", canonical);
    } else {
        rc = walk_manifest(ctx, ts_tree_root_node(tree), source, canonical);
    }
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    free(source);
    ctx->depth--;
    return rc;
}

static int compare_repo_path(const void *left, const void *right) {
    const cbm_aosp_repo_t *a = left;
    const cbm_aosp_repo_t *b = right;
    return strcmp(a->path, b->path);
}

static int compare_string_ptr(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static void digest_hex(const uint8_t digest[CBM_SHA256_DIGEST_LEN], char out[CBM_AOSP_HASH_LEN + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    out[CBM_AOSP_HASH_LEN] = '\0';
}

static void finalize_workspace(cbm_aosp_workspace_t *workspace) {
    qsort(workspace->repos, (size_t)workspace->repo_count, sizeof(workspace->repos[0]),
          compare_repo_path);
    char root_hash[CBM_AOSP_HASH_LEN + 1];
    cbm_sha256_hex(workspace->root, strlen(workspace->root), root_hash);
    memcpy(workspace->workspace_id, root_hash, CBM_AOSP_ID_LEN);
    workspace->workspace_id[CBM_AOSP_ID_LEN] = '\0';

    cbm_sha256_ctx manifest_ctx;
    cbm_sha256_init(&manifest_ctx);
    for (int i = 0; i < workspace->repo_count; i++) {
        cbm_aosp_repo_t *repo = &workspace->repos[i];
        cbm_sha256_update(&manifest_ctx, repo->name, strlen(repo->name));
        cbm_sha256_update(&manifest_ctx, "\0", 1);
        cbm_sha256_update(&manifest_ctx, repo->path, strlen(repo->path));
        cbm_sha256_update(&manifest_ctx, "\0", 1);

        char repo_key[AOSP_PATH_BUF + CBM_AOSP_ID_LEN + 2];
        (void)snprintf(repo_key, sizeof(repo_key), "%s:%s", workspace->workspace_id, repo->path);
        char repo_hash[CBM_AOSP_HASH_LEN + 1];
        cbm_sha256_hex(repo_key, strlen(repo_key), repo_hash);
        memcpy(repo->repo_id, repo_hash, CBM_AOSP_ID_LEN);
        repo->repo_id[CBM_AOSP_ID_LEN] = '\0';

        size_t needed = strlen(workspace->root) + strlen(repo->path) + 2;
        repo->abs_path = malloc(needed);
        if (repo->abs_path) {
            (void)snprintf(repo->abs_path, needed, "%s/%s", workspace->root, repo->path);
            repo->exists = cbm_is_dir(repo->abs_path);
        }
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&manifest_ctx, digest);
    digest_hex(digest, workspace->manifest_hash);
}

static int apply_local_manifests(manifest_ctx_t *ctx) {
    char dir_path[AOSP_PATH_BUF];
    (void)snprintf(dir_path, sizeof(dir_path), "%s/.repo/local_manifests", ctx->workspace->root);
    cbm_dir_t *dir = cbm_opendir(dir_path);
    if (!dir) {
        return 0;
    }
    char **files = NULL;
    int count = 0;
    int cap = 0;
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(dir)) != NULL) {
        size_t length = strlen(entry->name);
        if (entry->is_dir || length < 4 || strcmp(entry->name + length - 4, ".xml") != 0) {
            continue;
        }
        if (count == cap) {
            int new_cap = cap ? cap * 2 : 8;
            char **new_files = realloc(files, (size_t)new_cap * sizeof(*new_files));
            if (!new_files) {
                cbm_closedir(dir);
                for (int i = 0; i < count; i++) free(files[i]);
                free(files);
                return -1;
            }
            files = new_files;
            cap = new_cap;
        }
        files[count++] = strdup(entry->name);
    }
    cbm_closedir(dir);
    if (count > 1) {
        qsort(files, (size_t)count, sizeof(*files), compare_string_ptr);
    }
    int rc = 0;
    for (int i = 0; i < count && rc == 0; i++) {
        char path[AOSP_PATH_BUF];
        (void)snprintf(path, sizeof(path), "%s/%s", dir_path, files[i]);
        rc = parse_manifest_file(ctx, path);
    }
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    return rc;
}

int cbm_aosp_discover(const char *root, cbm_aosp_workspace_t *out, char *err, size_t err_size) {
    if (!root || !out) {
        set_error(err, err_size, "AOSP root is required", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char canonical[AOSP_PATH_BUF];
    if (!cbm_canonical_path(root, canonical, sizeof(canonical)) || !cbm_is_dir(canonical)) {
        set_error(err, err_size, "AOSP root does not exist", root);
        return -1;
    }
    out->root = strdup(canonical);
    if (!out->root) {
        set_error(err, err_size, "out of memory", NULL);
        return -1;
    }
    char manifest[AOSP_PATH_BUF];
    char manifests_root[AOSP_PATH_BUF];
    (void)snprintf(manifest, sizeof(manifest), "%s/.repo/manifest.xml", canonical);
    (void)snprintf(manifests_root, sizeof(manifests_root), "%s/.repo/manifests", canonical);
    manifest_ctx_t ctx = {
        .workspace = out,
        .repo_meta_root = manifests_root,
        .err = err,
        .err_size = err_size,
    };
    int rc = parse_manifest_file(&ctx, manifest);
    if (rc == 0) {
        rc = apply_local_manifests(&ctx);
    }
    for (int i = 0; i < ctx.visited_count; i++) free(ctx.visited[i]);
    free(ctx.visited);
    if (rc != 0 || out->repo_count == 0) {
        if (rc == 0) set_error(err, err_size, "AOSP manifest contains no projects", manifest);
        cbm_aosp_workspace_free(out);
        return -1;
    }
    finalize_workspace(out);
    return 0;
}

void cbm_aosp_workspace_free(cbm_aosp_workspace_t *workspace) {
    if (!workspace) return;
    for (int i = 0; i < workspace->repo_count; i++) {
        free(workspace->repos[i].name);
        free(workspace->repos[i].path);
        free(workspace->repos[i].abs_path);
    }
    free(workspace->repos);
    free(workspace->root);
    memset(workspace, 0, sizeof(*workspace));
}

int cbm_aosp_master_path(const cbm_aosp_workspace_t *workspace, char *out, size_t out_size,
                         bool create_dirs) {
    if (!workspace || !workspace->workspace_id[0] || !out || out_size == 0) return -1;
    char dir[AOSP_PATH_BUF];
    const char *cache_dir = cbm_resolve_cache_dir();
    if (!cache_dir) cache_dir = cbm_tmpdir();
    (void)snprintf(dir, sizeof(dir), "%s/workspaces/%s", cache_dir, workspace->workspace_id);
    if (create_dirs && !cbm_mkdir_p(dir, AOSP_DIR_MODE)) return -1;
    int written = snprintf(out, out_size, "%s/master.db", dir);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static int exec_sql(sqlite3 *db, const char *sql, char *err, size_t err_size) {
    char *sqlite_err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &sqlite_err);
    if (rc != SQLITE_OK) {
        set_error(err, err_size, "AOSP master database error", sqlite_err ? sqlite_err : "unknown");
        sqlite3_free(sqlite_err);
        return -1;
    }
    return 0;
}

static const char *AOSP_SCHEMA =
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS schema_versions(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(1,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(2,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(3,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(8,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(9,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(10,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(11,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(12,strftime('%s','now'));"
    "INSERT OR IGNORE INTO schema_versions(version,applied_at) VALUES(13,strftime('%s','now'));"
    "CREATE TABLE IF NOT EXISTS workspaces("
    " id TEXT PRIMARY KEY, root_path TEXT NOT NULL UNIQUE, manifest_hash TEXT NOT NULL, updated_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS repos("
    " repo_id TEXT PRIMARY KEY, workspace_id TEXT NOT NULL REFERENCES workspaces(id) ON DELETE CASCADE,"
    " manifest_name TEXT NOT NULL, path TEXT NOT NULL, abs_path TEXT NOT NULL,"
    " status TEXT NOT NULL DEFAULT 'discovered', index_mode TEXT NOT NULL DEFAULT 'catalog',"
    " db_path TEXT DEFAULT '', generation TEXT NOT NULL, error_msg TEXT DEFAULT '',"
    " last_indexed_at INTEGER DEFAULT 0, UNIQUE(workspace_id,path));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_repos_status ON repos(workspace_id,status);"
    "CREATE TABLE IF NOT EXISTS modules("
    " module_id TEXT PRIMARY KEY, workspace_id TEXT NOT NULL, repo_id TEXT NOT NULL,"
    " name TEXT NOT NULL, module_type TEXT NOT NULL, file_path TEXT NOT NULL, properties TEXT DEFAULT '{}');"
    "CREATE INDEX IF NOT EXISTS idx_aosp_modules_name ON modules(workspace_id,name);"
    "CREATE TABLE IF NOT EXISTS module_edges("
    " source_id TEXT NOT NULL, target_id TEXT NOT NULL, type TEXT NOT NULL, properties TEXT DEFAULT '{}',"
    " PRIMARY KEY(source_id,target_id,type));"
    "CREATE TABLE IF NOT EXISTS module_dependencies("
    " source_id TEXT NOT NULL, target_name TEXT NOT NULL, type TEXT NOT NULL,"
    " target_id TEXT, resolved INTEGER NOT NULL DEFAULT 0, properties TEXT DEFAULT '{}',"
    " PRIMARY KEY(source_id,target_name,type));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_module_deps_target ON module_dependencies(target_name);"
    "CREATE TABLE IF NOT EXISTS module_files("
    " source_id TEXT NOT NULL,path TEXT NOT NULL,role TEXT NOT NULL,properties TEXT DEFAULT '{}',"
    " PRIMARY KEY(source_id,path,role));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_module_files_role ON module_files(role);"
    "CREATE TABLE IF NOT EXISTS build_generated_files("
    " generated_id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,repo_id TEXT NOT NULL,"
    " producer_id TEXT NOT NULL,declared_path TEXT NOT NULL,workspace_path TEXT NOT NULL,"
    " properties TEXT NOT NULL DEFAULT '{}',UNIQUE(producer_id,declared_path));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_generated_producer "
    "ON build_generated_files(producer_id);"
    "CREATE TABLE IF NOT EXISTS module_file_links("
    " source_id TEXT NOT NULL,declared_path TEXT NOT NULL,role TEXT NOT NULL,"
    " workspace_path TEXT NOT NULL,target_repo_id TEXT NOT NULL,file_global_id TEXT,"
    " generated_id TEXT,status TEXT NOT NULL,properties TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(source_id,declared_path,role));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_module_file_links_file "
    "ON module_file_links(file_global_id);"
    "CREATE TABLE IF NOT EXISTS module_generated_links("
    " source_id TEXT NOT NULL,target_module_id TEXT NOT NULL,dependency_type TEXT NOT NULL,"
    " output_tag TEXT NOT NULL DEFAULT '',generated_id TEXT,status TEXT NOT NULL,"
    " properties TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(source_id,target_module_id,dependency_type,output_tag));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_module_generated_target "
    "ON module_generated_links(generated_id);"
    "CREATE TABLE IF NOT EXISTS module_symbol_links("
    " source_id TEXT NOT NULL,file_global_id TEXT NOT NULL,symbol_global_id TEXT NOT NULL,"
    " link_type TEXT NOT NULL,properties TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(source_id,symbol_global_id,link_type));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_module_symbols_symbol "
    "ON module_symbol_links(symbol_global_id);"
    "CREATE TABLE IF NOT EXISTS build_make_files("
    " workspace_id TEXT NOT NULL,repo_id TEXT NOT NULL,file_path TEXT NOT NULL,"
    " includes TEXT NOT NULL DEFAULT '[]',condition_count INTEGER NOT NULL DEFAULT 0,"
    " macro_count INTEGER NOT NULL DEFAULT 0,unsupported_expressions TEXT NOT NULL DEFAULT '[]',"
    " PRIMARY KEY(workspace_id,repo_id,file_path));"
    "CREATE TABLE IF NOT EXISTS build_namespaces("
    " workspace_id TEXT NOT NULL,namespace_path TEXT NOT NULL,repo_id TEXT NOT NULL,"
    " file_path TEXT NOT NULL,imports TEXT NOT NULL DEFAULT '[]',"
    " PRIMARY KEY(workspace_id,namespace_path));"
    "CREATE TABLE IF NOT EXISTS build_packages("
    " workspace_id TEXT NOT NULL,package_path TEXT NOT NULL,repo_id TEXT NOT NULL,"
    " file_path TEXT NOT NULL,default_visibility TEXT NOT NULL DEFAULT '[]',"
    " PRIMARY KEY(workspace_id,package_path));"
    "CREATE TABLE IF NOT EXISTS build_products("
    " product_id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,repo_id TEXT NOT NULL,"
    " name TEXT NOT NULL,kind TEXT NOT NULL,file_path TEXT NOT NULL,workspace_path TEXT NOT NULL,"
    " device TEXT NOT NULL DEFAULT '',brand TEXT NOT NULL DEFAULT '',model TEXT NOT NULL DEFAULT '',"
    " manufacturer TEXT NOT NULL DEFAULT '',device_owner TEXT NOT NULL DEFAULT '',"
    " vendor_owner TEXT NOT NULL DEFAULT '',partitions TEXT NOT NULL DEFAULT '[]',"
    " properties TEXT NOT NULL DEFAULT '{}');"
    "CREATE INDEX IF NOT EXISTS idx_aosp_build_products_name "
    "ON build_products(workspace_id,name);"
    "CREATE TABLE IF NOT EXISTS build_product_inheritance("
    " source_product_id TEXT NOT NULL,inherited_path TEXT NOT NULL,target_product_id TEXT,"
    " status TEXT NOT NULL,optional INTEGER NOT NULL DEFAULT 0,source_file TEXT NOT NULL,"
    " properties TEXT NOT NULL DEFAULT '{}',PRIMARY KEY(source_product_id,inherited_path));"
    "CREATE TABLE IF NOT EXISTS build_product_packages("
    " product_id TEXT NOT NULL,module_name TEXT NOT NULL,partition_name TEXT NOT NULL,"
    " module_id TEXT,resolved INTEGER NOT NULL DEFAULT 0,included INTEGER NOT NULL DEFAULT 1,"
    " source_variable TEXT NOT NULL,source_file TEXT NOT NULL,properties TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(product_id,module_name,partition_name));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_product_packages_module "
    "ON build_product_packages(module_name);"
    "CREATE TABLE IF NOT EXISTS build_board_configs("
    " workspace_id TEXT NOT NULL,repo_id TEXT NOT NULL,file_path TEXT NOT NULL,"
    " workspace_path TEXT NOT NULL,device_owner TEXT NOT NULL DEFAULT '',"
    " vendor_owner TEXT NOT NULL DEFAULT '',variables TEXT NOT NULL DEFAULT '{}',"
    " partitions TEXT NOT NULL DEFAULT '[]',PRIMARY KEY(workspace_id,repo_id,file_path));"
    "CREATE TABLE IF NOT EXISTS build_bazel_artifacts("
    " workspace_id TEXT NOT NULL,repo_id TEXT NOT NULL,file_path TEXT NOT NULL,"
    " format_version INTEGER NOT NULL,configuration TEXT NOT NULL DEFAULT '',"
    " target_count INTEGER NOT NULL DEFAULT 0,coverage_gaps TEXT NOT NULL DEFAULT '[]',"
    " PRIMARY KEY(workspace_id,repo_id,file_path));"
    "CREATE TABLE IF NOT EXISTS build_bazel_targets("
    " workspace_id TEXT NOT NULL,repo_id TEXT NOT NULL,artifact_path TEXT NOT NULL,"
    " label TEXT NOT NULL,configuration TEXT NOT NULL DEFAULT '',kind TEXT NOT NULL DEFAULT '',"
    " module_name TEXT NOT NULL,module_id TEXT,status TEXT NOT NULL,properties TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(workspace_id,repo_id,artifact_path,label,configuration));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_bazel_targets_label "
    "ON build_bazel_targets(workspace_id,label,configuration);"
    "CREATE TABLE IF NOT EXISTS build_bazel_dependencies("
    " workspace_id TEXT NOT NULL,source_repo_id TEXT NOT NULL,artifact_path TEXT NOT NULL,"
    " source_label TEXT NOT NULL,source_configuration TEXT NOT NULL DEFAULT '',"
    " target_label TEXT NOT NULL,target_configuration TEXT NOT NULL DEFAULT '',"
    " dependency_type TEXT NOT NULL,transition TEXT NOT NULL DEFAULT '',"
    " source_module_id TEXT,target_module_id TEXT,"
    " status TEXT NOT NULL,properties TEXT NOT NULL DEFAULT '{}',"
    " PRIMARY KEY(workspace_id,source_repo_id,artifact_path,source_label,"
    " source_configuration,target_label,target_configuration,dependency_type,transition));"
    "CREATE INDEX IF NOT EXISTS idx_aosp_bazel_deps_target "
    "ON build_bazel_dependencies(workspace_id,target_label,target_configuration);"
    "CREATE TABLE IF NOT EXISTS symbols("
    " id INTEGER PRIMARY KEY, global_id TEXT NOT NULL UNIQUE, workspace_id TEXT NOT NULL, repo_id TEXT NOT NULL,"
    " local_node_id INTEGER, name TEXT NOT NULL, qualified_name TEXT NOT NULL, label TEXT NOT NULL,"
    " qualified_leaf TEXT NOT NULL DEFAULT '',"
    " language TEXT DEFAULT '', file_path TEXT NOT NULL, start_line INTEGER DEFAULT 0, end_line INTEGER DEFAULT 0,"
    " visibility TEXT DEFAULT '', signature TEXT DEFAULT '', docstring TEXT DEFAULT '', properties TEXT DEFAULT '{}');"
    "CREATE INDEX IF NOT EXISTS idx_aosp_symbols_name ON symbols(workspace_id,name);"
    "CREATE INDEX IF NOT EXISTS idx_aosp_symbols_qn ON symbols(workspace_id,qualified_name);"
    "CREATE INDEX IF NOT EXISTS idx_aosp_symbols_repo ON symbols(repo_id);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5("
    " name,qualified_name,signature,docstring,content='symbols',content_rowid='id');"
    "CREATE TRIGGER IF NOT EXISTS symbols_ai AFTER INSERT ON symbols BEGIN"
    " INSERT INTO symbols_fts(rowid,name,qualified_name,signature,docstring)"
    " VALUES(new.id,new.name,new.qualified_name,new.signature,new.docstring); END;"
    "CREATE TRIGGER IF NOT EXISTS symbols_ad AFTER DELETE ON symbols BEGIN"
    " INSERT INTO symbols_fts(symbols_fts,rowid,name,qualified_name,signature,docstring)"
    " VALUES('delete',old.id,old.name,old.qualified_name,old.signature,old.docstring); END;"
    "CREATE TRIGGER IF NOT EXISTS symbols_au AFTER UPDATE ON symbols BEGIN"
    " INSERT INTO symbols_fts(symbols_fts,rowid,name,qualified_name,signature,docstring)"
    " VALUES('delete',old.id,old.name,old.qualified_name,old.signature,old.docstring);"
    " INSERT INTO symbols_fts(rowid,name,qualified_name,signature,docstring)"
    " VALUES(new.id,new.name,new.qualified_name,new.signature,new.docstring); END;"
    "CREATE TABLE IF NOT EXISTS protocol_nodes("
    " protocol_id TEXT PRIMARY KEY, workspace_id TEXT NOT NULL, repo_id TEXT NOT NULL,"
    " kind TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL, file_path TEXT DEFAULT '',"
    " symbol_global_id TEXT, properties TEXT DEFAULT '{}');"
    "CREATE INDEX IF NOT EXISTS idx_aosp_protocol_nodes_name ON protocol_nodes(workspace_id,name);"
    "CREATE TABLE IF NOT EXISTS protocol_edges("
    " source_id TEXT NOT NULL, target_id TEXT NOT NULL, type TEXT NOT NULL, confidence REAL NOT NULL,"
    " evidence TEXT NOT NULL, properties TEXT DEFAULT '{}',"
    " PRIMARY KEY(source_id,target_id,type,evidence));"
    "CREATE TABLE IF NOT EXISTS architecture_summaries("
    " scope_id TEXT PRIMARY KEY, workspace_id TEXT NOT NULL, scope_type TEXT NOT NULL,"
    " summary TEXT NOT NULL, source_hash TEXT NOT NULL, updated_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS coverage("
    " workspace_id TEXT NOT NULL, repo_id TEXT NOT NULL, kind TEXT NOT NULL, detail TEXT DEFAULT '',"
    " PRIMARY KEY(workspace_id,repo_id,kind));";

static bool aosp_table_has_column(sqlite3 *db, const char *table, const char *column) {
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

static const char *aosp_symbol_leaf(const char *qualified_name) {
    const char *leaf = qualified_name ? qualified_name : "";
    for (const char *p = leaf; *p; p++) {
        if (*p == '.' || *p == '/' || *p == '\\' || *p == ':' || *p == '>') leaf = p + 1;
    }
    return leaf;
}

static int ensure_symbol_resolver_schema(sqlite3 *db, char *err, size_t err_size) {
    if (!aosp_table_has_column(db, "symbols", "qualified_leaf") &&
        exec_sql(db, "ALTER TABLE symbols ADD COLUMN qualified_leaf TEXT NOT NULL DEFAULT '';",
                 err, err_size) != 0) return -1;
    if (exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_aosp_symbols_leaf "
                     "ON symbols(workspace_id,qualified_leaf);", err, err_size) != 0) return -1;
    sqlite3_stmt *needs_backfill = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT EXISTS(SELECT 1 FROM symbols WHERE qualified_leaf='');",
            -1, &needs_backfill, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot inspect AOSP symbol resolver schema", sqlite3_errmsg(db));
        return -1;
    }
    bool backfill = sqlite3_step(needs_backfill) == SQLITE_ROW &&
                    sqlite3_column_int(needs_backfill, 0) != 0;
    sqlite3_finalize(needs_backfill);
    if (backfill) {
        sqlite3_stmt *read_stmt = NULL;
        sqlite3_stmt *update_stmt = NULL;
        int rc = -1;
        if (exec_sql(db, "BEGIN IMMEDIATE;", err, err_size) != 0 ||
            sqlite3_prepare_v2(db,
                "SELECT id,qualified_name FROM symbols WHERE qualified_leaf='';",
                -1, &read_stmt, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(db,
                "UPDATE symbols SET qualified_leaf=?1 WHERE id=?2;",
                -1, &update_stmt, NULL) != SQLITE_OK) {
            set_error(err, err_size, "cannot prepare AOSP symbol leaf migration", sqlite3_errmsg(db));
            goto backfill_done;
        }
        int step_rc;
        while ((step_rc = sqlite3_step(read_stmt)) == SQLITE_ROW) {
            const char *qualified_name = (const char *)sqlite3_column_text(read_stmt, 1);
            sqlite3_reset(update_stmt);
            sqlite3_clear_bindings(update_stmt);
            sqlite3_bind_text(update_stmt, 1, aosp_symbol_leaf(qualified_name), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update_stmt, 2, sqlite3_column_int64(read_stmt, 0));
            if (sqlite3_step(update_stmt) != SQLITE_DONE) break;
        }
        if (step_rc == SQLITE_DONE && exec_sql(db, "COMMIT;", err, err_size) == 0) rc = 0;
backfill_done:
        sqlite3_finalize(read_stmt);
        sqlite3_finalize(update_stmt);
        if (rc != 0) {
            (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            if (err && err_size && !err[0]) {
                set_error(err, err_size, "cannot migrate AOSP symbol leaves", sqlite3_errmsg(db));
            }
            return -1;
        }
    }
    return exec_sql(db,
        "INSERT OR IGNORE INTO schema_versions(version,applied_at) "
        "VALUES(7,strftime('%s','now'));", err, err_size);
}

int cbm_aosp_master_sync(const cbm_aosp_workspace_t *workspace, char *err, size_t err_size) {
    char db_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, db_path, sizeof(db_path), true) != 0) {
        set_error(err, err_size, "cannot create AOSP workspace cache", workspace ? workspace->root : NULL);
        return -1;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP master database", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 10000);
    if (exec_sql(db, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;", err, err_size) != 0 ||
        exec_sql(db, AOSP_SCHEMA, err, err_size) != 0 ||
        ensure_symbol_resolver_schema(db, err, err_size) != 0 ||
        cbm_aosp_cross_edges_ensure_schema(db, err, err_size) != 0 ||
        exec_sql(db, "BEGIN IMMEDIATE;", err, err_size) != 0) {
        sqlite3_close(db);
        return -1;
    }
    int rc = -1;
    sqlite3_stmt *workspace_stmt = NULL;
    sqlite3_stmt *repo_stmt = NULL;
    sqlite3_stmt *delete_stmt = NULL;
    const char *workspace_sql =
        "INSERT INTO workspaces(id,root_path,manifest_hash,updated_at) VALUES(?1,?2,?3,?4) "
        "ON CONFLICT(id) DO UPDATE SET root_path=excluded.root_path,manifest_hash=excluded.manifest_hash,updated_at=excluded.updated_at;";
    const char *repo_sql =
        "INSERT INTO repos(repo_id,workspace_id,manifest_name,path,abs_path,status,generation) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(repo_id) DO UPDATE SET "
        "manifest_name=excluded.manifest_name,path=excluded.path,abs_path=excluded.abs_path,"
        "status=CASE WHEN excluded.status='missing' THEN 'missing' "
        "WHEN repos.status='missing' THEN 'discovered' "
        "WHEN repos.status IN('indexed','indexing','error') THEN repos.status ELSE excluded.status END,"
        "generation=excluded.generation;";
    if (sqlite3_prepare_v2(db, workspace_sql, -1, &workspace_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, repo_sql, -1, &repo_stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP master update", sqlite3_errmsg(db));
        goto done;
    }
    sqlite3_bind_text(workspace_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(workspace_stmt, 2, workspace->root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(workspace_stmt, 3, workspace->manifest_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(workspace_stmt, 4, (sqlite3_int64)time(NULL));
    if (sqlite3_step(workspace_stmt) != SQLITE_DONE) {
        set_error(err, err_size, "cannot update AOSP workspace", sqlite3_errmsg(db));
        goto done;
    }
    for (int i = 0; i < workspace->repo_count; i++) {
        const cbm_aosp_repo_t *repo = &workspace->repos[i];
        sqlite3_reset(repo_stmt);
        sqlite3_clear_bindings(repo_stmt);
        sqlite3_bind_text(repo_stmt, 1, repo->repo_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(repo_stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(repo_stmt, 3, repo->name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(repo_stmt, 4, repo->path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(repo_stmt, 5, repo->abs_path ? repo->abs_path : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(repo_stmt, 6, repo->exists ? "discovered" : "missing", -1, SQLITE_STATIC);
        sqlite3_bind_text(repo_stmt, 7, workspace->manifest_hash, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(repo_stmt) != SQLITE_DONE) {
            set_error(err, err_size, "cannot update AOSP repository", sqlite3_errmsg(db));
            goto done;
        }
    }
    if (sqlite3_prepare_v2(db,
            "DELETE FROM repos WHERE workspace_id=?1 AND generation<>?2;", -1, &delete_stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare stale AOSP repository cleanup", sqlite3_errmsg(db));
        goto done;
    }
    sqlite3_bind_text(delete_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_stmt, 2, workspace->manifest_hash, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
        set_error(err, err_size, "cannot remove stale AOSP repositories", sqlite3_errmsg(db));
        goto done;
    }
    rc = 0;

done:
    sqlite3_finalize(workspace_stmt);
    sqlite3_finalize(repo_stmt);
    sqlite3_finalize(delete_stmt);
    if (rc == 0) {
        if (exec_sql(db, "COMMIT;", err, err_size) != 0) rc = -1;
    } else {
        (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    return rc;
}

int cbm_aosp_master_stats(const cbm_aosp_workspace_t *workspace, cbm_aosp_master_stats_t *out,
                          char *err, size_t err_size) {
    if (!workspace || !out) return -1;
    memset(out, 0, sizeof(*out));
    char db_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, db_path, sizeof(db_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "AOSP workspace is not initialized", db_path);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT count(*),coalesce(sum(status<>'missing'),0),coalesce(sum(status='missing'),0),"
        "coalesce(sum(status='indexed'),0),coalesce(sum(status='error'),0),"
        "(SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1),"
        "(SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND status='resolved'),"
        "(SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND status='ambiguous'),"
        "(SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND status='unresolved'),"
        "(SELECT count(*) FROM cross_edge_refresh_queue WHERE workspace_id=?1),"
        "(SELECT count(*) FROM cross_symbol_edges e WHERE e.workspace_id=?1 AND EXISTS("
        "SELECT 1 FROM cross_edge_refresh_queue q WHERE q.workspace_id=e.workspace_id "
        "AND q.source_repo_id=e.source_repo_id)),"
        "(SELECT count(*) FROM cross_edge_refresh_failures WHERE workspace_id=?1) "
        "FROM repos WHERE workspace_id=?1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot read AOSP master database", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->repo_count = sqlite3_column_int(stmt, 0);
        out->existing_count = sqlite3_column_int(stmt, 1);
        out->missing_count = sqlite3_column_int(stmt, 2);
        out->indexed_count = sqlite3_column_int(stmt, 3);
        out->error_count = sqlite3_column_int(stmt, 4);
        out->cross_edge_count = sqlite3_column_int(stmt, 5);
        out->resolved_edge_count = sqlite3_column_int(stmt, 6);
        out->ambiguous_edge_count = sqlite3_column_int(stmt, 7);
        out->unresolved_edge_count = sqlite3_column_int(stmt, 8);
        out->stale_repo_count = sqlite3_column_int(stmt, 9);
        out->stale_edge_count = sqlite3_column_int(stmt, 10);
        out->refresh_failed_count = sqlite3_column_int(stmt, 11);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static int update_repo_state(const cbm_aosp_workspace_t *workspace, const cbm_aosp_repo_t *repo,
                             const char *status, const char *db_path, const char *message,
                             char *err, size_t err_size) {
    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) {
        set_error(err, err_size, "cannot resolve AOSP master database", workspace->root);
        return -1;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP master database", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 10000);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "UPDATE repos SET status=?1,db_path=?2,error_msg=?3,"
        "last_indexed_at=CASE WHEN ?1='indexed' THEN strftime('%s','now') ELSE last_indexed_at END "
        "WHERE workspace_id=?4 AND repo_id=?5;";
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP repository status update", sqlite3_errmsg(db));
        goto done;
    }
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, db_path ? db_path : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, message ? message : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, repo->repo_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1) {
        rc = 0;
    } else {
        set_error(err, err_size, "cannot update AOSP repository status", sqlite3_errmsg(db));
    }
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static bool is_catalog_symbol(const char *label) {
    static const char *labels[] = {
        "Function", "Method", "Class", "Interface", "Struct", "Enum", "Trait",
        "Type", "Namespace", "Module", "Variable", "Constant", "Field", "File", "Decorator",
    };
    if (!label) return false;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (strcmp(label, labels[i]) == 0) return true;
    }
    return false;
}

static void symbol_global_id(const cbm_aosp_repo_t *repo, const char *qualified_name,
                             const char *label, char out[CBM_AOSP_HASH_LEN + 1]) {
    cbm_sha256_ctx ctx;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_init(&ctx);
    cbm_sha256_update(&ctx, repo->repo_id, strlen(repo->repo_id));
    cbm_sha256_update(&ctx, "\0", 1);
    cbm_sha256_update(&ctx, qualified_name, strlen(qualified_name));
    cbm_sha256_update(&ctx, "\0", 1);
    cbm_sha256_update(&ctx, label, strlen(label));
    cbm_sha256_final(&ctx, digest);
    digest_hex(digest, out);
}

int cbm_aosp_catalog_repo_db(const cbm_aosp_workspace_t *workspace, const cbm_aosp_repo_t *repo,
                             const char *shard_path, char *err, size_t err_size) {
    sqlite3 *shard = NULL;
    sqlite3 *master = NULL;
    sqlite3_stmt *read_stmt = NULL;
    sqlite3_stmt *insert_stmt = NULL;
    sqlite3_stmt *coverage_stmt = NULL;
    sqlite3_stmt *self_queue_stmt = NULL;
    sqlite3_stmt *incoming_queue_stmt = NULL;
    sqlite3_stmt *candidate_queue_stmt = NULL;
    int rc = -1;
    if (sqlite3_open_v2(shard_path, &shard, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP repository graph", sqlite3_errmsg(shard));
        goto done;
    }
    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0 ||
        sqlite3_open(master_path, &master) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP master database",
                  master ? sqlite3_errmsg(master) : master_path);
        goto done;
    }
    sqlite3_busy_timeout(master, 10000);
    if (exec_sql(master, "BEGIN IMMEDIATE;", err, err_size) != 0) goto done;

    if (sqlite3_prepare_v2(master,
            "INSERT INTO cross_edge_refresh_queue(workspace_id,source_repo_id,reason,queued_at) "
            "VALUES(?1,?2,'source_reindexed',strftime('%s','now')) "
            "ON CONFLICT(workspace_id,source_repo_id) DO UPDATE SET "
            "reason=excluded.reason,queued_at=excluded.queued_at;",
            -1, &self_queue_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "INSERT INTO cross_edge_refresh_queue(workspace_id,source_repo_id,reason,queued_at) "
            "SELECT ?1,source_repo_id,'target_reindexed',strftime('%s','now') "
            "FROM cross_symbol_edges WHERE workspace_id=?1 AND target_repo_id=?2 "
            "GROUP BY source_repo_id ON CONFLICT(workspace_id,source_repo_id) DO UPDATE SET "
            "reason=excluded.reason,queued_at=excluded.queued_at;",
            -1, &incoming_queue_stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP cross-edge invalidation", sqlite3_errmsg(master));
        goto rollback;
    }
    sqlite3_bind_text(self_queue_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(self_queue_stmt, 2, repo->repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(incoming_queue_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(incoming_queue_stmt, 2, repo->repo_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(self_queue_stmt) != SQLITE_DONE ||
        sqlite3_step(incoming_queue_stmt) != SQLITE_DONE) {
        set_error(err, err_size, "cannot queue AOSP cross-edge invalidation", sqlite3_errmsg(master));
        goto rollback;
    }

    sqlite3_stmt *delete_stmt = NULL;
    if (sqlite3_prepare_v2(master, "DELETE FROM symbols WHERE repo_id=?1;", -1,
                           &delete_stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP symbol refresh", sqlite3_errmsg(master));
        sqlite3_finalize(delete_stmt);
        goto rollback;
    }
    sqlite3_bind_text(delete_stmt, 1, repo->repo_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
        set_error(err, err_size, "cannot clear stale AOSP symbols", sqlite3_errmsg(master));
        sqlite3_finalize(delete_stmt);
        goto rollback;
    }
    sqlite3_finalize(delete_stmt);

    const char *read_sql =
        "SELECT id,name,qualified_name,label,file_path,start_line,end_line,properties FROM nodes;";
    const char *insert_sql =
        "INSERT INTO symbols(global_id,workspace_id,repo_id,local_node_id,name,qualified_name,label,"
        "qualified_leaf,file_path,start_line,end_line,properties) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);";
    if (sqlite3_prepare_v2(shard, read_sql, -1, &read_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master, insert_sql, -1, &insert_stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP symbol catalog", sqlite3_errmsg(master));
        goto rollback;
    }
    int symbol_count = 0;
    while (sqlite3_step(read_stmt) == SQLITE_ROW) {
        const char *label = (const char *)sqlite3_column_text(read_stmt, 3);
        if (!is_catalog_symbol(label)) continue;
        const char *name = (const char *)sqlite3_column_text(read_stmt, 1);
        const char *qualified_name = (const char *)sqlite3_column_text(read_stmt, 2);
        const char *file_path = (const char *)sqlite3_column_text(read_stmt, 4);
        const char *properties = (const char *)sqlite3_column_text(read_stmt, 7);
        if (!name || !qualified_name || !qualified_name[0]) continue;
        char global_id[CBM_AOSP_HASH_LEN + 1];
        symbol_global_id(repo, qualified_name, label, global_id);
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        sqlite3_bind_text(insert_stmt, 1, global_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 3, repo->repo_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_stmt, 4, sqlite3_column_int64(read_stmt, 0));
        sqlite3_bind_text(insert_stmt, 5, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 6, qualified_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 7, label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 8, aosp_symbol_leaf(qualified_name), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 9, file_path ? file_path : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert_stmt, 10, sqlite3_column_int(read_stmt, 5));
        sqlite3_bind_int(insert_stmt, 11, sqlite3_column_int(read_stmt, 6));
        sqlite3_bind_text(insert_stmt, 12, properties ? properties : "{}", -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            set_error(err, err_size, "cannot write AOSP symbol catalog", sqlite3_errmsg(master));
            goto rollback;
        }
        symbol_count++;
    }
    if (sqlite3_errcode(shard) != SQLITE_DONE && sqlite3_errcode(shard) != SQLITE_OK) {
        set_error(err, err_size, "cannot read AOSP repository symbols", sqlite3_errmsg(shard));
        goto rollback;
    }

    if (sqlite3_prepare_v2(master,
            "INSERT INTO cross_edge_refresh_queue(workspace_id,source_repo_id,reason,queued_at) "
            "SELECT DISTINCT e.workspace_id,e.source_repo_id,'candidate_catalog_changed',"
            "strftime('%s','now') FROM cross_symbol_edges e JOIN symbols s ON "
            "s.workspace_id=e.workspace_id AND s.repo_id=?2 AND "
            "(s.name=e.target_leaf OR (e.target_leaf='' AND (e.target_name=s.name OR "
            "e.target_name LIKE '%.'||s.name OR e.target_name LIKE '%/'||s.name))) "
            "WHERE e.workspace_id=?1 AND e.status='unresolved' "
            "ON CONFLICT(workspace_id,source_repo_id) DO UPDATE SET "
            "reason=excluded.reason,queued_at=excluded.queued_at;",
            -1, &candidate_queue_stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP candidate invalidation", sqlite3_errmsg(master));
        goto rollback;
    }
    sqlite3_bind_text(candidate_queue_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(candidate_queue_stmt, 2, repo->repo_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(candidate_queue_stmt) != SQLITE_DONE) {
        set_error(err, err_size, "cannot queue AOSP candidate invalidation", sqlite3_errmsg(master));
        goto rollback;
    }

    if (sqlite3_prepare_v2(master,
            "INSERT INTO coverage(workspace_id,repo_id,kind,detail) VALUES(?1,?2,'symbols',?3) "
            "ON CONFLICT(workspace_id,repo_id,kind) DO UPDATE SET detail=excluded.detail;",
            -1, &coverage_stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP coverage update", sqlite3_errmsg(master));
        goto rollback;
    }
    char count_text[32];
    (void)snprintf(count_text, sizeof(count_text), "%d", symbol_count);
    sqlite3_bind_text(coverage_stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(coverage_stmt, 2, repo->repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(coverage_stmt, 3, count_text, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(coverage_stmt) != SQLITE_DONE) {
        set_error(err, err_size, "cannot update AOSP symbol coverage", sqlite3_errmsg(master));
        goto rollback;
    }
    if (exec_sql(master, "COMMIT;", err, err_size) != 0) goto done;
    rc = 0;
    goto done;

rollback:
    (void)sqlite3_exec(master, "ROLLBACK;", NULL, NULL, NULL);
done:
    sqlite3_finalize(read_stmt);
    sqlite3_finalize(insert_stmt);
    sqlite3_finalize(coverage_stmt);
    sqlite3_finalize(self_queue_stmt);
    sqlite3_finalize(incoming_queue_stmt);
    sqlite3_finalize(candidate_queue_stmt);
    sqlite3_close(shard);
    sqlite3_close(master);
    return rc;
}

int cbm_aosp_index_repo(const cbm_aosp_workspace_t *workspace, const cbm_aosp_repo_t *repo,
                        char *err, size_t err_size) {
    if (!workspace || !repo || !repo->exists || !repo->abs_path) {
        set_error(err, err_size, "AOSP repository is missing", repo ? repo->path : NULL);
        return -1;
    }
    if (cbm_aosp_master_sync(workspace, err, err_size) != 0) return -1;
    char project_name[CBM_AOSP_ID_LEN + 6];
    (void)snprintf(project_name, sizeof(project_name), "aosp-%s", repo->repo_id);
    char shard_path[AOSP_PATH_BUF];
    const char *cache_dir = cbm_resolve_cache_dir();
    if (!cache_dir) cache_dir = cbm_tmpdir();
    int written = snprintf(shard_path, sizeof(shard_path), "%s/%s.db", cache_dir, project_name);
    if (written <= 0 || (size_t)written >= sizeof(shard_path)) {
        set_error(err, err_size, "AOSP repository graph path is too long", repo->path);
        return -1;
    }
    if (update_repo_state(workspace, repo, "indexing", shard_path, "", err, err_size) != 0) {
        return -1;
    }
    char *response = cbm_mcp_index_run_supervised_named_path(repo->abs_path, project_name);
    if (!response || strstr(response, "\"isError\":true") != NULL) {
        const char *message = response ? "repository indexing failed" : "cannot start index worker";
        (void)update_repo_state(workspace, repo, "error", shard_path, message, NULL, 0);
        set_error(err, err_size, message, repo->path);
        free(response);
        return -1;
    }
    free(response);
    if (cbm_aosp_catalog_repo_db(workspace, repo, shard_path, err, err_size) != 0) {
        (void)update_repo_state(workspace, repo, "error", shard_path,
                                err && err[0] ? err : "symbol catalog failed", NULL, 0);
        return -1;
    }
    if (update_repo_state(workspace, repo, "indexed", shard_path, "", err, err_size) != 0) {
        return -1;
    }
    return 0;
}

static char *dup_column(sqlite3_stmt *stmt, int column) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return strdup(value ? (const char *)value : "");
}

void cbm_aosp_symbols_free(cbm_aosp_symbol_t *results, int count) {
    if (!results) return;
    for (int i = 0; i < count; i++) {
        free(results[i].global_id);
        free(results[i].repo_id);
        free(results[i].repo_path);
        free(results[i].manifest_name);
        free(results[i].project_name);
        free(results[i].name);
        free(results[i].qualified_name);
        free(results[i].label);
        free(results[i].language);
        free(results[i].file_path);
    }
    free(results);
}

static void aosp_symbol_clear(cbm_aosp_symbol_t *symbol) {
    if (!symbol) return;
    free(symbol->global_id);
    free(symbol->repo_id);
    free(symbol->repo_path);
    free(symbol->manifest_name);
    free(symbol->project_name);
    free(symbol->name);
    free(symbol->qualified_name);
    free(symbol->label);
    free(symbol->language);
    free(symbol->file_path);
    memset(symbol, 0, sizeof(*symbol));
}

static int aosp_symbol_from_row(sqlite3_stmt *stmt, cbm_aosp_symbol_t *item) {
    memset(item, 0, sizeof(*item));
    item->global_id = dup_column(stmt, 0);
    item->repo_id = dup_column(stmt, 1);
    item->local_node_id = sqlite3_column_int64(stmt, 2);
    item->repo_path = dup_column(stmt, 3);
    item->manifest_name = dup_column(stmt, 4);
    size_t project_len = strlen(item->repo_id ? item->repo_id : "") + 6;
    item->project_name = malloc(project_len);
    if (item->project_name) {
        (void)snprintf(item->project_name, project_len, "aosp-%s",
                       item->repo_id ? item->repo_id : "");
    }
    item->name = dup_column(stmt, 5);
    item->qualified_name = dup_column(stmt, 6);
    item->label = dup_column(stmt, 7);
    item->language = dup_column(stmt, 8);
    item->file_path = dup_column(stmt, 9);
    item->start_line = sqlite3_column_int(stmt, 10);
    item->end_line = sqlite3_column_int(stmt, 11);
    if (!item->global_id || !item->repo_id || !item->repo_path || !item->manifest_name ||
        !item->project_name || !item->name || !item->qualified_name || !item->label ||
        !item->language || !item->file_path) {
        aosp_symbol_clear(item);
        return -1;
    }
    return 0;
}

static int build_fts_query(const char *query, char *out, size_t out_size) {
    if (!query || !query[0] || !out || out_size < 5) return -1;
    size_t written = 0;
    out[written++] = '"';
    for (const char *p = query; *p; p++) {
        if ((unsigned char)*p < 0x20) continue;
        if (*p == '"') {
            if (written + 2 >= out_size) return -1;
            out[written++] = '"';
        }
        if (written + 1 >= out_size) return -1;
        out[written++] = *p;
    }
    if (written + 3 > out_size) return -1;
    out[written++] = '"';
    out[written++] = '*';
    out[written] = '\0';
    return 0;
}

int cbm_aosp_search_symbols(const cbm_aosp_workspace_t *workspace, const char *query, int limit,
                            cbm_aosp_symbol_t **results, int *count, char *err, size_t err_size) {
    if (!workspace || !results || !count || !query || !query[0]) {
        set_error(err, err_size, "AOSP symbol query is required", NULL);
        return -1;
    }
    *results = NULL;
    *count = 0;
    if (limit <= 0) limit = 20;
    if (limit > 200) limit = 200;
    char fts_query[1024];
    if (build_fts_query(query, fts_query, sizeof(fts_query)) != 0) {
        set_error(err, err_size, "AOSP symbol query is too long", NULL);
        return -1;
    }
    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "AOSP workspace is not initialized", master_path);
        sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT s.global_id,s.repo_id,s.local_node_id,r.path,r.manifest_name,s.name,"
        "s.qualified_name,s.label,s.language,s.file_path,s.start_line,s.end_line FROM symbols_fts "
        "JOIN symbols s ON s.id=symbols_fts.rowid JOIN repos r ON r.repo_id=s.repo_id "
        "WHERE symbols_fts MATCH ?1 AND s.workspace_id=?2 "
        "ORDER BY bm25(symbols_fts),s.name,r.path LIMIT ?3;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP symbol search", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    cbm_aosp_symbol_t *items = calloc((size_t)limit, sizeof(*items));
    if (!items) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        set_error(err, err_size, "out of memory", NULL);
        return -1;
    }
    int n = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cbm_aosp_symbol_t *item = &items[n];
        if (aosp_symbol_from_row(stmt, item) != 0) {
            cbm_aosp_symbols_free(items, n + 1);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            set_error(err, err_size, "out of memory", NULL);
            return -1;
        }
        n++;
    }
    if (step_rc != SQLITE_DONE) {
        set_error(err, err_size, "cannot search AOSP symbols", sqlite3_errmsg(db));
        cbm_aosp_symbols_free(items, n);
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

static int normalize_symbol_reference(const char *reference, char *out, size_t out_size) {
    while (reference && isspace((unsigned char)*reference)) reference++;
    if (!reference || !reference[0] || !out || out_size < 2) return -1;
    size_t input_len = strlen(reference);
    while (input_len > 0 && isspace((unsigned char)reference[input_len - 1])) input_len--;
    if (input_len == 0 || input_len >= out_size) return -1;
    size_t written = 0;
    for (size_t i = 0; i < input_len; i++) {
        char c = reference[i];
        bool separator = c == '/' || c == '\\' || c == ':' ||
                         (c == '-' && i + 1 < input_len && reference[i + 1] == '>');
        if (separator) {
            if (c == '-' || (c == ':' && i + 1 < input_len && reference[i + 1] == ':')) i++;
            while (i + 1 < input_len && reference[i + 1] == ':') i++;
            if (written > 0 && out[written - 1] != '.') out[written++] = '.';
        } else {
            out[written++] = c;
        }
    }
    while (written > 0 && out[written - 1] == '.') written--;
    out[written] = '\0';
    return written > 0 ? 0 : -1;
}

static bool symbol_qn_has_suffix(const char *qualified_name, const char *reference) {
    size_t qn_len = strlen(qualified_name);
    size_t ref_len = strlen(reference);
    if (ref_len > qn_len || strcmp(qualified_name + qn_len - ref_len, reference) != 0) {
        return false;
    }
    return qn_len == ref_len || qualified_name[qn_len - ref_len - 1] == '.';
}

static int symbol_match_score(sqlite3_stmt *stmt, const char *raw_reference,
                              const char *normalized_reference, bool qualified) {
    const char *global_id = (const char *)sqlite3_column_text(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 5);
    const char *qualified_name = (const char *)sqlite3_column_text(stmt, 6);
    if (!global_id || !name || !qualified_name) return 0;
    if (strcmp(global_id, raw_reference) == 0) return 400;
    char normalized_qn[AOSP_PATH_BUF];
    bool qn_normalized = normalize_symbol_reference(qualified_name, normalized_qn,
                                                     sizeof(normalized_qn)) == 0;
    if (strcmp(qualified_name, raw_reference) == 0 ||
        strcmp(qualified_name, normalized_reference) == 0 ||
        (qn_normalized && strcmp(normalized_qn, normalized_reference) == 0)) return 300;
    if (qualified && ((qn_normalized && symbol_qn_has_suffix(normalized_qn, normalized_reference)) ||
                      symbol_qn_has_suffix(qualified_name, raw_reference))) return 200;
    if (!qualified && strcmp(name, normalized_reference) == 0) return 100;
    return 0;
}

static cbm_aosp_symbol_match_kind_t symbol_match_kind(int score) {
    if (score == 400) return CBM_AOSP_SYMBOL_MATCH_GLOBAL_ID;
    if (score == 300) return CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME;
    if (score == 200) return CBM_AOSP_SYMBOL_MATCH_QUALIFIED_SUFFIX;
    if (score == 100) return CBM_AOSP_SYMBOL_MATCH_EXACT_NAME;
    return CBM_AOSP_SYMBOL_MATCH_NONE;
}

void cbm_aosp_symbol_resolution_free(cbm_aosp_symbol_resolution_t *resolution) {
    if (!resolution) return;
    cbm_aosp_symbols_free(resolution->candidates, resolution->candidate_count);
    memset(resolution, 0, sizeof(*resolution));
}

int cbm_aosp_resolve_symbol(const cbm_aosp_workspace_t *workspace, const char *reference,
                            cbm_aosp_symbol_resolution_t *out, char *err, size_t err_size) {
    if (!workspace || !reference || !out) {
        set_error(err, err_size, "AOSP symbol reference is required", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char normalized[AOSP_SYMBOL_REF_BUF];
    if (normalize_symbol_reference(reference, normalized, sizeof(normalized)) != 0) {
        set_error(err, err_size, "AOSP symbol reference is empty or too long", NULL);
        return -1;
    }
    while (isspace((unsigned char)*reference)) reference++;
    char raw[AOSP_SYMBOL_REF_BUF];
    size_t raw_len = strlen(reference);
    while (raw_len > 0 && isspace((unsigned char)reference[raw_len - 1])) raw_len--;
    memcpy(raw, reference, raw_len);
    raw[raw_len] = '\0';
    bool qualified = strchr(normalized, '.') != NULL;
    const char *leaf = aosp_symbol_leaf(normalized);

    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "AOSP workspace is not initialized", master_path);
        sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT s.global_id,s.repo_id,s.local_node_id,r.path,r.manifest_name,s.name,"
        "s.qualified_name,s.label,s.language,s.file_path,s.start_line,s.end_line "
        "FROM symbols s JOIN repos r ON r.repo_id=s.repo_id "
        "WHERE s.workspace_id=?1 AND (s.global_id=?2 OR s.qualified_name=?2 "
        "OR s.qualified_name=?3 OR s.qualified_leaf=?4) "
        "ORDER BY s.qualified_name,r.path,s.global_id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP symbol resolution", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, raw, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, normalized, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, leaf, -1, SQLITE_TRANSIENT);

    cbm_aosp_symbol_t *items = calloc(AOSP_RESOLVE_LIMIT, sizeof(*items));
    if (!items) {
        set_error(err, err_size, "out of memory", NULL);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    int best_score = 0;
    int stored = 0;
    int total = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int score = symbol_match_score(stmt, raw, normalized, qualified);
        if (score == 0 || score < best_score) continue;
        if (score > best_score) {
            for (int i = 0; i < stored; i++) aosp_symbol_clear(&items[i]);
            stored = 0;
            total = 0;
            best_score = score;
        }
        total++;
        if (stored < AOSP_RESOLVE_LIMIT) {
            if (aosp_symbol_from_row(stmt, &items[stored]) != 0) {
                cbm_aosp_symbols_free(items, stored);
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                set_error(err, err_size, "out of memory", NULL);
                return -1;
            }
            stored++;
        }
    }
    if (step_rc != SQLITE_DONE) {
        set_error(err, err_size, "cannot resolve AOSP symbol", sqlite3_errmsg(db));
        cbm_aosp_symbols_free(items, stored);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (total == 0) {
        free(items);
        out->status = CBM_AOSP_SYMBOL_NOT_FOUND;
        return 0;
    }
    out->status = total == 1 ? CBM_AOSP_SYMBOL_RESOLVED : CBM_AOSP_SYMBOL_AMBIGUOUS;
    out->match_kind = symbol_match_kind(best_score);
    out->candidates = items;
    out->candidate_count = stored;
    out->total_candidate_count = total;
    out->truncated = total > stored;
    return 0;
}

void cbm_aosp_shard_route_close(cbm_aosp_shard_route_t *route) {
    if (!route) return;
    sqlite3_close(route->shard);
    free(route->shard_path);
    free(route->repo_id);
    free(route->global_id);
    memset(route, 0, sizeof(*route));
}

int cbm_aosp_shard_route(const cbm_aosp_workspace_t *workspace, const char *global_id,
                         cbm_aosp_shard_route_t *out, char *err, size_t err_size) {
    if (!workspace || !global_id || !global_id[0] || !out) {
        set_error(err, err_size, "AOSP shard route requires workspace and global id", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) {
        set_error(err, err_size, "AOSP workspace is not initialized", NULL);
        return -1;
    }
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open_v2(master_path, &master, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP master database", master_path);
        sqlite3_close(master);
        return -1;
    }
    if (sqlite3_prepare_v2(master,
            "SELECT s.repo_id, s.local_node_id, r.db_path, r.status "
            "FROM symbols s JOIN repos r ON r.repo_id = s.repo_id "
            "WHERE s.workspace_id = ?1 AND s.global_id = ?2;",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP shard route lookup", sqlite3_errmsg(master));
        goto done;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, global_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        set_error(err, err_size, "AOSP symbol not found in workspace catalog", global_id);
        goto done;
    }
    const char *repo_id_text = (const char *)sqlite3_column_text(stmt, 0);
    const char *db_path_text = (const char *)sqlite3_column_text(stmt, 2);
    const char *status_text = (const char *)sqlite3_column_text(stmt, 3);
    if (!repo_id_text || !repo_id_text[0]) {
        set_error(err, err_size, "AOSP symbol has no repository", global_id);
        goto done;
    }
    if (!db_path_text || !db_path_text[0]) {
        set_error(err, err_size, "AOSP repository shard is not indexed", repo_id_text);
        goto done;
    }
    if (!status_text || strcmp(status_text, "indexed") != 0) {
        set_error(err, err_size, "AOSP repository is not indexed", repo_id_text);
        goto done;
    }
    out->repo_id = strdup(repo_id_text);
    out->global_id = strdup(global_id);
    out->shard_path = strdup(db_path_text);
    out->local_node_id = sqlite3_column_int64(stmt, 1);
    if (!out->repo_id || !out->global_id || !out->shard_path) {
        set_error(err, err_size, "out of memory", NULL);
        cbm_aosp_shard_route_close(out);
        goto done;
    }
    if (sqlite3_open_v2(out->shard_path, &out->shard, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP repository shard", out->shard_path);
        cbm_aosp_shard_route_close(out);
        goto done;
    }
    rc = 0;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(master);
    return rc;
}

int cbm_aosp_shard_route_symbol(const cbm_aosp_workspace_t *workspace,
                                const cbm_aosp_symbol_t *symbol,
                                cbm_aosp_shard_route_t *out, char *err, size_t err_size) {
    if (!workspace || !symbol || !symbol->global_id || !symbol->repo_id || !out) {
        set_error(err, err_size, "AOSP shard route requires a resolved symbol", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) {
        set_error(err, err_size, "AOSP workspace is not initialized", NULL);
        return -1;
    }
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open_v2(master_path, &master, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP master database", master_path);
        sqlite3_close(master);
        return -1;
    }
    if (sqlite3_prepare_v2(master,
            "SELECT db_path, status FROM repos "
            "WHERE workspace_id = ?1 AND repo_id = ?2;",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP shard route lookup", sqlite3_errmsg(master));
        goto done;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, symbol->repo_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        set_error(err, err_size, "AOSP repository not found in workspace", symbol->repo_id);
        goto done;
    }
    const char *db_path_text = (const char *)sqlite3_column_text(stmt, 0);
    const char *status_text = (const char *)sqlite3_column_text(stmt, 1);
    if (!db_path_text || !db_path_text[0]) {
        set_error(err, err_size, "AOSP repository shard is not indexed", symbol->repo_id);
        goto done;
    }
    if (!status_text || strcmp(status_text, "indexed") != 0) {
        set_error(err, err_size, "AOSP repository is not indexed", symbol->repo_id);
        goto done;
    }
    out->repo_id = strdup(symbol->repo_id);
    out->global_id = strdup(symbol->global_id);
    out->shard_path = strdup(db_path_text);
    out->local_node_id = symbol->local_node_id;
    if (!out->repo_id || !out->global_id || !out->shard_path) {
        set_error(err, err_size, "out of memory", NULL);
        cbm_aosp_shard_route_close(out);
        goto done;
    }
    if (sqlite3_open_v2(out->shard_path, &out->shard, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP repository shard", out->shard_path);
        cbm_aosp_shard_route_close(out);
        goto done;
    }
    rc = 0;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(master);
    return rc;
}

int cbm_aosp_shard_read_node(const cbm_aosp_shard_route_t *route, cbm_aosp_symbol_t *out,
                             char *err, size_t err_size) {
    if (!route || !route->shard || !out) {
        set_error(err, err_size, "AOSP shard route is not open", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(route->shard,
            "SELECT name, qualified_name, label, file_path, start_line, end_line "
            "FROM nodes WHERE id = ?1;",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP shard node lookup",
                  sqlite3_errmsg(route->shard));
        goto done;
    }
    sqlite3_bind_int64(stmt, 1, route->local_node_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        set_error(err, err_size, "AOSP shard node not found", NULL);
        goto done;
    }
    out->name = dup_column(stmt, 0);
    out->qualified_name = dup_column(stmt, 1);
    out->label = dup_column(stmt, 2);
    out->file_path = dup_column(stmt, 3);
    out->start_line = sqlite3_column_int(stmt, 4);
    out->end_line = sqlite3_column_int(stmt, 5);
    out->repo_id = route->repo_id ? strdup(route->repo_id) : NULL;
    out->global_id = route->global_id ? strdup(route->global_id) : NULL;
    out->local_node_id = route->local_node_id;
    out->repo_path = strdup("");
    out->manifest_name = strdup("");
    out->language = strdup("");
    if (out->repo_id) {
        size_t project_len = strlen(out->repo_id) + 6;
        out->project_name = malloc(project_len);
        if (out->project_name) {
            (void)snprintf(out->project_name, project_len, "aosp-%s", out->repo_id);
        }
    }
    if (!out->name || !out->qualified_name || !out->label || !out->file_path ||
        !out->repo_id || !out->global_id || !out->repo_path || !out->manifest_name ||
        !out->language || (out->repo_id && !out->project_name)) {
        set_error(err, err_size, "out of memory", NULL);
        aosp_symbol_clear(out);
        goto done;
    }
    rc = 0;
done:
    sqlite3_finalize(stmt);
    return rc;
}

void cbm_aosp_shard_edges_free(cbm_aosp_shard_edge_t *edges, int count) {
    if (!edges) return;
    for (int i = 0; i < count; i++) {
        free(edges[i].type);
        free(edges[i].properties);
        free(edges[i].neighbor_name);
        free(edges[i].neighbor_qualified_name);
        free(edges[i].neighbor_label);
        free(edges[i].neighbor_file_path);
    }
    free(edges);
}

int cbm_aosp_shard_read_edges(const cbm_aosp_shard_route_t *route,
                              cbm_aosp_shard_edge_direction_t direction,
                              cbm_aosp_shard_edge_t **edges, int *count,
                              char *err, size_t err_size) {
    if (!route || !route->shard || !edges || !count) {
        set_error(err, err_size, "AOSP shard route is not open", NULL);
        return -1;
    }
    *edges = NULL;
    *count = 0;
    const char *sql;
    if (direction == CBM_AOSP_SHARD_EDGE_OUTGOING) {
        sql = "SELECT e.id, e.source_id, e.target_id, e.type, e.properties, "
              "n.id, n.name, n.qualified_name, n.label, n.file_path "
              "FROM edges e JOIN nodes n ON n.id = e.target_id "
              "WHERE e.source_id = ?1 ORDER BY n.qualified_name, e.type;";
    } else {
        sql = "SELECT e.id, e.source_id, e.target_id, e.type, e.properties, "
              "n.id, n.name, n.qualified_name, n.label, n.file_path "
              "FROM edges e JOIN nodes n ON n.id = e.source_id "
              "WHERE e.target_id = ?1 ORDER BY n.qualified_name, e.type;";
    }
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(route->shard, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP shard edge lookup",
                  sqlite3_errmsg(route->shard));
        goto done;
    }
    sqlite3_bind_int64(stmt, 1, route->local_node_id);
    int n = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        n++;
    }
    if (step_rc != SQLITE_DONE) {
        set_error(err, err_size, "cannot read AOSP shard edges", sqlite3_errmsg(route->shard));
        goto done;
    }
    if (n == 0) {
        rc = 0;
        goto done;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, route->local_node_id);
    cbm_aosp_shard_edge_t *items = calloc((size_t)n, sizeof(*items));
    if (!items) {
        set_error(err, err_size, "out of memory", NULL);
        goto done;
    }
    int stored = 0;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cbm_aosp_shard_edge_t *item = &items[stored];
        memset(item, 0, sizeof(*item));
        item->edge_id = sqlite3_column_int64(stmt, 0);
        item->source_id = sqlite3_column_int64(stmt, 1);
        item->target_id = sqlite3_column_int64(stmt, 2);
        item->type = dup_column(stmt, 3);
        item->properties = dup_column(stmt, 4);
        item->neighbor_id = sqlite3_column_int64(stmt, 5);
        item->neighbor_name = dup_column(stmt, 6);
        item->neighbor_qualified_name = dup_column(stmt, 7);
        item->neighbor_label = dup_column(stmt, 8);
        item->neighbor_file_path = dup_column(stmt, 9);
        if (!item->type || !item->properties || !item->neighbor_name ||
            !item->neighbor_qualified_name || !item->neighbor_label ||
            !item->neighbor_file_path) {
            cbm_aosp_shard_edges_free(items, stored + 1);
            set_error(err, err_size, "out of memory", NULL);
            goto done;
        }
        stored++;
    }
    if (step_rc != SQLITE_DONE) {
        set_error(err, err_size, "cannot collect AOSP shard edges", sqlite3_errmsg(route->shard));
        cbm_aosp_shard_edges_free(items, stored);
        goto done;
    }
    *edges = items;
    *count = stored;
    rc = 0;
done:
    sqlite3_finalize(stmt);
    return rc;
}

/* ── Q3: Cross-shard trace_path traversal ──────────────────────── */

typedef struct {
    char *global_id;
    int depth;
    char *edge_type;     /* NULL for start */
    char *edge_evidence; /* NULL for start */
    double confidence;
    bool cross_repo;
} trace_item_t;

typedef struct {
    trace_item_t *items;
    int capacity;
    int head;
    int count;
} trace_queue_t;

typedef struct {
    char **ids;
    int count;
    int capacity;
} trace_visited_t;

static void trace_queue_init(trace_queue_t *q) {
    q->items = NULL;
    q->capacity = 0;
    q->head = 0;
    q->count = 0;
}

static void trace_queue_free(trace_queue_t *q) {
    if (!q->items) return;
    for (int i = 0; i < q->count; i++) {
        int idx = q->head + i;
        if (idx >= q->capacity) idx -= q->capacity;
        free(q->items[idx].global_id);
        free(q->items[idx].edge_type);
        free(q->items[idx].edge_evidence);
    }
    free(q->items);
    q->items = NULL;
    q->capacity = 0;
    q->head = 0;
    q->count = 0;
}

static int trace_queue_push(trace_queue_t *q, const char *global_id, int depth,
                            const char *edge_type, const char *edge_evidence,
                            double confidence, bool cross_repo) {
    if (q->count >= q->capacity) {
        int new_cap = q->capacity == 0 ? 64 : q->capacity * 2;
        trace_item_t *new_items = calloc((size_t)new_cap, sizeof(*new_items));
        if (!new_items) return -1;
        for (int i = 0; i < q->count; i++) {
            int idx = q->head + i;
            if (idx >= q->capacity) idx -= q->capacity;
            new_items[i] = q->items[idx];
        }
        free(q->items);
        q->items = new_items;
        q->capacity = new_cap;
        q->head = 0;
    }
    int tail = q->head + q->count;
    if (tail >= q->capacity) tail -= q->capacity;
    q->items[tail].global_id = strdup(global_id);
    q->items[tail].depth = depth;
    q->items[tail].edge_type = edge_type ? strdup(edge_type) : NULL;
    q->items[tail].edge_evidence = edge_evidence ? strdup(edge_evidence) : NULL;
    q->items[tail].confidence = confidence;
    q->items[tail].cross_repo = cross_repo;
    if (!q->items[tail].global_id || (edge_type && !q->items[tail].edge_type) ||
        (edge_evidence && !q->items[tail].edge_evidence)) {
        free(q->items[tail].global_id);
        free(q->items[tail].edge_type);
        free(q->items[tail].edge_evidence);
        return -1;
    }
    q->count++;
    return 0;
}

static trace_item_t trace_queue_pop(trace_queue_t *q) {
    trace_item_t item = q->items[q->head];
    q->head++;
    if (q->head >= q->capacity) q->head = 0;
    q->count--;
    return item;
}

static void trace_visited_free(trace_visited_t *v) {
    if (!v->ids) return;
    for (int i = 0; i < v->count; i++) free(v->ids[i]);
    free(v->ids);
    v->ids = NULL;
    v->count = 0;
    v->capacity = 0;
}

static bool trace_visited_contains(trace_visited_t *v, const char *id) {
    for (int i = 0; i < v->count; i++) {
        if (strcmp(v->ids[i], id) == 0) return true;
    }
    return false;
}

static int trace_visited_add(trace_visited_t *v, const char *id) {
    if (v->count >= v->capacity) {
        int new_cap = v->capacity == 0 ? 64 : v->capacity * 2;
        char **new_ids = realloc(v->ids, (size_t)new_cap * sizeof(char *));
        if (!new_ids) return -1;
        v->ids = new_ids;
        v->capacity = new_cap;
    }
    v->ids[v->count] = strdup(id);
    if (!v->ids[v->count]) return -1;
    v->count++;
    return 0;
}

void cbm_aosp_trace_result_free(cbm_aosp_trace_result_t *result) {
    if (!result) return;
    for (int i = 0; i < result->node_count; i++) {
        free(result->nodes[i].global_id);
        free(result->nodes[i].repo_id);
        free(result->nodes[i].qualified_name);
        free(result->nodes[i].label);
        free(result->nodes[i].file_path);
        free(result->nodes[i].edge_type);
        free(result->nodes[i].edge_evidence);
    }
    free(result->nodes);
    memset(result, 0, sizeof(*result));
}

int cbm_aosp_trace_path(const cbm_aosp_workspace_t *workspace,
                        const char *start_global_id,
                        const cbm_aosp_trace_options_t *options,
                        cbm_aosp_trace_result_t *out,
                        char *err, size_t err_size) {
    if (!workspace || !start_global_id || !out) {
        set_error(err, err_size, "AOSP trace requires workspace and start symbol", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));

    cbm_aosp_trace_options_t defaults;
    if (!options) {
        defaults.max_depth = -1;
        defaults.direction = CBM_AOSP_TRACE_OUTGOING;
        defaults.result_budget = 1000;
        defaults.cancel_flag = NULL;
        options = &defaults;
    }

    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) {
        set_error(err, err_size, "AOSP workspace is not initialized", NULL);
        return -1;
    }
    sqlite3 *master = NULL;
    sqlite3_stmt *sym_lookup = NULL;
    sqlite3_stmt *gid_lookup = NULL;
    sqlite3_stmt *cross_out = NULL;
    sqlite3_stmt *cross_in = NULL;
    int rc = -1;

    if (sqlite3_open_v2(master_path, &master, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP master database", master_path);
        sqlite3_close(master);
        return -1;
    }
    if (sqlite3_prepare_v2(master,
            "SELECT repo_id, local_node_id, qualified_name, label, file_path, start_line "
            "FROM symbols WHERE workspace_id=?1 AND global_id=?2;",
            -1, &sym_lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT global_id FROM symbols WHERE workspace_id=?1 AND repo_id=?2 "
            "AND local_node_id=?3;",
            -1, &gid_lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT target_global_id, type, confidence, evidence "
            "FROM cross_symbol_edges WHERE workspace_id=?1 AND source_global_id=?2 "
            "AND status='resolved';",
            -1, &cross_out, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT source_global_id, type, confidence, evidence "
            "FROM cross_symbol_edges WHERE workspace_id=?1 AND target_global_id=?2 "
            "AND status='resolved';",
            -1, &cross_in, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP trace statements", sqlite3_errmsg(master));
        goto done;
    }

    /* Verify start symbol exists */
    sqlite3_bind_text(sym_lookup, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sym_lookup, 2, start_global_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sym_lookup) != SQLITE_ROW) {
        set_error(err, err_size, "AOSP start symbol not found", start_global_id);
        goto done;
    }
    sqlite3_reset(sym_lookup);
    sqlite3_clear_bindings(sym_lookup);

    trace_queue_t queue;
    trace_visited_t visited;
    trace_queue_init(&queue);
    memset(&visited, 0, sizeof(visited));

    int result_cap = options->result_budget > 0 ? options->result_budget : 256;
    cbm_aosp_trace_node_t *results = calloc((size_t)result_cap, sizeof(*results));
    if (!results) {
        set_error(err, err_size, "out of memory", NULL);
        goto cleanup_queue;
    }

    /* Enqueue start node */
    if (trace_queue_push(&queue, start_global_id, 0, NULL, NULL, 0.0, false) != 0 ||
        trace_visited_add(&visited, start_global_id) != 0) {
        set_error(err, err_size, "out of memory", NULL);
        free(results);
        goto cleanup_queue;
    }

    bool truncated = false;
    int max_depth_reached = 0;

    while (queue.count > 0) {
        if (options->cancel_flag && *options->cancel_flag) {
            truncated = true;
            break;
        }
        trace_item_t item = trace_queue_pop(&queue);

        /* Look up symbol info from Master */
        sqlite3_reset(sym_lookup);
        sqlite3_clear_bindings(sym_lookup);
        sqlite3_bind_text(sym_lookup, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(sym_lookup, 2, item.global_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sym_lookup) != SQLITE_ROW) {
            /* Symbol disappeared between enqueue and dequeue — skip */
            free(item.global_id);
            free(item.edge_type);
            free(item.edge_evidence);
            continue;
        }

        /* Add to results */
        if (out->node_count >= result_cap) {
            if (options->result_budget > 0) {
                truncated = true;
                free(item.global_id);
                free(item.edge_type);
                free(item.edge_evidence);
                break;
            }
            int new_cap = result_cap * 2;
            cbm_aosp_trace_node_t *new_results =
                realloc(results, (size_t)new_cap * sizeof(*results));
            if (!new_results) {
                set_error(err, err_size, "out of memory", NULL);
                free(item.global_id);
                free(item.edge_type);
                free(item.edge_evidence);
                free(results);
                results = NULL;
                goto cleanup_queue;
            }
            memset(new_results + result_cap, 0,
                   (size_t)(new_cap - result_cap) * sizeof(*new_results));
            results = new_results;
            result_cap = new_cap;
        }

        cbm_aosp_trace_node_t *node = &results[out->node_count];
        memset(node, 0, sizeof(*node));
        node->global_id = item.global_id;
        node->repo_id = dup_column(sym_lookup, 0);
        node->qualified_name = dup_column(sym_lookup, 2);
        node->label = dup_column(sym_lookup, 3);
        node->file_path = dup_column(sym_lookup, 4);
        node->start_line = sqlite3_column_int(sym_lookup, 5);
        node->edge_type = item.edge_type;
        node->edge_evidence = item.edge_evidence;
        node->confidence = item.confidence;
        node->depth = item.depth;
        node->cross_repo = item.cross_repo;
        out->node_count++;
        if (item.depth > max_depth_reached) max_depth_reached = item.depth;

        /* Don't free item fields — they're now owned by the result node */
        /* item.global_id, item.edge_type, item.edge_evidence moved to node */

        /* Check depth limit */
        if (options->max_depth >= 0 && item.depth >= options->max_depth) continue;

        const char *repo_id = node->repo_id;
        if (!repo_id || !repo_id[0]) continue;

        /* Expand local edges via shard */
        cbm_aosp_shard_route_t route;
        if (cbm_aosp_shard_route(workspace, item.global_id, &route, NULL, 0) == 0) {
            /* Outgoing local edges */
            if (options->direction == CBM_AOSP_TRACE_OUTGOING ||
                options->direction == CBM_AOSP_TRACE_BOTH) {
                cbm_aosp_shard_edge_t *edges = NULL;
                int ecount = 0;
                if (cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_OUTGOING,
                                               &edges, &ecount, NULL, 0) == 0) {
                    for (int i = 0; i < ecount; i++) {
                        /* Look up neighbor global_id from Master */
                        sqlite3_reset(gid_lookup);
                        sqlite3_clear_bindings(gid_lookup);
                        sqlite3_bind_text(gid_lookup, 1, workspace->workspace_id, -1,
                                          SQLITE_TRANSIENT);
                        sqlite3_bind_text(gid_lookup, 2, repo_id, -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(gid_lookup, 3, edges[i].neighbor_id);
                        if (sqlite3_step(gid_lookup) == SQLITE_ROW) {
                            const char *nid = (const char *)sqlite3_column_text(gid_lookup, 0);
                            if (nid && !trace_visited_contains(&visited, nid)) {
                                if (trace_visited_add(&visited, nid) == 0 &&
                                    trace_queue_push(&queue, nid, item.depth + 1,
                                                     edges[i].type, "local", 1.0,
                                                     false) != 0) {
                                    /* OOM — best effort continue */
                                }
                            }
                        }
                    }
                    cbm_aosp_shard_edges_free(edges, ecount);
                }
            }
            /* Incoming local edges */
            if (options->direction == CBM_AOSP_TRACE_INCOMING ||
                options->direction == CBM_AOSP_TRACE_BOTH) {
                cbm_aosp_shard_edge_t *edges = NULL;
                int ecount = 0;
                if (cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_INCOMING,
                                               &edges, &ecount, NULL, 0) == 0) {
                    for (int i = 0; i < ecount; i++) {
                        sqlite3_reset(gid_lookup);
                        sqlite3_clear_bindings(gid_lookup);
                        sqlite3_bind_text(gid_lookup, 1, workspace->workspace_id, -1,
                                          SQLITE_TRANSIENT);
                        sqlite3_bind_text(gid_lookup, 2, repo_id, -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(gid_lookup, 3, edges[i].neighbor_id);
                        if (sqlite3_step(gid_lookup) == SQLITE_ROW) {
                            const char *nid = (const char *)sqlite3_column_text(gid_lookup, 0);
                            if (nid && !trace_visited_contains(&visited, nid)) {
                                if (trace_visited_add(&visited, nid) == 0 &&
                                    trace_queue_push(&queue, nid, item.depth + 1,
                                                     edges[i].type, "local", 1.0,
                                                     false) != 0) {
                                    /* OOM — best effort continue */
                                }
                            }
                        }
                    }
                    cbm_aosp_shard_edges_free(edges, ecount);
                }
            }
            cbm_aosp_shard_route_close(&route);
        }

        /* Expand cross-repository edges from Master */
        if (options->direction == CBM_AOSP_TRACE_OUTGOING ||
            options->direction == CBM_AOSP_TRACE_BOTH) {
            sqlite3_reset(cross_out);
            sqlite3_clear_bindings(cross_out);
            sqlite3_bind_text(cross_out, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(cross_out, 2, item.global_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(cross_out) == SQLITE_ROW) {
                const char *tid = (const char *)sqlite3_column_text(cross_out, 0);
                const char *etype = (const char *)sqlite3_column_text(cross_out, 1);
                double conf = sqlite3_column_double(cross_out, 2);
                const char *evid = (const char *)sqlite3_column_text(cross_out, 3);
                if (tid && !trace_visited_contains(&visited, tid)) {
                    if (trace_visited_add(&visited, tid) == 0 &&
                        trace_queue_push(&queue, tid, item.depth + 1, etype,
                                         evid ? evid : "cross_repo", conf,
                                         true) != 0) {
                        /* OOM — best effort continue */
                    }
                }
            }
        }
        if (options->direction == CBM_AOSP_TRACE_INCOMING ||
            options->direction == CBM_AOSP_TRACE_BOTH) {
            sqlite3_reset(cross_in);
            sqlite3_clear_bindings(cross_in);
            sqlite3_bind_text(cross_in, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(cross_in, 2, item.global_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(cross_in) == SQLITE_ROW) {
                const char *sid = (const char *)sqlite3_column_text(cross_in, 0);
                const char *etype = (const char *)sqlite3_column_text(cross_in, 1);
                double conf = sqlite3_column_double(cross_in, 2);
                const char *evid = (const char *)sqlite3_column_text(cross_in, 3);
                if (sid && !trace_visited_contains(&visited, sid)) {
                    if (trace_visited_add(&visited, sid) == 0 &&
                        trace_queue_push(&queue, sid, item.depth + 1, etype,
                                         evid ? evid : "cross_repo", conf,
                                         true) != 0) {
                        /* OOM — best effort continue */
                    }
                }
            }
        }
    }

    out->nodes = results;
    out->truncated = truncated;
    out->max_depth_reached = max_depth_reached;
    rc = 0;

cleanup_queue:
    trace_queue_free(&queue);
    trace_visited_free(&visited);
done:
    sqlite3_finalize(sym_lookup);
    sqlite3_finalize(gid_lookup);
    sqlite3_finalize(cross_out);
    sqlite3_finalize(cross_in);
    sqlite3_close(master);
    return rc;
}

/* ── Q4: Federated query_graph ─────────────────────────────────── */

typedef struct {
    char *node_id;
    char *repo_id;
    cbm_aosp_query_kind_t kind;
    int hop_index;
    char *edge_type;
    char *edge_evidence;
    double confidence;
    bool cross_repo;
} qg_queue_item_t;

typedef struct {
    qg_queue_item_t *items;
    int capacity;
    int head;
    int count;
} qg_queue_t;

static void qg_queue_init(qg_queue_t *q) {
    q->items = NULL; q->capacity = 0; q->head = 0; q->count = 0;
}

static void qg_queue_free(qg_queue_t *q) {
    if (!q->items) return;
    for (int i = 0; i < q->count; i++) {
        int idx = q->head + i;
        if (idx >= q->capacity) idx -= q->capacity;
        free(q->items[idx].node_id);
        free(q->items[idx].repo_id);
        free(q->items[idx].edge_type);
        free(q->items[idx].edge_evidence);
    }
    free(q->items);
    q->items = NULL; q->capacity = 0; q->head = 0; q->count = 0;
}

static int qg_queue_push(qg_queue_t *q, const char *node_id, const char *repo_id,
                         cbm_aosp_query_kind_t kind, int hop_index,
                         const char *edge_type, const char *edge_evidence,
                         double confidence, bool cross_repo) {
    if (q->count >= q->capacity) {
        int new_cap = q->capacity == 0 ? 64 : q->capacity * 2;
        qg_queue_item_t *ni = calloc((size_t)new_cap, sizeof(*ni));
        if (!ni) return -1;
        for (int i = 0; i < q->count; i++) {
            int idx = q->head + i;
            if (idx >= q->capacity) idx -= q->capacity;
            ni[i] = q->items[idx];
        }
        free(q->items);
        q->items = ni; q->capacity = new_cap; q->head = 0;
    }
    int tail = q->head + q->count;
    if (tail >= q->capacity) tail -= q->capacity;
    q->items[tail].node_id = strdup(node_id);
    q->items[tail].repo_id = strdup(repo_id ? repo_id : "");
    q->items[tail].kind = kind;
    q->items[tail].hop_index = hop_index;
    q->items[tail].edge_type = edge_type ? strdup(edge_type) : NULL;
    q->items[tail].edge_evidence = edge_evidence ? strdup(edge_evidence) : NULL;
    q->items[tail].confidence = confidence;
    q->items[tail].cross_repo = cross_repo;
    if (!q->items[tail].node_id || !q->items[tail].repo_id ||
        (edge_type && !q->items[tail].edge_type) ||
        (edge_evidence && !q->items[tail].edge_evidence)) {
        free(q->items[tail].node_id);
        free(q->items[tail].repo_id);
        free(q->items[tail].edge_type);
        free(q->items[tail].edge_evidence);
        return -1;
    }
    q->count++;
    return 0;
}

static qg_queue_item_t qg_queue_pop(qg_queue_t *q) {
    qg_queue_item_t item = q->items[q->head];
    q->head++;
    if (q->head >= q->capacity) q->head = 0;
    q->count--;
    return item;
}

void cbm_aosp_query_graph_result_free(cbm_aosp_query_graph_result_t *result) {
    if (!result) return;
    for (int i = 0; i < result->node_count; i++) {
        free(result->nodes[i].node_id);
        free(result->nodes[i].repo_id);
        free(result->nodes[i].name);
        free(result->nodes[i].qualified_name);
        free(result->nodes[i].file_path);
        free(result->nodes[i].edge_type);
        free(result->nodes[i].edge_evidence);
    }
    free(result->nodes);
    memset(result, 0, sizeof(*result));
}

void cbm_aosp_source_snippet_free(cbm_aosp_source_snippet_t *snippet) {
    if (!snippet)
        return;
    aosp_symbol_clear(&snippet->symbol);
    free(snippet->workspace_file_path);
    free(snippet->absolute_file_path);
    free(snippet->source);
    memset(snippet, 0, sizeof(*snippet));
}

static const cbm_aosp_repo_t *find_workspace_repo(const cbm_aosp_workspace_t *workspace,
                                                  const char *repo_id) {
    for (int i = 0; workspace && repo_id && i < workspace->repo_count; i++) {
        if (strcmp(workspace->repos[i].repo_id, repo_id) == 0) {
            return &workspace->repos[i];
        }
    }
    return NULL;
}

static int replace_owned_string(char **target, const char *value) {
    char *replacement = strdup(value ? value : "");
    if (!replacement)
        return -1;
    free(*target);
    *target = replacement;
    return 0;
}

static char *read_exact_source_lines(const char *path, int start_line, int end_line, char *err,
                                     size_t err_size) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        set_error(err, err_size, "cannot open AOSP source file", path);
        return NULL;
    }
    size_t capacity = 4096;
    size_t length = 0;
    char *source = malloc(capacity);
    if (!source) {
        (void)fclose(file);
        set_error(err, err_size, "out of memory", NULL);
        return NULL;
    }
    int line = 1;
    bool reached_end = false;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (line >= start_line && line <= end_line) {
            if (ch == '\0') {
                set_error(err, err_size, "AOSP source range contains a NUL byte", path);
                free(source);
                (void)fclose(file);
                return NULL;
            }
            if (length >= AOSP_MAX_SNIPPET_BYTES) {
                set_error(err, err_size, "AOSP source snippet exceeds size limit", path);
                free(source);
                (void)fclose(file);
                return NULL;
            }
            if (length + 1 >= capacity) {
                size_t next = capacity * 2;
                if (next > (size_t)AOSP_MAX_SNIPPET_BYTES + 1) {
                    next = (size_t)AOSP_MAX_SNIPPET_BYTES + 1;
                }
                char *grown = realloc(source, next);
                if (!grown) {
                    set_error(err, err_size, "out of memory", NULL);
                    free(source);
                    (void)fclose(file);
                    return NULL;
                }
                source = grown;
                capacity = next;
            }
            source[length++] = (char)ch;
        }
        if (ch == '\n') {
            if (line == end_line) {
                reached_end = true;
                break;
            }
            line++;
        }
    }
    if (ferror(file)) {
        set_error(err, err_size, "cannot read AOSP source file", path);
        free(source);
        (void)fclose(file);
        return NULL;
    }
    (void)fclose(file);
    if (!reached_end && !(line == end_line && length > 0)) {
        set_error(err, err_size, "AOSP source range exceeds current file", path);
        free(source);
        return NULL;
    }
    source[length] = '\0';
    return source;
}

int cbm_aosp_read_source_snippet(const cbm_aosp_workspace_t *workspace, const char *global_id,
                                 cbm_aosp_source_snippet_t *out, char *err, size_t err_size) {
    if (!workspace || !global_id || !global_id[0] || !out) {
        set_error(err, err_size, "AOSP source snippet requires workspace and global id", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    cbm_aosp_symbol_resolution_t resolution;
    if (cbm_aosp_resolve_symbol(workspace, global_id, &resolution, err, err_size) != 0) {
        return -1;
    }
    if (resolution.status != CBM_AOSP_SYMBOL_RESOLVED || resolution.candidate_count != 1) {
        set_error(err, err_size, "AOSP symbol not found in workspace catalog", global_id);
        cbm_aosp_symbol_resolution_free(&resolution);
        return -1;
    }
    const cbm_aosp_symbol_t *catalog_symbol = &resolution.candidates[0];
    cbm_aosp_shard_route_t route;
    if (cbm_aosp_shard_route_symbol(workspace, catalog_symbol, &route, err, err_size) != 0) {
        cbm_aosp_symbol_resolution_free(&resolution);
        return -1;
    }
    if (cbm_aosp_shard_read_node(&route, &out->symbol, err, err_size) != 0) {
        cbm_aosp_shard_route_close(&route);
        cbm_aosp_symbol_resolution_free(&resolution);
        return -1;
    }
    cbm_aosp_shard_route_close(&route);
    if (strcmp(out->symbol.qualified_name, catalog_symbol->qualified_name) != 0 ||
        strcmp(out->symbol.label, catalog_symbol->label) != 0) {
        set_error(err, err_size, "AOSP symbol catalog is stale", global_id);
        cbm_aosp_symbol_resolution_free(&resolution);
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    const cbm_aosp_repo_t *repo = find_workspace_repo(workspace, out->symbol.repo_id);
    if (!repo || !repo->exists || !repo->abs_path || !repo->abs_path[0]) {
        set_error(err, err_size, "AOSP symbol repository is missing", out->symbol.repo_id);
        cbm_aosp_symbol_resolution_free(&resolution);
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    if (!out->symbol.file_path || !out->symbol.file_path[0] || out->symbol.start_line < 1 ||
        out->symbol.end_line < out->symbol.start_line) {
        set_error(err, err_size, "AOSP symbol has no exact source range", global_id);
        cbm_aosp_symbol_resolution_free(&resolution);
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    if (replace_owned_string(&out->symbol.repo_path, repo->path) != 0 ||
        replace_owned_string(&out->symbol.manifest_name, repo->name) != 0 ||
        replace_owned_string(&out->symbol.language, catalog_symbol->language) != 0) {
        set_error(err, err_size, "out of memory", NULL);
        cbm_aosp_symbol_resolution_free(&resolution);
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    size_t absolute_size = strlen(repo->abs_path) + strlen(out->symbol.file_path) + 2;
    char *candidate_path = malloc(absolute_size);
    if (!candidate_path) {
        set_error(err, err_size, "out of memory", NULL);
        cbm_aosp_symbol_resolution_free(&resolution);
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    (void)snprintf(candidate_path, absolute_size, "%s/%s", repo->abs_path, out->symbol.file_path);
    char canonical_root[AOSP_PATH_BUF];
    char canonical_file[AOSP_PATH_BUF];
    bool canonicalized =
        cbm_canonical_path(repo->abs_path, canonical_root, sizeof(canonical_root)) &&
        cbm_canonical_path(candidate_path, canonical_file, sizeof(canonical_file));
    if (!canonicalized || !path_under_root(canonical_root, canonical_file)) {
        set_error(err, err_size, "AOSP source path escapes repository root", out->symbol.file_path);
        free(candidate_path);
        cbm_aosp_symbol_resolution_free(&resolution);
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    free(candidate_path);
    size_t workspace_size = strlen(repo->path) + strlen(out->symbol.file_path) + 2;
    out->workspace_file_path = malloc(workspace_size);
    out->absolute_file_path = strdup(canonical_file);
    if (!out->workspace_file_path || !out->absolute_file_path) {
        set_error(err, err_size, "out of memory", NULL);
        cbm_aosp_symbol_resolution_free(&resolution);
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    (void)snprintf(out->workspace_file_path, workspace_size, "%s/%s", repo->path,
                   out->symbol.file_path);
    out->source = read_exact_source_lines(canonical_file, out->symbol.start_line,
                                          out->symbol.end_line, err, err_size);
    cbm_aosp_symbol_resolution_free(&resolution);
    if (!out->source) {
        cbm_aosp_source_snippet_free(out);
        return -1;
    }
    return 0;
}

int cbm_aosp_query_graph(const cbm_aosp_workspace_t *workspace,
                         const char *start_global_id,
                         const cbm_aosp_query_graph_options_t *options,
                         cbm_aosp_query_graph_result_t *out,
                         char *err, size_t err_size) {
    if (!workspace || !start_global_id || !out) {
        set_error(err, err_size, "AOSP query requires workspace and start symbol", NULL);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!options || options->hop_count <= 0) {
        set_error(err, err_size, "AOSP query requires at least one hop", NULL);
        return -1;
    }

    char master_path[AOSP_PATH_BUF];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) {
        set_error(err, err_size, "AOSP workspace is not initialized", NULL);
        return -1;
    }
    sqlite3 *master = NULL;
    sqlite3_stmt *sym_lookup = NULL;
    sqlite3_stmt *gid_lookup = NULL;
    sqlite3_stmt *cross_out = NULL;
    sqlite3_stmt *cross_in = NULL;
    sqlite3_stmt *mod_by_file = NULL;
    sqlite3_stmt *mod_dep_out = NULL;
    sqlite3_stmt *mod_dep_in = NULL;
    sqlite3_stmt *mod_by_id = NULL;
    sqlite3_stmt *sym_by_file = NULL;
    sqlite3_stmt *proto_by_sym = NULL;
    sqlite3_stmt *proto_edge_out = NULL;
    sqlite3_stmt *proto_edge_in = NULL;
    sqlite3_stmt *proto_by_id = NULL;
    sqlite3_stmt *proto_sym = NULL;
    int rc = -1;

    if (sqlite3_open_v2(master_path, &master, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot open AOSP master database", master_path);
        sqlite3_close(master);
        return -1;
    }
    if (sqlite3_prepare_v2(master,
            "SELECT repo_id, local_node_id, name, qualified_name, label, file_path, start_line "
            "FROM symbols WHERE workspace_id=?1 AND global_id=?2;",
            -1, &sym_lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT global_id FROM symbols WHERE workspace_id=?1 AND repo_id=?2 "
            "AND local_node_id=?3;",
            -1, &gid_lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT target_global_id, type, confidence, evidence "
            "FROM cross_symbol_edges WHERE workspace_id=?1 AND source_global_id=?2 "
            "AND status='resolved';",
            -1, &cross_out, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT source_global_id, type, confidence, evidence "
            "FROM cross_symbol_edges WHERE workspace_id=?1 AND target_global_id=?2 "
            "AND status='resolved';",
            -1, &cross_in, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT module_id, name, module_type, file_path FROM modules "
            "WHERE workspace_id=?1 AND repo_id=?2 AND file_path=?3;",
            -1, &mod_by_file, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT target_id FROM module_dependencies WHERE source_id=?1 AND resolved=1;",
            -1, &mod_dep_out, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT source_id FROM module_dependencies WHERE target_id=?1 AND resolved=1;",
            -1, &mod_dep_in, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT module_id, name, module_type, file_path, repo_id FROM modules "
            "WHERE module_id=?1;",
            -1, &mod_by_id, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT global_id, qualified_name, label, start_line FROM symbols "
            "WHERE workspace_id=?1 AND repo_id=?2 AND file_path=?3;",
            -1, &sym_by_file, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT protocol_id, kind, name, qualified_name, file_path, repo_id "
            "FROM protocol_nodes WHERE workspace_id=?1 AND symbol_global_id=?2;",
            -1, &proto_by_sym, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT target_id, type, confidence, evidence FROM protocol_edges "
            "WHERE source_id=?1;",
            -1, &proto_edge_out, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT source_id, type, confidence, evidence FROM protocol_edges "
            "WHERE target_id=?1;",
            -1, &proto_edge_in, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT protocol_id, kind, name, qualified_name, file_path, repo_id, "
            "symbol_global_id FROM protocol_nodes WHERE protocol_id=?1;",
            -1, &proto_by_id, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(master,
            "SELECT repo_id, qualified_name, label, file_path, start_line "
            "FROM symbols WHERE workspace_id=?1 AND global_id=?2;",
            -1, &proto_sym, NULL) != SQLITE_OK) {
        set_error(err, err_size, "cannot prepare AOSP query statements", sqlite3_errmsg(master));
        goto done;
    }

    /* Verify start symbol */
    sqlite3_bind_text(sym_lookup, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sym_lookup, 2, start_global_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sym_lookup) != SQLITE_ROW) {
        set_error(err, err_size, "AOSP start symbol not found", start_global_id);
        goto done;
    }
    const char *start_repo_raw = (const char *)sqlite3_column_text(sym_lookup, 0);
    char *start_repo = strdup(start_repo_raw ? start_repo_raw : "");
    sqlite3_reset(sym_lookup);
    sqlite3_clear_bindings(sym_lookup);
    if (!start_repo) {
        set_error(err, err_size, "out of memory", NULL);
        goto done;
    }

    qg_queue_t queue;
    qg_queue_init(&queue);
    trace_visited_t visited;
    memset(&visited, 0, sizeof(visited));

    int result_cap = options->max_results > 0 ? options->max_results : 256;
    cbm_aosp_query_node_t *results = calloc((size_t)result_cap, sizeof(*results));
    if (!results) {
        set_error(err, err_size, "out of memory", NULL);
        free(start_repo);
        goto cleanup_q;
    }

    /* Enqueue start node */
    if (qg_queue_push(&queue, start_global_id, start_repo, CBM_AOSP_QUERY_KIND_SYMBOL,
                      0, NULL, NULL, 0.0, false) != 0 ||
        trace_visited_add(&visited, start_global_id) != 0) {
        set_error(err, err_size, "out of memory", NULL);
        free(results);
        results = NULL;
        free(start_repo);
        goto cleanup_q;
    }
    free(start_repo);

    bool truncated = false;

    while (queue.count > 0) {
        qg_queue_item_t item = qg_queue_pop(&queue);

        /* Determine which hop this node belongs to */
        int hop_idx = item.hop_index;
        const cbm_aosp_query_hop_t *hop = NULL;
        if (hop_idx < options->hop_count) {
            hop = &options->hops[hop_idx];
        }

        /* Look up node info and add to results */
        if (out->node_count >= result_cap) {
            if (options->max_results > 0) {
                truncated = true;
                free(item.node_id);
                free(item.repo_id);
                free(item.edge_type);
                free(item.edge_evidence);
                break;
            }
            int nc = result_cap * 2;
            cbm_aosp_query_node_t *nr = realloc(results, (size_t)nc * sizeof(*nr));
            if (!nr) {
                set_error(err, err_size, "out of memory", NULL);
                free(item.node_id);
                free(item.repo_id);
                free(item.edge_type);
                free(item.edge_evidence);
                free(results);
                results = NULL;
                goto cleanup_q;
            }
            memset(nr + result_cap, 0, (size_t)(nc - result_cap) * sizeof(*nr));
            results = nr;
            result_cap = nc;
        }

        cbm_aosp_query_node_t *qn = &results[out->node_count];
        memset(qn, 0, sizeof(*qn));
        qn->node_id = item.node_id;
        qn->repo_id = item.repo_id;
        qn->kind = item.kind;
        qn->hop_index = item.hop_index;
        qn->edge_type = item.edge_type;
        qn->edge_evidence = item.edge_evidence;
        qn->confidence = item.confidence;
        qn->cross_repo = item.cross_repo;
        qn->name = strdup("");
        qn->qualified_name = strdup("");
        qn->file_path = strdup("");
        qn->start_line = 0;

        /* Fill in node details based on kind */
        if (item.kind == CBM_AOSP_QUERY_KIND_SYMBOL) {
            sqlite3_reset(sym_lookup);
            sqlite3_clear_bindings(sym_lookup);
            sqlite3_bind_text(sym_lookup, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sym_lookup, 2, item.node_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(sym_lookup) == SQLITE_ROW) {
                free(qn->repo_id);
                free(qn->name);
                free(qn->qualified_name);
                free(qn->file_path);
                qn->repo_id = dup_column(sym_lookup, 0);
                qn->name = dup_column(sym_lookup, 2);
                qn->qualified_name = dup_column(sym_lookup, 3);
                qn->file_path = dup_column(sym_lookup, 5);
                qn->start_line = sqlite3_column_int(sym_lookup, 6);
            }
        } else if (item.kind == CBM_AOSP_QUERY_KIND_MODULE) {
            sqlite3_reset(mod_by_id);
            sqlite3_clear_bindings(mod_by_id);
            sqlite3_bind_text(mod_by_id, 1, item.node_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(mod_by_id) == SQLITE_ROW) {
                free(qn->name);
                free(qn->qualified_name);
                free(qn->file_path);
                free(qn->repo_id);
                qn->name = dup_column(mod_by_id, 1);
                qn->qualified_name = dup_column(mod_by_id, 1);
                qn->file_path = dup_column(mod_by_id, 3);
                qn->repo_id = dup_column(mod_by_id, 4);
            }
        } else if (item.kind == CBM_AOSP_QUERY_KIND_PROTOCOL) {
            sqlite3_reset(proto_by_id);
            sqlite3_clear_bindings(proto_by_id);
            sqlite3_bind_text(proto_by_id, 1, item.node_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(proto_by_id) == SQLITE_ROW) {
                free(qn->name);
                free(qn->qualified_name);
                free(qn->file_path);
                free(qn->repo_id);
                qn->name = dup_column(proto_by_id, 2);
                qn->qualified_name = dup_column(proto_by_id, 3);
                qn->file_path = dup_column(proto_by_id, 4);
                qn->repo_id = dup_column(proto_by_id, 5);
            }
        }
        out->node_count++;

        /* Don't expand beyond the last hop */
        if (!hop) continue;

        const char *cur_repo = qn->repo_id;

        /* Expand based on hop kind and current node kind */
        if (hop->kind == CBM_AOSP_QUERY_KIND_SYMBOL && item.kind == CBM_AOSP_QUERY_KIND_SYMBOL) {
            /* Follow code edges (local shard + cross-repo) */
            cbm_aosp_shard_route_t route;
            if (cbm_aosp_shard_route(workspace, item.node_id, &route, NULL, 0) == 0) {
                if (hop->direction == CBM_AOSP_TRACE_OUTGOING ||
                    hop->direction == CBM_AOSP_TRACE_BOTH) {
                    cbm_aosp_shard_edge_t *edges = NULL;
                    int ec = 0;
                    if (cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_OUTGOING,
                                                   &edges, &ec, NULL, 0) == 0) {
                        for (int i = 0; i < ec; i++) {
                            if (hop->edge_type && strcmp(hop->edge_type, edges[i].type) != 0)
                                continue;
                            sqlite3_reset(gid_lookup);
                            sqlite3_clear_bindings(gid_lookup);
                            sqlite3_bind_text(gid_lookup, 1, workspace->workspace_id, -1,
                                              SQLITE_TRANSIENT);
                            sqlite3_bind_text(gid_lookup, 2, cur_repo, -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(gid_lookup, 3, edges[i].neighbor_id);
                            if (sqlite3_step(gid_lookup) == SQLITE_ROW) {
                                const char *nid = (const char *)sqlite3_column_text(gid_lookup, 0);
                                if (nid && !trace_visited_contains(&visited, nid)) {
                                    char vid[128];
                                    (void)snprintf(vid, sizeof(vid), "S:%s", nid);
                                    if (!trace_visited_contains(&visited, vid) &&
                                        trace_visited_add(&visited, vid) == 0)
                                        (void)qg_queue_push(&queue, nid, cur_repo,
                                                            CBM_AOSP_QUERY_KIND_SYMBOL,
                                                            hop_idx + 1, edges[i].type,
                                                            "local", 1.0, false);
                                }
                            }
                        }
                        cbm_aosp_shard_edges_free(edges, ec);
                    }
                }
                if (hop->direction == CBM_AOSP_TRACE_INCOMING ||
                    hop->direction == CBM_AOSP_TRACE_BOTH) {
                    cbm_aosp_shard_edge_t *edges = NULL;
                    int ec = 0;
                    if (cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_INCOMING,
                                                   &edges, &ec, NULL, 0) == 0) {
                        for (int i = 0; i < ec; i++) {
                            if (hop->edge_type && strcmp(hop->edge_type, edges[i].type) != 0)
                                continue;
                            sqlite3_reset(gid_lookup);
                            sqlite3_clear_bindings(gid_lookup);
                            sqlite3_bind_text(gid_lookup, 1, workspace->workspace_id, -1,
                                              SQLITE_TRANSIENT);
                            sqlite3_bind_text(gid_lookup, 2, cur_repo, -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(gid_lookup, 3, edges[i].neighbor_id);
                            if (sqlite3_step(gid_lookup) == SQLITE_ROW) {
                                const char *nid = (const char *)sqlite3_column_text(gid_lookup, 0);
                                if (nid) {
                                    char vid[128];
                                    (void)snprintf(vid, sizeof(vid), "S:%s", nid);
                                    if (!trace_visited_contains(&visited, vid) &&
                                        trace_visited_add(&visited, vid) == 0)
                                        (void)qg_queue_push(&queue, nid, cur_repo,
                                                            CBM_AOSP_QUERY_KIND_SYMBOL,
                                                            hop_idx + 1, edges[i].type,
                                                            "local", 1.0, false);
                                }
                            }
                        }
                        cbm_aosp_shard_edges_free(edges, ec);
                    }
                }
                cbm_aosp_shard_route_close(&route);
            }
            /* Cross-repo edges */
            if (hop->direction == CBM_AOSP_TRACE_OUTGOING ||
                hop->direction == CBM_AOSP_TRACE_BOTH) {
                sqlite3_reset(cross_out);
                sqlite3_clear_bindings(cross_out);
                sqlite3_bind_text(cross_out, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cross_out, 2, item.node_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(cross_out) == SQLITE_ROW) {
                    const char *tid = (const char *)sqlite3_column_text(cross_out, 0);
                    const char *et = (const char *)sqlite3_column_text(cross_out, 1);
                    double conf = sqlite3_column_double(cross_out, 2);
                    const char *evidence = (const char *)sqlite3_column_text(cross_out, 3);
                    if (hop->edge_type && et && strcmp(hop->edge_type, et) != 0) continue;
                    if (tid) {
                        char vid[128];
                        (void)snprintf(vid, sizeof(vid), "S:%s", tid);
                        if (!trace_visited_contains(&visited, vid) &&
                            trace_visited_add(&visited, vid) == 0)
                            (void)qg_queue_push(&queue, tid, "", CBM_AOSP_QUERY_KIND_SYMBOL,
                                                hop_idx + 1, et,
                                                evidence ? evidence : "cross_repo", conf, true);
                    }
                }
            }
            if (hop->direction == CBM_AOSP_TRACE_INCOMING ||
                hop->direction == CBM_AOSP_TRACE_BOTH) {
                sqlite3_reset(cross_in);
                sqlite3_clear_bindings(cross_in);
                sqlite3_bind_text(cross_in, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cross_in, 2, item.node_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(cross_in) == SQLITE_ROW) {
                    const char *sid = (const char *)sqlite3_column_text(cross_in, 0);
                    const char *et = (const char *)sqlite3_column_text(cross_in, 1);
                    double conf = sqlite3_column_double(cross_in, 2);
                    const char *evidence = (const char *)sqlite3_column_text(cross_in, 3);
                    if (hop->edge_type && et && strcmp(hop->edge_type, et) != 0) continue;
                    if (sid) {
                        char vid[128];
                        (void)snprintf(vid, sizeof(vid), "S:%s", sid);
                        if (!trace_visited_contains(&visited, vid) &&
                            trace_visited_add(&visited, vid) == 0)
                            (void)qg_queue_push(&queue, sid, "", CBM_AOSP_QUERY_KIND_SYMBOL,
                                                hop_idx + 1, et,
                                                evidence ? evidence : "cross_repo", conf, true);
                    }
                }
            }
        } else if (hop->kind == CBM_AOSP_QUERY_KIND_MODULE &&
                   item.kind == CBM_AOSP_QUERY_KIND_SYMBOL) {
            /* Transition: find containing module(s) */
            sqlite3_reset(sym_lookup);
            sqlite3_clear_bindings(sym_lookup);
            sqlite3_bind_text(sym_lookup, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sym_lookup, 2, item.node_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(sym_lookup) == SQLITE_ROW) {
                const char *file = (const char *)sqlite3_column_text(sym_lookup, 5);
                if (file && file[0]) {
                    sqlite3_reset(mod_by_file);
                    sqlite3_clear_bindings(mod_by_file);
                    sqlite3_bind_text(mod_by_file, 1, workspace->workspace_id, -1,
                                      SQLITE_TRANSIENT);
                    sqlite3_bind_text(mod_by_file, 2, cur_repo, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(mod_by_file, 3, file, -1, SQLITE_TRANSIENT);
                    while (sqlite3_step(mod_by_file) == SQLITE_ROW) {
                        const char *mid = (const char *)sqlite3_column_text(mod_by_file, 0);
                        if (mid) {
                            char vid[128];
                            (void)snprintf(vid, sizeof(vid), "M:%s", mid);
                            if (!trace_visited_contains(&visited, vid) &&
                                trace_visited_add(&visited, vid) == 0)
                                (void)qg_queue_push(&queue, mid, cur_repo,
                                                    CBM_AOSP_QUERY_KIND_MODULE,
                                                    hop_idx + 1, "defined_in",
                                                    "module.file_path", 1.0, false);
                        }
                    }
                }
            }
        } else if (hop->kind == CBM_AOSP_QUERY_KIND_MODULE &&
                   item.kind == CBM_AOSP_QUERY_KIND_MODULE) {
            /* Follow module dependencies */
            if (hop->direction == CBM_AOSP_TRACE_OUTGOING ||
                hop->direction == CBM_AOSP_TRACE_BOTH) {
                sqlite3_reset(mod_dep_out);
                sqlite3_clear_bindings(mod_dep_out);
                sqlite3_bind_text(mod_dep_out, 1, item.node_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(mod_dep_out) == SQLITE_ROW) {
                    const char *tid = (const char *)sqlite3_column_text(mod_dep_out, 0);
                    if (tid) {
                        char vid[128];
                        (void)snprintf(vid, sizeof(vid), "M:%s", tid);
                        if (!trace_visited_contains(&visited, vid) &&
                            trace_visited_add(&visited, vid) == 0)
                            (void)qg_queue_push(&queue, tid, "", CBM_AOSP_QUERY_KIND_MODULE,
                                                hop_idx + 1, "depends_on",
                                                "module_dependencies", 1.0, false);
                    }
                }
            }
            if (hop->direction == CBM_AOSP_TRACE_INCOMING ||
                hop->direction == CBM_AOSP_TRACE_BOTH) {
                sqlite3_reset(mod_dep_in);
                sqlite3_clear_bindings(mod_dep_in);
                sqlite3_bind_text(mod_dep_in, 1, item.node_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(mod_dep_in) == SQLITE_ROW) {
                    const char *sid = (const char *)sqlite3_column_text(mod_dep_in, 0);
                    if (sid) {
                        char vid[128];
                        (void)snprintf(vid, sizeof(vid), "M:%s", sid);
                        if (!trace_visited_contains(&visited, vid) &&
                            trace_visited_add(&visited, vid) == 0)
                            (void)qg_queue_push(&queue, sid, "", CBM_AOSP_QUERY_KIND_MODULE,
                                                hop_idx + 1, "depended_by",
                                                "module_dependencies", 1.0, false);
                    }
                }
            }
        } else if (hop->kind == CBM_AOSP_QUERY_KIND_PROTOCOL &&
                   item.kind == CBM_AOSP_QUERY_KIND_SYMBOL) {
            /* Transition: find linked protocol node(s) */
            sqlite3_reset(proto_by_sym);
            sqlite3_clear_bindings(proto_by_sym);
            sqlite3_bind_text(proto_by_sym, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(proto_by_sym, 2, item.node_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(proto_by_sym) == SQLITE_ROW) {
                const char *pid = (const char *)sqlite3_column_text(proto_by_sym, 0);
                if (pid) {
                    char vid[128];
                    (void)snprintf(vid, sizeof(vid), "P:%s", pid);
                    if (!trace_visited_contains(&visited, vid) &&
                        trace_visited_add(&visited, vid) == 0)
                        (void)qg_queue_push(&queue, pid, cur_repo,
                                            CBM_AOSP_QUERY_KIND_PROTOCOL,
                                            hop_idx + 1, "protocol_link",
                                            "protocol_nodes.symbol_global_id", 1.0, false);
                }
            }
        } else if (hop->kind == CBM_AOSP_QUERY_KIND_PROTOCOL &&
                   item.kind == CBM_AOSP_QUERY_KIND_PROTOCOL) {
            /* Follow protocol edges */
            if (hop->direction == CBM_AOSP_TRACE_OUTGOING ||
                hop->direction == CBM_AOSP_TRACE_BOTH) {
                sqlite3_reset(proto_edge_out);
                sqlite3_clear_bindings(proto_edge_out);
                sqlite3_bind_text(proto_edge_out, 1, item.node_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(proto_edge_out) == SQLITE_ROW) {
                    const char *tid = (const char *)sqlite3_column_text(proto_edge_out, 0);
                    const char *et = (const char *)sqlite3_column_text(proto_edge_out, 1);
                    double conf = sqlite3_column_double(proto_edge_out, 2);
                    const char *evidence = (const char *)sqlite3_column_text(proto_edge_out, 3);
                    if (hop->edge_type && et && strcmp(hop->edge_type, et) != 0) continue;
                    if (tid) {
                        char vid[128];
                        (void)snprintf(vid, sizeof(vid), "P:%s", tid);
                        if (!trace_visited_contains(&visited, vid) &&
                            trace_visited_add(&visited, vid) == 0)
                            (void)qg_queue_push(&queue, tid, "", CBM_AOSP_QUERY_KIND_PROTOCOL,
                                                hop_idx + 1, et, evidence, conf, false);
                    }
                }
            }
            if (hop->direction == CBM_AOSP_TRACE_INCOMING ||
                hop->direction == CBM_AOSP_TRACE_BOTH) {
                sqlite3_reset(proto_edge_in);
                sqlite3_clear_bindings(proto_edge_in);
                sqlite3_bind_text(proto_edge_in, 1, item.node_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(proto_edge_in) == SQLITE_ROW) {
                    const char *sid = (const char *)sqlite3_column_text(proto_edge_in, 0);
                    const char *et = (const char *)sqlite3_column_text(proto_edge_in, 1);
                    double conf = sqlite3_column_double(proto_edge_in, 2);
                    const char *evidence = (const char *)sqlite3_column_text(proto_edge_in, 3);
                    if (hop->edge_type && et && strcmp(hop->edge_type, et) != 0) continue;
                    if (sid) {
                        char vid[128];
                        (void)snprintf(vid, sizeof(vid), "P:%s", sid);
                        if (!trace_visited_contains(&visited, vid) &&
                            trace_visited_add(&visited, vid) == 0)
                            (void)qg_queue_push(&queue, sid, "", CBM_AOSP_QUERY_KIND_PROTOCOL,
                                                hop_idx + 1, et, evidence, conf, false);
                    }
                }
            }
        }
        /* Other transitions (MODULE→SYMBOL, PROTOCOL→SYMBOL) not needed for
         * the initial Q4 contract; can be added in later tasks. */
    }

    out->nodes = results;
    out->truncated = truncated;
    rc = 0;

cleanup_q:
    qg_queue_free(&queue);
    trace_visited_free(&visited);
done:
    sqlite3_finalize(sym_lookup);
    sqlite3_finalize(gid_lookup);
    sqlite3_finalize(cross_out);
    sqlite3_finalize(cross_in);
    sqlite3_finalize(mod_by_file);
    sqlite3_finalize(mod_dep_out);
    sqlite3_finalize(mod_dep_in);
    sqlite3_finalize(mod_by_id);
    sqlite3_finalize(sym_by_file);
    sqlite3_finalize(proto_by_sym);
    sqlite3_finalize(proto_edge_out);
    sqlite3_finalize(proto_edge_in);
    sqlite3_finalize(proto_by_id);
    sqlite3_finalize(proto_sym);
    sqlite3_close(master);
    return rc;
}

static const char *aosp_resolution_status_name(cbm_aosp_symbol_resolution_status_t status) {
    switch (status) {
        case CBM_AOSP_SYMBOL_RESOLVED: return "resolved";
        case CBM_AOSP_SYMBOL_AMBIGUOUS: return "ambiguous";
        case CBM_AOSP_SYMBOL_NOT_FOUND: return "not_found";
    }
    return "not_found";
}

static const char *aosp_match_kind_name(cbm_aosp_symbol_match_kind_t kind) {
    switch (kind) {
        case CBM_AOSP_SYMBOL_MATCH_GLOBAL_ID: return "global_id";
        case CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME: return "exact_qualified_name";
        case CBM_AOSP_SYMBOL_MATCH_QUALIFIED_SUFFIX: return "qualified_suffix";
        case CBM_AOSP_SYMBOL_MATCH_EXACT_NAME: return "exact_name";
        case CBM_AOSP_SYMBOL_MATCH_NONE: return "none";
    }
    return "none";
}

static const char *aosp_trace_direction_name(cbm_aosp_trace_direction_t direction) {
    switch (direction) {
        case CBM_AOSP_TRACE_OUTGOING: return "outgoing";
        case CBM_AOSP_TRACE_INCOMING: return "incoming";
        case CBM_AOSP_TRACE_BOTH: return "both";
    }
    return "outgoing";
}

static const char *aosp_query_kind_name(cbm_aosp_query_kind_t kind) {
    switch (kind) {
        case CBM_AOSP_QUERY_KIND_SYMBOL: return "symbol";
        case CBM_AOSP_QUERY_KIND_MODULE: return "module";
        case CBM_AOSP_QUERY_KIND_PROTOCOL: return "protocol";
    }
    return "symbol";
}

static int aosp_parse_direction(const char *value, cbm_aosp_trace_direction_t *out) {
    if (!value || !out) return -1;
    if (strcmp(value, "outgoing") == 0 || strcmp(value, "out") == 0) {
        *out = CBM_AOSP_TRACE_OUTGOING;
    } else if (strcmp(value, "incoming") == 0 || strcmp(value, "in") == 0) {
        *out = CBM_AOSP_TRACE_INCOMING;
    } else if (strcmp(value, "both") == 0) {
        *out = CBM_AOSP_TRACE_BOTH;
    } else {
        return -1;
    }
    return 0;
}

static int aosp_parse_query_kind(const char *value, size_t length,
                                 cbm_aosp_query_kind_t *out) {
    if (!value || !out) return -1;
    if (length == strlen("symbol") && strncmp(value, "symbol", length) == 0) {
        *out = CBM_AOSP_QUERY_KIND_SYMBOL;
    } else if (length == strlen("module") && strncmp(value, "module", length) == 0) {
        *out = CBM_AOSP_QUERY_KIND_MODULE;
    } else if (length == strlen("protocol") && strncmp(value, "protocol", length) == 0) {
        *out = CBM_AOSP_QUERY_KIND_PROTOCOL;
    } else {
        return -1;
    }
    return 0;
}

static int aosp_parse_query_hop(const char *spec, cbm_aosp_query_hop_t *out,
                                char **owned_edge_type) {
    if (!spec || !out || !owned_edge_type) return -1;
    const char *first = strchr(spec, ':');
    if (!first || first == spec || first[1] == '\0') return -1;
    const char *second = strchr(first + 1, ':');
    size_t direction_length = second ? (size_t)(second - first - 1) : strlen(first + 1);
    char direction[16];
    if (direction_length == 0 || direction_length >= sizeof(direction)) return -1;
    memcpy(direction, first + 1, direction_length);
    direction[direction_length] = '\0';
    if (aosp_parse_query_kind(spec, (size_t)(first - spec), &out->kind) != 0 ||
        aosp_parse_direction(direction, &out->direction) != 0) {
        return -1;
    }
    *owned_edge_type = NULL;
    out->edge_type = NULL;
    if (second) {
        if (second[1] == '\0') return -1;
        *owned_edge_type = strdup(second + 1);
        if (!*owned_edge_type) return -1;
        out->edge_type = *owned_edge_type;
    }
    return 0;
}

static void aosp_free_query_edges(char **edge_types, int count) {
    if (!edge_types) return;
    for (int i = 0; i < count; i++) free(edge_types[i]);
}

static int aosp_parse_int(const char *value, int minimum, int maximum, int *out) {
    if (!value || !value[0] || !out) return -1;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int aosp_resolve_query_start(const cbm_aosp_workspace_t *workspace,
                                    const char *reference, char **global_id,
                                    char *err, size_t err_size) {
    cbm_aosp_symbol_resolution_t resolution;
    if (!global_id) return -1;
    *global_id = NULL;
    if (cbm_aosp_resolve_symbol(workspace, reference, &resolution, err, err_size) != 0) {
        return -1;
    }
    if (resolution.status == CBM_AOSP_SYMBOL_NOT_FOUND) {
        set_error(err, err_size, "symbol not found", reference);
    } else if (resolution.status == CBM_AOSP_SYMBOL_AMBIGUOUS) {
        char detail[128];
        (void)snprintf(detail, sizeof(detail), "%s (%d candidates)", reference,
                       resolution.total_candidate_count);
        set_error(err, err_size, "ambiguous symbol reference", detail);
    } else if (resolution.candidate_count != 1 || !resolution.candidates[0].global_id) {
        set_error(err, err_size, "invalid symbol resolution", reference);
    } else {
        *global_id = strdup(resolution.candidates[0].global_id);
    }
    cbm_aosp_symbol_resolution_free(&resolution);
    if (!*global_id && (!err || !err[0])) set_error(err, err_size, "out of memory", NULL);
    return *global_id ? 0 : -1;
}

static void print_aosp_usage(FILE *stream) {
    (void)fprintf(stream,
        "Usage:\n"
        "  codebase-memory-mcp aosp init [root]\n"
        "  codebase-memory-mcp aosp index [root] [--repo manifest-path]\n"
        "  codebase-memory-mcp aosp build [root]\n"
        "  codebase-memory-mcp aosp modules [root] [--query text] [--limit N]\n"
        "  codebase-memory-mcp aosp link [root]\n"
        "  codebase-memory-mcp aosp federate [root]\n"
        "  codebase-memory-mcp aosp protocols [root] [--query text] [--limit N]\n"
        "  codebase-memory-mcp aosp status [root]\n"
        "  codebase-memory-mcp aosp repos [root]\n"
        "  codebase-memory-mcp aosp search <query> [root] [--limit N]\n"
        "  codebase-memory-mcp aosp resolve <reference> [root]\n"
        "  codebase-memory-mcp aosp snippet <global-id> [root]\n"
        "  codebase-memory-mcp aosp trace <reference> [root] [--depth N] "
        "[--direction outgoing|incoming|both] [--limit N]\n"
        "  codebase-memory-mcp aosp query <reference> [root] "
        "--hop kind:direction[:edge-type] [--hop ...] [--limit N]\n");
}

int cbm_cmd_aosp(int argc, char **argv) {
    if (argc < 1 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
        print_aosp_usage(argc < 1 ? stderr : stdout);
        return argc < 1 ? 1 : 0;
    }
    const char *action = argv[0];
    const char *root = ".";
    const char *repo_filter = NULL;
    const char *search_query = NULL;
    int search_limit = 20;
    const char *query_reference = NULL;
    int trace_depth = 3;
    cbm_aosp_trace_direction_t trace_direction = CBM_AOSP_TRACE_OUTGOING;
    cbm_aosp_query_hop_t query_hops[64] = {0};
    char *query_edge_types[64] = {0};
    int query_hop_count = 0;
    if (strcmp(action, "search") == 0) {
        if (argc < 2) {
            print_aosp_usage(stderr);
            return 1;
        }
        search_query = argv[1];
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
                search_limit = atoi(argv[++i]);
            } else if (argv[i][0] != '-') {
                root = argv[i];
            } else {
                print_aosp_usage(stderr);
                return 1;
            }
        }
    } else if (strcmp(action, "resolve") == 0 || strcmp(action, "snippet") == 0 ||
               strcmp(action, "trace") == 0 || strcmp(action, "query") == 0) {
        if (argc < 2) {
            print_aosp_usage(stderr);
            return 1;
        }
        query_reference = argv[1];
        bool root_set = false;
        for (int i = 2; i < argc; i++) {
            if ((strcmp(action, "trace") == 0 || strcmp(action, "query") == 0) &&
                strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
                if (aosp_parse_int(argv[++i], 1, 10000, &search_limit) != 0) {
                    (void)fprintf(stderr, "error: --limit must be between 1 and 10000\n");
                    aosp_free_query_edges(query_edge_types, query_hop_count);
                    return 1;
                }
            } else if (strcmp(action, "trace") == 0 &&
                       strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
                if (aosp_parse_int(argv[++i], 0, 1000, &trace_depth) != 0) {
                    (void)fprintf(stderr, "error: --depth must be between 0 and 1000\n");
                    return 1;
                }
            } else if (strcmp(action, "trace") == 0 &&
                       strcmp(argv[i], "--direction") == 0 && i + 1 < argc) {
                if (aosp_parse_direction(argv[++i], &trace_direction) != 0) {
                    (void)fprintf(stderr, "error: invalid trace direction\n");
                    return 1;
                }
            } else if (strcmp(action, "query") == 0 && strcmp(argv[i], "--hop") == 0 &&
                       i + 1 < argc) {
                if (query_hop_count >= (int)(sizeof(query_hops) / sizeof(query_hops[0])) ||
                    aosp_parse_query_hop(argv[++i], &query_hops[query_hop_count],
                                         &query_edge_types[query_hop_count]) != 0) {
                    (void)fprintf(stderr,
                                  "error: --hop must be kind:direction[:edge-type]\n");
                    aosp_free_query_edges(query_edge_types, query_hop_count);
                    return 1;
                }
                query_hop_count++;
            } else if (argv[i][0] != '-' && !root_set) {
                root = argv[i];
                root_set = true;
            } else {
                print_aosp_usage(stderr);
                aosp_free_query_edges(query_edge_types, query_hop_count);
                return 1;
            }
        }
        if (strcmp(action, "query") == 0 && query_hop_count == 0) {
            (void)fprintf(stderr, "error: query requires at least one --hop\n");
            return 1;
        }
    } else if (strcmp(action, "index") == 0) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
                repo_filter = argv[++i];
            } else if (argv[i][0] != '-') {
                root = argv[i];
            } else {
                print_aosp_usage(stderr);
                return 1;
            }
        }
    } else if (strcmp(action, "modules") == 0 || strcmp(action, "protocols") == 0) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--query") == 0 && i + 1 < argc) {
                search_query = argv[++i];
            } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
                search_limit = atoi(argv[++i]);
            } else if (argv[i][0] != '-') {
                root = argv[i];
            } else {
                print_aosp_usage(stderr);
                return 1;
            }
        }
    } else if (argc > 1) {
        root = argv[1];
    }
    cbm_aosp_workspace_t workspace;
    char err[1024] = {0};
    if (cbm_aosp_discover(root, &workspace, err, sizeof(err)) != 0) {
        (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP discovery failed");
        aosp_free_query_edges(query_edge_types, query_hop_count);
        return 1;
    }
    int exit_code = 0;
    if (strcmp(action, "init") == 0) {
        if (cbm_aosp_master_sync(&workspace, err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err);
            exit_code = 1;
        } else {
            char db_path[AOSP_PATH_BUF];
            (void)cbm_aosp_master_path(&workspace, db_path, sizeof(db_path), false);
            int existing = 0;
            for (int i = 0; i < workspace.repo_count; i++) existing += workspace.repos[i].exists ? 1 : 0;
            printf("AOSP workspace initialized\n");
            printf("  id: %s\n  root: %s\n  repositories: %d (%d present, %d missing)\n  master: %s\n",
                   workspace.workspace_id, workspace.root, workspace.repo_count, existing,
                   workspace.repo_count - existing, db_path);
        }
    } else if (strcmp(action, "index") == 0) {
        int selected = 0;
        int indexed = 0;
        int failed = 0;
        for (int i = 0; i < workspace.repo_count; i++) {
            cbm_aosp_repo_t *repo = &workspace.repos[i];
            if (repo_filter && strcmp(repo_filter, repo->path) != 0 &&
                strcmp(repo_filter, repo->name) != 0) {
                continue;
            }
            selected++;
            if (!repo->exists) {
                printf("missing  %s\n", repo->path);
                failed++;
                continue;
            }
            printf("indexing %s\n", repo->path);
            (void)fflush(stdout);
            err[0] = '\0';
            if (cbm_aosp_index_repo(&workspace, repo, err, sizeof(err)) == 0) {
                printf("indexed  %s\n", repo->path);
                indexed++;
            } else {
                (void)fprintf(stderr, "failed   %s: %s\n", repo->path,
                              err[0] ? err : "unknown error");
                failed++;
            }
        }
        if (selected == 0) {
            (void)fprintf(stderr, "error: repository not found: %s\n",
                          repo_filter ? repo_filter : "(none)");
            exit_code = 1;
        } else {
            printf("AOSP index complete: %d indexed, %d failed\n", indexed, failed);
            exit_code = failed == 0 ? 0 : 1;
        }
    } else if (strcmp(action, "build") == 0) {
        cbm_aosp_build_stats_t stats;
        if (cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP build scan failed");
            exit_code = 1;
        } else {
            printf("AOSP build graph complete\n");
            printf("  files: %d Android.bp, %d Android.mk, %d product Makefiles, "
                   "%d BoardConfigs, %d Bazel metadata, %d AIDL\n",
                   stats.blueprint_files, stats.make_files, stats.product_make_files,
                   stats.board_config_files, stats.bazel_metadata_files, stats.aidl_files);
            printf("  modules: %d\n  dependencies: %d (%d resolved, %d unresolved)\n",
                   stats.module_count, stats.dependency_count, stats.resolved_count,
                   stats.unresolved_count);
            printf("  defaults: %d inherited dependencies, %d cycles\n",
                   stats.inherited_dependency_count, stats.defaults_cycle_count);
            printf("  variants: %d conditional dependencies, %d branches\n",
                   stats.variant_dependency_count, stats.variant_branch_count);
            printf("  boundaries: %d namespaces (%d imports), %d packages\n",
                   stats.namespace_count, stats.namespace_import_count,
                   stats.package_count);
            printf("  rejected: %d ambiguous, %d visibility blocked, "
                   "%d unsupported visibility\n",
                   stats.ambiguous_dependency_count, stats.visibility_blocked_count,
                   stats.unsupported_visibility_count);
            printf("  generators: %d filegroups, %d genrules, %d generated dependencies, "
                   "%d tool dependencies\n",
                   stats.filegroup_count, stats.genrule_count,
                   stats.generated_dependency_count, stats.tool_dependency_count);
            printf("  declared files: %d sources, %d outputs, %d tool files, "
                   "%d tagged dependencies\n",
                   stats.source_file_count, stats.generated_output_count,
                   stats.tool_file_count, stats.tagged_dependency_count);
            printf("  Android.mk: %d includes, %d conditions, %d macro expansions, "
                   "%d unsupported expressions\n",
                   stats.make_include_count, stats.make_condition_count,
                   stats.make_macro_count, stats.make_unsupported_count);
            printf("  products: %d products, %d fragments, %d inheritance edges "
                   "(%d resolved, %d cycles)\n",
                   stats.product_count, stats.product_fragment_count,
                   stats.product_inheritance_count,
                   stats.product_inheritance_resolved_count,
                   stats.product_inheritance_cycle_count);
            printf("  product packages: %d (%d resolved, %d unresolved), "
                   "%d BoardConfigs, %d partitions\n",
                   stats.product_package_count, stats.product_package_resolved_count,
                   stats.product_package_unresolved_count, stats.board_config_count,
                   stats.product_partition_count);
            printf("  Bazel mixed builds: %d artifacts, %d targets (%d resolved), "
                   "%d dependencies (%d resolved), %d ambiguous, %d missing, %d coverage gaps\n",
                   stats.bazel_artifact_count, stats.bazel_target_count,
                   stats.bazel_target_resolved_count, stats.bazel_dependency_count,
                   stats.bazel_dependency_resolved_count, stats.bazel_ambiguous_count,
                   stats.bazel_missing_count, stats.bazel_coverage_gap_count);
            printf("  file links: %d (%d resolved, %d unresolved; %d ambiguous, "
                   "%d missing, %d unindexed), %d definition symbols\n",
                   stats.file_link_count, stats.file_link_resolved_count,
                   stats.file_link_unresolved_count, stats.file_link_ambiguous_count,
                   stats.file_link_missing_count, stats.file_link_unindexed_count,
                   stats.definition_symbol_link_count);
            printf("  generated files: %d outputs, %d consumer links "
                   "(%d resolved, %d unresolved)\n",
                   stats.generated_file_count, stats.generated_link_count,
                   stats.generated_link_resolved_count,
                   stats.generated_link_unresolved_count);
        }
    } else if (strcmp(action, "modules") == 0) {
        cbm_aosp_module_t *modules = NULL;
        int count = 0;
        if (cbm_aosp_search_modules(&workspace, search_query, search_limit, &modules, &count,
                                    err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP module search failed");
            exit_code = 1;
        } else {
            for (int i = 0; i < count; i++) {
                printf("%s\t%s\t%s\t%s\tout:%d\tin:%d\n", modules[i].repo_path,
                       modules[i].module_type, modules[i].name, modules[i].file_path,
                       modules[i].outgoing_dependencies, modules[i].incoming_dependencies);
            }
            printf("%d module%s\n", count, count == 1 ? "" : "s");
        }
        cbm_aosp_modules_free(modules, count);
    } else if (strcmp(action, "link") == 0) {
        cbm_aosp_protocol_stats_t stats;
        if (cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP protocol link failed");
            exit_code = 1;
        } else {
            printf("AOSP protocol graph complete\n");
            printf("  AIDL: %d interfaces, %d methods\n",
                   stats.aidl_interfaces, stats.aidl_methods);
            printf("  Binder: %d server, %d client edges\n",
                   stats.binder_server_edges, stats.binder_client_edges);
            printf("  JNI: %d static, %d dynamic edges\n",
                   stats.jni_static_edges, stats.jni_dynamic_edges);
            printf("  total: %d nodes, %d edges\n", stats.node_count, stats.edge_count);
        }
    } else if (strcmp(action, "federate") == 0) {
        cbm_aosp_structural_stats_t stats;
        if (cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP federation failed");
            exit_code = 1;
        } else {
            printf("AOSP structural federation complete\n");
            printf("  repositories: %d, files: %d, references: %d\n",
                   stats.repos_scanned, stats.files_scanned, stats.references_seen);
            printf("  edges: %d (%d resolved, %d ambiguous, %d unresolved)\n",
                   stats.edges.edge_count, stats.edges.resolved_count,
                   stats.edges.ambiguous_count, stats.edges.unresolved_count);
            printf("  local skipped: %d, missing sources: %d\n",
                   stats.local_references_skipped, stats.missing_sources);
        }
    } else if (strcmp(action, "protocols") == 0) {
        cbm_aosp_protocol_node_t *nodes = NULL;
        int count = 0;
        if (cbm_aosp_search_protocols(&workspace, search_query, search_limit, &nodes, &count,
                                      err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP protocol search failed");
            exit_code = 1;
        } else {
            for (int i = 0; i < count; i++) {
                printf("%s\t%s\t%s\t%s\tout:%d\tin:%d\n", nodes[i].repo_path,
                       nodes[i].kind, nodes[i].qualified_name, nodes[i].file_path,
                       nodes[i].outgoing_edges, nodes[i].incoming_edges);
            }
            printf("%d protocol node%s\n", count, count == 1 ? "" : "s");
        }
        cbm_aosp_protocol_nodes_free(nodes, count);
    } else if (strcmp(action, "status") == 0) {
        cbm_aosp_master_stats_t stats;
        if (cbm_aosp_master_stats(&workspace, &stats, err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err);
            exit_code = 1;
        } else {
            printf("AOSP workspace %s\n", workspace.workspace_id);
            printf("  repositories: %d\n  present: %d\n  missing: %d\n  indexed: %d\n  errors: %d\n",
                   stats.repo_count, stats.existing_count, stats.missing_count,
                   stats.indexed_count, stats.error_count);
            printf("  cross edges: %d\n  resolved: %d\n  ambiguous: %d\n  unresolved: %d\n",
                   stats.cross_edge_count, stats.resolved_edge_count,
                   stats.ambiguous_edge_count, stats.unresolved_edge_count);
            printf("  stale repositories: %d\n  stale edges: %d\n  refresh failures: %d\n",
                   stats.stale_repo_count, stats.stale_edge_count,
                   stats.refresh_failed_count);
        }
    } else if (strcmp(action, "repos") == 0) {
        for (int i = 0; i < workspace.repo_count; i++) {
            printf("%-10s %s\t%s\n", workspace.repos[i].exists ? "present" : "missing",
                   workspace.repos[i].path, workspace.repos[i].name);
        }
    } else if (strcmp(action, "search") == 0) {
        cbm_aosp_symbol_t *results = NULL;
        int count = 0;
        if (cbm_aosp_search_symbols(&workspace, search_query, search_limit, &results, &count,
                                    err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP search failed");
            exit_code = 1;
        } else {
            for (int i = 0; i < count; i++) {
                const cbm_aosp_symbol_t *symbol = &results[i];
                printf("%s\t%s\t%s\t%s\t%s:%d\t%s\n", symbol->repo_path,
                       symbol->project_name, symbol->label, symbol->qualified_name,
                       symbol->file_path, symbol->start_line, symbol->global_id);
            }
            printf("%d symbol%s\n", count, count == 1 ? "" : "s");
        }
        cbm_aosp_symbols_free(results, count);
    } else if (strcmp(action, "resolve") == 0) {
        cbm_aosp_symbol_resolution_t resolution;
        if (cbm_aosp_resolve_symbol(&workspace, query_reference, &resolution,
                                    err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n",
                          err[0] ? err : "AOSP symbol resolution failed");
            exit_code = 1;
        } else {
            printf("%s\t%s\t%d%s\n", aosp_resolution_status_name(resolution.status),
                   aosp_match_kind_name(resolution.match_kind),
                   resolution.total_candidate_count, resolution.truncated ? "\ttruncated" : "");
            for (int i = 0; i < resolution.candidate_count; i++) {
                const cbm_aosp_symbol_t *symbol = &resolution.candidates[i];
                printf("%s\t%s\t%s\t%s\t%s:%d\n", symbol->global_id,
                       symbol->repo_path, symbol->label, symbol->qualified_name,
                       symbol->file_path, symbol->start_line);
            }
            exit_code = resolution.status == CBM_AOSP_SYMBOL_RESOLVED ? 0 : 2;
            cbm_aosp_symbol_resolution_free(&resolution);
        }
    } else if (strcmp(action, "snippet") == 0) {
        cbm_aosp_source_snippet_t snippet;
        if (cbm_aosp_read_source_snippet(&workspace, query_reference, &snippet,
                                         err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n",
                          err[0] ? err : "AOSP source snippet read failed");
            exit_code = 1;
        } else {
            printf("%s\t%s\t%s:%d-%d\t%s\n", snippet.symbol.global_id,
                   snippet.symbol.repo_path, snippet.workspace_file_path,
                   snippet.symbol.start_line, snippet.symbol.end_line,
                   snippet.symbol.qualified_name);
            (void)fputs(snippet.source, stdout);
            if (snippet.source[0] && snippet.source[strlen(snippet.source) - 1] != '\n') {
                (void)fputc('\n', stdout);
            }
            cbm_aosp_source_snippet_free(&snippet);
        }
    } else if (strcmp(action, "trace") == 0) {
        char *global_id = NULL;
        if (aosp_resolve_query_start(&workspace, query_reference, &global_id,
                                     err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP start resolution failed");
            exit_code = 1;
        } else {
            cbm_aosp_trace_options_t options = {
                .max_depth = trace_depth,
                .direction = trace_direction,
                .result_budget = search_limit,
                .cancel_flag = NULL,
            };
            cbm_aosp_trace_result_t result;
            if (cbm_aosp_trace_path(&workspace, global_id, &options, &result,
                                    err, sizeof(err)) != 0) {
                (void)fprintf(stderr, "error: %s\n",
                              err[0] ? err : "AOSP trace failed");
                exit_code = 1;
            } else {
                printf("direction:%s\tnodes:%d\tmax-depth:%d%s\n",
                       aosp_trace_direction_name(trace_direction), result.node_count,
                       result.max_depth_reached, result.truncated ? "\ttruncated" : "");
                for (int i = 0; i < result.node_count; i++) {
                    const cbm_aosp_trace_node_t *node = &result.nodes[i];
                    printf("%d\t%s\t%s\t%s\t%s:%d\t%s\t%.3f\t%s\t%s\n",
                           node->depth, node->global_id, node->repo_id,
                           node->qualified_name, node->file_path, node->start_line,
                           node->edge_type ? node->edge_type : "start", node->confidence,
                           node->cross_repo ? "cross-repo" : "local",
                           node->edge_evidence ? node->edge_evidence : "");
                }
                cbm_aosp_trace_result_free(&result);
            }
        }
        free(global_id);
    } else if (strcmp(action, "query") == 0) {
        char *global_id = NULL;
        if (aosp_resolve_query_start(&workspace, query_reference, &global_id,
                                     err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "error: %s\n", err[0] ? err : "AOSP start resolution failed");
            exit_code = 1;
        } else {
            cbm_aosp_query_graph_options_t options = {
                .hops = query_hops,
                .hop_count = query_hop_count,
                .max_results = search_limit,
            };
            cbm_aosp_query_graph_result_t result;
            if (cbm_aosp_query_graph(&workspace, global_id, &options, &result,
                                     err, sizeof(err)) != 0) {
                (void)fprintf(stderr, "error: %s\n",
                              err[0] ? err : "AOSP federated query failed");
                exit_code = 1;
            } else {
                printf("nodes:%d\thops:%d%s\n", result.node_count, query_hop_count,
                       result.truncated ? "\ttruncated" : "");
                for (int i = 0; i < result.node_count; i++) {
                    const cbm_aosp_query_node_t *node = &result.nodes[i];
                    printf("%d\t%s\t%s\t%s\t%s\t%s:%d\t%s\t%.3f\t%s\t%s\n",
                           node->hop_index, aosp_query_kind_name(node->kind), node->node_id,
                           node->repo_id ? node->repo_id : "",
                           node->qualified_name ? node->qualified_name : node->name,
                           node->file_path ? node->file_path : "", node->start_line,
                           node->edge_type ? node->edge_type : "start", node->confidence,
                           node->cross_repo ? "cross-repo" : "local",
                           node->edge_evidence ? node->edge_evidence : "");
                }
                cbm_aosp_query_graph_result_free(&result);
            }
        }
        free(global_id);
    } else {
        print_aosp_usage(stderr);
        exit_code = 1;
    }
    cbm_aosp_workspace_free(&workspace);
    aosp_free_query_edges(query_edge_types, query_hop_count);
    return exit_code;
}
