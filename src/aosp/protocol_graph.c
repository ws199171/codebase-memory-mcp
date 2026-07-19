/* AOSP Binder/AIDL/JNI protocol linking. */
#include "aosp/protocol_graph.h"

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
    PG_AT,
    PG_EQUAL,
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
    size_t last_start;
    size_t last_end;
} pg_lexer_t;

typedef struct {
    char *name;
    char *return_type;
    pg_strings_t parameter_types;
    pg_strings_t annotations;
    bool oneway;
} aidl_method_t;

typedef struct {
    aidl_method_t *items;
    int count;
    int cap;
} aidl_methods_t;

typedef struct {
    char *name;
    char *type;
    pg_strings_t annotations;
} aidl_member_t;

typedef struct {
    aidl_member_t *items;
    int count;
    int cap;
} aidl_members_t;

typedef struct {
    char *package_name;
    char *name;
    char *kind;
    pg_strings_t imports;
    pg_strings_t annotations;
    aidl_methods_t methods;
    aidl_members_t members;
    bool oneway;
} aidl_decl_t;

typedef struct {
    sqlite3 *db;
    const cbm_aosp_workspace_t *workspace;
    sqlite3_stmt *insert_node;
    sqlite3_stmt *insert_edge;
    sqlite3_stmt *find_symbols;
    sqlite3_stmt *insert_service;
    char *err;
    size_t err_size;
} link_ctx_t;

typedef struct {
    char *global_id;
    char *repo_id;
    char *name;
    char *qualified_name;
    char *file_path;
    char *label;
    int start_line;
    int end_line;
} binder_symbol_t;

typedef struct {
    binder_symbol_t *items;
    int count;
    int cap;
} binder_symbols_t;

typedef enum {
    BINDER_BACKEND_UNKNOWN,
    BINDER_BACKEND_JAVA,
    BINDER_BACKEND_CPP_NDK,
    BINDER_BACKEND_RUST,
} binder_backend_t;

static int link_binder_flow(link_ctx_t *ctx, const char *method_id, const char *method,
                            const char *interface_name, const char *stem);
static bool contains_class_token(const char *qualified_name, const char *token);
static void hash_id(const char *workspace_id, const char *kind, const char *qualified,
                    char out[65]);

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

static void binder_symbols_free(binder_symbols_t *symbols) {
    if (!symbols) return;
    for (int i = 0; i < symbols->count; i++) {
        binder_symbol_t *item = &symbols->items[i];
        free(item->global_id);
        free(item->repo_id);
        free(item->name);
        free(item->qualified_name);
        free(item->file_path);
        free(item->label);
    }
    free(symbols->items);
    memset(symbols, 0, sizeof(*symbols));
}

static void binder_symbol_free(binder_symbol_t *symbol) {
    if (!symbol) return;
    free(symbol->global_id);
    free(symbol->repo_id);
    free(symbol->name);
    free(symbol->qualified_name);
    free(symbol->file_path);
    free(symbol->label);
    memset(symbol, 0, sizeof(*symbol));
}

static int binder_symbols_load(link_ctx_t *ctx, const char *name, binder_symbols_t *symbols) {
    const char *sql =
        "SELECT global_id,repo_id,name,qualified_name,file_path,label,start_line,end_line "
        "FROM symbols WHERE workspace_id=?1 AND name=?2 ORDER BY qualified_name,global_id;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    int rc = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (symbols->count == symbols->cap) {
            int new_cap = symbols->cap ? symbols->cap * 2 : 8;
            binder_symbol_t *items = realloc(symbols->items,
                                             (size_t)new_cap * sizeof(*items));
            if (!items) {
                rc = -1;
                break;
            }
            symbols->items = items;
            symbols->cap = new_cap;
        }
        binder_symbol_t *item = &symbols->items[symbols->count];
        memset(item, 0, sizeof(*item));
        const char *global_id = (const char *)sqlite3_column_text(stmt, 0);
        const char *repo_id = (const char *)sqlite3_column_text(stmt, 1);
        const char *symbol_name = (const char *)sqlite3_column_text(stmt, 2);
        const char *qualified = (const char *)sqlite3_column_text(stmt, 3);
        const char *file_path = (const char *)sqlite3_column_text(stmt, 4);
        const char *label = (const char *)sqlite3_column_text(stmt, 5);
        item->global_id = strdup(global_id ? global_id : "");
        item->repo_id = strdup(repo_id ? repo_id : "");
        item->name = strdup(symbol_name ? symbol_name : "");
        item->qualified_name = strdup(qualified ? qualified : "");
        item->file_path = strdup(file_path ? file_path : "");
        item->label = strdup(label ? label : "");
        item->start_line = sqlite3_column_int(stmt, 6);
        item->end_line = sqlite3_column_int(stmt, 7);
        if (!item->global_id || !item->repo_id || !item->name ||
            !item->qualified_name || !item->file_path || !item->label) {
            symbols->count++;
            rc = -1;
            break;
        }
        symbols->count++;
    }
    if (rc == 0 && sqlite3_errcode(ctx->db) != SQLITE_OK &&
        sqlite3_errcode(ctx->db) != SQLITE_DONE) rc = -1;
    sqlite3_finalize(stmt);
    if (rc != 0) binder_symbols_free(symbols);
    return rc;
}

static int binder_symbols_add_source_constant(link_ctx_t *ctx, binder_symbols_t *symbols,
                                              const binder_symbol_t *handler,
                                              const char *symbol_name,
                                              const char *qualified_suffix) {
    if (symbols->count == symbols->cap) {
        int new_cap = symbols->cap ? symbols->cap * 2 : 4;
        binder_symbol_t *items = realloc(symbols->items, (size_t)new_cap * sizeof(*items));
        if (!items) return -1;
        symbols->items = items;
        symbols->cap = new_cap;
    }
    size_t qualified_length = strlen(handler->qualified_name);
    size_t handler_length = strlen(handler->name);
    if (qualified_length <= handler_length) return -1;
    size_t prefix_length = qualified_length - handler_length;
    size_t size = prefix_length + strlen(qualified_suffix) + 1;
    char *qualified = malloc(size);
    if (!qualified) return -1;
    memcpy(qualified, handler->qualified_name, prefix_length);
    (void)snprintf(qualified + prefix_length, size - prefix_length, "%s", qualified_suffix);
    char global_id[65];
    hash_id(ctx->workspace->workspace_id, "BINDER_TRANSACTION_CONSTANT", qualified, global_id);

    binder_symbol_t *item = &symbols->items[symbols->count];
    memset(item, 0, sizeof(*item));
    item->global_id = strdup(global_id);
    item->repo_id = strdup(handler->repo_id);
    item->name = strdup(symbol_name);
    item->qualified_name = qualified;
    item->file_path = strdup(handler->file_path);
    item->label = strdup("SourceConstant");
    item->start_line = handler->start_line;
    item->end_line = handler->end_line;
    if (!item->global_id || !item->repo_id || !item->name || !item->qualified_name ||
        !item->file_path || !item->label) {
        symbols->count++;
        return -1;
    }
    symbols->count++;
    return 0;
}

static const cbm_aosp_repo_t *binder_symbol_repo(const link_ctx_t *ctx, const char *repo_id) {
    for (int i = 0; i < ctx->workspace->repo_count; i++) {
        if (repo_id && strcmp(ctx->workspace->repos[i].repo_id, repo_id) == 0) {
            return &ctx->workspace->repos[i];
        }
    }
    return NULL;
}

static char *binder_symbol_source(const link_ctx_t *ctx, const binder_symbol_t *symbol,
                                  bool range_only) {
    const cbm_aosp_repo_t *repo = binder_symbol_repo(ctx, symbol->repo_id);
    if (!repo || !repo->abs_path || !symbol->file_path || !symbol->file_path[0]) return NULL;
    char path[PG_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s", repo->abs_path, symbol->file_path);
    if (written < 0 || (size_t)written >= sizeof(path)) return NULL;
    size_t length = 0;
    char *source = pg_read_file(path, &length);
    if (!source || !range_only || symbol->start_line <= 0 || symbol->end_line <= 0) return source;
    size_t start = 0;
    size_t end = length;
    int line = 1;
    bool found_start = symbol->start_line == 1;
    for (size_t i = 0; i < length; i++) {
        if (line == symbol->start_line) {
            start = i;
            found_start = true;
            break;
        }
        if (source[i] == '\n') line++;
    }
    if (!found_start) {
        free(source);
        return NULL;
    }
    line = 1;
    for (size_t i = 0; i < length; i++) {
        if (source[i] == '\n') {
            if (line == symbol->end_line) {
                end = i + 1;
                break;
            }
            line++;
        }
    }
    if (end < start) end = start;
    char *range = malloc(end - start + 1);
    if (range) {
        memcpy(range, source + start, end - start);
        range[end - start] = '\0';
    }
    free(source);
    return range;
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

static bool pg_strings_append(pg_strings_t *strings, const char *value) {
    if (!strings || !value || !value[0]) return true;
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
    lexer->last_start = lexer->pos;
    if (lexer->pos >= lexer->length) {
        token.kind = PG_EOF;
        lexer->last_end = lexer->pos;
        return token;
    }
    char c = lexer->source[lexer->pos++];
    lexer->last_end = lexer->pos;
    switch (c) {
        case '{': token.kind = PG_LBRACE; return token;
        case '}': token.kind = PG_RBRACE; return token;
        case '(': token.kind = PG_LPAREN; return token;
        case ')': token.kind = PG_RPAREN; return token;
        case ',': token.kind = PG_COMMA; return token;
        case ';': token.kind = PG_SEMI; return token;
        case '@': token.kind = PG_AT; return token;
        case '=': token.kind = PG_EQUAL; return token;
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
            lexer->last_end = lexer->pos;
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
        lexer->last_end = lexer->pos;
        return token;
    }
    token.kind = PG_OTHER;
    return token;
}

static char *binder_symbol_owner(const char *qualified_name, const char *method_name) {
    if (!qualified_name || !method_name) return NULL;
    size_t length = strlen(qualified_name);
    size_t method_length = strlen(method_name);
    if (length <= method_length) return NULL;
    size_t end = length - method_length;
    while (end > 0 && (qualified_name[end - 1] == '.' || qualified_name[end - 1] == ':' ||
                       qualified_name[end - 1] == '$')) end--;
    if (end == 0) return NULL;
    size_t start = end;
    while (start > 0 && qualified_name[start - 1] != '.' &&
           qualified_name[start - 1] != ':' && qualified_name[start - 1] != '$') start--;
    char *owner = malloc(end - start + 1);
    if (!owner) return NULL;
    memcpy(owner, qualified_name + start, end - start);
    owner[end - start] = '\0';
    return owner;
}

static char *binder_symbol_qualified_owner(const char *qualified_name,
                                           const char *method_name) {
    if (!qualified_name || !method_name) return NULL;
    size_t length = strlen(qualified_name);
    size_t method_length = strlen(method_name);
    if (length <= method_length || strcmp(qualified_name + length - method_length,
                                          method_name) != 0) return NULL;
    size_t end = length - method_length;
    while (end > 0 && (qualified_name[end - 1] == '.' || qualified_name[end - 1] == ':' ||
                       qualified_name[end - 1] == '$')) end--;
    if (end == 0) return NULL;
    char *owner = malloc(end + 1);
    if (!owner) return NULL;
    memcpy(owner, qualified_name, end);
    owner[end] = '\0';
    return owner;
}

static bool binder_path_has_extension(const char *file_path, const char *extension) {
    if (!file_path || !extension) return false;
    size_t path_length = strlen(file_path);
    size_t extension_length = strlen(extension);
    if (path_length < extension_length) return false;
    const char *suffix = file_path + path_length - extension_length;
    for (size_t i = 0; i < extension_length; i++) {
        if (tolower((unsigned char)suffix[i]) !=
            tolower((unsigned char)extension[i])) return false;
    }
    return true;
}

static binder_backend_t binder_backend(const char *qualified_name, const char *file_path) {
    (void)qualified_name;
    if (binder_path_has_extension(file_path, ".java") ||
        binder_path_has_extension(file_path, ".kt")) return BINDER_BACKEND_JAVA;
    if (binder_path_has_extension(file_path, ".rs")) return BINDER_BACKEND_RUST;
    static const char *cpp_extensions[] = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    };
    for (size_t i = 0; i < sizeof(cpp_extensions) / sizeof(cpp_extensions[0]); i++) {
        if (binder_path_has_extension(file_path, cpp_extensions[i])) {
            return BINDER_BACKEND_CPP_NDK;
        }
    }
    return BINDER_BACKEND_UNKNOWN;
}

static bool binder_same_backend(const binder_symbol_t *left,
                                const binder_symbol_t *right) {
    binder_backend_t left_backend = binder_backend(left->qualified_name, left->file_path);
    binder_backend_t right_backend = binder_backend(right->qualified_name, right->file_path);
    return left_backend != BINDER_BACKEND_UNKNOWN && left_backend == right_backend;
}

static bool binder_source_declares_implementation(const char *source, const char *owner,
                                                  const char *bn_name,
                                                  const char *interface_name) {
    if (!source || !owner || !owner[0]) return false;
    pg_lexer_t rust_lexer = {.source = source, .length = strlen(source)};
    int rust_state = 0;
    bool rust_trait_matches = false;
    pg_token_t rust_token;
    while ((rust_token = pg_next(&rust_lexer)).kind != PG_EOF) {
        if (rust_token.kind == PG_IDENT && rust_token.text) {
            if (strcmp(rust_token.text, "impl") == 0) {
                rust_state = 1;
                rust_trait_matches = false;
            } else if (rust_state == 1 && strcmp(rust_token.text, "for") == 0) {
                rust_state = rust_trait_matches ? 2 : 0;
            } else if (rust_state == 1 && interface_name &&
                       contains_class_token(rust_token.text, interface_name)) {
                rust_trait_matches = true;
            } else if (rust_state == 2) {
                bool matches = contains_class_token(rust_token.text, owner);
                pg_token_free(&rust_token);
                if (matches) return true;
                rust_state = 0;
            }
        } else if (rust_token.kind == PG_LBRACE || rust_token.kind == PG_SEMI) {
            rust_state = 0;
            rust_trait_matches = false;
        }
        pg_token_free(&rust_token);
    }
    pg_token_free(&rust_token);

    pg_lexer_t lexer = {.source = source, .length = strlen(source)};
    bool after_class = false;
    bool matching_owner = false;
    pg_token_t token;
    while ((token = pg_next(&lexer)).kind != PG_EOF) {
        if (token.kind == PG_IDENT && token.text &&
            (strcmp(token.text, "class") == 0 || strcmp(token.text, "struct") == 0)) {
            after_class = true;
            matching_owner = false;
        } else if (after_class && token.kind == PG_IDENT && token.text) {
            matching_owner = strcmp(token.text, owner) == 0;
            after_class = false;
        } else if (matching_owner && token.kind == PG_LBRACE) {
            matching_owner = false;
        } else if (matching_owner && token.kind == PG_SEMI) {
            matching_owner = false;
        } else if (matching_owner && token.kind == PG_IDENT && token.text) {
            bool cpp_base = bn_name && strcmp(token.text, bn_name) == 0;
            bool java_base = interface_name && strstr(token.text, interface_name) &&
                             strstr(token.text, ".Stub");
            if (cpp_base || java_base) {
                pg_token_free(&token);
                return true;
            }
        }
        pg_token_free(&token);
    }
    pg_token_free(&token);
    return false;
}

static void aidl_method_free(aidl_method_t *method) {
    if (!method) return;
    free(method->name);
    free(method->return_type);
    pg_strings_free(&method->parameter_types);
    pg_strings_free(&method->annotations);
    memset(method, 0, sizeof(*method));
}

static void aidl_member_free(aidl_member_t *member) {
    if (!member) return;
    free(member->name);
    free(member->type);
    pg_strings_free(&member->annotations);
    memset(member, 0, sizeof(*member));
}

static bool aidl_methods_add(aidl_methods_t *methods, aidl_method_t *method) {
    if (methods->count == methods->cap) {
        int new_cap = methods->cap ? methods->cap * 2 : 8;
        aidl_method_t *items = realloc(methods->items, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        methods->items = items;
        methods->cap = new_cap;
    }
    methods->items[methods->count++] = *method;
    memset(method, 0, sizeof(*method));
    return true;
}

static bool aidl_members_add(aidl_members_t *members, aidl_member_t *member) {
    if (members->count == members->cap) {
        int new_cap = members->cap ? members->cap * 2 : 8;
        aidl_member_t *items = realloc(members->items, (size_t)new_cap * sizeof(*items));
        if (!items) return false;
        members->items = items;
        members->cap = new_cap;
    }
    members->items[members->count++] = *member;
    memset(member, 0, sizeof(*member));
    return true;
}

static void aidl_decl_free(aidl_decl_t *decl) {
    if (!decl) return;
    free(decl->package_name);
    free(decl->name);
    free(decl->kind);
    pg_strings_free(&decl->imports);
    pg_strings_free(&decl->annotations);
    for (int i = 0; i < decl->methods.count; i++) aidl_method_free(&decl->methods.items[i]);
    free(decl->methods.items);
    for (int i = 0; i < decl->members.count; i++) aidl_member_free(&decl->members.items[i]);
    free(decl->members.items);
    memset(decl, 0, sizeof(*decl));
}

static bool aidl_is_builtin_type(const char *value) {
    static const char *const builtins[] = {
        "void", "boolean", "byte", "char", "int", "long", "float", "double",
        "String", "IBinder", "ParcelFileDescriptor", "ParcelableHolder", "List",
        "Map", "CharSequence", "FileDescriptor", "in", "out", "inout", "oneway",
        "const", "cpp_header", "ndk_header", "rust_type",
    };
    if (!value || !value[0]) return true;
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(value, builtins[i]) == 0) return true;
    }
    return false;
}

static bool aidl_process_parameter(pg_strings_t *identifiers, aidl_method_t *method) {
    if (!identifiers || identifiers->count < 2) return true;
    for (int i = 0; i + 1 < identifiers->count; i++) {
        if (!aidl_is_builtin_type(identifiers->items[i]) &&
            !pg_strings_add(&method->parameter_types, identifiers->items[i])) return false;
    }
    return true;
}

static bool aidl_parse_method(const char *source, size_t length, aidl_method_t *method) {
    pg_lexer_t lexer = {.source = source, .length = length};
    pg_strings_t before = {0};
    pg_strings_t parameter = {0};
    bool before_parameters = true;
    bool annotation_name = false;
    bool maybe_annotation_args = false;
    int annotation_depth = 0;
    int paren_depth = 0;
    bool ok = true;
    pg_token_t token;
    while ((token = pg_next(&lexer)).kind != PG_EOF) {
        if (annotation_depth > 0) {
            if (token.kind == PG_LPAREN) annotation_depth++;
            if (token.kind == PG_RPAREN) annotation_depth--;
            pg_token_free(&token);
            continue;
        }
        if (token.kind == PG_AT) {
            annotation_name = true;
            maybe_annotation_args = false;
            pg_token_free(&token);
            continue;
        }
        if (annotation_name && token.kind == PG_IDENT && token.text) {
            if (before_parameters && !pg_strings_add(&method->annotations, token.text)) ok = false;
            annotation_name = false;
            maybe_annotation_args = true;
            pg_token_free(&token);
            if (!ok) break;
            continue;
        }
        if (maybe_annotation_args) {
            maybe_annotation_args = false;
            if (token.kind == PG_LPAREN) {
                annotation_depth = 1;
                pg_token_free(&token);
                continue;
            }
        }
        if (before_parameters && token.kind == PG_IDENT && token.text) {
            if (strcmp(token.text, "oneway") == 0) method->oneway = true;
            if (!pg_strings_append(&before, token.text)) ok = false;
        } else if (before_parameters && token.kind == PG_LPAREN) {
            if (before.count < 2) {
                ok = false;
            } else {
                method->name = strdup(before.items[before.count - 1]);
                for (int i = before.count - 2; i >= 0 && !method->return_type; i--) {
                    if (!aidl_is_builtin_type(before.items[i]) ||
                        strcmp(before.items[i], "void") == 0 ||
                        strcmp(before.items[i], "boolean") == 0 ||
                        strcmp(before.items[i], "byte") == 0 ||
                        strcmp(before.items[i], "char") == 0 ||
                        strcmp(before.items[i], "int") == 0 ||
                        strcmp(before.items[i], "long") == 0 ||
                        strcmp(before.items[i], "float") == 0 ||
                        strcmp(before.items[i], "double") == 0 ||
                        strcmp(before.items[i], "String") == 0) {
                        method->return_type = strdup(before.items[i]);
                    }
                }
                ok = method->name && method->return_type;
            }
            before_parameters = false;
            paren_depth = 1;
        } else if (!before_parameters && token.kind == PG_LPAREN) {
            paren_depth++;
        } else if (!before_parameters && token.kind == PG_RPAREN) {
            if (paren_depth == 1 && !aidl_process_parameter(&parameter, method)) ok = false;
            pg_strings_free(&parameter);
            paren_depth--;
        } else if (!before_parameters && paren_depth == 1 && token.kind == PG_COMMA) {
            if (!aidl_process_parameter(&parameter, method)) ok = false;
            pg_strings_free(&parameter);
        } else if (!before_parameters && paren_depth >= 1 && token.kind == PG_IDENT && token.text) {
            if (!pg_strings_append(&parameter, token.text)) ok = false;
        }
        pg_token_free(&token);
        if (!ok) break;
    }
    pg_token_free(&token);
    pg_strings_free(&before);
    pg_strings_free(&parameter);
    if (!ok || !method->name) {
        aidl_method_free(method);
        return false;
    }
    return true;
}

static bool aidl_parse_member(const char *source, size_t length, bool enum_value,
                              aidl_member_t *member) {
    pg_lexer_t lexer = {.source = source, .length = length};
    pg_strings_t identifiers = {0};
    bool annotation_name = false;
    bool maybe_annotation_args = false;
    int annotation_depth = 0;
    bool ok = true;
    pg_token_t token;
    while ((token = pg_next(&lexer)).kind != PG_EOF) {
        if (annotation_depth > 0) {
            if (token.kind == PG_LPAREN) annotation_depth++;
            if (token.kind == PG_RPAREN) annotation_depth--;
            pg_token_free(&token);
            continue;
        }
        if (maybe_annotation_args) {
            maybe_annotation_args = false;
            if (token.kind == PG_LPAREN) {
                annotation_depth = 1;
                pg_token_free(&token);
                continue;
            }
        }
        if (token.kind == PG_EQUAL) {
            pg_token_free(&token);
            break;
        }
        if (token.kind == PG_AT) {
            annotation_name = true;
        } else if (annotation_name && token.kind == PG_IDENT && token.text) {
            if (!pg_strings_add(&member->annotations, token.text)) ok = false;
            annotation_name = false;
            maybe_annotation_args = true;
        } else if (token.kind == PG_IDENT && token.text) {
            if (!pg_strings_append(&identifiers, token.text)) ok = false;
        }
        pg_token_free(&token);
        if (!ok) break;
    }
    pg_token_free(&token);
    if (ok && identifiers.count > 0) {
        if (enum_value) {
            member->name = strdup(identifiers.items[0]);
            member->type = strdup("enum_value");
        } else if (identifiers.count >= 2) {
            member->name = strdup(identifiers.items[identifiers.count - 1]);
            for (int i = identifiers.count - 2; i >= 0 && !member->type; i--) {
                if (strcmp(identifiers.items[i], "const") != 0) {
                    member->type = strdup(identifiers.items[i]);
                }
            }
        }
        ok = member->name && member->type;
    } else {
        ok = false;
    }
    pg_strings_free(&identifiers);
    if (!ok) aidl_member_free(member);
    return ok;
}

static bool aidl_parse_statement(aidl_decl_t *decl, const char *source, size_t length) {
    while (length && isspace((unsigned char)*source)) {
        source++;
        length--;
    }
    while (length && isspace((unsigned char)source[length - 1])) length--;
    if (!length) return true;
    if (strcmp(decl->kind, "interface") == 0 && memchr(source, '(', length)) {
        aidl_method_t method = {0};
        if (!aidl_parse_method(source, length, &method)) return false;
        if (!aidl_methods_add(&decl->methods, &method)) {
            aidl_method_free(&method);
            return false;
        }
        return true;
    }
    aidl_member_t member = {0};
    if (!aidl_parse_member(source, length, strcmp(decl->kind, "enum") == 0, &member)) {
        return false;
    }
    if (!aidl_members_add(&decl->members, &member)) {
        aidl_member_free(&member);
        return false;
    }
    return true;
}

static bool aidl_declaration_keyword(const char *text) {
    return text && (strcmp(text, "interface") == 0 || strcmp(text, "parcelable") == 0 ||
                    strcmp(text, "union") == 0 || strcmp(text, "enum") == 0);
}

static bool parse_aidl_protocol(const char *source, size_t length, aidl_decl_t *decl) {
    pg_lexer_t lexer = {.source = source, .length = length};
    bool annotation_name = false;
    bool pending_oneway = false;
    bool in_body = false;
    int brace_depth = 0;
    size_t statement_start = SIZE_MAX;
    pg_token_t token;
    while ((token = pg_next(&lexer)).kind != PG_EOF) {
        size_t token_start = lexer.last_start;
        if (!decl->kind && token.kind == PG_AT) {
            annotation_name = true;
        } else if (!decl->kind && annotation_name && token.kind == PG_IDENT && token.text) {
            if (!pg_strings_add(&decl->annotations, token.text)) goto fail;
            annotation_name = false;
        } else if (!decl->kind && token.kind == PG_IDENT && token.text &&
                   strcmp(token.text, "package") == 0) {
            pg_token_free(&token);
            token = pg_next(&lexer);
            if (token.kind == PG_IDENT && token.text) {
                free(decl->package_name);
                decl->package_name = strdup(token.text);
                if (!decl->package_name) goto fail;
            }
        } else if (!decl->kind && token.kind == PG_IDENT && token.text &&
                   strcmp(token.text, "import") == 0) {
            pg_token_free(&token);
            token = pg_next(&lexer);
            if (token.kind == PG_IDENT && token.text &&
                !pg_strings_add(&decl->imports, token.text)) goto fail;
        } else if (!decl->kind && token.kind == PG_IDENT && token.text &&
                   strcmp(token.text, "oneway") == 0) {
            pending_oneway = true;
        } else if (!decl->kind && token.kind == PG_IDENT &&
                   aidl_declaration_keyword(token.text)) {
            decl->kind = strdup(token.text);
            decl->oneway = pending_oneway;
            pg_token_free(&token);
            token = pg_next(&lexer);
            if (token.kind == PG_IDENT && token.text) decl->name = strdup(token.text);
            if (!decl->kind || !decl->name) goto fail;
        } else if (decl->kind && token.kind == PG_LBRACE) {
            brace_depth++;
            in_body = true;
        } else if (in_body && token.kind == PG_RBRACE) {
            if (brace_depth == 1 && statement_start != SIZE_MAX &&
                strcmp(decl->kind, "enum") == 0 &&
                !aidl_parse_statement(decl, source + statement_start,
                                      token_start - statement_start)) goto fail;
            statement_start = SIZE_MAX;
            if (--brace_depth == 0) in_body = false;
        } else if (in_body && brace_depth == 1) {
            if (statement_start == SIZE_MAX && token.kind != PG_SEMI && token.kind != PG_COMMA) {
                statement_start = token_start;
            }
            bool boundary = token.kind == PG_SEMI ||
                            (token.kind == PG_COMMA && strcmp(decl->kind, "enum") == 0);
            if (boundary && statement_start != SIZE_MAX) {
                if (!aidl_parse_statement(decl, source + statement_start,
                                          token_start - statement_start)) goto fail;
                statement_start = SIZE_MAX;
            }
        }
        pg_token_free(&token);
    }
    pg_token_free(&token);
    return true;
fail:
    pg_token_free(&token);
    return false;
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

static int insert_protocol_node_properties(link_ctx_t *ctx, const char *id, const char *repo_id,
                                           const char *kind, const char *name,
                                           const char *qualified_name, const char *file_path,
                                           const char *symbol_global_id, const char *properties) {
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
    sqlite3_bind_text(ctx->insert_node, 9, properties ? properties : "{}", -1,
                      SQLITE_TRANSIENT);
    return sqlite3_step(ctx->insert_node) == SQLITE_DONE ? 0 : -1;
}

static int insert_protocol_node(link_ctx_t *ctx, const char *id, const char *repo_id,
                                const char *kind, const char *name, const char *qualified_name,
                                const char *file_path, const char *symbol_global_id) {
    return insert_protocol_node_properties(ctx, id, repo_id, kind, name, qualified_name,
                                           file_path, symbol_global_id, "{}");
}

static int insert_protocol_edge_properties(link_ctx_t *ctx, const char *source_id,
                                           const char *target_id, const char *type,
                                           double confidence, const char *evidence,
                                           const char *properties) {
    sqlite3_reset(ctx->insert_edge);
    sqlite3_clear_bindings(ctx->insert_edge);
    sqlite3_bind_text(ctx->insert_edge, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_edge, 2, target_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_edge, 3, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(ctx->insert_edge, 4, confidence);
    sqlite3_bind_text(ctx->insert_edge, 5, evidence, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_edge, 6, properties ? properties : "{}", -1,
                      SQLITE_TRANSIENT);
    return sqlite3_step(ctx->insert_edge) == SQLITE_DONE ? 0 : -1;
}

static int insert_protocol_edge(link_ctx_t *ctx, const char *source_id, const char *target_id,
                                const char *type, double confidence, const char *evidence) {
    return insert_protocol_edge_properties(ctx, source_id, target_id, type, confidence,
                                           evidence, "{}");
}

static bool contains_class_token(const char *qualified_name, const char *token) {
    if (!qualified_name || !token || !token[0]) return false;
    const char *hit = strstr(qualified_name, token);
    while (hit) {
        bool left = hit == qualified_name || hit[-1] == '.' || hit[-1] == '$' || hit[-1] == ':';
        char right_char = hit[strlen(token)];
        bool right = right_char == '\0' || right_char == '.' || right_char == '$' ||
                     right_char == ':';
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

static int add_binder_symbol_endpoint(link_ctx_t *ctx, const binder_symbol_t *symbol,
                                      const char *kind) {
    return insert_protocol_node(ctx, symbol->global_id, symbol->repo_id, kind,
                                symbol->name, symbol->qualified_name, symbol->file_path,
                                symbol->global_id);
}

static char *binder_evidence_properties(const char *transaction_name,
                                        const char *method_name,
                                        const char *owner_name,
                                        const char *qualified_owner) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "transaction", transaction_name);
    yyjson_mut_obj_add_strcpy(doc, root, "method", method_name);
    if (owner_name) yyjson_mut_obj_add_strcpy(doc, root, "owner", owner_name);
    if (qualified_owner) {
        yyjson_mut_obj_add_strcpy(doc, root, "qualified_owner", qualified_owner);
    }
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
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

static int link_java_generated_type(link_ctx_t *ctx, const char *aidl_id,
                                    const char *interface_name, const char *candidate,
                                    bool require_stub_owner, const char *kind,
                                    const char *edge_type) {
    sqlite3_reset(ctx->find_symbols);
    sqlite3_clear_bindings(ctx->find_symbols);
    sqlite3_bind_text(ctx->find_symbols, 1, ctx->workspace->workspace_id, -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->find_symbols, 2, candidate, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(ctx->find_symbols) == SQLITE_ROW) {
        const char *qualified = (const char *)sqlite3_column_text(ctx->find_symbols, 3);
        const char *file_path = (const char *)sqlite3_column_text(ctx->find_symbols, 4);
        if (binder_backend(qualified, file_path) != BINDER_BACKEND_JAVA ||
            !contains_class_token(qualified, interface_name) ||
            !contains_class_token(qualified, candidate) ||
            (require_stub_owner && !contains_class_token(qualified, "Stub"))) continue;
        char target_id[65];
        if (add_symbol_endpoint(ctx, ctx->find_symbols, kind, target_id) != 0 ||
            insert_protocol_edge(ctx, aidl_id, target_id, edge_type, 1.0,
                                 "aidl_generated_name") != 0) return -1;
    }
    return sqlite3_errcode(ctx->db) == SQLITE_OK ||
           sqlite3_errcode(ctx->db) == SQLITE_DONE ? 0 : -1;
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
        const char *label = (const char *)sqlite3_column_text(ctx->find_symbols, 5);
        const char *kind = NULL;
        const char *edge_type = NULL;
        if (!label || strcmp(label, "Method") != 0) continue;
        if (contains_class_token(qualified, bn) ||
            (strstr(qualified, interface_name) &&
             contains_class_token(qualified, "Stub") &&
             !contains_class_token(qualified, "Proxy"))) {
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

static yyjson_mut_val *aidl_string_array(yyjson_mut_doc *doc, const pg_strings_t *values) {
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    for (int i = 0; values && i < values->count; i++) {
        yyjson_mut_arr_add_strcpy(doc, array, values->items[i]);
    }
    return array;
}

static const char *aidl_stability(const pg_strings_t *annotations) {
    for (int i = 0; annotations && i < annotations->count; i++) {
        if (strcmp(annotations->items[i], "VintfStability") == 0) return "vintf";
        if (strcmp(annotations->items[i], "StableParcelable") == 0 ||
            strcmp(annotations->items[i], "JavaOnlyStableParcelable") == 0) {
            return "stable_parcelable";
        }
    }
    return "local";
}

static char *aidl_write_properties(yyjson_mut_doc *doc) {
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *aidl_decl_properties(const aidl_decl_t *decl) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "declaration_kind", decl->kind);
    yyjson_mut_obj_add_strcpy(doc, root, "package",
                             decl->package_name ? decl->package_name : "");
    yyjson_mut_obj_add_bool(doc, root, "oneway", decl->oneway);
    yyjson_mut_obj_add_strcpy(doc, root, "stability", aidl_stability(&decl->annotations));
    yyjson_mut_obj_add_val(doc, root, "annotations",
                           aidl_string_array(doc, &decl->annotations));
    yyjson_mut_obj_add_val(doc, root, "imports", aidl_string_array(doc, &decl->imports));
    return aidl_write_properties(doc);
}

static char *aidl_method_properties(const aidl_method_t *method, bool effective_oneway) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "return_type",
                             method->return_type ? method->return_type : "void");
    yyjson_mut_obj_add_bool(doc, root, "oneway", effective_oneway);
    yyjson_mut_obj_add_val(doc, root, "annotations",
                           aidl_string_array(doc, &method->annotations));
    yyjson_mut_obj_add_val(doc, root, "parameter_types",
                           aidl_string_array(doc, &method->parameter_types));
    return aidl_write_properties(doc);
}

static char *aidl_member_properties(const aidl_member_t *member) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "type", member->type ? member->type : "");
    yyjson_mut_obj_add_val(doc, root, "annotations",
                           aidl_string_array(doc, &member->annotations));
    return aidl_write_properties(doc);
}

static char *aidl_reference_properties(const char *target, const char *role) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "target_qualified_name", target);
    yyjson_mut_obj_add_strcpy(doc, root, "role", role);
    yyjson_mut_obj_add_strcpy(doc, root, "resolution", "unresolved");
    yyjson_mut_obj_add_int(doc, root, "candidate_count", 0);
    return aidl_write_properties(doc);
}

static char *aidl_qualified_type(const aidl_decl_t *decl, const char *type) {
    if (!type || !type[0] || aidl_is_builtin_type(type)) return NULL;
    if (strchr(type, '.')) return strdup(type);
    for (int i = 0; i < decl->imports.count; i++) {
        const char *leaf = strrchr(decl->imports.items[i], '.');
        leaf = leaf ? leaf + 1 : decl->imports.items[i];
        if (strcmp(leaf, type) == 0) return strdup(decl->imports.items[i]);
    }
    size_t size = strlen(type) + (decl->package_name ? strlen(decl->package_name) + 1 : 0) + 1;
    char *qualified = malloc(size);
    if (!qualified) return NULL;
    if (decl->package_name && decl->package_name[0]) {
        (void)snprintf(qualified, size, "%s.%s", decl->package_name, type);
    } else {
        (void)snprintf(qualified, size, "%s", type);
    }
    return qualified;
}

static int add_aidl_reference(link_ctx_t *ctx, const cbm_aosp_repo_t *repo,
                              const aidl_decl_t *decl, const char *owner_id,
                              const char *owner_qn, const char *type, const char *role,
                              const char *rel_path) {
    char *target = aidl_qualified_type(decl, type);
    if (!target) return aidl_is_builtin_type(type) ? 0 : -1;
    size_t qn_size = strlen(owner_qn) + strlen(role) + strlen(target) + 9;
    char *reference_qn = malloc(qn_size);
    char *properties = aidl_reference_properties(target, role);
    if (!reference_qn || !properties) {
        free(target);
        free(reference_qn);
        free(properties);
        return -1;
    }
    (void)snprintf(reference_qn, qn_size, "%s::type:%s:%s", owner_qn, role, target);
    char reference_id[65];
    hash_id(ctx->workspace->workspace_id, "AIDL_TYPE_REFERENCE", reference_qn,
            reference_id);
    int rc = insert_protocol_node_properties(ctx, reference_id, repo->repo_id,
                                             "AIDL_TYPE_REFERENCE", target, reference_qn,
                                             rel_path, NULL, properties);
    if (rc == 0) {
        rc = insert_protocol_edge_properties(ctx, owner_id, reference_id,
                                             "REFERENCES_TYPE", 1.0, "aidl_ast",
                                             properties);
    }
    free(target);
    free(reference_qn);
    free(properties);
    return rc;
}

static const char *aidl_protocol_kind(const char *kind) {
    if (strcmp(kind, "interface") == 0) return "AIDL_INTERFACE";
    if (strcmp(kind, "parcelable") == 0) return "AIDL_PARCELABLE";
    if (strcmp(kind, "union") == 0) return "AIDL_UNION";
    if (strcmp(kind, "enum") == 0) return "AIDL_ENUM";
    return "AIDL_DECLARATION";
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
    if (!decl.name || !decl.kind) {
        aidl_decl_free(&decl);
        return 0;
    }
    size_t qn_len = strlen(decl.name) +
                    (decl.package_name ? strlen(decl.package_name) + 1 : 0) + 1;
    char *declaration_qn = malloc(qn_len);
    if (!declaration_qn) {
        aidl_decl_free(&decl);
        return -1;
    }
    if (decl.package_name) {
        (void)snprintf(declaration_qn, qn_len, "%s.%s", decl.package_name, decl.name);
    } else {
        (void)snprintf(declaration_qn, qn_len, "%s", decl.name);
    }
    const char *protocol_kind = aidl_protocol_kind(decl.kind);
    char declaration_id[65];
    hash_id(ctx->workspace->workspace_id, protocol_kind, declaration_qn, declaration_id);
    char *declaration_properties = aidl_decl_properties(&decl);
    if (!declaration_properties ||
        insert_protocol_node_properties(ctx, declaration_id, repo->repo_id, protocol_kind,
                                        decl.name, declaration_qn, rel_path, NULL,
                                        declaration_properties) != 0) {
        free(declaration_properties);
        free(declaration_qn);
        aidl_decl_free(&decl);
        return -1;
    }
    free(declaration_properties);

    for (int i = 0; i < decl.imports.count; i++) {
        size_t import_qn_size = strlen(declaration_qn) + strlen(decl.imports.items[i]) + 10;
        char *import_qn = malloc(import_qn_size);
        char *properties = aidl_reference_properties(decl.imports.items[i], "import");
        if (!import_qn || !properties) {
            free(import_qn);
            free(properties);
            goto fail;
        }
        (void)snprintf(import_qn, import_qn_size, "%s::import:%s", declaration_qn,
                       decl.imports.items[i]);
        char import_id[65];
        hash_id(ctx->workspace->workspace_id, "AIDL_IMPORT", import_qn, import_id);
        int rc = insert_protocol_node_properties(ctx, import_id, repo->repo_id,
                                                 "AIDL_IMPORT", decl.imports.items[i],
                                                 import_qn, rel_path, NULL, properties);
        if (rc == 0) {
            rc = insert_protocol_edge_properties(ctx, declaration_id, import_id,
                                                 "DECLARES_IMPORT", 1.0, "aidl_ast",
                                                 properties);
        }
        free(import_qn);
        free(properties);
        if (rc != 0) goto fail;
    }

    const char *stem = decl.name[0] == 'I' && decl.name[1] ? decl.name + 1 : decl.name;
    if (strcmp(decl.kind, "interface") == 0) {
        char candidate[512];
        (void)snprintf(candidate, sizeof(candidate), "Bn%s", stem);
        if (link_generated_type(ctx, declaration_id, candidate, "BINDER_SERVER_TYPE",
                                "AIDL_GENERATES_SERVER") != 0) goto fail;
        (void)snprintf(candidate, sizeof(candidate), "Bp%s", stem);
        if (link_generated_type(ctx, declaration_id, candidate, "BINDER_CLIENT_TYPE",
                                "AIDL_GENERATES_CLIENT") != 0) goto fail;
        if (link_java_generated_type(ctx, declaration_id, decl.name, "Stub", false,
                                     "BINDER_SERVER_TYPE",
                                     "AIDL_GENERATES_SERVER") != 0) goto fail;
        if (link_java_generated_type(ctx, declaration_id, decl.name, "Proxy", true,
                                     "BINDER_CLIENT_TYPE",
                                     "AIDL_GENERATES_CLIENT") != 0) goto fail;
        if (link_generated_type(ctx, declaration_id, decl.name, "BINDER_INTERFACE_TYPE",
                                "AIDL_GENERATES_INTERFACE") != 0) goto fail;
    }

    for (int i = 0; i < decl.methods.count; i++) {
        aidl_method_t *method = &decl.methods.items[i];
        size_t method_qn_len = strlen(declaration_qn) + strlen(method->name) + 2;
        char *method_qn = malloc(method_qn_len);
        if (!method_qn) goto fail;
        (void)snprintf(method_qn, method_qn_len, "%s.%s", declaration_qn, method->name);
        char method_id[65];
        hash_id(ctx->workspace->workspace_id, "AIDL_METHOD", method_qn, method_id);
        bool effective_oneway = decl.oneway || method->oneway;
        char *properties = aidl_method_properties(method, effective_oneway);
        int rc = properties
            ? insert_protocol_node_properties(ctx, method_id, repo->repo_id, "AIDL_METHOD",
                                              method->name, method_qn, rel_path, NULL, properties)
            : -1;
        if (rc == 0) {
            rc = insert_protocol_edge_properties(ctx, declaration_id, method_id,
                                                 "DECLARES_METHOD", 1.0, "aidl_ast",
                                                 properties);
        }
        if (rc == 0 && strcmp(decl.kind, "interface") == 0) {
            rc = link_binder_method(ctx, method_id, method->name, decl.name, stem);
        }
        if (rc == 0 && strcmp(decl.kind, "interface") == 0) {
            rc = link_binder_flow(ctx, method_id, method->name, decl.name, stem);
        }
        if (rc == 0 && !aidl_is_builtin_type(method->return_type)) {
            rc = add_aidl_reference(ctx, repo, &decl, method_id, method_qn,
                                    method->return_type, "return", rel_path);
        }
        for (int p = 0; p < method->parameter_types.count && rc == 0; p++) {
            rc = add_aidl_reference(ctx, repo, &decl, method_id, method_qn,
                                    method->parameter_types.items[p], "parameter", rel_path);
        }
        free(properties);
        free(method_qn);
        if (rc != 0) goto fail;
    }

    for (int i = 0; i < decl.members.count; i++) {
        aidl_member_t *member = &decl.members.items[i];
        size_t member_qn_len = strlen(declaration_qn) + strlen(member->name) + 2;
        char *member_qn = malloc(member_qn_len);
        char *properties = aidl_member_properties(member);
        if (!member_qn || !properties) {
            free(member_qn);
            free(properties);
            goto fail;
        }
        (void)snprintf(member_qn, member_qn_len, "%s.%s", declaration_qn, member->name);
        const char *member_kind = strcmp(decl.kind, "enum") == 0 ? "AIDL_ENUM_VALUE" :
                                  strcmp(decl.kind, "interface") == 0 ? "AIDL_CONSTANT" :
                                  "AIDL_FIELD";
        const char *edge_type = strcmp(decl.kind, "enum") == 0 ? "DECLARES_ENUM_VALUE" :
                                strcmp(decl.kind, "interface") == 0 ? "DECLARES_CONSTANT" :
                                "DECLARES_FIELD";
        char member_id[65];
        hash_id(ctx->workspace->workspace_id, member_kind, member_qn, member_id);
        int rc = insert_protocol_node_properties(ctx, member_id, repo->repo_id, member_kind,
                                                 member->name, member_qn, rel_path, NULL,
                                                 properties);
        if (rc == 0) {
            rc = insert_protocol_edge_properties(ctx, declaration_id, member_id, edge_type,
                                                 1.0, "aidl_ast", properties);
        }
        if (rc == 0 && strcmp(decl.kind, "enum") != 0 &&
            !aidl_is_builtin_type(member->type)) {
            rc = add_aidl_reference(ctx, repo, &decl, member_id, member_qn, member->type,
                                    "field", rel_path);
        }
        free(member_qn);
        free(properties);
        if (rc != 0) goto fail;
    }
    free(declaration_qn);
    aidl_decl_free(&decl);
    return 0;
fail:
    free(declaration_qn);
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

static const char *service_operation_kind(const char *identifier, const char **api_out) {
    const char *leaf = identifier ? strrchr(identifier, '.') : NULL;
    leaf = leaf ? leaf + 1 : identifier;
    if (!leaf) return NULL;
    static const char *const registrations[] = {
        "addService", "AServiceManager_addService", "publishBinderService", "registerService",
    };
    static const char *const lookups[] = {
        "getService", "checkService", "getDeclaredService",
        "AServiceManager_getService", "AServiceManager_checkService",
    };
    static const char *const waits[] = {
        "waitForService", "waitForDeclaredService", "AServiceManager_waitForService",
    };
    for (size_t i = 0; i < sizeof(registrations) / sizeof(registrations[0]); i++) {
        if (strcmp(leaf, registrations[i]) == 0) {
            if (api_out) *api_out = registrations[i];
            return "register";
        }
    }
    for (size_t i = 0; i < sizeof(lookups) / sizeof(lookups[0]); i++) {
        if (strcmp(leaf, lookups[i]) == 0) {
            if (api_out) *api_out = lookups[i];
            return "lookup";
        }
    }
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
        if (strcmp(leaf, waits[i]) == 0) {
            if (api_out) *api_out = waits[i];
            return "wait";
        }
    }
    return NULL;
}

static bool service_hint_excluded(const char *component) {
    static const char *const excluded[] = {
        "String", "String16", "String8", "ServiceManager", "IServiceManager",
        "AServiceManager_addService", "AServiceManager_getService",
        "AServiceManager_checkService", "AServiceManager_waitForService", "IBinder",
        "Parcel", "Status", "Stub", "Proxy",
    };
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
        if (strcmp(component, excluded[i]) == 0) return true;
    }
    return false;
}

static void service_consider_hint(const char *identifier, char **implementation_hint,
                                  char **interface_hint) {
    if (!identifier) return;
    char *copy = strdup(identifier);
    if (!copy) return;
    char *component = strtok(copy, ".");
    while (component) {
        size_t length = strlen(component);
        if (length > 1 && component[0] == 'I' && isupper((unsigned char)component[1]) &&
            !service_hint_excluded(component)) {
            if (interface_hint && !*interface_hint) *interface_hint = strdup(component);
        } else if (implementation_hint && isupper((unsigned char)component[0]) &&
                   !service_hint_excluded(component) &&
                   !(length > 1 && component[0] == 'I' &&
                     isupper((unsigned char)component[1]))) {
            char *replacement = strdup(component);
            if (replacement) {
                free(*implementation_hint);
                *implementation_hint = replacement;
            }
        }
        component = strtok(NULL, ".");
    }
    free(copy);
}

static int service_source_line(const char *source, size_t offset) {
    int line = 1;
    for (size_t i = 0; source && i < offset; i++) if (source[i] == '\n') line++;
    return line;
}

static int service_source_column(const char *source, size_t offset) {
    size_t line_start = offset;
    while (line_start > 0 && source[line_start - 1] != '\n') line_start--;
    return (int)(offset - line_start + 1);
}

static int service_find_caller(link_ctx_t *ctx, const cbm_aosp_repo_t *repo,
                               const char *rel_path, int line, binder_symbol_t *caller) {
    const char *sql =
        "SELECT global_id,repo_id,name,qualified_name,file_path,label,start_line,end_line "
        "FROM symbols WHERE workspace_id=?1 AND repo_id=?2 AND file_path=?3 "
        "AND start_line<=?4 AND end_line>=?4 AND label IN('Function','Method') "
        "ORDER BY (end_line-start_line),start_line DESC,global_id LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, ctx->workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, repo->repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, rel_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, line);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        const char *values[6];
        for (int i = 0; i < 6; i++) values[i] = (const char *)sqlite3_column_text(stmt, i);
        caller->global_id = strdup(values[0] ? values[0] : "");
        caller->repo_id = strdup(values[1] ? values[1] : "");
        caller->name = strdup(values[2] ? values[2] : "");
        caller->qualified_name = strdup(values[3] ? values[3] : "");
        caller->file_path = strdup(values[4] ? values[4] : "");
        caller->label = strdup(values[5] ? values[5] : "");
        caller->start_line = sqlite3_column_int(stmt, 6);
        caller->end_line = sqlite3_column_int(stmt, 7);
        if (!caller->global_id || !caller->repo_id || !caller->name ||
            !caller->qualified_name || !caller->file_path || !caller->label) {
            binder_symbol_free(caller);
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW ? 1 : step == SQLITE_DONE ? 0 : -1;
}

static char *service_operation_properties(const char *operation, const char *api,
                                          const char *service_name, int line, int column,
                                          const char *implementation_hint,
                                          const char *interface_hint,
                                          const char *caller_global_id) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "operation", operation);
    yyjson_mut_obj_add_strcpy(doc, root, "api", api);
    yyjson_mut_obj_add_strcpy(doc, root, "service_name", service_name);
    yyjson_mut_obj_add_int(doc, root, "line", line);
    yyjson_mut_obj_add_int(doc, root, "column", column);
    if (implementation_hint) {
        yyjson_mut_obj_add_strcpy(doc, root, "implementation_hint", implementation_hint);
    }
    if (interface_hint) yyjson_mut_obj_add_strcpy(doc, root, "interface_hint", interface_hint);
    if (caller_global_id) {
        yyjson_mut_obj_add_strcpy(doc, root, "caller_global_id", caller_global_id);
    }
    return aidl_write_properties(doc);
}

static int insert_service_node(link_ctx_t *ctx, const cbm_aosp_repo_t *repo,
                               const char *service_name, const char *rel_path,
                               char service_id[65]) {
    size_t size = strlen(service_name) + 16;
    char *qualified = malloc(size);
    char *properties = NULL;
    if (!qualified) return -1;
    (void)snprintf(qualified, size, "binder-service:%s", service_name);
    hash_id(ctx->workspace->workspace_id, "BINDER_SERVICE", qualified, service_id);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc) {
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_strcpy(doc, root, "service_name", service_name);
        properties = aidl_write_properties(doc);
    }
    if (!properties) {
        free(qualified);
        return -1;
    }
    sqlite3_reset(ctx->insert_service);
    sqlite3_clear_bindings(ctx->insert_service);
    sqlite3_bind_text(ctx->insert_service, 1, service_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_service, 2, ctx->workspace->workspace_id, -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_service, 3, repo->repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_service, 4, "BINDER_SERVICE", -1, SQLITE_STATIC);
    sqlite3_bind_text(ctx->insert_service, 5, service_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_service, 6, qualified, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->insert_service, 7, rel_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_null(ctx->insert_service, 8);
    sqlite3_bind_text(ctx->insert_service, 9, properties, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(ctx->insert_service) == SQLITE_DONE ? 0 : -1;
    free(qualified);
    free(properties);
    return rc;
}

static int link_service_operation(link_ctx_t *ctx, const cbm_aosp_repo_t *repo,
                                  const char *rel_path, int line, int column,
                                  const char *operation,
                                  const char *api, const char *service_name,
                                  char *implementation_hint, char *interface_hint) {
    binder_symbol_t caller = {0};
    int caller_status = service_find_caller(ctx, repo, rel_path, line, &caller);
    if (caller_status < 0) {
        free(interface_hint);
        return -1;
    }
    if (caller_status > 0 && !interface_hint) {
        char *caller_source = binder_symbol_source(ctx, &caller, true);
        if (caller_source) {
            pg_lexer_t lexer = {.source = caller_source, .length = strlen(caller_source)};
            pg_token_t token;
            while (!interface_hint && (token = pg_next(&lexer)).kind != PG_EOF) {
                if (token.kind == PG_IDENT) service_consider_hint(token.text, NULL, &interface_hint);
                pg_token_free(&token);
            }
            pg_token_free(&token);
            free(caller_source);
        }
    }

    char service_id[65];
    if (insert_service_node(ctx, repo, service_name, rel_path, service_id) != 0) {
        binder_symbol_free(&caller);
        free(interface_hint);
        return -1;
    }
    const char *caller_qn = caller_status > 0 ? caller.qualified_name : rel_path;
    size_t operation_qn_size = strlen(caller_qn) + strlen(operation) +
                               strlen(service_name) + 48;
    char *operation_qn = malloc(operation_qn_size);
    char *properties = service_operation_properties(operation, api, service_name, line, column,
        implementation_hint, interface_hint, caller_status > 0 ? caller.global_id : NULL);
    if (!operation_qn || !properties) {
        free(operation_qn);
        free(properties);
        binder_symbol_free(&caller);
        free(interface_hint);
        return -1;
    }
    (void)snprintf(operation_qn, operation_qn_size, "%s::service:%s:%s:%d:%d", caller_qn,
                   operation, service_name, line, column);
    const char *operation_node_kind = strcmp(operation, "register") == 0
        ? "BINDER_SERVICE_REGISTRATION"
        : strcmp(operation, "wait") == 0 ? "BINDER_SERVICE_WAIT" : "BINDER_SERVICE_LOOKUP";
    const char *service_edge = strcmp(operation, "register") == 0
        ? "REGISTERS_BINDER_SERVICE"
        : strcmp(operation, "wait") == 0 ? "WAITS_FOR_BINDER_SERVICE"
                                           : "LOOKS_UP_BINDER_SERVICE";
    char operation_id[65];
    hash_id(ctx->workspace->workspace_id, operation_node_kind, operation_qn, operation_id);
    int rc = insert_protocol_node_properties(ctx, operation_id, repo->repo_id,
        operation_node_kind, api, operation_qn, rel_path, NULL, properties);
    if (rc == 0) {
        rc = insert_protocol_edge_properties(ctx, operation_id, service_id, service_edge,
                                             1.0, "service_manager_literal", properties);
    }
    if (rc == 0 && caller_status > 0) {
        size_t caller_wrapper_size = strlen(caller.qualified_name) + 24;
        char *caller_wrapper_qn = malloc(caller_wrapper_size);
        if (!caller_wrapper_qn) {
            rc = -1;
        } else {
            (void)snprintf(caller_wrapper_qn, caller_wrapper_size, "%s::service-caller",
                           caller.qualified_name);
            char caller_id[65];
            hash_id(ctx->workspace->workspace_id, "BINDER_SERVICE_CALLER", caller_wrapper_qn,
                    caller_id);
            if (insert_protocol_node_properties(ctx, caller_id, caller.repo_id,
                    "BINDER_SERVICE_CALLER",
                    caller.name, caller_wrapper_qn, caller.file_path, caller.global_id, "{}") != 0 ||
                insert_protocol_edge_properties(ctx, caller_id, operation_id,
                    "CALLS_SERVICE_MANAGER", 1.0, "enclosing_symbol", properties) != 0) rc = -1;
            free(caller_wrapper_qn);
        }
    }
    free(operation_qn);
    free(properties);
    binder_symbol_free(&caller);
    free(interface_hint);
    return rc;
}

static int parse_service_manager_calls(link_ctx_t *ctx, const cbm_aosp_repo_t *repo,
                                       const char *rel_path, const char *source) {
    pg_lexer_t lexer = {.source = source, .length = strlen(source)};
    pg_token_t token;
    int rc = 0;
    while (rc == 0 && (token = pg_next(&lexer)).kind != PG_EOF) {
        const char *api = NULL;
        const char *operation = token.kind == PG_IDENT
            ? service_operation_kind(token.text, &api) : NULL;
        size_t call_offset = lexer.last_start;
        if (!operation) {
            pg_token_free(&token);
            continue;
        }
        pg_token_free(&token);
        token = pg_next(&lexer);
        if (token.kind != PG_LPAREN) {
            pg_token_free(&token);
            continue;
        }
        pg_token_free(&token);
        int depth = 1;
        int argument_index = 0;
        int service_argument = strcmp(api, "AServiceManager_addService") == 0 ? 1 : 0;
        char *service_literal = NULL;
        char *implementation_hint = NULL;
        char *interface_hint = NULL;
        while (depth > 0 && (token = pg_next(&lexer)).kind != PG_EOF) {
            if (token.kind == PG_LPAREN) depth++;
            if (token.kind == PG_RPAREN) depth--;
            if (depth == 1 && token.kind == PG_COMMA) argument_index++;
            if (depth > 0 && token.kind == PG_STRING && token.text &&
                argument_index == service_argument && !service_literal) {
                service_literal = strdup(token.text);
            } else if (depth > 0 && token.kind == PG_IDENT && token.text) {
                service_consider_hint(token.text, &implementation_hint, &interface_hint);
            }
            pg_token_free(&token);
        }
        pg_token_free(&token);
        if (service_literal && service_literal[0]) {
            int line = service_source_line(source, call_offset);
            int column = service_source_column(source, call_offset);
            rc = link_service_operation(ctx, repo, rel_path, line, column, operation, api,
                                        service_literal, implementation_hint, interface_hint);
            interface_hint = NULL;
        }
        free(service_literal);
        free(implementation_hint);
        free(interface_hint);
    }
    pg_token_free(&token);
    return rc;
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
        bool managed = (name_len > 5 && strcmp(entry->name + name_len - 5, ".java") == 0) ||
                       (name_len > 3 && strcmp(entry->name + name_len - 3, ".kt") == 0);
        if (aidl) {
            rc = link_aidl_file(ctx, repo, abs_path, rel_path);
        } else if (native || managed) {
            size_t length = 0;
            char *source = pg_read_file(abs_path, &length);
            if (source) {
                if (native && strstr(source, "JNINativeMethod")) {
                    rc = parse_dynamic_jni(ctx, repo, rel_path, source);
                }
                if (rc == 0 && (strstr(source, "ServiceManager") ||
                                strstr(source, "addService") ||
                                strstr(source, "registerService") ||
                                strstr(source, "getService") ||
                                strstr(source, "checkService") ||
                                strstr(source, "waitForService") ||
                                strstr(source, "publishBinderService"))) {
                    rc = parse_service_manager_calls(ctx, repo, rel_path, source);
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

static int resolve_aidl_references(link_ctx_t *ctx) {
    const char *decl_kinds =
        "('AIDL_INTERFACE','AIDL_PARCELABLE','AIDL_UNION','AIDL_ENUM')";
    char sql[16384];
    (void)snprintf(sql, sizeof(sql),
        "UPDATE protocol_nodes SET properties=json_set(properties,"
        "'$.candidate_count',(SELECT count(*) FROM protocol_nodes target "
        "WHERE target.workspace_id=protocol_nodes.workspace_id "
        "AND target.qualified_name=json_extract(protocol_nodes.properties,'$.target_qualified_name') "
        "AND target.kind IN %s),"
        "'$.resolution',CASE (SELECT count(*) FROM protocol_nodes target "
        "WHERE target.workspace_id=protocol_nodes.workspace_id "
        "AND target.qualified_name=json_extract(protocol_nodes.properties,'$.target_qualified_name') "
        "AND target.kind IN %s) WHEN 0 THEN 'not_found' WHEN 1 THEN 'resolved' ELSE 'ambiguous' END) "
        "WHERE workspace_id='%s' AND kind IN('AIDL_IMPORT','AIDL_TYPE_REFERENCE');"
        "UPDATE protocol_edges SET properties=(SELECT ref.properties FROM protocol_nodes ref "
        "WHERE ref.protocol_id=protocol_edges.target_id) WHERE type IN('DECLARES_IMPORT','REFERENCES_TYPE') "
        "AND target_id IN(SELECT protocol_id FROM protocol_nodes WHERE workspace_id='%s' "
        "AND kind IN('AIDL_IMPORT','AIDL_TYPE_REFERENCE'));"
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
        "SELECT owner.source_id,target.protocol_id,'AIDL_IMPORTS',1.0,'aidl_import',"
        "json_object('reference_node',ref.protocol_id,'target_qualified_name',target.qualified_name) "
        "FROM protocol_nodes ref JOIN protocol_edges owner ON owner.target_id=ref.protocol_id "
        "AND owner.type='DECLARES_IMPORT' JOIN protocol_nodes target "
        "ON target.workspace_id=ref.workspace_id "
        "AND target.qualified_name=json_extract(ref.properties,'$.target_qualified_name') "
        "AND target.kind IN %s WHERE ref.workspace_id='%s' AND ref.kind='AIDL_IMPORT' "
        "AND json_extract(ref.properties,'$.resolution')='resolved';"
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
        "SELECT ref.protocol_id,target.protocol_id,'RESOLVES_TO',1.0,'aidl_import_resolution',"
        "json_object('role','import') FROM protocol_nodes ref JOIN protocol_nodes target "
        "ON target.workspace_id=ref.workspace_id "
        "AND target.qualified_name=json_extract(ref.properties,'$.target_qualified_name') "
        "AND target.kind IN %s WHERE ref.workspace_id='%s' AND ref.kind='AIDL_IMPORT' "
        "AND json_extract(ref.properties,'$.resolution')='resolved';"
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
        "SELECT ref.protocol_id,target.protocol_id,'RESOLVES_TO',1.0,'aidl_type_resolution',"
        "json_object('role',json_extract(ref.properties,'$.role')) FROM protocol_nodes ref "
        "JOIN protocol_nodes target ON target.workspace_id=ref.workspace_id "
        "AND target.qualified_name=json_extract(ref.properties,'$.target_qualified_name') "
        "AND target.kind IN %s WHERE ref.workspace_id='%s' AND ref.kind='AIDL_TYPE_REFERENCE' "
        "AND json_extract(ref.properties,'$.resolution')='resolved';"
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
        "SELECT owner.source_id,target.protocol_id,'USES_TYPE',1.0,'aidl_type_reference',"
        "json_object('reference_node',ref.protocol_id,'role',json_extract(ref.properties,'$.role')) "
        "FROM protocol_nodes ref JOIN protocol_edges owner ON owner.target_id=ref.protocol_id "
        "AND owner.type='REFERENCES_TYPE' JOIN protocol_nodes target "
        "ON target.workspace_id=ref.workspace_id "
        "AND target.qualified_name=json_extract(ref.properties,'$.target_qualified_name') "
        "AND target.kind IN %s WHERE ref.workspace_id='%s' AND ref.kind='AIDL_TYPE_REFERENCE' "
        "AND json_extract(ref.properties,'$.resolution')='resolved';"
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
        "SELECT owner.source_id,target.protocol_id,'USES_CALLBACK',1.0,'aidl_callback_parameter',"
        "json_object('reference_node',ref.protocol_id,'role','parameter') "
        "FROM protocol_nodes ref JOIN protocol_edges owner ON owner.target_id=ref.protocol_id "
        "AND owner.type='REFERENCES_TYPE' JOIN protocol_nodes target "
        "ON target.workspace_id=ref.workspace_id "
        "AND target.qualified_name=json_extract(ref.properties,'$.target_qualified_name') "
        "AND target.kind='AIDL_INTERFACE' WHERE ref.workspace_id='%s' "
        "AND ref.kind='AIDL_TYPE_REFERENCE' AND json_extract(ref.properties,'$.role')='parameter' "
        "AND json_extract(ref.properties,'$.resolution')='resolved';",
        decl_kinds, decl_kinds, ctx->workspace->workspace_id,
        ctx->workspace->workspace_id,
        decl_kinds, ctx->workspace->workspace_id,
        decl_kinds, ctx->workspace->workspace_id,
        decl_kinds, ctx->workspace->workspace_id,
        decl_kinds, ctx->workspace->workspace_id,
        ctx->workspace->workspace_id);
    char *sql_error = NULL;
    int rc = sqlite3_exec(ctx->db, sql, NULL, NULL, &sql_error);
    if (rc != SQLITE_OK) {
        pg_error(ctx->err, ctx->err_size, "cannot resolve AIDL references",
                 sql_error ? sql_error : sqlite3_errmsg(ctx->db));
        sqlite3_free(sql_error);
        return -1;
    }
    return 0;
}

static int resolve_binder_services(link_ctx_t *ctx) {
    const char *sql_template =
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
        "SELECT service.protocol_id,impl.protocol_id,'BINDER_SERVICE_SERVER',1.0,"
        "'service_registration_implementation',json_object("
        "'service_name',json_extract(operation.properties,'$.service_name'),"
        "'implementation_hint',json_extract(operation.properties,'$.implementation_hint')) "
        "FROM protocol_edges registration JOIN protocol_nodes operation "
        "ON operation.protocol_id=registration.source_id "
        "JOIN protocol_nodes service ON service.protocol_id=registration.target_id "
        "JOIN protocol_nodes impl ON impl.workspace_id=service.workspace_id "
        "AND impl.kind='BINDER_IMPLEMENTATION_METHOD' "
        "AND json_extract(impl.properties,'$.owner')="
        "json_extract(operation.properties,'$.implementation_hint') "
        "WHERE registration.type='REGISTERS_BINDER_SERVICE' "
        "AND operation.workspace_id='%s' "
        "AND json_extract(operation.properties,'$.implementation_hint') IS NOT NULL "
        "AND (SELECT count(DISTINCT json_extract(candidate.properties,'$.qualified_owner')) "
        "FROM protocol_nodes candidate WHERE candidate.workspace_id=service.workspace_id "
        "AND candidate.kind='BINDER_IMPLEMENTATION_METHOD' "
        "AND json_extract(candidate.properties,'$.owner')="
        "json_extract(operation.properties,'$.implementation_hint'))=1;"
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
        "SELECT service.protocol_id,iface.protocol_id,'BINDER_SERVICE_INTERFACE',1.0,"
        "'service_client_interface',json_object("
        "'service_name',json_extract(operation.properties,'$.service_name'),"
        "'interface_hint',json_extract(operation.properties,'$.interface_hint')) "
        "FROM protocol_edges access JOIN protocol_nodes operation "
        "ON operation.protocol_id=access.source_id "
        "JOIN protocol_nodes service ON service.protocol_id=access.target_id "
        "JOIN protocol_nodes iface ON iface.workspace_id=service.workspace_id "
        "AND iface.kind='AIDL_INTERFACE' "
        "AND iface.name=json_extract(operation.properties,'$.interface_hint') "
        "WHERE access.type IN('REGISTERS_BINDER_SERVICE','LOOKS_UP_BINDER_SERVICE',"
        "'WAITS_FOR_BINDER_SERVICE') AND operation.workspace_id='%s' "
        "AND json_extract(operation.properties,'$.interface_hint') IS NOT NULL "
        "AND (SELECT count(*) FROM protocol_nodes candidate "
        "WHERE candidate.workspace_id=service.workspace_id AND candidate.kind='AIDL_INTERFACE' "
        "AND candidate.name=json_extract(operation.properties,'$.interface_hint'))=1;"
        "UPDATE protocol_nodes AS service SET properties=json_set(properties,"
        "'$.server_candidate_count',(SELECT count(DISTINCT "
        "json_extract(candidate.properties,'$.qualified_owner')) "
        "FROM protocol_edges registration JOIN protocol_nodes operation "
        "ON operation.protocol_id=registration.source_id JOIN protocol_nodes candidate "
        "ON candidate.workspace_id=service.workspace_id "
        "AND candidate.kind='BINDER_IMPLEMENTATION_METHOD' "
        "AND json_extract(candidate.properties,'$.owner')="
        "json_extract(operation.properties,'$.implementation_hint') "
        "WHERE registration.target_id=service.protocol_id "
        "AND registration.type='REGISTERS_BINDER_SERVICE'),"
        "'$.server_resolution',CASE (SELECT count(DISTINCT "
        "json_extract(candidate.properties,'$.qualified_owner')) "
        "FROM protocol_edges registration JOIN protocol_nodes operation "
        "ON operation.protocol_id=registration.source_id JOIN protocol_nodes candidate "
        "ON candidate.workspace_id=service.workspace_id "
        "AND candidate.kind='BINDER_IMPLEMENTATION_METHOD' "
        "AND json_extract(candidate.properties,'$.owner')="
        "json_extract(operation.properties,'$.implementation_hint') "
        "WHERE registration.target_id=service.protocol_id "
        "AND registration.type='REGISTERS_BINDER_SERVICE') "
        "WHEN 0 THEN 'not_found' WHEN 1 THEN 'resolved' ELSE 'ambiguous' END,"
        "'$.interface_candidate_count',(SELECT count(DISTINCT candidate.protocol_id) "
        "FROM protocol_edges access JOIN protocol_nodes operation "
        "ON operation.protocol_id=access.source_id JOIN protocol_nodes candidate "
        "ON candidate.workspace_id=service.workspace_id AND candidate.kind='AIDL_INTERFACE' "
        "AND candidate.name=json_extract(operation.properties,'$.interface_hint') "
        "WHERE access.target_id=service.protocol_id AND access.type IN("
        "'REGISTERS_BINDER_SERVICE','LOOKS_UP_BINDER_SERVICE','WAITS_FOR_BINDER_SERVICE')) ,"
        "'$.interface_resolution',CASE (SELECT count(DISTINCT candidate.protocol_id) "
        "FROM protocol_edges access JOIN protocol_nodes operation "
        "ON operation.protocol_id=access.source_id JOIN protocol_nodes candidate "
        "ON candidate.workspace_id=service.workspace_id AND candidate.kind='AIDL_INTERFACE' "
        "AND candidate.name=json_extract(operation.properties,'$.interface_hint') "
        "WHERE access.target_id=service.protocol_id AND access.type IN("
        "'REGISTERS_BINDER_SERVICE','LOOKS_UP_BINDER_SERVICE','WAITS_FOR_BINDER_SERVICE')) "
        "WHEN 0 THEN 'not_found' WHEN 1 THEN 'resolved' ELSE 'ambiguous' END) "
        "WHERE service.workspace_id='%s' AND service.kind='BINDER_SERVICE';"
        "DELETE FROM protocol_edges WHERE type='BINDER_SERVICE_SERVER' AND source_id IN("
        "SELECT protocol_id FROM protocol_nodes WHERE workspace_id='%s' "
        "AND kind='BINDER_SERVICE' AND json_extract(properties,'$.server_resolution')!='resolved');"
        "DELETE FROM protocol_edges WHERE type='BINDER_SERVICE_INTERFACE' AND source_id IN("
        "SELECT protocol_id FROM protocol_nodes WHERE workspace_id='%s' "
        "AND kind='BINDER_SERVICE' "
        "AND json_extract(properties,'$.interface_resolution')!='resolved');";
    char sql[32768];
    (void)snprintf(sql, sizeof(sql), sql_template, ctx->workspace->workspace_id,
                   ctx->workspace->workspace_id, ctx->workspace->workspace_id,
                   ctx->workspace->workspace_id, ctx->workspace->workspace_id);
    char *sql_error = NULL;
    int sqlite_rc = sqlite3_exec(ctx->db, sql, NULL, NULL, &sql_error);
    int rc = sqlite_rc == SQLITE_OK ? 0 : -1;
    if (rc != 0) {
        pg_error(ctx->err, ctx->err_size, "cannot resolve Binder services",
                 sql_error ? sql_error : sqlite3_errmsg(ctx->db));
    }
    sqlite3_free(sql_error);
    return rc;
}

static bool binder_generated_server(const char *qualified_name, const char *bn_name,
                                    const char *interface_name) {
    return contains_class_token(qualified_name, bn_name) ||
           (strstr(qualified_name ? qualified_name : "", interface_name) &&
            contains_class_token(qualified_name, "Stub") &&
            !contains_class_token(qualified_name, "Proxy"));
}

static bool binder_generated_proxy(const char *qualified_name, const char *bp_name,
                                   const char *interface_name) {
    return contains_class_token(qualified_name, bp_name) ||
           (strstr(qualified_name ? qualified_name : "", interface_name) &&
            strstr(qualified_name, ".Proxy."));
}

static bool binder_range_contains(link_ctx_t *ctx, const binder_symbol_t *symbol,
                                  const char *first, const char *second) {
    char *source = binder_symbol_source(ctx, symbol, true);
    if (!source) return false;
    bool found_first = false;
    bool found_second = second == NULL;
    pg_lexer_t lexer = {.source = source, .length = strlen(source)};
    pg_token_t token;
    while ((token = pg_next(&lexer)).kind != PG_EOF) {
        if (token.kind == PG_IDENT && token.text) {
            if (contains_class_token(token.text, first)) found_first = true;
            if (second && contains_class_token(token.text, second)) found_second = true;
        }
        pg_token_free(&token);
    }
    pg_token_free(&token);
    bool matches = found_first && found_second;
    free(source);
    return matches;
}

static int link_binder_flow(link_ctx_t *ctx, const char *method_id, const char *method,
                            const char *interface_name, const char *stem) {
    char bn[512];
    char bp[512];
    char transaction[768];
    (void)snprintf(bn, sizeof(bn), "Bn%s", stem);
    (void)snprintf(bp, sizeof(bp), "Bp%s", stem);
    (void)snprintf(transaction, sizeof(transaction), "TRANSACTION_%s", method);

    binder_symbols_t methods = {0};
    binder_symbols_t constants = {0};
    binder_symbols_t handlers = {0};
    int rc = binder_symbols_load(ctx, method, &methods);
    if (rc == 0) rc = binder_symbols_load(ctx, transaction, &constants);
    if (rc == 0) rc = binder_symbols_load(ctx, method, &constants);
    if (rc == 0) rc = binder_symbols_load(ctx, "onTransact", &handlers);
    if (rc == 0) rc = binder_symbols_load(ctx, "on_transact", &handlers);
    if (rc != 0) goto done;

    for (int i = 0; i < methods.count && rc == 0; i++) {
        binder_symbol_t *implementation = &methods.items[i];
        if (strcmp(implementation->label, "Method") != 0 ||
            binder_generated_server(implementation->qualified_name, bn, interface_name) ||
            binder_generated_proxy(implementation->qualified_name, bp, interface_name)) continue;
        char *owner = binder_symbol_owner(implementation->qualified_name, method);
        char *qualified_owner = binder_symbol_qualified_owner(
            implementation->qualified_name, method);
        char *source = binder_symbol_source(ctx, implementation, false);
        bool direct_implementation = source && owner && qualified_owner &&
            binder_source_declares_implementation(source, owner, bn, interface_name);
        free(source);
        if (!direct_implementation) {
            free(owner);
            free(qualified_owner);
            continue;
        }
        int server_count = 0;
        for (int s = 0; s < methods.count; s++) {
            binder_symbol_t *server = &methods.items[s];
            if (strcmp(server->label, "Method") == 0 &&
                binder_generated_server(server->qualified_name, bn, interface_name) &&
                binder_same_backend(server, implementation)) server_count++;
        }
        char implementation_transaction[768];
        if (binder_backend(implementation->qualified_name, implementation->file_path) ==
            BINDER_BACKEND_RUST) {
            (void)snprintf(implementation_transaction, sizeof(implementation_transaction),
                           "transactions::%s", method);
        } else {
            (void)snprintf(implementation_transaction, sizeof(implementation_transaction),
                           "%s", transaction);
        }
        char *properties = binder_evidence_properties(implementation_transaction, method,
                                                       owner, qualified_owner);
        if (!properties ||
            insert_protocol_node_properties(ctx, implementation->global_id,
                implementation->repo_id, "BINDER_IMPLEMENTATION_METHOD",
                implementation->name, implementation->qualified_name,
                implementation->file_path, implementation->global_id, properties) != 0) {
            rc = -1;
        }
        for (int s = 0; s < methods.count && rc == 0; s++) {
            binder_symbol_t *server = &methods.items[s];
            if (strcmp(server->label, "Method") != 0 ||
                !binder_generated_server(server->qualified_name, bn, interface_name) ||
                !binder_same_backend(server, implementation)) continue;
            if (insert_protocol_edge_properties(ctx, server->global_id,
                    implementation->global_id, "BINDER_IMPLEMENTED_BY", 1.0,
                    "binder_direct_inheritance", properties) != 0) rc = -1;
        }
        if (rc == 0 && server_count == 0 &&
            insert_protocol_edge_properties(ctx, method_id, implementation->global_id,
                "BINDER_IMPLEMENTED_BY", 1.0, "binder_direct_inheritance",
                properties) != 0) rc = -1;
        free(properties);
        free(owner);
        free(qualified_owner);
    }

    bool has_constant_backend[4] = {false};
    for (int c = 0; c < constants.count; c++) {
        binder_symbol_t *constant = &constants.items[c];
        binder_backend_t backend = binder_backend(constant->qualified_name,
                                                   constant->file_path);
        bool rust_constant = backend == BINDER_BACKEND_RUST &&
            strcmp(constant->label, "Method") != 0 &&
            strcmp(constant->name, method) == 0 &&
            (strcmp(constant->label, "SourceConstant") == 0 ||
             contains_class_token(constant->qualified_name, interface_name)) &&
            contains_class_token(constant->qualified_name, "transactions");
        bool traditional_constant = strcmp(constant->label, "Method") != 0 &&
            strcmp(constant->name, transaction) == 0 &&
            binder_generated_server(constant->qualified_name, bn, interface_name);
        if (rust_constant || traditional_constant) {
            has_constant_backend[backend] = true;
        }
    }
    for (int h = 0; h < handlers.count && rc == 0; h++) {
        binder_symbol_t *handler = &handlers.items[h];
        binder_backend_t backend = binder_backend(handler->qualified_name,
                                                   handler->file_path);
        if (backend == BINDER_BACKEND_UNKNOWN || has_constant_backend[backend] ||
            strcmp(handler->label, "Method") != 0 ||
            !binder_generated_server(handler->qualified_name, bn, interface_name)) continue;
        bool rust_handler = backend == BINDER_BACKEND_RUST &&
            strcmp(handler->name, "on_transact") == 0 &&
            binder_range_contains(ctx, handler, "transactions", method);
        bool traditional_handler = backend != BINDER_BACKEND_RUST &&
            strcmp(handler->name, "onTransact") == 0 &&
            binder_range_contains(ctx, handler, transaction, method);
        if (rust_handler) {
            char rust_transaction[768];
            (void)snprintf(rust_transaction, sizeof(rust_transaction),
                           "transactions::%s", method);
            rc = binder_symbols_add_source_constant(ctx, &constants, handler, method,
                                                    rust_transaction);
        } else if (traditional_handler) {
            rc = binder_symbols_add_source_constant(ctx, &constants, handler, transaction,
                                                    transaction);
        }
        if (rc == 0 && (rust_handler || traditional_handler)) {
            has_constant_backend[backend] = true;
        }
    }

    for (int c = 0; c < constants.count && rc == 0; c++) {
        binder_symbol_t *constant = &constants.items[c];
        binder_backend_t constant_backend = binder_backend(constant->qualified_name,
                                                            constant->file_path);
        bool rust_constant = constant_backend == BINDER_BACKEND_RUST &&
            strcmp(constant->label, "Method") != 0 &&
            strcmp(constant->name, method) == 0 &&
            (strcmp(constant->label, "SourceConstant") == 0 ||
             contains_class_token(constant->qualified_name, interface_name)) &&
            contains_class_token(constant->qualified_name, "transactions");
        bool traditional_constant = strcmp(constant->label, "Method") != 0 &&
            strcmp(constant->name, transaction) == 0 &&
            binder_generated_server(constant->qualified_name, bn, interface_name);
        if (!rust_constant && !traditional_constant) continue;
        char transaction_evidence[768];
        if (rust_constant) {
            (void)snprintf(transaction_evidence, sizeof(transaction_evidence),
                           "transactions::%s", method);
        } else {
            (void)snprintf(transaction_evidence, sizeof(transaction_evidence), "%s",
                           transaction);
        }
        char *properties = binder_evidence_properties(transaction_evidence, method, NULL,
                                                       NULL);
        if (!properties) {
            rc = -1;
            break;
        }
        int endpoint_rc = strcmp(constant->label, "SourceConstant") == 0
            ? insert_protocol_node_properties(ctx, constant->global_id, constant->repo_id,
                "BINDER_TRANSACTION_CONSTANT", constant->name, constant->qualified_name,
                constant->file_path, NULL, properties)
            : add_binder_symbol_endpoint(ctx, constant, "BINDER_TRANSACTION_CONSTANT");
        if (endpoint_rc != 0 ||
            insert_protocol_edge_properties(ctx, method_id, constant->global_id,
                "BINDER_TRANSACTION", 1.0, "binder_transaction_constant", properties) != 0) {
            free(properties);
            rc = -1;
            break;
        }

        for (int h = 0; h < handlers.count && rc == 0; h++) {
            binder_symbol_t *handler = &handlers.items[h];
            if (strcmp(handler->label, "Method") != 0 ||
                !binder_generated_server(handler->qualified_name, bn, interface_name) ||
                !binder_same_backend(constant, handler)) continue;
            bool handler_matches = rust_constant
                ? strcmp(handler->name, "on_transact") == 0 &&
                  binder_range_contains(ctx, handler, "transactions", method)
                : strcmp(handler->name, "onTransact") == 0 &&
                  binder_range_contains(ctx, handler, transaction, method);
            if (!handler_matches) continue;
            if (add_binder_symbol_endpoint(ctx, handler, "BINDER_ON_TRANSACT_HANDLER") != 0 ||
                insert_protocol_edge_properties(ctx, constant->global_id, handler->global_id,
                    "BINDER_DISPATCH_CASE", 1.0, "binder_on_transact_case", properties) != 0) {
                rc = -1;
                break;
            }
            bool dispatched_to_server = false;
            for (int s = 0; s < methods.count && rc == 0; s++) {
                binder_symbol_t *server = &methods.items[s];
                if (strcmp(server->label, "Method") != 0 ||
                    !binder_generated_server(server->qualified_name, bn, interface_name) ||
                    !binder_same_backend(handler, server)) continue;
                dispatched_to_server = true;
                if (insert_protocol_edge_properties(ctx, handler->global_id, server->global_id,
                        "BINDER_DISPATCHES_TO", 1.0, "binder_on_transact_case",
                        properties) != 0) rc = -1;
            }
            if (rc == 0 && !dispatched_to_server &&
                insert_protocol_edge_properties(ctx, handler->global_id, method_id,
                    "BINDER_DISPATCHES_TO", 1.0, "binder_on_transact_case",
                    properties) != 0) rc = -1;
        }

        for (int p = 0; p < methods.count && rc == 0; p++) {
            binder_symbol_t *proxy = &methods.items[p];
            if (strcmp(proxy->label, "Method") != 0 ||
                !binder_generated_proxy(proxy->qualified_name, bp, interface_name) ||
                !binder_same_backend(proxy, constant)) continue;
            bool proxy_matches = rust_constant
                ? binder_range_contains(ctx, proxy, "transactions", "transact")
                : binder_range_contains(ctx, proxy, transaction, "transact");
            if (!proxy_matches) continue;
            if (insert_protocol_edge_properties(ctx, proxy->global_id, constant->global_id,
                    "BINDER_TRANSACT_CALL", 1.0, "binder_proxy_transact_call",
                    properties) != 0) rc = -1;
        }
        free(properties);
    }

done:
    binder_symbols_free(&methods);
    binder_symbols_free(&constants);
    binder_symbols_free(&handlers);
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
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_PARCELABLE'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_UNION'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_ENUM'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_IMPORT'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id "
        "WHERE n.workspace_id=?1 AND e.type='USES_CALLBACK'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_METHOD' "
        "AND json_extract(properties,'$.oneway')=1),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 "
        "AND kind IN('AIDL_INTERFACE','AIDL_PARCELABLE','AIDL_UNION','AIDL_ENUM') "
        "AND json_extract(properties,'$.stability')!='local'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind IN("
        "'BINDER_SERVER_TYPE','BINDER_CLIENT_TYPE','BINDER_INTERFACE_TYPE',"
        "'BINDER_SERVER_METHOD','BINDER_CLIENT_METHOD','BINDER_TRANSACTION_CONSTANT',"
        "'BINDER_ON_TRANSACT_HANDLER') AND (lower(file_path) LIKE '%.java' "
        "OR lower(file_path) LIKE '%.kt')) ,"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind IN("
        "'BINDER_SERVER_TYPE','BINDER_CLIENT_TYPE','BINDER_INTERFACE_TYPE',"
        "'BINDER_SERVER_METHOD','BINDER_CLIENT_METHOD','BINDER_TRANSACTION_CONSTANT',"
        "'BINDER_ON_TRANSACT_HANDLER') AND (lower(file_path) LIKE '%.c' "
        "OR lower(file_path) LIKE '%.cc' OR lower(file_path) LIKE '%.cpp' "
        "OR lower(file_path) LIKE '%.cxx' OR lower(file_path) LIKE '%.h' "
        "OR lower(file_path) LIKE '%.hh' OR lower(file_path) LIKE '%.hpp' "
        "OR lower(file_path) LIKE '%.hxx')) ,"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind IN("
        "'BINDER_SERVER_TYPE','BINDER_CLIENT_TYPE','BINDER_INTERFACE_TYPE',"
        "'BINDER_SERVER_METHOD','BINDER_CLIENT_METHOD','BINDER_TRANSACTION_CONSTANT',"
        "'BINDER_ON_TRANSACT_HANDLER') AND lower(file_path) LIKE '%.rs'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.type='BINDER_SERVER_IMPL'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.type='BINDER_CLIENT_PROXY'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_TRANSACTION_CONSTANT'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_ON_TRANSACT_HANDLER'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.type='BINDER_TRANSACT_CALL'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_IMPLEMENTATION_METHOD'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_SERVICE'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_SERVICE_REGISTRATION'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_SERVICE_LOOKUP'),"
        "(SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_SERVICE_WAIT'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.type='BINDER_SERVICE_SERVER'),"
        "(SELECT count(*) FROM protocol_edges e JOIN protocol_nodes n ON n.protocol_id=e.source_id WHERE n.workspace_id=?1 AND e.type='BINDER_SERVICE_INTERFACE'),"
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
            stats->aidl_parcelables = sqlite3_column_int(stmt, 4);
            stats->aidl_unions = sqlite3_column_int(stmt, 5);
            stats->aidl_enums = sqlite3_column_int(stmt, 6);
            stats->aidl_imports = sqlite3_column_int(stmt, 7);
            stats->aidl_callbacks = sqlite3_column_int(stmt, 8);
            stats->aidl_oneway_methods = sqlite3_column_int(stmt, 9);
            stats->aidl_stable_types = sqlite3_column_int(stmt, 10);
            stats->aidl_java_generated_nodes = sqlite3_column_int(stmt, 11);
            stats->aidl_cpp_ndk_generated_nodes = sqlite3_column_int(stmt, 12);
            stats->aidl_rust_generated_nodes = sqlite3_column_int(stmt, 13);
            stats->binder_server_edges = sqlite3_column_int(stmt, 14);
            stats->binder_client_edges = sqlite3_column_int(stmt, 15);
            stats->binder_transaction_constants = sqlite3_column_int(stmt, 16);
            stats->binder_on_transact_handlers = sqlite3_column_int(stmt, 17);
            stats->binder_transact_calls = sqlite3_column_int(stmt, 18);
            stats->binder_implementation_methods = sqlite3_column_int(stmt, 19);
            stats->binder_services = sqlite3_column_int(stmt, 20);
            stats->binder_service_registrations = sqlite3_column_int(stmt, 21);
            stats->binder_service_lookups = sqlite3_column_int(stmt, 22);
            stats->binder_service_waits = sqlite3_column_int(stmt, 23);
            stats->binder_service_server_links = sqlite3_column_int(stmt, 24);
            stats->binder_service_interface_links = sqlite3_column_int(stmt, 25);
            stats->jni_static_edges = sqlite3_column_int(stmt, 26);
            stats->jni_dynamic_edges = sqlite3_column_int(stmt, 27);
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
        "INSERT INTO protocol_nodes(protocol_id,workspace_id,repo_id,kind,name,qualified_name,"
        "file_path,symbol_global_id,properties) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
        "ON CONFLICT(protocol_id) DO UPDATE SET workspace_id=excluded.workspace_id,"
        "repo_id=excluded.repo_id,kind=excluded.kind,name=excluded.name,"
        "qualified_name=excluded.qualified_name,file_path=excluded.file_path,"
        "symbol_global_id=excluded.symbol_global_id,properties=excluded.properties;";
    const char *edge_sql =
        "INSERT OR REPLACE INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties)"
        " VALUES(?1,?2,?3,?4,?5,?6);";
    const char *service_sql =
        "INSERT INTO protocol_nodes(protocol_id,workspace_id,repo_id,kind,name,qualified_name,"
        "file_path,symbol_global_id,properties) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
        "ON CONFLICT(protocol_id) DO UPDATE SET repo_id=CASE "
        "WHEN excluded.repo_id<protocol_nodes.repo_id THEN excluded.repo_id "
        "ELSE protocol_nodes.repo_id END,file_path=CASE "
        "WHEN excluded.repo_id<protocol_nodes.repo_id THEN excluded.file_path "
        "ELSE protocol_nodes.file_path END,properties=excluded.properties;";
    const char *find_sql =
        "SELECT global_id,repo_id,name,qualified_name,file_path,label FROM symbols "
        "WHERE workspace_id=?1 AND name=?2;";
    if (sqlite3_prepare_v2(db, node_sql, -1, &ctx.insert_node, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, edge_sql, -1, &ctx.insert_edge, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, service_sql, -1, &ctx.insert_service, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, find_sql, -1, &ctx.find_symbols, NULL) != SQLITE_OK) goto fail_ctx;
    int rc = 0;
    for (int i = 0; i < workspace->repo_count && rc == 0; i++) {
        if (workspace->repos[i].exists) {
            rc = scan_repo_tree(&ctx, &workspace->repos[i], workspace->repos[i].abs_path, "", 0);
        }
    }
    if (rc == 0) rc = resolve_aidl_references(&ctx);
    if (rc == 0) rc = resolve_binder_services(&ctx);
    if (rc == 0) rc = link_static_jni(&ctx);
    sqlite3_finalize(ctx.insert_node);
    sqlite3_finalize(ctx.insert_edge);
    sqlite3_finalize(ctx.insert_service);
    sqlite3_finalize(ctx.find_symbols);
    if (rc != 0 || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    sqlite3_close(db);
    return cbm_aosp_protocol_stats(workspace, stats, err, err_size);

fail_ctx:
    sqlite3_finalize(ctx.insert_node);
    sqlite3_finalize(ctx.insert_edge);
    sqlite3_finalize(ctx.insert_service);
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
        free(results[i].properties);
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
        "SELECT n.protocol_id,r.path,n.kind,n.name,n.qualified_name,n.file_path,n.properties,"
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
        item->properties = column_dup(stmt, 6);
        item->outgoing_edges = sqlite3_column_int(stmt, 7);
        item->incoming_edges = sqlite3_column_int(stmt, 8);
        if (!item->protocol_id || !item->repo_path || !item->kind || !item->name ||
            !item->qualified_name || !item->file_path || !item->properties) {
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
        free(results[i].properties);
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
        "SELECT s.qualified_name,t.qualified_name,e.type,e.confidence,e.evidence,e.properties "
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
        item->properties = column_dup(stmt, 5);
        if (!item->source_qualified_name || !item->target_qualified_name || !item->type ||
            !item->evidence || !item->properties) {
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
