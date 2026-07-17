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
#include "aosp/protocol_graph.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "mcp/mcp.h"
#include "tree_sitter/api.h"

#include <sqlite3.h>

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
           (path[root_len] == '\0' || path[root_len] == '/');
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
    "CREATE TABLE IF NOT EXISTS symbols("
    " id INTEGER PRIMARY KEY, global_id TEXT NOT NULL UNIQUE, workspace_id TEXT NOT NULL, repo_id TEXT NOT NULL,"
    " local_node_id INTEGER, name TEXT NOT NULL, qualified_name TEXT NOT NULL, label TEXT NOT NULL,"
    " language TEXT DEFAULT '', file_path TEXT NOT NULL, start_line INTEGER DEFAULT 0, end_line INTEGER DEFAULT 0,"
    " visibility TEXT DEFAULT '', signature TEXT DEFAULT '', docstring TEXT DEFAULT '', properties TEXT DEFAULT '{}');"
    "CREATE INDEX IF NOT EXISTS idx_aosp_symbols_name ON symbols(workspace_id,name);"
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
    "CREATE TABLE IF NOT EXISTS cross_symbol_edges("
    " source_global_id TEXT NOT NULL, target_global_id TEXT NOT NULL, type TEXT NOT NULL,"
    " confidence REAL NOT NULL DEFAULT 0, evidence TEXT DEFAULT '', properties TEXT DEFAULT '{}',"
    " PRIMARY KEY(source_global_id,target_global_id,type));"
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
        "SELECT count(*),sum(status<>'missing'),sum(status='missing'),sum(status='indexed'),sum(status='error') "
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
        "Type", "Namespace", "Module", "Variable", "Constant", "Field",
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
        "file_path,start_line,end_line,properties) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);";
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
        sqlite3_bind_text(insert_stmt, 8, file_path ? file_path : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert_stmt, 9, sqlite3_column_int(read_stmt, 5));
        sqlite3_bind_int(insert_stmt, 10, sqlite3_column_int(read_stmt, 6));
        sqlite3_bind_text(insert_stmt, 11, properties ? properties : "{}", -1, SQLITE_TRANSIENT);
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
        free(results[i].repo_path);
        free(results[i].manifest_name);
        free(results[i].project_name);
        free(results[i].name);
        free(results[i].qualified_name);
        free(results[i].label);
        free(results[i].file_path);
    }
    free(results);
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
        "SELECT r.path,r.manifest_name,r.repo_id,s.name,s.qualified_name,s.label,s.file_path,"
        "s.start_line,s.end_line FROM symbols_fts "
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
        item->repo_path = dup_column(stmt, 0);
        item->manifest_name = dup_column(stmt, 1);
        const unsigned char *repo_id = sqlite3_column_text(stmt, 2);
        size_t project_len = strlen(repo_id ? (const char *)repo_id : "") + 6;
        item->project_name = malloc(project_len);
        if (item->project_name) {
            (void)snprintf(item->project_name, project_len, "aosp-%s",
                           repo_id ? (const char *)repo_id : "");
        }
        item->name = dup_column(stmt, 3);
        item->qualified_name = dup_column(stmt, 4);
        item->label = dup_column(stmt, 5);
        item->file_path = dup_column(stmt, 6);
        item->start_line = sqlite3_column_int(stmt, 7);
        item->end_line = sqlite3_column_int(stmt, 8);
        if (!item->repo_path || !item->manifest_name || !item->project_name || !item->name ||
            !item->qualified_name || !item->label || !item->file_path) {
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

static void print_aosp_usage(FILE *stream) {
    (void)fprintf(stream,
        "Usage:\n"
        "  codebase-memory-mcp aosp init [root]\n"
        "  codebase-memory-mcp aosp index [root] [--repo manifest-path]\n"
        "  codebase-memory-mcp aosp build [root]\n"
        "  codebase-memory-mcp aosp modules [root] [--query text] [--limit N]\n"
        "  codebase-memory-mcp aosp link [root]\n"
        "  codebase-memory-mcp aosp protocols [root] [--query text] [--limit N]\n"
        "  codebase-memory-mcp aosp status [root]\n"
        "  codebase-memory-mcp aosp repos [root]\n"
        "  codebase-memory-mcp aosp search <query> [root] [--limit N]\n");
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
            printf("  files: %d Android.bp, %d Android.mk, %d AIDL\n",
                   stats.blueprint_files, stats.make_files, stats.aidl_files);
            printf("  modules: %d\n  dependencies: %d (%d resolved, %d unresolved)\n",
                   stats.module_count, stats.dependency_count, stats.resolved_count,
                   stats.unresolved_count);
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
                printf("%s\t%s\t%s\t%s\t%s:%d\n", symbol->repo_path,
                       symbol->project_name, symbol->label, symbol->qualified_name,
                       symbol->file_path, symbol->start_line);
            }
            printf("%d symbol%s\n", count, count == 1 ? "" : "s");
        }
        cbm_aosp_symbols_free(results, count);
    } else {
        print_aosp_usage(stderr);
        exit_code = 1;
    }
    cbm_aosp_workspace_free(&workspace);
    return exit_code;
}
