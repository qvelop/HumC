#include "compiler.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// An arena structure that allows temporary memory allocations made by the compiler to be kept in one place
typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t used;
    size_t capacity;
    max_align_t alignment;
    unsigned char data[];
} ArenaChunk;

typedef struct {
    ArenaChunk *head;
} Arena;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuffer;

typedef enum {
    TYPE_INT,
    TYPE_DOUBLE,
    TYPE_CHAR,
    TYPE_STRING,
    TYPE_POINTER,
    TYPE_OTHER
} ValueType;

typedef struct Variable {
    const char *name;
    const char *declared_type;
    ValueType value_type;
    size_t scope_depth;
    bool is_fixed_array;
    struct Variable *next;
} Variable;

typedef enum {
    BLOCK_FUNCTION,
    BLOCK_IF,
    BLOCK_ELIF,
    BLOCK_ELSE,
    BLOCK_WHILE,
    BLOCK_FOR,
    BLOCK_SWITCH,
    BLOCK_CASE,
    BLOCK_DEFAULT
} BlockType;

typedef struct {
    BlockType type;
    int indent;
    int body_indent;
    Variable *variables_before;
} Block;

typedef struct {
    Arena arena;
    StringBuffer output;
    Block *blocks;
    size_t block_count;
    size_t block_capacity;
    Variable *variables;
    bool saw_print;
    bool saw_bool;
    bool saw_bounds_check;
    bool saw_error;
    char *error_message;
    size_t error_line;
    const char *source;
} Compiler;

static void arena_destroy(Arena *arena) {
    ArenaChunk *chunk = arena->head;
    while (chunk) {
        ArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    arena->head = NULL;
}

static void *arena_alloc(Arena *arena, size_t size) {
    const size_t alignment = _Alignof(max_align_t);
    if (size == 0) size = 1;
    if (size > SIZE_MAX - (alignment - 1)) return NULL;
    size = (size + alignment - 1) & ~(alignment - 1);

    ArenaChunk *chunk = arena->head;
    if (!chunk || chunk->capacity - chunk->used < size) {
        size_t capacity = size > 8192 ? size : 8192;
        if (capacity > SIZE_MAX - sizeof(*chunk)) return NULL;
        ArenaChunk *created = (ArenaChunk *)malloc(sizeof(*created) + capacity);
        if (!created) return NULL;
        created->next = chunk;
        created->used = 0;
        created->capacity = capacity;
        arena->head = created;
        chunk = created;
    }

    void *result = chunk->data + chunk->used;
    chunk->used += size;
    return result;
}

static char *arena_strndup(Arena *arena, const char *text, size_t length) {
    char *result = (char *)arena_alloc(arena, length + 1);
    if (!result) return NULL;
    memcpy(result, text, length);
    result[length] = '\0';
    return result;
}

static char *arena_strdup(Arena *arena, const char *text) {
    return arena_strndup(arena, text, strlen(text));
}

static bool buffer_reserve(StringBuffer *buffer, size_t extra) {
    if (buffer->length == SIZE_MAX || extra > SIZE_MAX - buffer->length - 1) return false;
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    char *memory = (char *)realloc(buffer->data, capacity);
    if (!memory) return false;
    buffer->data = memory;
    buffer->capacity = capacity;
    return true;
}

static bool buffer_append_n(StringBuffer *buffer, const char *text, size_t length) {
    if (!buffer_reserve(buffer, length) || !buffer->data) return false;
    if (length) memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool buffer_append(StringBuffer *buffer, const char *text) {
    return buffer_append_n(buffer, text, strlen(text));
}

static bool buffer_append_char(StringBuffer *buffer, char value) {
    return buffer_append_n(buffer, &value, 1);
}

static bool buffer_appendf(StringBuffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return false;
    }
    if (!buffer_reserve(buffer, (size_t)length)) {
        va_end(args);
        return false;
    }
    (void)vsnprintf(buffer->data + buffer->length,
                    buffer->capacity - buffer->length, format, args);
    buffer->length += (size_t)length;
    va_end(args);
    return true;
}

static char *buffer_take(StringBuffer *buffer) {
    if (!buffer->data) {
        buffer->data = (char *)malloc(1);
        if (!buffer->data) return NULL;
        buffer->data[0] = '\0';
    }
    char *result = buffer->data;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    return result;
}

static char *trim_left(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static void trim_right(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                           text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[--length] = '\0';
    }
}

static char *trim_place(char *text) {
    text = trim_left(text);
    trim_right(text);
    return text;
}

static bool starts_word(const char *text, const char *word) {
    size_t length = strlen(word);
    return strncmp(text, word, length) == 0 &&
           (text[length] == '\0' || isspace((unsigned char)text[length]) ||
            text[length] == '(' || text[length] == ':');
}

static bool is_ident_start(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static bool ident_part(char ch) {
    return is_ident_start(ch) || (ch >= '0' && ch <= '9');
}

static bool keyword(const char *name) {
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
        "int", "long", "register", "restrict", "return", "short", "signed", "sizeof",
        "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile",
        "while", "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
        "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcmp(name, keywords[i]) == 0) return true;
    }
    return false;
}

static bool valid_ident(const char *name) {
    if (!name || !is_ident_start(name[0])) return false;
    for (size_t i = 1; name[i]; i++) {
        if (!ident_part(name[i])) return false;
    }
    return !keyword(name);
}

static int indet_width(const char *line, size_t *character_count) {
    int width = 0;
    size_t i = 0;
    while (line[i] == ' ' || line[i] == '\t') {
        width += line[i] == '\t' ? 4 : 1;
        i++;
    }
    if (character_count) *character_count = i;
    return width;
}

static bool compiler_fail(Compiler *compiler, size_t line, const char *format, ...) {
    if (!compiler->saw_error) {
        va_list args;
        va_start(args, format);
        va_list copy;
        va_copy(copy, args);
        int length = vsnprintf(NULL, 0, format, copy);
        va_end(copy);
        if (length >= 0) {
            compiler->error_message = (char *)malloc((size_t)length + 1);
            if (compiler->error_message) {
                (void)vsnprintf(compiler->error_message, (size_t)length + 1, format, args);
            }
        }
        va_end(args);
        compiler->error_line = line;
        compiler->saw_error = true;
    }
    return false;
}

static char *rm_comment(Compiler *compiler, const char *line, char **comment_out) {
    if (comment_out) *comment_out = NULL;
    char quote = '\0';
    bool escaped = false;
    for (size_t i = 0; line[i]; i++) {
        char ch = line[i];
        if (quote) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == quote) quote = '\0';
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '`') {
            compiler_fail(compiler, compiler->error_line, "backtick strings are not supported in HumC; use C string literals");
            return NULL;
        }
        if (ch == '/' && line[i + 1] == '/') {
            if (comment_out) *comment_out = arena_strdup(&compiler->arena, line + i);
            return arena_strndup(&compiler->arena, line, i);
        }
    }
    return arena_strdup(&compiler->arena, line);
}

static char *find_equal(char *text) {
    char quote = '\0';
    bool escaped = false;
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (quote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == quote) quote = '\0';
            continue;
        }
        if (ch == '"' || ch == '\'') { quote = ch; continue; }
        if (ch == '(') paren++;
        else if (ch == ')' && paren > 0) paren--;
        else if (ch == '[') bracket++;
        else if (ch == ']' && bracket > 0) bracket--;
        else if (ch == '{') brace++;
        else if (ch == '}' && brace > 0) brace--;
        else if (ch == '=' && paren == 0 && bracket == 0 && brace == 0) {
            char previous = i ? text[i - 1] : '\0';
            char next = text[i + 1];
            if (previous != '=' && previous != '!' && previous != '<' && previous != '>' && next != '=') {
                return text + i;
            }
        }
    }
    return NULL;
}

static char *find_char(char *text, char wanted) {
    char quote = '\0';
    bool escaped = false;
    int paren = 0;
    int bracket = 0;
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (quote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == quote) quote = '\0';
            continue;
        }
        if (ch == '"' || ch == '\'') { quote = ch; continue; }
        if (ch == '(') paren++;
        else if (ch == ')' && paren > 0) paren--;
        else if (ch == '[') bracket++;
        else if (ch == ']' && bracket > 0) bracket--;
        else if (ch == wanted && paren == 0 && bracket == 0) return text + i;
    }
    return NULL;
}

static char *find_close(char *text, char open, char close) {
    if (*text != open) return NULL;
    int depth = 0;
    char quote = '\0';
    bool escaped = false;
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (quote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == quote) quote = '\0';
            continue;
        }
        if (ch == '"' || ch == '\'') { quote = ch; continue; }
        if (ch == open) depth++;
        else if (ch == close) {
            depth--;
            if (depth == 0) return text + i;
        }
    }
    return NULL;
}

static void push_block(Compiler *compiler, BlockType type, int indent) {
    if (compiler->block_count == compiler->block_capacity) {
        if (compiler->block_capacity > SIZE_MAX / 2 / sizeof(*compiler->blocks)) {
            compiler_fail(compiler, compiler->error_line, "too many nested blocks");
            return;
        }
        size_t capacity = compiler->block_capacity ? compiler->block_capacity * 2 : 16;
        Block *blocks = (Block *)realloc(compiler->blocks, capacity * sizeof(*blocks));
        if (!blocks) {
            compiler_fail(compiler, compiler->error_line, "out of memory while parsing blocks");
            return;
        }
        compiler->blocks = blocks;
        compiler->block_capacity = capacity;
    }
    compiler->blocks[compiler->block_count++] = (Block){
        type, indent, -1, compiler->variables
    };
}

static Block *top_block(Compiler *compiler) {
    if (!compiler->block_count) return NULL;
    return &compiler->blocks[compiler->block_count - 1];
}

static void emit_indent(StringBuffer *output, size_t depth) {
    for (size_t i = 0; i < depth; i++) (void)buffer_append(output, "    ");
}

static bool case_block(BlockType type) {
    return type == BLOCK_CASE || type == BLOCK_DEFAULT;
}

static bool close_top_block(Compiler *compiler) {
    Block *top = top_block(compiler);
    if (!top) return true;
    if (top->body_indent < 0 && !case_block(top->type)) {
        return compiler_fail(compiler, compiler->error_line, "expected an indented block");
    }
    // The arena owns these records, while the header of this list defines a lexical scope
    compiler->variables = top->variables_before;
    compiler->block_count--;
    emit_indent(&compiler->output, compiler->block_count);
    if (!buffer_append(&compiler->output, "}\n")) {
        return compiler_fail(compiler, compiler->error_line, "out of memory while generating output");
    }
    return true;
}

static Variable *find_variable(Compiler *compiler, const char *name) {
    for (Variable *variable = compiler->variables; variable; variable = variable->next) {
        if (strcmp(variable->name, name) == 0) return variable;
    }
    return NULL;
}

static bool add_variable(Compiler *compiler, const char *name, const char *declared_type,
                         ValueType type, bool is_fixed_array) {
    for (Variable *existing = compiler->variables; existing; existing = existing->next) {
        if (existing->scope_depth < compiler->block_count) break;
        if (existing->scope_depth == compiler->block_count && strcmp(existing->name, name) == 0) {
            return compiler_fail(compiler, compiler->error_line,
                                 "'%s' is already declared in this scope", name);
        }
    }
    Variable *variable = (Variable *)arena_alloc(&compiler->arena, sizeof(*variable));
    if (!variable) return compiler_fail(compiler, compiler->error_line, "out of memory while recording variable");
    variable->name = arena_strdup(&compiler->arena, name);
    variable->declared_type = arena_strdup(&compiler->arena, declared_type);
    if (!variable->name || !variable->declared_type) {
        return compiler_fail(compiler, compiler->error_line, "out of memory while recording variable");
    }
    variable->value_type = type;
    variable->scope_depth = compiler->block_count;
    variable->is_fixed_array = is_fixed_array;
    variable->next = compiler->variables;
    compiler->variables = variable;
    return true;
}

static ValueType type_name(const char *type_name) {
    if (strstr(type_name, "char *") || strstr(type_name, "char*") || strstr(type_name, "string")) return TYPE_STRING;
    if (strstr(type_name, "double") || strstr(type_name, "float")) return TYPE_DOUBLE;
    if (strstr(type_name, "char") && !strstr(type_name, "*")) return TYPE_CHAR;
    if (strstr(type_name, "*") || strstr(type_name, "[")) return TYPE_POINTER;
    if (strcmp(type_name, "int") == 0 || strcmp(type_name, "long") == 0 ||
        strcmp(type_name, "short") == 0 || strstr(type_name, "long ") ||
        strcmp(type_name, "unsigned") == 0 || strcmp(type_name, "bool") == 0 ||
        strcmp(type_name, "_Bool") == 0) return TYPE_INT;
    return TYPE_OTHER;
}

static const char *find_const(const char *open) {
    if (*open != '[') return NULL;
    size_t depth = 0;
    char quote = '\0';
    bool escaped = false;
    for (const char *p = open; *p; p++) {
        char ch = *p;
        if (quote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == quote) quote = '\0';
            continue;
        }
        if (ch == '"' || ch == '\'') { quote = ch; continue; }
        if (ch == '[') depth++;
        else if (ch == ']' && depth > 0) {
            depth--;
            if (depth == 0) return p;
        }
    }
    return NULL;
}

// Known fixed-array accesses are routed through a runtime bounds check
static char *array_accesses(Compiler *compiler, const char *expression) {
    StringBuffer output = {0};
    size_t i = 0;
    bool changed = false;
    char quote = '\0';
    bool escaped = false;

    while (expression[i]) {
        char ch = expression[i];
        if (quote) {
            if (!buffer_append_char(&output, ch)) goto oom;
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == quote) quote = '\0';
            i++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            if (!buffer_append_char(&output, ch)) goto oom;
            i++;
            continue;
        }
        if (is_ident_start(ch)) {
            size_t start = i++;
            while (ident_part(expression[i])) i++;
            size_t name_len = i - start;
            char *name = arena_strndup(&compiler->arena, expression + start, name_len);
            if (!name) goto oom;
            bool member_name = start > 0 && expression[start - 1] == '.';
            if (start > 1 && expression[start - 1] == '>' && expression[start - 2] == '-') member_name = true;
            Variable *variable = member_name ? NULL : find_variable(compiler, name);
            size_t open = i;
            while (expression[open] == ' ' || expression[open] == '\t') open++;
            if (variable && expression[open] == '[') {
                if (!variable->is_fixed_array) {
                    if (strchr(variable->declared_type, '[') || variable->value_type == TYPE_POINTER) {
                        free(output.data);
                        compiler_fail(compiler, compiler->error_line,
                            "indexing '%s' is not supported safely because its length is unknown; use a fixed array literal", name);
                        return NULL;
                    }
                } else {
                    const char *close = find_const(expression + open);
                    if (!close) {
                        free(output.data);
                        compiler_fail(compiler, compiler->error_line,
                                      "unclosed array index for '%s'", name);
                        return NULL;
                    }
                    char *index_expression = arena_strndup(
                        &compiler->arena, expression + open + 1,
                        (size_t)(close - (expression + open + 1)));
                    if (!index_expression) goto oom;
                    char *safe_index = array_accesses(compiler, index_expression);
                    if (!safe_index) {
                        free(output.data);
                        return NULL;
                    }
                    if (!buffer_append(&output, "HUMC_ARRAY_AT(") ||
                        !buffer_append_n(&output, expression + start, name_len) ||
                        !buffer_append(&output, ", (") ||
                        !buffer_append(&output, safe_index) ||
                        !buffer_append(&output, "))")) goto oom;
                    changed = true;
                    compiler->saw_bounds_check = true;
                    i = (size_t)(close - expression) + 1;
                    continue;
                }
            }
            if (!buffer_append_n(&output, expression + start, name_len)) goto oom;
            continue;
        }
        if (!buffer_append_char(&output, ch)) goto oom;
        i++;
    }

    if (!changed) {
        free(output.data);
        return arena_strdup(&compiler->arena, expression);
    }
    {
        char *taken = buffer_take(&output);
        if (!taken) return NULL;
        char *stable = arena_strdup(&compiler->arena, taken);
        free(taken);
        return stable;
    }

oom:
    free(output.data);
    compiler_fail(compiler, compiler->error_line, "out of memory while checking array access");
    return NULL;
}

static ValueType expression_type(Compiler *compiler, const char *expression) {
    while (*expression == ' ' || *expression == '\t') expression++;
    if (*expression == '"') return TYPE_STRING;
    if (*expression == '\'') return TYPE_CHAR;
    if (!strncmp(expression, "true", 4) && !ident_part(expression[4])) return TYPE_INT;
    if (!strncmp(expression, "false", 5) && !ident_part(expression[5])) return TYPE_INT;
    if (!strncmp(expression, "NULL", 4) && !ident_part(expression[4])) return TYPE_POINTER;

    bool saw_digit = false;
    for (size_t i = 0; expression[i]; i++) {
        if (isdigit((unsigned char)expression[i])) {
            saw_digit = true;
            if (expression[i + 1] == '.' || expression[i + 1] == 'e' || expression[i + 1] == 'E') return TYPE_DOUBLE;
        }
    }

    ValueType best = TYPE_INT;
    for (size_t i = 0; expression[i];) {
        if (!is_ident_start(expression[i])) { i++; continue; }
        size_t start = i++;
        while (ident_part(expression[i])) i++;
        char *name = arena_strndup(&compiler->arena, expression + start, i - start);
        if (!name) return TYPE_OTHER;
        Variable *variable = find_variable(compiler, name);
        if (variable) {
            size_t after_name = i;
            while (expression[after_name] == ' ' || expression[after_name] == '\t') after_name++;
            ValueType variable_type = variable->value_type;
            if (variable->is_fixed_array && expression[after_name] == '[') {
                const char *bracket = strchr(variable->declared_type, '[');
                if (bracket) {
                    char *element_name = arena_strndup(&compiler->arena, variable->declared_type,
                                                       (size_t)(bracket - variable->declared_type));
                    if (element_name) variable_type = type_name(trim_place(element_name));
                }
            }
            if (variable_type == TYPE_STRING) return TYPE_STRING;
            if (variable_type == TYPE_POINTER) best = TYPE_POINTER;
            else if (variable_type == TYPE_DOUBLE) best = TYPE_DOUBLE;
            else if (variable_type == TYPE_CHAR && best == TYPE_INT) best = TYPE_CHAR;
        }
    }
    (void)saw_digit;
    return best;
}

static bool express_arg(Compiler *compiler, char *arguments,
                                   size_t wanted_index, ValueType *type_out) {
    size_t index = 0;
    char *part = arguments;
    for (;;) {
        char *comma = find_char(part, ',');
        if (comma) *comma = '\0';
        char *expression = trim_place(part);
        if (index == wanted_index) {
            if (!*expression) return false;
            *type_out = expression_type(compiler, expression);
            return true;
        }
        if (!comma) return false;
        part = comma + 1;
        index++;
    }
}

static ValueType param_calls(Compiler *compiler,
                                            const char *function_name,
                                            size_t parameter_index) {
    const char *line = compiler->source;
    size_t function_name_length = strlen(function_name);

    while (line && *line) {
        const char *line_end = strchr(line, '\n');
        size_t line_length = line_end ? (size_t)(line_end - line) : strlen(line);
        char *copy = arena_strndup(&compiler->arena, line, line_length);
        if (!copy) return TYPE_INT;
        char *trimmed = trim_place(copy);

        if (!starts_word(trimmed, "fn")) {
            for (char *cursor = trimmed; *cursor;) {
                char *found = strstr(cursor, function_name);
                if (!found) break;
                char before = found == trimmed ? '\0' : found[-1];
                char after = found[function_name_length];
                cursor = found + function_name_length;
                if (ident_part(before) || ident_part(after)) continue;

                char *open = cursor;
                while (*open == ' ' || *open == '\t') open++;
                if (*open != '(') continue;
                char *close = find_close(open, '(', ')');
                if (!close) continue;
                char saved = *close;
                *close = '\0';
                ValueType found_type = TYPE_INT;
                bool got_type = express_arg(compiler, open + 1,
                                                       parameter_index, &found_type);
                *close = saved;
                if (got_type) return found_type;
            }
        }
        if (!line_end) break;
        line = line_end + 1;
    }
    return TYPE_INT;
}

static bool typed_param(Compiler *compiler, const char *function_name,
                                    char *parameters, StringBuffer *result) {
    char *cursor = parameters;
    bool first = true;
    while (*cursor) {
        char *start = cursor;
        char quote = '\0';
        int paren = 0;
        int bracket = 0;
        while (*cursor) {
            char ch = *cursor;
            if (quote) {
                if (ch == '\\' && cursor[1]) { cursor += 2; continue; }
                if (ch == quote) quote = '\0';
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '(') {
                paren++;
            } else if (ch == ')' && paren > 0) {
                paren--;
            } else if (ch == '[') {
                bracket++;
            } else if (ch == ']' && bracket > 0) {
                bracket--;
            } else if (ch == ',' && paren == 0 && bracket == 0) {
                break;
            }
            cursor++;
        }

        char saved = *cursor;
        *cursor = '\0';
        char *parameter = trim_place(start);
        if (*parameter && strcmp(parameter, "void") != 0) {
            // Preserve explicit C parameter declarations unchanged
            char *last_space = strrchr(parameter, ' ');
            char *typed_parameter = parameter;
            if (!last_space && valid_ident(parameter)) {
                size_t index = 0;
                for (const char *p = parameters; p < start; p++) {
                    if (*p == ',') index++;
                }
                ValueType type = param_calls(compiler, function_name, index);
                const char *type_name = type == TYPE_STRING ? "const char *" :
                    type == TYPE_DOUBLE ? "double" :
                    type == TYPE_CHAR ? "char" : "int";
                StringBuffer parameter_buffer = {0};
                (void)buffer_appendf(&parameter_buffer, "%s %s", type_name, parameter);
                typed_parameter = arena_strdup(&compiler->arena,
                    parameter_buffer.data ? parameter_buffer.data : parameter);
                free(parameter_buffer.data);
                if (!typed_parameter) {
                    *cursor = saved;
                    return false;
                }
            }
            if (!first) (void)buffer_append(result, ", ");
            (void)buffer_append(result, typed_parameter);
            first = false;
        }
        if (!saved) break;
        *cursor = saved;
        cursor++;
    }
    return true;
}

static bool param_string(const char *parameters, const char *name) {
    const char *cursor = parameters;
    size_t name_length = strlen(name);
    while (*cursor) {
        const char *end = strchr(cursor, ',');
        if (!end) end = cursor + strlen(cursor);
        const char *found = cursor;
        while (found < end) {
            found = strstr(found, name);
            if (!found || found >= end) break;
            bool left_ok = found == cursor || !ident_part(found[-1]);
            bool right_ok = found + name_length == end || !ident_part(found[name_length]);
            if (left_ok && right_ok) {
                size_t part_length = (size_t)(end - cursor);
                char *part = (char *)malloc(part_length + 1);
                if (!part) return false;
                memcpy(part, cursor, part_length);
                part[part_length] = '\0';
                bool is_string = strstr(part, "char *") || strstr(part, "char*");
                free(part);
                if (is_string) return true;
                break;
            }
            found += name_length;
        }
        if (!*end) break;
        cursor = end + 1;
    }
    return false;
}

static const char *fn_type(Compiler *compiler, int function_indent,
                                             const char *parameters, bool *has_value_return) {
    *has_value_return = false;
    const char *line = compiler->source;
    size_t line_number = 1;
    while (line && *line && line_number < compiler->error_line) {
        const char *next = strchr(line, '\n');
        if (!next) return "void";
        line = next + 1;
        line_number++;
    }
    if (!line) return "void";
    const char *next = strchr(line, '\n');
    line = next ? next + 1 : line + strlen(line);

    const char *result_type = "void";
    while (*line) {
        const char *line_end = strchr(line, '\n');
        size_t length = line_end ? (size_t)(line_end - line) : strlen(line);
        char *copy = arena_strndup(&compiler->arena, line, length);
        if (!copy) return "int";
        size_t indent_characters = 0;
        int indent = indet_width(copy, &indent_characters);
        char *statement = trim_place(copy);
        if (*statement && indent <= function_indent) break;
        if (*statement && starts_word(statement, "return")) {
            char *expression = trim_place(statement + 6);
            if (*expression) {
                ValueType type = expression_type(compiler, expression);
                if (type == TYPE_STRING) result_type = "const char *";
                else if (type == TYPE_DOUBLE) result_type = "double";
                else if (type == TYPE_CHAR) result_type = "char";
                else if (param_string(parameters, expression)) result_type = "const char *";
                else result_type = "int";
                *has_value_return = true;
            }
        }
        if (!line_end) break;
        line = line_end + 1;
    }
    return result_type;
}

static char *without_parens(Compiler *compiler, char *expression) {
    (void)compiler;
    expression = trim_place(expression);
    size_t length = strlen(expression);
    if (length >= 2 && expression[0] == '(') {
        char *close = find_close(expression, '(', ')');
        if (close == expression + length - 1) {
            expression[length - 1] = '\0';
            return trim_place(expression + 1);
        }
    }
    return expression;
}

static bool emit_print(Compiler *compiler, char *expression, size_t depth) {
    expression = trim_place(expression);
    size_t length = strlen(expression);
    if (length >= 2 && expression[0] == '(') {
        char *close = find_close(expression, '(', ')');
        if (close == expression + length - 1) {
            expression[length - 1] = '\0';
            expression = trim_place(expression + 1);
        }
    }
    if (!*expression) return compiler_fail(compiler, compiler->error_line, "print requires an expression");

    // Support for simple string concatenation using the "+" operator
    char *concat = arena_strdup(&compiler->arena, expression);
    char *plus = concat ? find_char(concat, '+') : NULL;
    if (plus) {
        *plus = '\0';
        char *left = trim_place(concat);
        char *right = trim_place(plus + 1);
        ValueType left_type = expression_type(compiler, left);
        ValueType right_type = expression_type(compiler, right);
        if (left_type == TYPE_STRING || right_type == TYPE_STRING) {
            char *safe_left = array_accesses(compiler, left);
            char *safe_right = array_accesses(compiler, right);
            if (!safe_left || !safe_right) return false;
            emit_indent(&compiler->output, depth);
            (void)buffer_appendf(&compiler->output,
                "printf(\"%%s%%s\\n\", (const char *)(%s), (const char *)(%s));\n",
                safe_left, safe_right);
            compiler->saw_print = true;
            return true;
        }
    }

    ValueType type = expression_type(compiler, expression);
    expression = array_accesses(compiler, expression);
    if (!expression) return false;
    emit_indent(&compiler->output, depth);
    if (type == TYPE_STRING) {
        (void)buffer_appendf(&compiler->output, "printf(\"%%s\\n\", (const char *)(%s));\n", expression);
    } else if (type == TYPE_DOUBLE) {
        (void)buffer_appendf(&compiler->output, "printf(\"%%g\\n\", (double)(%s));\n", expression);
    } else if (type == TYPE_CHAR) {
        (void)buffer_appendf(&compiler->output, "printf(\"%%c\\n\", (int)(%s));\n", expression);
    } else if (type == TYPE_POINTER) {
        (void)buffer_appendf(&compiler->output, "printf(\"%%p\\n\", (void *)(%s));\n", expression);
    } else {
        (void)buffer_appendf(&compiler->output, "printf(\"%%lld\\n\", (long long)(%s));\n", expression);
    }
    compiler->saw_print = true;
    return true;
}

static bool emit_let(Compiler *compiler, char *statement, size_t depth) {
    char *body = trim_place(statement + 3);
    if (!*body) return compiler_fail(compiler, compiler->error_line, "let requires a variable name");

    char *equal = find_equal(body);
    char *initializer = NULL;
    if (equal) {
        *equal = '\0';
        initializer = trim_place(equal + 1);
        if (!*initializer) return compiler_fail(compiler, compiler->error_line, "let initializer cannot be empty");
        size_t init_len = strlen(initializer);
        if (init_len && initializer[init_len - 1] == ';') initializer[--init_len] = '\0';
        trim_place(initializer);
    }

    char *left = trim_place(body);
    char *colon = find_char(left, ':');
    char *name = left;
    char *explicit_type = NULL;
    if (colon) {
        *colon = '\0';
        name = trim_place(left);
        explicit_type = trim_place(colon + 1);
        if (!*explicit_type) return compiler_fail(compiler, compiler->error_line, "type annotation after ':' cannot be empty");
    }

    if (!valid_ident(name)) return compiler_fail(compiler, compiler->error_line, "invalid variable name in let declaration");
    if (!initializer) {
        return compiler_fail(compiler, compiler->error_line,
                            "safe HumC requires every variable to be initialized at declaration; use 'let name = value'");
    }

    bool array_initializer = initializer && initializer[0] == '[' &&
        strlen(initializer) >= 2 && initializer[strlen(initializer) - 1] == ']';
    ValueType element_type = TYPE_INT;
    if (array_initializer) {
        char *items = arena_strndup(&compiler->arena, initializer + 1, strlen(initializer) - 2);
        if (!items) return compiler_fail(compiler, compiler->error_line, "out of memory");
        char *cursor = items;
        bool saw_item = false;
        while (*cursor) {
            char *comma = find_char(cursor, ',');
            if (comma) *comma = '\0';
            char *item = trim_place(cursor);
            if (*item) {
                ValueType item_type = expression_type(compiler, item);
                if (!saw_item) {
                    element_type = item_type;
                    saw_item = true;
                } else if (item_type != element_type &&
                           !((item_type == TYPE_INT && element_type == TYPE_DOUBLE) ||
                             (item_type == TYPE_DOUBLE && element_type == TYPE_INT))) {
                    return compiler_fail(compiler, compiler->error_line,
                                         "array literal elements must have compatible types");
                } else if (item_type == TYPE_DOUBLE) {
                    element_type = TYPE_DOUBLE;
                }
            } else if (!comma && saw_item) {
            } else {
                return compiler_fail(compiler, compiler->error_line, "array literal contains an empty element");
            }
            if (!comma) break;
            cursor = comma + 1;
        }
        if (!saw_item) return compiler_fail(compiler, compiler->error_line, "array literal cannot be empty in this compiler version");
    }

    ValueType type = array_initializer ? TYPE_POINTER :
        (explicit_type ? type_name(explicit_type) : expression_type(compiler, initializer));
    if (initializer && !array_initializer) {
        initializer = array_accesses(compiler, initializer);
        if (!initializer) return false;
    }
    const char *declared_type = explicit_type;
    if (!declared_type) {
        if (array_initializer) {
            if (element_type == TYPE_STRING) declared_type = "const char *[]";
            else if (element_type == TYPE_DOUBLE) declared_type = "double[]";
            else if (element_type == TYPE_CHAR) declared_type = "char[]";
            else declared_type = "int[]";
        } else if (type == TYPE_STRING) declared_type = "const char *";
        else if (type == TYPE_DOUBLE) declared_type = "double";
        else if (type == TYPE_CHAR) declared_type = "char";
        else if (type == TYPE_POINTER) declared_type = "void *";
        else declared_type = "int";
    }

    if (array_initializer && !strchr(declared_type, '[')) {
        return compiler_fail(compiler, compiler->error_line,
            "array initializer requires an array type; use 'let name = [..]' or 'let name: int[] = [..]'");
    }

    emit_indent(&compiler->output, depth);
    if (strchr(declared_type, '[')) {
        const char *array = strchr(declared_type, '[');
        char *base_type = arena_strndup(&compiler->arena, declared_type, (size_t)(array - declared_type));
        (void)buffer_appendf(&compiler->output, "%s %s%s", trim_place(base_type), name, array);
        if (initializer) {
            if (array_initializer) {
                (void)buffer_append(&compiler->output, " = {");
                (void)buffer_append_n(&compiler->output, initializer + 1, strlen(initializer) - 2);
                (void)buffer_append_char(&compiler->output, '}');
            } else {
                (void)buffer_appendf(&compiler->output, " = %s", initializer);
            }
        }
        (void)buffer_append(&compiler->output, ";\n");
    } else {
        (void)buffer_appendf(&compiler->output, "%s %s", declared_type, name);
        if (initializer) (void)buffer_appendf(&compiler->output, " = %s", initializer);
        (void)buffer_append(&compiler->output, ";\n");
    }
    if (!add_variable(compiler, name, declared_type, type, array_initializer)) return false;
    return !compiler->saw_error;
}

static void fn_paramets(Compiler *compiler, char *params) {
    char *cursor = params;
    while (*cursor) {
        char *start = cursor;
        char quote = '\0';
        int paren = 0;
        int bracket = 0;
        while (*cursor) {
            char ch = *cursor;
            if (quote) {
                if (ch == '\\' && cursor[1]) { cursor += 2; continue; }
                if (ch == quote) quote = '\0';
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '(') {
                paren++;
            } else if (ch == ')' && paren > 0) {
                paren--;
            } else if (ch == '[') {
                bracket++;
            } else if (ch == ']' && bracket > 0) {
                bracket--;
            } else if (ch == ',' && paren == 0 && bracket == 0) {
                break;
            }
            cursor++;
        }
        char saved = *cursor;
        *cursor = '\0';
        char *parameter = trim_place(start);
        if (*parameter && strcmp(parameter, "void") != 0 && strcmp(parameter, "...") != 0) {
            char *end = parameter + strlen(parameter);
            while (end > parameter && !ident_part(end[-1])) end--;
            char *name_end = end;
            while (end > parameter && ident_part(end[-1])) end--;
            if (end < name_end && is_ident_start(*end)) {
                char *name = arena_strndup(&compiler->arena, end, (size_t)(name_end - end));
                char *type = arena_strndup(&compiler->arena, parameter, (size_t)(end - parameter));
                if (name && type && *trim_place(type)) {
                    type = trim_place(type);
                    (void)add_variable(compiler, name, type, type_name(type), false);
                }
            }
        }
        if (!saved) break;
        *cursor = saved;
        cursor++;
    }
}

static bool emit_fn(Compiler *compiler, char *statement, size_t depth, int indent) {
    char *spec = trim_place(statement + 2);
    if (!*spec) return compiler_fail(compiler, compiler->error_line, "fn requires a function name");

    char *name_start = spec;
    char *p = name_start;
    while (ident_part(*p)) p++;
    if (p == name_start || !is_ident_start(*name_start)) return compiler_fail(compiler, compiler->error_line, "invalid function name");
    char *name = arena_strndup(&compiler->arena, name_start, (size_t)(p - name_start));
    if (!name) return compiler_fail(compiler, compiler->error_line, "out of memory");
    if (p > name_start && !valid_ident(name)) return compiler_fail(compiler, compiler->error_line, "invalid function name");

    char *params = NULL;
    char *suffix = trim_place(p);
    if (*suffix == '(') {
        char *close = find_close(suffix, '(', ')');
        if (!close) return compiler_fail(compiler, compiler->error_line, "unclosed parameter list in function declaration");
        params = arena_strndup(&compiler->arena, suffix + 1, (size_t)(close - suffix - 1));
        suffix = trim_place(close + 1);
    } else {
        params = arena_strdup(&compiler->arena, "");
    }
    if (!params) return compiler_fail(compiler, compiler->error_line, "out of memory");

    const char *return_type = NULL;
    if (*suffix) {
        if (strncmp(suffix, "->", 2) != 0) return compiler_fail(compiler, compiler->error_line, "expected '-> return_type' after function signature");
        return_type = trim_place(suffix + 2);
        if (!*return_type) return compiler_fail(compiler, compiler->error_line, "function return type after '->' cannot be empty");
    }
    StringBuffer typed_parameter_buffer = {0};
    if (!typed_param(compiler, name, params, &typed_parameter_buffer)) {
        free(typed_parameter_buffer.data);
        return compiler_fail(compiler, compiler->error_line, "out of memory while inferring function parameters");
    }
    char *typed_params_owned = buffer_take(&typed_parameter_buffer);
    if (!typed_params_owned) return compiler_fail(compiler, compiler->error_line, "out of memory");
    char *typed_params = arena_strdup(&compiler->arena, *typed_params_owned ? typed_params_owned : "void");
    free(typed_params_owned);
    if (!typed_params) return compiler_fail(compiler, compiler->error_line, "out of memory");

    if (!return_type) {
        if (strcmp(name, "main") == 0) {
            return_type = "int";
        } else {
            bool has_value_return = false;
            const char *inferred_return = fn_type(
                compiler, indent, typed_params, &has_value_return);
            return_type = has_value_return ? inferred_return : "void";
        }
    }

    emit_indent(&compiler->output, depth);
    (void)buffer_appendf(&compiler->output, "%s %s(%s) {\n", return_type, name, typed_params);
    push_block(compiler, BLOCK_FUNCTION, indent);
    fn_paramets(compiler, typed_params);
    return !compiler->saw_error;
}

static bool emit_range_for(Compiler *compiler, char *expression, size_t depth) {
    char *in = strstr(expression, " in ");
    if (!in) return false;
    *in = '\0';
    char *variable = trim_place(expression);
    char *range = trim_place(in + 4);
    char *dots = strstr(range, "..");
    if (!valid_ident(variable) || !dots) return false;
    *dots = '\0';
    char *start = trim_place(range);
    char *end = trim_place(dots + 2);
    if (!*start || !*end) return false;
    start = array_accesses(compiler, start);
    end = array_accesses(compiler, end);
    if (!start || !end) return false;
    emit_indent(&compiler->output, depth);
    (void)buffer_appendf(&compiler->output, "for (int %s = %s; %s < %s; ++%s) {\n", variable, start, variable, end, variable);
    return true;
}

static bool emit_array_for(Compiler *compiler, char *expression, size_t depth, int indent) {
    char *in = strstr(expression, " in ");
    if (!in) return false;
    *in = '\0';
    char *variable_name = trim_place(expression);
    char *iterable_name = trim_place(in + 4);
    if (!valid_ident(variable_name) || !valid_ident(iterable_name)) return false;

    Variable *iterable = find_variable(compiler, iterable_name);
    if (!iterable || !strchr(iterable->declared_type, '[')) return false;
    if (!iterable->is_fixed_array) {
        compiler_fail(compiler, compiler->error_line,
                      "cannot safely iterate '%s': this compiler version needs a fixed array literal", iterable_name);
        return false;
    }

    const char *suffix = strchr(iterable->declared_type, '[');
    char *element_type = arena_strndup(&compiler->arena, iterable->declared_type,
                                      (size_t)(suffix - iterable->declared_type));
    if (!element_type) {
        compiler_fail(compiler, compiler->error_line, "out of memory");
        return false;
    }
    element_type = trim_place(element_type);

    char index_name[64];
    (void)snprintf(index_name, sizeof(index_name), "__humc_index_%zu", compiler->error_line);
    emit_indent(&compiler->output, depth);
    (void)buffer_appendf(&compiler->output,
        "for (size_t %s = 0; %s < sizeof(%s) / sizeof(%s[0]); ++%s) {\n",
        index_name, index_name, iterable_name, iterable_name, index_name);
    push_block(compiler, BLOCK_FOR, indent);
    emit_indent(&compiler->output, depth + 1);
    (void)buffer_appendf(&compiler->output, "%s %s = %s[%s];\n",
        element_type, variable_name, iterable_name, index_name);
    if (!add_variable(compiler, variable_name, element_type, type_name(element_type), false)) return false;
    return !compiler->saw_error;
}

static bool emit_state(Compiler *compiler, char *statement, size_t depth) {
    statement = trim_place(statement);
    if (!*statement) return true;
    size_t length = strlen(statement);
    if (statement[length - 1] == ':') {
        return compiler_fail(compiler, compiler->error_line, "HumC blocks use indentation and do not end with ':'");
    }
    if (statement[0] == '#') {
        (void)buffer_appendf(&compiler->output, "%s\n", statement);
        return true;
    }
    if (strncmp(statement, "//", 2) == 0 || strncmp(statement, "/*", 2) == 0 ||
        strncmp(statement, "*", 1) == 0 || strncmp(statement, "*/", 2) == 0) {
        emit_indent(&compiler->output, depth);
        (void)buffer_appendf(&compiler->output, "%s\n", statement);
        return true;
    }

    statement = array_accesses(compiler, statement);
    if (!statement) return false;
    length = strlen(statement);
    emit_indent(&compiler->output, depth);
    if (statement[length - 1] == ';' || statement[length - 1] == '{' || statement[length - 1] == '}' ||
        strcmp(statement, "}") == 0) {
        (void)buffer_appendf(&compiler->output, "%s\n", statement);
    } else if (starts_word(statement, "return") || starts_word(statement, "break") || starts_word(statement, "continue")) {
        (void)buffer_appendf(&compiler->output, "%s;\n", statement);
    } else {
        (void)buffer_appendf(&compiler->output, "%s;\n", statement);
    }
    return true;
}

static bool process_statement(Compiler *compiler, char *statement, int indent, size_t line_number) {
    compiler->error_line = line_number;
    statement = trim_place(statement);
    if (!*statement) return true;
    if (strstr(statement, "true") || strstr(statement, "false")) compiler->saw_bool = true;
    if (statement[strlen(statement) - 1] == ':') {
        return compiler_fail(compiler, line_number, "HumC blocks use indentation and do not end with ':'");
    }

    size_t depth = compiler->block_count;
    if (starts_word(statement, "fn")) return emit_fn(compiler, statement, depth, indent);
    if (starts_word(statement, "let")) return emit_let(compiler, statement, depth);
    if (starts_word(statement, "print")) return emit_print(compiler, trim_place(statement + 5), depth);

    if (starts_word(statement, "if")) {
        char *expression = without_parens(compiler, trim_place(statement + 2));
        if (!*expression) return compiler_fail(compiler, line_number, "if requires a condition");
        expression = array_accesses(compiler, expression);
        if (!expression) return false;
        emit_indent(&compiler->output, depth);
        (void)buffer_appendf(&compiler->output, "if (%s) {\n", expression);
        push_block(compiler, BLOCK_IF, indent);
        return !compiler->saw_error;
    }
    if (starts_word(statement, "elif")) {
        Block *top = top_block(compiler);
        if (!top || top->indent != indent || (top->type != BLOCK_IF && top->type != BLOCK_ELIF))
            return compiler_fail(compiler, line_number, "elif without a matching if");
        if (top->body_indent < 0) return compiler_fail(compiler, line_number, "elif without a previous block");
        compiler->variables = top->variables_before;
        char *expression = without_parens(compiler, trim_place(statement + 4));
        if (!*expression) return compiler_fail(compiler, line_number, "elif requires a condition");
        expression = array_accesses(compiler, expression);
        if (!expression) return false;
        emit_indent(&compiler->output, compiler->block_count - 1);
        (void)buffer_appendf(&compiler->output, "} else if (%s) {\n", expression);
        top->type = BLOCK_ELIF;
        top->body_indent = -1;
        return true;
    }
    if (starts_word(statement, "else")) {
        if (*trim_place(statement + 4)) return compiler_fail(compiler, line_number, "use 'elif condition' instead of 'else if condition'");
        Block *top = top_block(compiler);
        if (!top || top->indent != indent || (top->type != BLOCK_IF && top->type != BLOCK_ELIF))
            return compiler_fail(compiler, line_number, "else without a matching if");
        if (top->body_indent < 0) return compiler_fail(compiler, line_number, "else without a previous block");
        compiler->variables = top->variables_before;
        emit_indent(&compiler->output, compiler->block_count - 1);
        (void)buffer_append(&compiler->output, "} else {\n");
        top->type = BLOCK_ELSE;
        top->body_indent = -1;
        return true;
    }
    if (starts_word(statement, "while")) {
        char *expression = without_parens(compiler, trim_place(statement + 5));
        if (!*expression) return compiler_fail(compiler, line_number, "while requires a condition");
        expression = array_accesses(compiler, expression);
        if (!expression) return false;
        emit_indent(&compiler->output, depth);
        (void)buffer_appendf(&compiler->output, "while (%s) {\n", expression);
        push_block(compiler, BLOCK_WHILE, indent);
        return !compiler->saw_error;
    }
    if (starts_word(statement, "for")) {
        char *expression = trim_place(statement + 3);
        char *copy = arena_strdup(&compiler->arena, expression);
        if (!copy) return compiler_fail(compiler, line_number, "out of memory");
        if (emit_range_for(compiler, copy, depth)) {
            push_block(compiler, BLOCK_FOR, indent);
            // The range variable is registered in the loop's scope
            char *source_expr = arena_strdup(&compiler->arena, expression);
            if (!source_expr) return compiler_fail(compiler, line_number, "out of memory");
            char *source_in = strstr(source_expr, " in ");
            if (source_in) {
                *source_in = '\0';
                char *range_name = trim_place(source_expr);
                if (!add_variable(compiler, range_name, "int", TYPE_INT, false)) return false;
            }
            return !compiler->saw_error;
        }

        copy = arena_strdup(&compiler->arena, expression);
        if (!copy) return compiler_fail(compiler, line_number, "out of memory");
        if (strstr(copy, " in ") && emit_array_for(compiler, copy, depth, indent)) {
            return !compiler->saw_error;
        }
        if (strstr(expression, " in ")) {
            return compiler_fail(compiler, line_number,
                "for-in iteration expects an array variable, e.g. 'for item in items'");
        }
        expression = without_parens(compiler, expression);
        if (!*expression) return compiler_fail(compiler, line_number, "for requires a C-style loop header, a numeric range, or an array iteration");
        expression = array_accesses(compiler, expression);
        if (!expression) return false;
        emit_indent(&compiler->output, depth);
        (void)buffer_appendf(&compiler->output, "for (%s) {\n", expression);
        push_block(compiler, BLOCK_FOR, indent);
        return !compiler->saw_error;
    }
    if (starts_word(statement, "switch")) {
        char *expression = without_parens(compiler, trim_place(statement + 6));
        if (!*expression) return compiler_fail(compiler, line_number, "switch requires an expression");
        expression = array_accesses(compiler, expression);
        if (!expression) return false;
        emit_indent(&compiler->output, depth);
        (void)buffer_appendf(&compiler->output, "switch (%s) {\n", expression);
        push_block(compiler, BLOCK_SWITCH, indent);
        return !compiler->saw_error;
    }
    if (starts_word(statement, "case")) {
        Block *top = top_block(compiler);
        if (!top) return compiler_fail(compiler, line_number, "case without a matching switch");
        if (top->type == BLOCK_CASE || top->type == BLOCK_DEFAULT) {
            if (top->indent != indent) return compiler_fail(compiler, line_number, "case has invalid indentation");
            if (!close_top_block(compiler)) return false;
            top = top_block(compiler);
        }
        if (!top || top->type != BLOCK_SWITCH || indent <= top->indent)
            return compiler_fail(compiler, line_number, "case must be indented inside switch");
        char *expression = trim_place(statement + 4);
        if (!*expression) return compiler_fail(compiler, line_number, "case requires a value");
        expression = array_accesses(compiler, expression);
        if (!expression) return false;
        emit_indent(&compiler->output, compiler->block_count);
        (void)buffer_appendf(&compiler->output, "case %s: {\n", expression);
        push_block(compiler, BLOCK_CASE, indent);
        return !compiler->saw_error;
    }
    if (starts_word(statement, "default")) {
        Block *top = top_block(compiler);
        if (top && (top->type == BLOCK_CASE || top->type == BLOCK_DEFAULT)) {
            if (top->indent != indent) return compiler_fail(compiler, line_number, "default has invalid indentation");
            if (!close_top_block(compiler)) return false;
            top = top_block(compiler);
        }
        if (!top || top->type != BLOCK_SWITCH || indent <= top->indent)
            return compiler_fail(compiler, line_number, "default must be indented inside switch");
        emit_indent(&compiler->output, compiler->block_count);
        (void)buffer_append(&compiler->output, "default: {\n");
        push_block(compiler, BLOCK_DEFAULT, indent);
        return !compiler->saw_error;
    }
    if (starts_word(statement, "try") || starts_word(statement, "catch") || starts_word(statement, "finally") || starts_word(statement, "async")) {
        return compiler_fail(compiler, line_number, "'%s' is not a C construct supported by HumC", statement);
    }

    return emit_state(compiler, statement, depth);
}

static bool close_indent(Compiler *compiler, int indent, bool is_branch, size_t line_number) {
    compiler->error_line = line_number;
    while (compiler->block_count) {
        Block *top = top_block(compiler);
        if (is_branch) {
            if (top->indent < indent) break;
            if (top->indent == indent &&
                (top->type == BLOCK_IF || top->type == BLOCK_ELIF || top->type == BLOCK_SWITCH)) break;
            if (top->indent == indent && case_block(top->type)) {
                if (!close_top_block(compiler)) return false;
                continue;
            }
            if (top->indent > indent || top->indent == indent) {
                if (!close_top_block(compiler)) return false;
                continue;
            }
        } else {
            if (indent > top->indent) break;
            if (!close_top_block(compiler)) return false;
        }
    }
    return true;
}

static bool process_source(Compiler *compiler, const char *source) {
    const char *cursor = source;
    size_t line_number = 0;
    bool in_block_comment = false;

    while (*cursor) {
        line_number++;
        compiler->error_line = line_number;
        const char *line_start = cursor;
        while (*cursor && *cursor != '\n') cursor++;
        size_t raw_length = (size_t)(cursor - line_start);
        if (*cursor == '\n') cursor++;
        if (raw_length && line_start[raw_length - 1] == '\r') raw_length--;
        char *raw = arena_strndup(&compiler->arena, line_start, raw_length);
        if (!raw) return compiler_fail(compiler, line_number, "out of memory");

        char *trimmed_raw = trim_place(arena_strdup(&compiler->arena, raw));
        if (!trimmed_raw) return compiler_fail(compiler, line_number, "out of memory");
        if (!*trimmed_raw) {
            (void)buffer_append(&compiler->output, "\n");
            continue;
        }

        if (in_block_comment) {
            (void)buffer_appendf(&compiler->output, "%s\n", raw);
            if (strstr(raw, "*/")) in_block_comment = false;
            continue;
        }

        size_t indent_chars = 0;
        int indent = indet_width(raw, &indent_chars);
        char *content = raw + indent_chars;

        if (strncmp(content, "/*", 2) == 0) {
            (void)buffer_appendf(&compiler->output, "%s\n", content);
            if (!strstr(content + 2, "*/")) in_block_comment = true;
            continue;
        }
        if (content[0] == '#' || strncmp(content, "//", 2) == 0) {
            (void)buffer_appendf(&compiler->output, "%s\n", content);
            continue;
        }

        char *inline_comment = NULL;
        char *without_comment = rm_comment(compiler, content, &inline_comment);
        if (!without_comment) return false;
        if (inline_comment) {
            (void)buffer_append_n(&compiler->output, raw, indent_chars);
            (void)buffer_appendf(&compiler->output, "%s\n", inline_comment);
        }
        char *statement = trim_place(without_comment);
        if (!*statement) {
            emit_indent(&compiler->output, compiler->block_count);
            (void)buffer_append(&compiler->output, "\n");
            continue;
        }

        bool is_branch = starts_word(statement, "elif") || starts_word(statement, "else") ||
                         starts_word(statement, "case") || starts_word(statement, "default");
        if (!close_indent(compiler, indent, is_branch, line_number)) return false;

        Block *top = top_block(compiler);
        bool matching_if_branch = is_branch && top && top->indent == indent &&
            (top->type == BLOCK_IF || top->type == BLOCK_ELIF) &&
            (starts_word(statement, "else") || starts_word(statement, "elif"));
        if (!matching_if_branch) {
            if (top) {
                if (indent <= top->indent) {
                    return compiler_fail(compiler, line_number, "inconsistent indentation");
                }
                if (top->body_indent < 0) {
                    top->body_indent = indent;
                } else if (indent != top->body_indent) {
                    return compiler_fail(compiler, line_number, "inconsistent indentation: expected indentation %d, got %d", top->body_indent, indent);
                }
            } else if (indent != 0) {
                return compiler_fail(compiler, line_number, "unexpected indentation at top level");
            }
        }

        if (!process_statement(compiler, statement, indent, line_number)) return false;
    }

    compiler->error_line = line_number ? line_number : 1;
    while (compiler->block_count) {
        if (!close_top_block(compiler)) return false;
    }
    if (in_block_comment) {
        return compiler_fail(compiler, line_number ? line_number : 1, "unterminated block comment");
    }
    return !compiler->saw_error;
}

typedef struct {
    const char *start;
    size_t length;
    int indent;
    bool in_function;
    bool directive;
} SourceLine;

// Top-level executable statements are treated as a script and wrapped in main
static char *normalize_script(const char *source) {
    size_t line_count = 1;
    for (const char *p = source; *p; p++) if (*p == '\n') line_count++;

    SourceLine *lines = (SourceLine *)calloc(line_count, sizeof(*lines));
    if (!lines) return NULL;

    const char *cursor = source;
    bool has_main = false;
    for (size_t i = 0; i < line_count; i++) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        lines[i].start = cursor;
        lines[i].length = length;
        char *copy = (char *)malloc(length + 1);
        if (!copy) { free(lines); return NULL; }
        memcpy(copy, cursor, length);
        copy[length] = '\0';
        size_t indent_characters = 0;
        lines[i].indent = indet_width(copy, &indent_characters);
        char *trimmed = trim_place(copy);
        lines[i].directive = trimmed[0] == '#';
        if (lines[i].indent == 0 && starts_word(trimmed, "fn")) {
            char *name = trim_place(trimmed + 2);
            if (starts_word(name, "main")) has_main = true;
        }
        free(copy);
        cursor = end ? end + 1 : cursor + length;
    }

    if (has_main) {
        StringBuffer original = {0};
        (void)buffer_append(&original, source);
        free(lines);
        return buffer_take(&original);
    }

    bool has_script_statement = false;
    for (size_t i = 0; i < line_count; i++) {
        if (lines[i].indent != 0 || lines[i].directive) continue;
        char *copy = (char *)malloc(lines[i].length + 1);
        if (!copy) { free(lines); return NULL; }
        memcpy(copy, lines[i].start, lines[i].length);
        copy[lines[i].length] = '\0';
        char *trimmed = trim_place(copy);
        bool is_function = starts_word(trimmed, "fn");
        bool is_comment = strncmp(trimmed, "//", 2) == 0 ||
                          strncmp(trimmed, "/*", 2) == 0 ||
                          strncmp(trimmed, "*", 1) == 0 ||
                          !*trimmed;
        if (!is_function && !is_comment) has_script_statement = true;
        free(copy);
    }

    if (!has_script_statement) {
        StringBuffer original = {0};
        (void)buffer_append(&original, source);
        free(lines);
        return buffer_take(&original);
    }

    // Marks each top-level function and its body with indentation
    for (size_t i = 0; i < line_count; i++) {
        if (lines[i].indent != 0) continue;
        char *copy = (char *)malloc(lines[i].length + 1);
        if (!copy) { free(lines); return NULL; }
        memcpy(copy, lines[i].start, lines[i].length);
        copy[lines[i].length] = '\0';
        char *trimmed = trim_place(copy);
        bool is_function = starts_word(trimmed, "fn");
        free(copy);
        if (!is_function) continue;

        size_t end = i + 1;
        for (; end < line_count; end++) {
            char *next_copy = (char *)malloc(lines[end].length + 1);
            if (!next_copy) { free(lines); return NULL; }
            memcpy(next_copy, lines[end].start, lines[end].length);
            next_copy[lines[end].length] = '\0';
            char *next_trimmed = trim_place(next_copy);
            bool ends_function = *next_trimmed && lines[end].indent == 0;
            free(next_copy);
            if (ends_function) break;
        }
        for (size_t j = i; j < end; j++) lines[j].in_function = true;
        i = end ? end - 1 : i;
    }

    StringBuffer result = {0};
    for (size_t i = 0; i < line_count; i++) {
        if (lines[i].directive && !lines[i].in_function) {
            (void)buffer_append_n(&result, lines[i].start, lines[i].length);
            (void)buffer_append_char(&result, '\n');
        }
    }
    for (size_t i = 0; i < line_count; i++) {
        if (lines[i].in_function) {
            (void)buffer_append_n(&result, lines[i].start, lines[i].length);
            (void)buffer_append_char(&result, '\n');
        }
    }
    (void)buffer_append(&result, "fn main\n");
    for (size_t i = 0; i < line_count; i++) {
        if (lines[i].in_function || lines[i].directive) continue;
        if (lines[i].length > 0) (void)buffer_append(&result, "    ");
        (void)buffer_append_n(&result, lines[i].start, lines[i].length);
        (void)buffer_append_char(&result, '\n');
    }

    free(lines);
    return buffer_take(&result);
}

static size_t leading_comment(const char *source, size_t length) {
    size_t offset = 0;
    bool in_block_comment = false;
    while (offset < length) {
        size_t line_start = offset;
        while (offset < length && source[offset] != '\n') offset++;
        size_t line_end = offset;
        if (offset < length && source[offset] == '\n') offset++;
        size_t first = line_start;
        while (first < line_end && (source[first] == ' ' || source[first] == '\t' || source[first] == '\r')) first++;
        size_t content_length = line_end - first;
        bool blank = content_length == 0;
        bool preprocessor = content_length > 0 && source[first] == '#';
        bool line_comment = content_length >= 2 && source[first] == '/' && source[first + 1] == '/';
        bool block_start = content_length >= 2 && source[first] == '/' && source[first + 1] == '*';
        bool block_end = content_length >= 2 && source[first] == '*' && source[first + 1] == '/';
        if (in_block_comment) {
            if (content_length >= 2 && strstr(source + first, "*/") && strstr(source + first, "*/") < source + line_end) {
                in_block_comment = false;
            }
            continue;
        }
        if (blank || preprocessor || line_comment || block_start || block_end) {
            if (block_start) {
                bool has_end = false;
                for (size_t j = first + 2; j + 1 < line_end; j++) {
                    if (source[j] == '*' && source[j + 1] == '/') { has_end = true; break; }
                }
                if (!has_end) in_block_comment = true;
            }
            continue;
        }
        return line_start;
    }
    return length;
}

static bool body_include(const char *body, const char *header) {
    const char *cursor = body;
    size_t header_length = strlen(header);
    while (*cursor) {
        const char *line = cursor;
        while (*cursor && *cursor != '\n') cursor++;
        const char *line_end = cursor;
        if (*cursor == '\n') cursor++;
        while (line < line_end && (*line == ' ' || *line == '\t')) line++;
        if (line_end - line < 8 || strncmp(line, "#include", 8) != 0) continue;
        line += 8;
        while (line < line_end && (*line == ' ' || *line == '\t')) line++;
        if (line >= line_end || (*line != '<' && *line != '"')) continue;
        char close = *line == '<' ? '>' : '"';
        line++;
        if ((size_t)(line_end - line) >= header_length &&
            strncmp(line, header, header_length) == 0 &&
            line + header_length < line_end && line[header_length] == close) return true;
    }
    return false;
}

static bool stdio_include(const char *body) {
    return body_include(body, "stdio.h");
}

static bool stdbool_include(const char *body) {
    return body_include(body, "stdbool.h");
}

char *humc_compile(const char *source, char **error_message) {
    if (error_message) *error_message = NULL;
    if (!source) {
        if (error_message) {
            *error_message = (char *)malloc(29);
            if (*error_message) strcpy(*error_message, "source string is null");
        }
        return NULL;
    }

    char *normalized_source = normalize_script(source);
    if (!normalized_source) {
        if (error_message) {
            *error_message = (char *)malloc(29);
            if (*error_message) strcpy(*error_message, "out of memory");
        }
        return NULL;
    }

    Compiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.error_line = 1;
    compiler.source = normalized_source;

    if (!process_source(&compiler, normalized_source)) {
        if (compiler.error_message) {
            StringBuffer message = {0};
            (void)buffer_appendf(&message, "line %zu: %s", compiler.error_line, compiler.error_message);
            free(compiler.error_message);
            compiler.error_message = buffer_take(&message);
        }
        if (error_message) *error_message = compiler.error_message;
        else free(compiler.error_message);
        free(compiler.blocks);
        free(compiler.output.data);
        arena_destroy(&compiler.arena);
        free(normalized_source);
        return NULL;
    }

    StringBuffer final_output = {0};
    const char *body = compiler.output.data ? compiler.output.data : "";
    size_t body_length = compiler.output.length;
    bool need_stdio = (compiler.saw_print || compiler.saw_bounds_check) && !stdio_include(body);
    bool need_stdbool = compiler.saw_bool && !stdbool_include(body);
    if (need_stdio || need_stdbool || compiler.saw_bounds_check) {
        size_t insertion = leading_comment(body, body_length);
        (void)buffer_append_n(&final_output, body, insertion);
        if (need_stdio) (void)buffer_append(&final_output, "#include <stdio.h>\n");
        if (need_stdbool) (void)buffer_append(&final_output, "#include <stdbool.h>\n");
        if (compiler.saw_bounds_check) {
            (void)buffer_append(&final_output,
                "#include <stdint.h>\n"
                "#include <stdlib.h>\n"
                "static size_t humc_checked_index(intmax_t index, size_t length, const char *array_name, const char *file, int line) {\n"
                "    if (index < 0 || (uintmax_t)index >= (uintmax_t)length) {\n"
                "        fprintf(stderr, \"HumC bounds error: index %lld out of bounds for array '%s' (length %zu) at %s:%d\\n\", (long long)index, array_name, length, file, line);\n"
                "        exit(EXIT_FAILURE);\n"
                "    }\n"
                "    return (size_t)index;\n"
                "}\n"
                "#define HUMC_ARRAY_AT(array, index) ((array)[humc_checked_index((intmax_t)(index), sizeof(array) / sizeof((array)[0]), #array, __FILE__, __LINE__)])\n\n");
        }
        (void)buffer_append_n(&final_output, body + insertion, body_length - insertion);
    } else {
        (void)buffer_append_n(&final_output, body, body_length);
    }
    if (!final_output.length || final_output.data[final_output.length - 1] != '\n') (void)buffer_append(&final_output, "\n");

    free(compiler.blocks);
    free(compiler.output.data);
    arena_destroy(&compiler.arena);
    free(compiler.error_message);
    free(normalized_source);
    return buffer_take(&final_output);
}

int humc_check(const char *source, char **error_message) {
    char *output = humc_compile(source, error_message);
    if (!output) return 0;
    free(output);
    return 1;
}

char *humc_format(const char *source) {
    if (!source) return NULL;
    StringBuffer output = {0};
    const char *cursor = source;
    while (*cursor) {
        const char *line_start = cursor;
        while (*cursor && *cursor != '\n') cursor++;
        size_t length = (size_t)(cursor - line_start);
        if (*cursor == '\n') cursor++;
        while (length && (line_start[length - 1] == ' ' || line_start[length - 1] == '\t' || line_start[length - 1] == '\r')) length--;
        if (!buffer_append_n(&output, line_start, length)) {
            free(output.data);
            return NULL;
        }
        (void)buffer_append_char(&output, '\n');
    }
    if (!output.length) (void)buffer_append_char(&output, '\n');
    while (output.length > 1 && output.data[output.length - 1] == '\n' && output.data[output.length - 2] == '\n') {
        output.data[--output.length] = '\0';
    }
    return buffer_take(&output);
}
