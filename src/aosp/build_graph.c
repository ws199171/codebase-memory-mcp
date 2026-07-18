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
} dep_decl_t;

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
} module_decl_t;

typedef struct {
    module_decl_t *items;
    int count;
    int cap;
} module_vec_t;

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
    dep_decl_t *dep = module_ensure_dep(module, name, kind);
    return dep && dep_add_variant(dep, variant);
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

static bool module_add_inherited_dep(module_decl_t *module, const dep_decl_t *source,
                                     const dep_decl_t *defaults_dep,
                                     const char *defaults_name) {
    if (!module || !source || !defaults_dep || !defaults_name) return false;
    dep_decl_t *dep = module_find_dep(module, source->name, source->kind);
    bool created = dep == NULL;
    if (!dep) dep = module_ensure_dep(module, source->name, source->kind);
    if (!dep || !dep_merge_inherited_variants(dep, defaults_dep, source)) return false;
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

static void module_free(module_decl_t *module) {
    if (!module) return;
    free(module->name);
    free(module->type);
    free(module->file_path);
    free(module->defaults_cycle);
    free(module->compile_multilib);
    for (int i = 0; i < module->dep_count; i++) {
        free(module->deps[i].name);
        free(module->deps[i].kind);
        free(module->deps[i].inherited_from);
        free(module->deps[i].inheritance_path);
        str_vec_free(&module->deps[i].variants);
    }
    free(module->deps);
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
    if (strcmp(key, "tools") == 0 || strcmp(key, "tool_files") == 0) return "TOOL";
    if (strcmp(key, "plugins") == 0) return "PLUGIN";
    if (strcmp(key, "aidl_libs") == 0 || strcmp(key, "imports") == 0) return "AIDL_IMPORT";
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
    const char *kind = dependency_kind(key);
    if (kind && strcmp(key, "imports") == 0 &&
        (!module->type || strcmp(module->type, "aidl_interface") != 0)) {
        kind = NULL;
    }
    const char *variant = is_variant_scope(scope) ? scope : NULL;
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

static bool parse_blueprint(const char *source, size_t length, const char *file_path,
                            module_vec_t *modules) {
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
                return false;
            }
            continue;
        }
        if (parser.current.kind != TOK_LBRACE) {
            free(type);
            continue;
        }
        parser_advance(&parser);
        module_decl_t module = {.type = type, .file_path = strdup(file_path)};
        if (!module.type || !module.file_path ||
            !parse_bp_object(&parser, &module, &vars, "", 1) ||
            !module_vec_add(modules, &module)) {
            module_free(&module);
            token_free(&parser.current);
            bp_var_vec_free(&vars);
            return false;
        }
    }
    token_free(&parser.current);
    bp_var_vec_free(&vars);
    return true;
}

static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static void make_add_words(module_decl_t *module, char *value, const char *kind) {
    char *save = NULL;
    for (char *word = strtok_r(value, " \t", &save); word; word = strtok_r(NULL, " \t", &save)) {
        (void)module_add_dep(module, word, kind);
    }
}

static const char *make_dependency_kind(const char *key) {
    if (strcmp(key, "LOCAL_SHARED_LIBRARIES") == 0) return "SHARED_LIB";
    if (strcmp(key, "LOCAL_STATIC_LIBRARIES") == 0 || strcmp(key, "LOCAL_WHOLE_STATIC_LIBRARIES") == 0) return "STATIC_LIB";
    if (strcmp(key, "LOCAL_HEADER_LIBRARIES") == 0) return "HEADER_LIB";
    if (strcmp(key, "LOCAL_JAVA_LIBRARIES") == 0 || strcmp(key, "LOCAL_STATIC_JAVA_LIBRARIES") == 0) return "LIB";
    if (strcmp(key, "LOCAL_REQUIRED_MODULES") == 0) return "REQUIRED";
    return NULL;
}

static bool parse_android_mk(char *source, const char *file_path, module_vec_t *modules) {
    module_decl_t current = {.file_path = strdup(file_path), .type = strdup("android_make")};
    if (!current.file_path || !current.type) {
        module_free(&current);
        return false;
    }
    str_vec_t logical = {0};
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
            str_vec_free(&logical);
            module_free(&current);
            return false;
        }
        pending = grown;
        if (pending_len) pending[pending_len++] = ' ';
        memcpy(pending + pending_len, part, len + 1);
        pending_len += len;
        if (continued) continue;
        if (!str_vec_add(&logical, pending)) {
            free(pending);
            str_vec_free(&logical);
            module_free(&current);
            return false;
        }
        free(pending);
        pending = NULL;
        pending_len = 0;
    }
    free(pending);

    for (int i = 0; i < logical.count; i++) {
        char *line = logical.items[i];
        if (strstr(line, "CLEAR_VARS")) {
            module_free(&current);
            current.file_path = strdup(file_path);
            current.type = strdup("android_make");
            continue;
        }
        if (strncmp(line, "include", 7) == 0 && strstr(line, "BUILD_")) {
            const char *build = strstr(line, "BUILD_");
            free(current.type);
            size_t build_len = 0;
            while (build && (isupper((unsigned char)build[build_len]) || build[build_len] == '_')) {
                build_len++;
            }
            current.type = build_len ? cbm_strndup(build, build_len) : strdup("android_make");
            if (!current.type || !module_vec_add(modules, &current)) {
                module_free(&current);
                str_vec_free(&logical);
                return false;
            }
            current.file_path = strdup(file_path);
            current.type = strdup("android_make");
            continue;
        }
        char *assign = strstr(line, ":=");
        size_t op_len = 2;
        if (!assign) assign = strstr(line, "+=");
        if (!assign) {
            assign = strchr(line, '=');
            op_len = 1;
        }
        if (!assign) continue;
        *assign = '\0';
        char *key = trim(line);
        char *value = trim(assign + op_len);
        if (strcmp(key, "LOCAL_MODULE") == 0) {
            free(current.name);
            current.name = strdup(value);
        } else {
            const char *kind = make_dependency_kind(key);
            if (kind) make_add_words(&current, value, kind);
        }
    }
    module_free(&current);
    str_vec_free(&logical);
    return true;
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

static module_decl_t *find_defaults_module(scan_ctx_t *contexts, int context_count,
                                           scan_ctx_t *source_ctx, const char *name,
                                           scan_ctx_t **target_ctx) {
    if (target_ctx) *target_ctx = NULL;
    module_decl_t *best = NULL;
    char best_id[65] = {0};
    for (int i = 0; source_ctx && i < source_ctx->modules.count; i++) {
        if (strcmp(source_ctx->modules.items[i].name, name) == 0) {
            char candidate_id[65];
            module_id(source_ctx->repo, &source_ctx->modules.items[i], candidate_id);
            if (!best || strcmp(candidate_id, best_id) < 0) {
                best = &source_ctx->modules.items[i];
                (void)memcpy(best_id, candidate_id, sizeof(best_id));
            }
        }
    }
    if (best) {
        if (target_ctx) *target_ctx = source_ctx;
        return best;
    }
    scan_ctx_t *best_ctx = NULL;
    for (int c = 0; c < context_count; c++) {
        if (&contexts[c] == source_ctx) continue;
        for (int i = 0; i < contexts[c].modules.count; i++) {
            if (strcmp(contexts[c].modules.items[i].name, name) == 0) {
                char candidate_id[65];
                module_id(contexts[c].repo, &contexts[c].modules.items[i], candidate_id);
                if (!best || strcmp(candidate_id, best_id) < 0) {
                    best = &contexts[c].modules.items[i];
                    best_ctx = &contexts[c];
                    (void)memcpy(best_id, candidate_id, sizeof(best_id));
                }
            }
        }
    }
    if (target_ctx) *target_ctx = best_ctx;
    return best;
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

static bool expand_module_defaults(scan_ctx_t *contexts, int context_count,
                                   scan_ctx_t *ctx, module_decl_t *module,
                                   module_stack_t *stack) {
    if (module->defaults_state == 2) return true;
    if (module->defaults_state == 1) return mark_defaults_cycle(stack, module);
    module->defaults_state = 1;
    if (!module_stack_push(stack, module)) return false;
    int direct_dep_count = module->dep_count;
    for (int i = 0; i < direct_dep_count; i++) {
        dep_decl_t *defaults_dep = &module->deps[i];
        if (strcmp(defaults_dep->kind, "DEFAULTS") != 0) continue;
        scan_ctx_t *target_ctx = NULL;
        module_decl_t *target = find_defaults_module(contexts, context_count, ctx,
                                                     defaults_dep->name, &target_ctx);
        if (!target) continue;
        if (target->defaults_state == 1) {
            if (!mark_defaults_cycle(stack, target)) return false;
            continue;
        }
        if (!expand_module_defaults(contexts, context_count, target_ctx, target, stack)) return false;
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
    }
    stack->count--;
    module->defaults_state = 2;
    return true;
}

static bool expand_all_defaults(scan_ctx_t *contexts, int context_count) {
    module_stack_t stack = {0};
    bool ok = true;
    for (int c = 0; c < context_count && ok; c++) {
        for (int i = 0; i < contexts[c].modules.count && ok; i++) {
            ok = expand_module_defaults(contexts, context_count, &contexts[c],
                                        &contexts[c].modules.items[i], &stack);
        }
    }
    free(stack.items);
    return ok;
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
        bool is_aidl = name_len > 5 && strcmp(entry->name + name_len - 5, ".aidl") == 0;
        if (!is_bp && !is_mk && !is_aidl) continue;
        size_t length = 0;
        char *source = bg_read_file(abs_path, &length);
        if (!source) {
            bg_error(ctx->err, ctx->err_size, "cannot read AOSP build file", abs_path);
            rc = -1;
            continue;
        }
        bool ok;
        if (is_bp) {
            ctx->stats.blueprint_files++;
            ok = parse_blueprint(source, length, rel_path, &ctx->modules);
        } else if (is_mk) {
            ctx->stats.make_files++;
            ok = parse_android_mk(source, rel_path, &ctx->modules);
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

static char *module_properties_json(const module_decl_t *module) {
    if (!module->defaults_cycle && !module->compile_multilib) return strdup("{}");
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
            "INSERT OR IGNORE INTO modules(module_id,workspace_id,repo_id,name,module_type,file_path,properties)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7);", -1, &insert_module, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO module_dependencies(source_id,target_name,type,properties) "
            "VALUES(?1,?2,?3,?4);",
            -1, &insert_dep, NULL) != SQLITE_OK) goto fail_insert;
    for (int c = 0; c < context_count; c++) {
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
            for (int d = 0; d < module->dep_count; d++) {
                sqlite3_reset(insert_dep);
                sqlite3_clear_bindings(insert_dep);
                sqlite3_bind_text(insert_dep, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_dep, 2, module->deps[d].name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_dep, 3, module->deps[d].kind, -1, SQLITE_TRANSIENT);
                char *dep_properties = dependency_properties_json(&module->deps[d]);
                if (!dep_properties) goto fail_insert;
                sqlite3_bind_text(insert_dep, 4, dep_properties, -1, SQLITE_TRANSIENT);
                int dep_step = sqlite3_step(insert_dep);
                free(dep_properties);
                if (dep_step != SQLITE_DONE) goto fail_insert;
            }
        }
    }
    sqlite3_finalize(insert_module);
    sqlite3_finalize(insert_dep);
    insert_module = NULL;
    insert_dep = NULL;

    const char *resolve_sql =
        "UPDATE module_dependencies SET target_id=(SELECT t.module_id FROM modules t "
        "JOIN modules s ON s.module_id=module_dependencies.source_id "
        "WHERE t.workspace_id=?1 AND t.name=module_dependencies.target_name "
        "ORDER BY (t.repo_id=s.repo_id) DESC,t.module_id LIMIT 1) WHERE source_id IN "
        "(SELECT module_id FROM modules WHERE workspace_id=?2);";
    if (sqlite3_prepare_v2(db, resolve_sql, -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE module_dependencies SET resolved=(target_id IS NOT NULL) WHERE source_id IN "
            "(SELECT module_id FROM modules WHERE workspace_id=?1);", -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
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
        "WHERE m.workspace_id=?1);";
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
    for (int i = 0; i < workspace->repo_count && rc == 0; i++) {
        contexts[i].workspace = workspace;
        contexts[i].repo = &workspace->repos[i];
        contexts[i].err = err;
        contexts[i].err_size = err_size;
        if (workspace->repos[i].exists) {
            rc = scan_tree(&contexts[i], workspace->repos[i].abs_path, "", 0);
            file_stats.blueprint_files += contexts[i].stats.blueprint_files;
            file_stats.make_files += contexts[i].stats.make_files;
            file_stats.aidl_files += contexts[i].stats.aidl_files;
        }
    }
    if (rc == 0 && !expand_all_defaults(contexts, workspace->repo_count)) {
        bg_error(err, err_size, "cannot expand AOSP defaults", "out of memory");
        rc = -1;
    }
    if (rc == 0) rc = persist_build_graph(workspace, contexts, workspace->repo_count, err, err_size);
    if (rc == 0) {
        rc = cbm_aosp_build_stats(workspace, stats, err, err_size);
        stats->blueprint_files = file_stats.blueprint_files;
        stats->make_files = file_stats.make_files;
        stats->aidl_files = file_stats.aidl_files;
    }
    for (int i = 0; i < workspace->repo_count; i++) module_vec_free(&contexts[i].modules);
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
