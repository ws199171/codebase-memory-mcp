/* AOSP Binder/AIDL/JNI protocol linking. */
#include "aosp/protocol_graph.h"

#include "foundation/compat_fs.h"
#include "foundation/sha256.h"

#include <sqlite3.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PG_PATH_MAX = 4096,
    PG_MAX_FILE_BYTES = 32 * 1024 * 1024,
    PG_MAX_DEPTH = 256,
};

typedef struct {
    char **items;
    int count;
    int cap;
} pg_strings_t;

typedef enum {
    PG_EOF,
    PG_IDENT,
    PG_STRING,
    PG_LBRACE,
    PG_RBRACE,
    PG_LPAREN,
    PG_RPAREN,
    PG_COMMA,
    PG_SEMI,
    PG_OTHER,
} pg_token_kind_t;

typedef struct {
    pg_token_kind_t kind;
    char *text;
} pg_token_t;

typedef struct {
    const char *source;
    size_t length;
    size_t pos;
} pg_lexer_t;

typedef struct {
    char *package_name;
    char *interface_name;
    pg_strings_t methods;
} aidl_decl_t;

typedef struct {
    sqlite3 *db;
    const cbm_aosp_workspace_t *workspace;
    sqlite3_stmt *insert_node;
    sqlite3_stmt *insert_edge;
    sqlite3_stmt *find_symbols;
    char *err;
    size_t err_size;
} link_ctx_t;

static void pg_error(char *err, size_t err_size, const char *message, const char *detail) {
    if (!err || err_size == 0) return;
    if (detail && detail[0]) {
        (void)snprintf(err, err_size, "%s: %s", message, detail);
    } else {
        (void)snprintf(err, err_size, "%s", message);
    }
}

static char *pg_read_file(const char *path, size_t *length_out) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || length > PG_MAX_FILE_BYTES || fseek(file, 0, SEEK_SET) != 0) {
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

static bool pg_strings_add(pg_strings_t *strings, const char *value) {
    if (!strings || !value || !value[0]) return true;
    for (int i = 0; i < strings->count; i++) {
        if (strcmp(strings->items[i], value) == 0) return true;
    }
    if (strings->count == strings->cap) {
        int new_cap = strings->cap ? strings->cap * 2 : 8;
        char **items = realloc(strings->items, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        strings->items = items;
        strings->cap = new_cap;
    }
    strings->items[strings->count] = strdup(value);
    if (!strings->items[strings->count]) return false;
    strings->count++;
    return true;
}

static void pg_strings_free(pg_strings_t *strings) {
    if (!strings) return;
    for (int i = 0; i < strings->count; i++) free(strings->items[i]);
    free(strings->items);
    memset(strings, 0, sizeof(*strings));
}

static void pg_token_free(pg_token_t *token) {
    if (!token) return;
    free(token->text);
    token->text = NULL;
}

static void pg_skip(pg_lexer_t *lexer) {
    while (lexer->pos < lexer->length) {
        unsigned char c = (unsigned char)lexer->source[lexer->pos];
        if (isspace(c)) {
            lexer->pos++;
        } else if (c == '/' && lexer->pos + 1 < lexer->length &&
                   lexer->source[lexer->pos + 1] == '/') {
            lexer->pos += 2;
            while (lexer->pos < lexer->length && lexer->source[lexer->pos] != '\n') lexer->pos++;
        } else if (c == '/' && lexer->pos + 1 < lexer->length &&
                   lexer->source[lexer->pos + 1] == '*') {
            lexer->pos += 2;
            while (lexer->pos + 1 < lexer->length &&
                   !(lexer->source[lexer->pos] == '*' && lexer->source[lexer->pos + 1] == '/')) {
                lexer->pos++;
            }
            if (lexer->pos + 1 < lexer->length) lexer->pos += 2;
        } else {
            break;
        }
    }
}

static pg_token_t pg_next(pg_lexer_t *lexer) {
    pg_skip(lexer);
    pg_token_t token = {0};
    if (lexer->pos >= lexer->length) {
        token.kind = PG_EOF;
        return token;
    }
    char c = lexer->source[lexer->pos++];
    switch (c) {
        case '{': token.kind = PG_LBRACE; return token;
        case '}': token.kind = PG_RBRACE; return token;
        case '(': token.kind = PG_LPAREN; return token;
        case ')': token.kind = PG_RPAREN; return token;
        case ',': token.kind = PG_COMMA; return token;
        case ';': token.kind = PG_SEMI; return token;
        case '"': {
            token.kind = PG_STRING;
            size_t cap = 32;
            size_t n = 0;
            token.text = malloc(cap);
            if (!token.text) return token;
            while (lexer->pos < lexer->length) {
                char ch = lexer->source[lexer->pos++];
                if (ch == '"') break;
                if (ch == '\\' && lexer->pos < lexer->length) {
                    char escaped = lexer->source[lexer->pos++];
                    ch = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
                }
                if (n + 1 >= cap) {
                    cap *= 2;
                    char *grown = realloc(token.text, cap);
                    if (!grown) {
                        pg_token_free(&token);
                        return token;
                    }
                    token.text = grown;
                }
                token.text[n++] = ch;
            }
            token.text[n] = '\0';
            return token;
        }
        default: break;
    }
    if (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$') {
        size_t start = lexer->pos - 1;
        while (lexer->pos < lexer->length) {
            unsigned char ch = (unsigned char)lexer->source[lexer->pos];
            if (!isalnum(ch) && ch != '_' && ch != '.' && ch != '$') break;
            lexer->pos++;
        }
        size_t length = lexer->pos - start;
        token.kind = PG_IDENT;
        token.text = malloc(length + 1);
        if (token.text) {
            memcpy(token.text, lexer->source + start, length);
            token.text[length] = '\0';
        }
        return token;
    }
    token.kind = PG_OTHER;
    return token;
}

static void aidl_decl_free(aidl_decl_t *decl) {
    if (!decl) return;
    free(decl->package_name);
    free(decl->interface_name);
    pg_strings_free(&decl->methods);
    memset(decl, 0, sizeof(*decl));
}

static bool parse_aidl_protocol(const char *source, size_t length, aidl_decl_t *decl) {
    pg_lexer_t lexer = {.source = source, .length = length};
    pg_token_t token = pg_next(&lexer);
    bool in_interface = false;
    int brace_depth = 0;
    char *last_ident = NULL;
    while (token.kind != PG_EOF) {
        if (!in_interface && token.kind == PG_IDENT && token.text &&
            strcmp(token.text, "package") == 0) {
            pg_token_free(&token);
            token = pg_next(&lexer);
            if (token.kind == PG_IDENT && token.text) {
                free(decl->package_name);
                decl->package_name = strdup(token.text);
            }
        } else if (!in_interface && token.kind == PG_IDENT && token.text &&
                   strcmp(token.text, "interface") == 0) {
            pg_token_free(&token);
            token = pg_next(&lexer);
            if (token.kind == PG_IDENT && token.text) {
                decl->interface_name = strdup(token.text);
                in_interface = decl->interface_name != NULL;
            }
        } else if (in_interface && token.kind == PG_LBRACE) {
            brace_depth++;
        } else if (in_interface && token.kind == PG_RBRACE) {
            if (--brace_depth <= 0) in_interface = false;
        } else if (in_interface && brace_depth == 1 && token.kind == PG_IDENT && token.text) {
            free(last_ident);
            last_ident = strdup(token.text);
        } else if (in_interface && brace_depth == 1 && token.kind == PG_LPAREN && last_ident) {
            if (!pg_strings_add(&decl->methods, last_ident)) {
                free(last_ident);
                pg_token_free(&token);
                return false;
            }
            free(last_ident);
            last_ident = NULL;
        } else if (token.kind == PG_SEMI) {
            free(last_ident);
            last_ident = NULL;
        }
        pg_token_free(&token);
        token = pg_next(&lexer);
    }
    free(last_ident);
    pg_token_free(&token);
    return true;
}

static void hash_id(const char *workspace_id, const char *kind, const char *qualified, char out[65]) {
    cbm_sha256_ctx ctx;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_init(&ctx);
    cbm_sha256_update(&ctx, workspace_id, strlen(workspace_id));
    cbm_sha256_update(&ctx, "\0", 1);
    cbm_sha256_update(&ctx, kind, strlen(kind));
    cbm_sha256_update(&ctx, "\0", 1);
    cbm_sha256_update(&ctx, qualified, strlen(qualified));
    cbm_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}

static int insert_protocol_node(link_ctx_t *ctx, const char *id, const char *repo_id,
                                const char *kind, const char *name, const char *qualified_name,
                                const char *file_path, const char *symbol_global_id) {
    sqlite3_reset(ctx->insert_node);
    sqlite3_clear_bindings(ctx->insert_node);
    sqlite3_bind_text(ctx->insert_node, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_node, 2, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_node, 3, repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_node, 4, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_node, 5, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_node, 6, qualified_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_node, 7, file_path ? file_path : "", -1, SQLITE_TRANSIENT);
    if (symbol_global_id) {
        sqlite3_bind_text(ctx->insert_node, 8, symbol_global_id, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(ctx->insert_node, 8);
    }
    return sqlite3_step(ctx->insert_node) == SQLITE_DONE ? 0 : -1;
}

static int insert_protocol_edge(link_ctx_t *ctx, const char *source_id, const char *target_id,
                                const char *type, double confidence, const char *evidence) {
    sqlite3_reset(ctx->insert_edge);
    sqlite3_clear_bindings(ctx->insert_edge);
    sqlite3_bind_text(ctx->insert_edge, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_edge, 2, target_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_edge, 3, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(ctx->insert_edge, 4, confidence);
    sqlite3_bind_text(ctx->insert_edge, 5, evidence, -1, SQLITE_TRANSIENT);
    return sqlite3_step(ctx->insert_edge) == SQLITE_DONE ? 0 : -1;
}

static bool contains_class_token(const char *qualified_name, const char *token) {
    if (!qualified_name || !token || !token[0]) return false;
    const char *hit = strstr(qualified_name, token);
    while (hit) {
        bool left = hit == qualified_name || hit[-1] == '.' || hit[-1] == '$';
        char right_char = hit[strlen(token)];
        bool right = right_char == '\0' || right_char == '.' || right_char == '$';
        if (left && right) return true;
        hit = strstr(hit + 1, token);
    }
    return false;
}

static int add_symbol_endpoint(link_ctx_t *ctx, sqlite3_stmt *row, const char *kind,
                               char id_out[65]) {
    const char *global_id = (const char *)sqlite3_column_text(row, 0);
    const char *repo_id = (const char *)sqlite3_column_text(row, 1);
    const char *name = (const char *)sqlite3_column_text(row, 2);
    const char *qualified = (const char *)sqlite3_column_text(row, 3);
    const char *file_path = (const char *)sqlite3_column_text(row, 4);
    if (!global_id || !repo_id || !name || !qualified) return -1;
    (void)snprintf(id_out, 65, "%s", global_id);
    return insert_protocol_node(ctx, global_id, repo_id, kind, name, qualified,
                                file_path ? file_path : "", global_id);
}

static int link_generated_type(link_ctx_t *ctx, const char *aidl_id, const char *candidate,
                               const char *kind, const char *edge_type) {
    sqlite3_reset(ctx->find_symbols);
    sqlite3_clear_bindings(ctx->find_symbols);
    sqlite3_bind_text(ctx->find_symbols, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->find_symbols, 2, candidate, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(ctx->find_symbols) == SQLITE_ROW) {
        char target_id[65];
        if (add_symbol_endpoint(ctx, ctx->find_symbols, kind, target_id) != 0 ||
            insert_protocol_edge(ctx, aidl_id, target_id, edge_type, 1.0,
                                 "aidl_generated_name") != 0) {
            return -1;
        }
    }
    return sqlite3_errcode(ctx->db) == SQLITE_OK || sqlite3_errcode(ctx->db) == SQLITE_DONE ? 0 : -1;
}

static int link_binder_method(link_ctx_t *ctx, const char *method_id, const char *method,
                              const char *interface_name, const char *stem) {
    sqlite3_reset(ctx->find_symbols);
    sqlite3_clear_bindings(ctx->find_symbols);
    sqlite3_bind_text(ctx->find_symbols, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->find_symbols, 2, method, -1, SQLITE_TRANSIENT);
    char bn[512];
    char bp[512];
    (void)snprintf(bn, sizeof(bn), "Bn%s", stem);
    (void)snprintf(bp, sizeof(bp), "Bp%s", stem);
    while (sqlite3_step(ctx->find_symbols) == SQLITE_ROW) {
        const char *qualified = (const char *)sqlite3_column_text(ctx->find_symbols, 3);
        const char *kind = NULL;
        const char *edge_type = NULL;
        if (contains_class_token(qualified, bn) ||
            (strstr(qualified, interface_name) && strstr(qualified, ".Stub."))) {
            kind = "BINDER_SERVER_METHOD";
            edge_type = "BINDER_SERVER_IMPL";
        } else if (contains_class_token(qualified, bp) ||
                   (strstr(qualified, interface_name) && strstr(qualified, ".Proxy."))) {
            kind = "BINDER_CLIENT_METHOD";
            edge_type = "BINDER_CLIENT_PROXY";
        }
        if (kind) {
            char target_id[65];
            if (add_symbol_endpoint(ctx, ctx->find_symbols, kind, target_id) != 0 ||
                insert_protocol_edge(ctx, method_id, target_id, edge_type, 0.98,
                                     "aidl_method_generated_owner") != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int link_aidl_file(link_ctx_t *ctx, const cbm_aosp_repo_t *repo, const char *abs_path,
                          const char *rel_path) {
    size_t length = 0;
    char *source = pg_read_file(abs_path, &length);
    if (!source) return -1;
    aidl_decl_t decl = {0};
    bool parsed = parse_aidl_protocol(source, length, &decl);
    free(source);
    if (!parsed) {
        aidl_decl_free(&decl);
        return -1;
    }
    if (!decl.interface_name) {
        aidl_decl_free(&decl);
        return 0;
    }
    size_t qn_len = strlen(decl.interface_name) +
                    (decl.package_name ? strlen(decl.package_name) + 1 : 0) + 1;
    char *interface_qn = malloc(qn_len);
    if (!interface_qn) {
        aidl_decl_free(&decl);
        return -1;
    }
    if (decl.package_name) {
        (void)snprintf(interface_qn, qn_len, "%s.%s", decl.package_name, decl.interface_name);
    } else {
        (void)snprintf(interface_qn, qn_len, "%s", decl.interface_name);
    }
    char interface_id[65];
    hash_id(ctx->workspace->workspace_id, "AIDL_INTERFACE", interface_qn, interface_id);
    if (insert_protocol_node(ctx, interface_id, repo->repo_id, "AIDL_INTERFACE",
                             decl.interface_name, interface_qn, rel_path, NULL) != 0) {
        free(interface_qn);
        aidl_decl_free(&decl);
        return -1;
    }
    const char *stem = decl.interface_name[0] == 'I' && decl.interface_name[1]
                           ? decl.interface_name + 1
                           : decl.interface_name;
    char candidate[512];
    (void)snprintf(candidate, sizeof(candidate), "Bn%s", stem);
    if (link_generated_type(ctx, interface_id, candidate, "BINDER_SERVER_TYPE",
                            "AIDL_GENERATES_SERVER") != 0) goto fail;
    (void)snprintf(candidate, sizeof(candidate), "Bp%s", stem);
    if (link_generated_type(ctx, interface_id, candidate, "BINDER_CLIENT_TYPE",
                            "AIDL_GENERATES_CLIENT") != 0) goto fail;
    if (link_generated_type(ctx, interface_id, decl.interface_name, "BINDER_INTERFACE_TYPE",
                            "AIDL_GENERATES_INTERFACE") != 0) goto fail;

    for (int i = 0; i < decl.methods.count; i++) {
        size_t method_qn_len = strlen(interface_qn) + strlen(decl.methods.items[i]) + 2;
        char *method_qn = malloc(method_qn_len);
        if (!method_qn) goto fail;
        (void)snprintf(method_qn, method_qn_len, "%s.%s", interface_qn, decl.methods.items[i]);
        char method_id[65];
        hash_id(ctx->workspace->workspace_id, "AIDL_METHOD", method_qn, method_id);
        int rc = insert_protocol_node(ctx, method_id, repo->repo_id, "AIDL_METHOD",
                                      decl.methods.items[i], method_qn, rel_path, NULL);
        if (rc == 0) rc = insert_protocol_edge(ctx, interface_id, method_id, "DECLARES_METHOD",
                                               1.0, "aidl_ast");
        if (rc == 0) rc = link_binder_method(ctx, method_id, decl.methods.items[i],
                                             decl.interface_name, stem);
        free(method_qn);
        if (rc != 0) goto fail;
    }
    free(interface_qn);
    aidl_decl_free(&decl);
    return 0;
fail:
    free(interface_qn);
    aidl_decl_free(&decl);
    return -1;
}

static bool skip_dir(const char *name) {
    return strcmp(name, ".git") == 0 || strcmp(name, ".repo") == 0 || strcmp(name, "out") == 0 ||
           strcmp(name, "node_modules") == 0 || strncmp(name, "bazel-", 6) == 0;
}

static bool nested_repo(const cbm_aosp_workspace_t *workspace, const cbm_aosp_repo_t *current,
                        const char *abs_path) {
    for (int i = 0; i < workspace->repo_count; i++) {
        const cbm_aosp_repo_t *repo = &workspace->repos[i];
        if (repo != current && repo->abs_path && strcmp(repo->abs_path, abs_path) == 0) return true;
    }
    return false;
}

static int find_dynamic_class(const char *source, char *out, size_t out_size) {
    pg_lexer_t lexer = {.source = source, .length = strlen(source)};
    pg_token_t token;
    while ((token = pg_next(&lexer)).kind != PG_EOF) {
        int slash_count = 0;
        if (token.kind == PG_STRING && token.text) {
            for (const char *p = token.text; *p; p++) slash_count += *p == '/' ? 1 : 0;
        }
        if (token.kind == PG_STRING && token.text && slash_count >= 2 &&
            token.text[0] != '(' && !strchr(token.text, ' ') && !strchr(token.text, '.')) {
            (void)snprintf(out, out_size, "%s", token.text);
            pg_token_free(&token);
            return 0;
        }
        pg_token_free(&token);
    }
    pg_token_free(&token);
    return -1;
}

static int link_jni_pair(link_ctx_t *ctx, const char *java_method, const char *class_name,
                         const char *native_function, const char *native_repo_id,
                         const char *native_file_path, const char *evidence, double confidence) {
    const char *sql =
        "SELECT global_id,repo_id,name,qualified_name,file_path,label FROM symbols "
        "WHERE workspace_id=?1 AND name=?2;";
    sqlite3_stmt *java_stmt = NULL;
    sqlite3_stmt *native_stmt = NULL;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &java_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(ctx->db, sql, -1, &native_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(java_stmt);
        sqlite3_finalize(native_stmt);
        return -1;
    }
    sqlite3_bind_text(java_stmt, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(java_stmt, 2, java_method, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(native_stmt, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(native_stmt, 2, native_function, -1, SQLITE_TRANSIENT);
    int rc = 0;
    while (rc == 0 && sqlite3_step(java_stmt) == SQLITE_ROW) {
        const char *qualified = (const char *)sqlite3_column_text(java_stmt, 3);
        const char *java_label = (const char *)sqlite3_column_text(java_stmt, 5);
        if (!java_label || strcmp(java_label, "Method") != 0) continue;
        if (class_name && class_name[0]) {
            char normalized[PG_PATH_MAX];
            (void)snprintf(normalized, sizeof(normalized), "%s", class_name);
            for (char *p = normalized; *p; p++) if (*p == '/') *p = '.';
            if (!strstr(qualified ? qualified : "", normalized)) continue;
        }
        sqlite3_reset(native_stmt);
        while (sqlite3_step(native_stmt) == SQLITE_ROW) {
            const char *native_repo = (const char *)sqlite3_column_text(native_stmt, 1);
            const char *native_file = (const char *)sqlite3_column_text(native_stmt, 4);
            const char *native_label = (const char *)sqlite3_column_text(native_stmt, 5);
            if (!native_label || strcmp(native_label, "Function") != 0) continue;
            if (native_repo_id && (!native_repo || strcmp(native_repo, native_repo_id) != 0)) {
                continue;
            }
            if (native_file_path &&
                (!native_file || strcmp(native_file, native_file_path) != 0)) {
                continue;
            }
            char java_id[65];
            char native_id[65];
            if (add_symbol_endpoint(ctx, java_stmt, "JNI_JAVA_METHOD", java_id) != 0 ||
                add_symbol_endpoint(ctx, native_stmt, "JNI_NATIVE_FUNCTION", native_id) != 0 ||
                insert_protocol_edge(ctx, java_id, native_id, "JNI_NATIVE_IMPLEMENTATION",
                                     confidence, evidence) != 0) {
                rc = -1;
                break;
            }
        }
    }
    sqlite3_finalize(java_stmt);
    sqlite3_finalize(native_stmt);
    return rc;
}

static int parse_dynamic_jni(link_ctx_t *ctx, const cbm_aosp_repo_t *repo,
                             const char *file_path, const char *source) {
    if (!strstr(source, "JNINativeMethod")) return 0;
    char class_name[PG_PATH_MAX] = {0};
    (void)find_dynamic_class(source, class_name, sizeof(class_name));
    const char *cursor = strstr(source, "JNINativeMethod");
    while (cursor) {
        const char *table_open = strchr(cursor, '{');
        if (!table_open) break;
        int depth = 1;
        const char *table_end = table_open + 1;
        while (*table_end && depth > 0) {
            if (*table_end == '{') depth++;
            else if (*table_end == '}') depth--;
            table_end++;
        }
        pg_lexer_t lexer = {.source = table_open, .length = (size_t)(table_end - table_open)};
        pg_token_t token;
        char *java_name = NULL;
        char *signature = NULL;
        char *last_ident = NULL;
        int entry_depth = 0;
        while ((token = pg_next(&lexer)).kind != PG_EOF) {
            if (token.kind == PG_LBRACE) {
                entry_depth++;
                free(java_name); java_name = NULL;
                free(signature); signature = NULL;
                free(last_ident); last_ident = NULL;
            } else if (token.kind == PG_STRING && entry_depth >= 2) {
                if (!java_name) java_name = strdup(token.text ? token.text : "");
                else if (!signature) signature = strdup(token.text ? token.text : "");
            } else if (token.kind == PG_IDENT && entry_depth >= 2) {
                if (token.text && strcmp(token.text, "void") != 0 &&
                    strcmp(token.text, "reinterpret_cast") != 0 &&
                    strcmp(token.text, "static_cast") != 0) {
                    free(last_ident);
                    last_ident = strdup(token.text);
                }
            } else if (token.kind == PG_RBRACE && entry_depth >= 2) {
                if (java_name && signature && last_ident &&
                    link_jni_pair(ctx, java_name, class_name, last_ident,
                                  repo->repo_id, file_path,
                                  "jni_native_method_table", class_name[0] ? 0.98 : 0.75) != 0) {
                    pg_token_free(&token);
                    free(java_name); free(signature); free(last_ident);
                    return -1;
                }
                entry_depth--;
                free(java_name); java_name = NULL;
                free(signature); signature = NULL;
                free(last_ident); last_ident = NULL;
            }
            pg_token_free(&token);
        }
        pg_token_free(&token);
        free(java_name); free(signature); free(last_ident);
        cursor = strstr(table_end, "JNINativeMethod");
    }
    return 0;
}

static int scan_repo_tree(link_ctx_t *ctx, const cbm_aosp_repo_t *repo, const char *abs_dir,
                          const char *rel_dir, int depth) {
    if (depth > PG_MAX_DEPTH) return -1;
    cbm_dir_t *dir = cbm_opendir(abs_dir);
    if (!dir) return 0;
    cbm_dirent_t *entry;
    int rc = 0;
    while (rc == 0 && (entry = cbm_readdir(dir)) != NULL) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) continue;
        char abs_path[PG_PATH_MAX];
        char rel_path[PG_PATH_MAX];
        (void)snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, entry->name);
        (void)snprintf(rel_path, sizeof(rel_path), "%s%s%s", rel_dir, rel_dir[0] ? "/" : "",
                       entry->name);
        if (entry->is_dir) {
            if (!skip_dir(entry->name) && !nested_repo(ctx->workspace, repo, abs_path)) {
                rc = scan_repo_tree(ctx, repo, abs_path, rel_path, depth + 1);
            }
            continue;
        }
        size_t name_len = strlen(entry->name);
        bool aidl = name_len > 5 && strcmp(entry->name + name_len - 5, ".aidl") == 0;
        bool native = (name_len > 2 && strcmp(entry->name + name_len - 2, ".c") == 0) ||
                      (name_len > 3 && strcmp(entry->name + name_len - 3, ".cc") == 0) ||
                      (name_len > 4 && strcmp(entry->name + name_len - 4, ".cpp") == 0);
        if (aidl) {
            rc = link_aidl_file(ctx, repo, abs_path, rel_path);
        } else if (native) {
            size_t length = 0;
            char *source = pg_read_file(abs_path, &length);
            if (source) {
                if (strstr(source, "JNINativeMethod")) {
                    rc = parse_dynamic_jni(ctx, repo, rel_path, source);
                }
                free(source);
            }
        }
    }
    cbm_closedir(dir);
    return rc;
}

static int decode_static_jni(const char *native_name, char *class_name, size_t class_size,
                             char *method_name, size_t method_size) {
    if (!native_name || strncmp(native_name, "Java_", 5) != 0) return -1;
    char encoded[PG_PATH_MAX];
    (void)snprintf(encoded, sizeof(encoded), "%s", native_name + 5);
    char *signature = strstr(encoded, "__");
    if (signature) *signature = '\0';
    char *split = NULL;
    for (char *p = encoded + strlen(encoded); p > encoded;) {
        p--;
        if (*p == '_' && p[1] != '0' && p[1] != '1' && p[1] != '2' && p[1] != '3') {
            split = p;
            break;
        }
    }
    if (!split || !split[1]) return -1;
    *split++ = '\0';
    (void)snprintf(method_name, method_size, "%s", split);
    size_t written = 0;
    for (size_t i = 0; encoded[i] && written + 1 < class_size; i++) {
        if (encoded[i] == '_' && encoded[i + 1] == '1') {
            class_name[written++] = '_';
            i++;
        } else {
            class_name[written++] = encoded[i] == '_' ? '/' : encoded[i];
        }
    }
    class_name[written] = '\0';
    return 0;
}

static int link_static_jni(link_ctx_t *ctx) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT name FROM symbols WHERE workspace_id=?1 AND name GLOB 'Java_*' "
        "AND label IN('Function','Method');";
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    int rc = 0;
    while (rc == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *native_name = (const char *)sqlite3_column_text(stmt, 0);
        char class_name[PG_PATH_MAX];
        char method_name[1024];
        if (decode_static_jni(native_name, class_name, sizeof(class_name),
                              method_name, sizeof(method_name)) == 0) {
            rc = link_jni_pair(ctx, method_name, class_name, native_name, NULL, NULL,
                               "jni_exported_name", 1.0);
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

int cbm_aosp_protocol_stats(const cbm_aosp_workspace_t *workspace,
                            cbm_aosp_protocol_stats_t *stats, char *err, size_t err_size) {
    if (!workspace || !stats) return -1;
    memset(stats, 0, sizeof(*stats));
    char path[PG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        pg_error(err, err_size, "AOSP workspace is not initialized", path);
        sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT (SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_INTERFACE'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_METHOD'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.type='BINDER_SERVER_IMPL'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.type='BINDER_CLIENT_PROXY'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.evidence='jni_exported_name'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.evidence='jni_native_method_table');";
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats->node_count = sqlite3_column_int(stmt, 0);
            stats->edge_count = sqlite3_column_int(stmt, 1);
            stats->aidl_interfaces = sqlite3_column_int(stmt, 2);
            stats->aidl_methods = sqlite3_column_int(stmt, 3);
            stats->binder_server_edges = sqlite3_column_int(stmt, 4);
            stats->binder_client_edges = sqlite3_column_int(stmt, 5);
            stats->jni_static_edges = sqlite3_column_int(stmt, 6);
            stats->jni_dynamic_edges = sqlite3_column_int(stmt, 7);
            rc = 0;
        }
    } else {
        pg_error(err, err_size, "cannot read AOSP protocol graph", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

int cbm_aosp_protocol_link(const cbm_aosp_workspace_t *workspace,
                           cbm_aosp_protocol_stats_t *stats, char *err, size_t err_size) {
    if (!workspace || !stats) return -1;
    if (cbm_aosp_master_sync(workspace, err, err_size) != 0) return -1;
    char path[PG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        pg_error(err, err_size, "cannot open AOSP master database", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 10000);
    sqlite3_stmt *clear = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM protocol_edges WHERE source_id IN "
            "(SELECT protocol_id FROM protocol_nodes WHERE workspace_id=?1) OR target_id IN "
            "(SELECT protocol_id FROM protocol_nodes WHERE workspace_id=?1);", -1, &clear, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(clear, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(clear) != SQLITE_DONE) goto fail;
    sqlite3_finalize(clear);
    clear = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM protocol_nodes WHERE workspace_id=?1;", -1,
                           &clear, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(clear, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(clear) != SQLITE_DONE) goto fail;
    sqlite3_finalize(clear);
    clear = NULL;

    link_ctx_t ctx = {.db = db, .workspace = workspace, .err = err, .err_size = err_size};
    const char *node_sql =
        "INSERT OR REPLACE INTO protocol_nodes(protocol_id,workspace_id,repo_id,kind,name,"
        "qualified_name,file_path,symbol_global_id,properties) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,'{}');";
    const char *edge_sql =
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties)"
        " VALUES(?1,?2,?3,?4,?5,'{}');";
    const char *find_sql =
        "SELECT global_id,repo_id,name,qualified_name,file_path,label FROM symbols "
        "WHERE workspace_id=?1 AND name=?2;";
    if (sqlite3_prepare_v2(db, node_sql, -1, &ctx.insert_node, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, edge_sql, -1, &ctx.insert_edge, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, find_sql, -1, &ctx.find_symbols, NULL) != SQLITE_OK) goto fail_ctx;
    int rc = 0;
    for (int i = 0; i < workspace->repo_count && rc == 0; i++) {
        if (workspace->repos[i].exists) {
            rc = scan_repo_tree(&ctx, &workspace->repos[i], workspace->repos[i].abs_path, "", 0);
        }
    }
    if (rc == 0) rc = link_static_jni(&ctx);
    sqlite3_finalize(ctx.insert_node);
    sqlite3_finalize(ctx.insert_edge);
    sqlite3_finalize(ctx.find_symbols);
    if (rc != 0 || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    sqlite3_close(db);
    return cbm_aosp_protocol_stats(workspace, stats, err, err_size);

fail_ctx:
    sqlite3_finalize(ctx.insert_node);
    sqlite3_finalize(ctx.insert_edge);
    sqlite3_finalize(ctx.find_symbols);
fail:
    sqlite3_finalize(clear);
    (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    pg_error(err, err_size, "cannot build AOSP protocol graph", sqlite3_errmsg(db));
    sqlite3_close(db);
    return -1;
}

void cbm_aosp_protocol_nodes_free(cbm_aosp_protocol_node_t *results, int count) {
    if (!results) return;
    for (int i = 0; i < count; i++) {
        free(results[i].protocol_id);
        free(results[i].repo_path);
        free(results[i].kind);
        free(results[i].name);
        free(results[i].qualified_name);
        free(results[i].file_path);
    }
    free(results);
}

static char *column_dup(sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    return strdup(text ? (const char *)text : "");
}

int cbm_aosp_search_protocols(const cbm_aosp_workspace_t *workspace, const char *query, int limit,
                              cbm_aosp_protocol_node_t **results, int *count,
                              char *err, size_t err_size) {
    if (!workspace || !results || !count) return -1;
    *results = NULL;
    *count = 0;
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    char path[PG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        pg_error(err, err_size, "AOSP workspace is not initialized", path);
        sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT n.protocol_id,r.path,n.kind,n.name,n.qualified_name,n.file_path,"
        "(SELECT count(*) FROM protocol_edges e WHERE e.source_id=n.protocol_id),"
        "(SELECT count(*) FROM protocol_edges e WHERE e.target_id=n.protocol_id) "
        "FROM protocol_nodes n JOIN repos r ON r.repo_id=n.repo_id WHERE n.workspace_id=?1 "
        "AND (?2='' OR n.name LIKE '%'||?2||'%' OR n.qualified_name LIKE '%'||?2||'%' "
        "OR n.kind LIKE '%'||?2||'%') ORDER BY n.qualified_name,n.protocol_id LIMIT ?3;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pg_error(err, err_size, "cannot prepare AOSP protocol search", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, query ? query : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    cbm_aosp_protocol_node_t *items = calloc((size_t)limit, sizeof(*items));
    if (!items) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    int n = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cbm_aosp_protocol_node_t *item = &items[n];
        item->protocol_id = column_dup(stmt, 0);
        item->repo_path = column_dup(stmt, 1);
        item->kind = column_dup(stmt, 2);
        item->name = column_dup(stmt, 3);
        item->qualified_name = column_dup(stmt, 4);
        item->file_path = column_dup(stmt, 5);
        item->outgoing_edges = sqlite3_column_int(stmt, 6);
        item->incoming_edges = sqlite3_column_int(stmt, 7);
        if (!item->protocol_id || !item->repo_path || !item->kind || !item->name ||
            !item->qualified_name || !item->file_path) {
            cbm_aosp_protocol_nodes_free(items, n + 1);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return -1;
        }
        n++;
    }
    if (step_rc != SQLITE_DONE) {
        pg_error(err, err_size, "cannot search AOSP protocols", sqlite3_errmsg(db));
        cbm_aosp_protocol_nodes_free(items, n);
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

void cbm_aosp_protocol_edges_free(cbm_aosp_protocol_edge_t *results, int count) {
    if (!results) return;
    for (int i = 0; i < count; i++) {
        free(results[i].source_qualified_name);
        free(results[i].target_qualified_name);
        free(results[i].type);
        free(results[i].evidence);
    }
    free(results);
}

int cbm_aosp_search_protocol_edges(const cbm_aosp_workspace_t *workspace, const char *query,
                                   int limit, cbm_aosp_protocol_edge_t **results, int *count,
                                   char *err, size_t err_size) {
    if (!workspace || !results || !count) return -1;
    *results = NULL;
    *count = 0;
    if (limit <= 0) limit = 100;
    if (limit > 1000) limit = 1000;
    char path[PG_PATH_MAX];
    if (cbm_aosp_master_path(workspace, path, sizeof(path), false) != 0) return -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        pg_error(err, err_size, "AOSP workspace is not initialized", path);
        sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT s.qualified_name,t.qualified_name,e.type,e.confidence,e.evidence "
        "FROM protocol_edges e JOIN protocol_nodes s ON s.protocol_id=e.source_id "
        "JOIN protocol_nodes t ON t.protocol_id=e.target_id WHERE s.workspace_id=?1 "
        "AND (?2='' OR s.name LIKE '%'||?2||'%' OR s.qualified_name LIKE '%'||?2||'%' "
        "OR t.name LIKE '%'||?2||'%' OR t.qualified_name LIKE '%'||?2||'%' "
        "OR e.type LIKE '%'||?2||'%') ORDER BY s.qualified_name,e.type,t.qualified_name LIMIT ?3;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pg_error(err, err_size, "cannot prepare AOSP protocol edge search", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, query ? query : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    cbm_aosp_protocol_edge_t *items = calloc((size_t)limit, sizeof(*items));
    if (!items) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    int n = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cbm_aosp_protocol_edge_t *item = &items[n];
        item->source_qualified_name = column_dup(stmt, 0);
        item->target_qualified_name = column_dup(stmt, 1);
        item->type = column_dup(stmt, 2);
        item->confidence = sqlite3_column_double(stmt, 3);
        item->evidence = column_dup(stmt, 4);
        if (!item->source_qualified_name || !item->target_qualified_name || !item->type ||
            !item->evidence) {
            cbm_aosp_protocol_edges_free(items, n + 1);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return -1;
        }
        n++;
    }
    if (step_rc != SQLITE_DONE) {
        pg_error(err, err_size, "cannot search AOSP protocol edges", sqlite3_errmsg(db));
        cbm_aosp_protocol_edges_free(items, n);
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
