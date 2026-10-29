#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "../src/compiler.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

typedef struct {
    char *path;
    time_t modified;
    long modified_nanoseconds;
    off_t size;
} FileStamp;

static volatile sig_atomic_t interrupted = 0;

static void on_signal(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

static void usage(FILE *stream) {
    fprintf(stream,
        "\n"
        "Usage:\n"
        "  humc <file.humc> [-o output.c]\n"
        "  humc build [file.humc|dir] [-o output-dir]\n"
        "  humc check [file.humc|dir]\n"
        "  humc fmt [file.humc|dir]\n"
        "  humc watch [file.humc|dir] [-o output-dir]\n"
        "  humc run <file.humc> [program-args...]\n"
        "  humc init [dir]\n"
        "  humc version\n"
        "  humc help\n"
        "\n"
        "HumC translates indentation-based .humc files into ordinary C source.\n"
        "Use 'humc check' to validate syntax without writing .c files.\n"
        "Use 'humc run' to translate, compile with $CC (or cc), and execute.\n"
        "\n");
}

static void fail(const char *message) {
    fprintf(stderr, "Error: %s\n", message ? message : "unknown error");
}

static char *string_duplicate(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

static char *path_join(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool separator = left_len > 0 && left[left_len - 1] != '/';
    if (left_len > SIZE_MAX - right_len - (separator ? 2u : 1u)) return NULL;
    size_t total = left_len + right_len + (separator ? 2u : 1u);
    char *joined = (char *)malloc(total);
    if (!joined) return NULL;
    memcpy(joined, left, left_len);
    size_t position = left_len;
    if (separator) joined[position++] = '/';
    memcpy(joined + position, right, right_len);
    joined[position + right_len] = '\0';
    return joined;
}

static bool has_humc_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || strlen(dot) != 5) return false;
    return (dot[1] == 'h' || dot[1] == 'H') &&
           (dot[2] == 'u' || dot[2] == 'U') &&
           (dot[3] == 'm' || dot[3] == 'M') &&
           (dot[4] == 'c' || dot[4] == 'C');
}

static char *replace_extension(const char *path, const char *extension) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) dot = path + strlen(path);
    size_t stem_length = (size_t)(dot - path);
    size_t extension_length = strlen(extension);
    char *result = (char *)malloc(stem_length + extension_length + 1);
    if (!result) return NULL;
    memcpy(result, path, stem_length);
    memcpy(result + stem_length, extension, extension_length + 1);
    return result;
}

static bool list_push(StringList *list, const char *item) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        char **items = (char **)realloc(list->items, capacity * sizeof(*items));
        if (!items) return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count] = string_duplicate(item);
    if (!list->items[list->count]) return false;
    list->count++;
    return true;
}

static void list_free(StringList *list) {
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int compare_strings(const void *left, const void *right) {
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static void list_sort(StringList *list) {
    if (list->count > 1) qsort(list->items, list->count, sizeof(*list->items), compare_strings);
}

static bool should_skip_directory(const char *name) {
    return strcmp(name, ".git") == 0 || strcmp(name, ".hg") == 0 ||
           strcmp(name, ".svn") == 0 || strcmp(name, "node_modules") == 0 ||
           strcmp(name, "build") == 0 || strcmp(name, "dist") == 0 ||
           strcmp(name, ".humc-cache") == 0;
}

static bool collect_directory(const char *directory, StringList *files, char **error) {
    DIR *dir = opendir(directory);
    if (!dir) {
        if (error) {
            size_t needed = strlen(directory) + strlen(strerror(errno)) + 64;
            *error = (char *)malloc(needed);
            if (*error) snprintf(*error, needed, "cannot read directory '%s': %s", directory, strerror(errno));
        }
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (entry->d_name[0] == '.' && should_skip_directory(entry->d_name)) continue;
        if (should_skip_directory(entry->d_name)) continue;

        char *path = path_join(directory, entry->d_name);
        if (!path) {
            closedir(dir);
            if (error) *error = string_duplicate("out of memory while walking directory");
            return false;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            free(path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!collect_directory(path, files, error)) {
                free(path);
                closedir(dir);
                return false;
            }
        } else if (S_ISREG(st.st_mode) && has_humc_extension(path)) {
            if (!list_push(files, path)) {
                free(path);
                closedir(dir);
                if (error) *error = string_duplicate("out of memory while collecting source files");
                return false;
            }
        }
        free(path);
    }

    closedir(dir);
    return true;
}

static bool resolve_targets(const char *target, StringList *files, bool *is_directory, char **error) {
    struct stat st;
    if (stat(target, &st) != 0) {
        if (error) {
            size_t needed = strlen(target) + strlen(strerror(errno)) + 48;
            *error = (char *)malloc(needed);
            if (*error) snprintf(*error, needed, "path '%s' not found: %s", target, strerror(errno));
        }
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (is_directory) *is_directory = true;
        if (!collect_directory(target, files, error)) return false;
        list_sort(files);
        return true;
    }
    if (!S_ISREG(st.st_mode)) {
        if (error) *error = string_duplicate("target is not a regular file or directory");
        return false;
    }
    if (!has_humc_extension(target)) {
        if (error) *error = string_duplicate("source file must have a .humc extension");
        return false;
    }
    if (is_directory) *is_directory = false;
    if (!list_push(files, target)) {
        if (error) *error = string_duplicate("out of memory while collecting source file");
        return false;
    }
    return true;
}

static char *read_file(const char *path, char **error) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error) {
            size_t needed = strlen(path) + strlen(strerror(errno)) + 48;
            *error = (char *)malloc(needed);
            if (*error) snprintf(*error, needed, "cannot read '%s': %s", path, strerror(errno));
        }
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        if (error) *error = string_duplicate("cannot seek in source file");
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        if (error) *error = string_duplicate("cannot determine source file size");
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        if (error) *error = string_duplicate("out of memory while reading source file");
        return NULL;
    }
    size_t read_count = fread(data, 1, (size_t)size, file);
    if (read_count != (size_t)size && ferror(file)) {
        fclose(file);
        free(data);
        if (error) *error = string_duplicate("failed while reading source file");
        return NULL;
    }
    fclose(file);
    data[read_count] = '\0';
    return data;
}

static bool make_directories(const char *path) {
    char *copy = string_duplicate(path);
    if (!copy) return false;
    size_t length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/') copy[--length] = '\0';
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            *p = '/';
        }
    }
    bool okay = mkdir(copy, 0777) == 0 || errno == EEXIST;
    free(copy);
    return okay;
}

static bool ensure_parent_directory(const char *path) {
    char *copy = string_duplicate(path);
    if (!copy) return false;
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return true;
    }
    if (slash == copy) slash[1] = '\0';
    else *slash = '\0';
    bool okay = make_directories(copy);
    free(copy);
    return okay;
}

static bool write_file(const char *path, const char *data) {
    if (!ensure_parent_directory(path)) {
        fprintf(stderr, "Error: cannot create parent directory for '%s': %s\n", path, strerror(errno));
        return false;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Error: cannot write '%s': %s\n", path, strerror(errno));
        return false;
    }
    size_t length = strlen(data);
    bool okay = fwrite(data, 1, length, file) == length;
    if (fclose(file) != 0) okay = false;
    if (!okay) fprintf(stderr, "Error: failed while writing '%s'\n", path);
    return okay;
}

static char *default_output_path(const char *source) {
    return replace_extension(source, ".c");
}

static char *relative_to_root(const char *root, const char *path) {
    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/') length--;
    if (length == 1 && root[0] == '/') return string_duplicate(path[0] == '/' ? path + 1 : path);
    if (strncmp(root, path, length) == 0 && (path[length] == '/' || path[length] == '\0')) {
        const char *relative = path + length;
        while (*relative == '/') relative++;
        return string_duplicate(relative);
    }
    return string_duplicate(path);
}

static char *output_for_target(const char *source, const char *target_root, const char *output_root, bool target_is_directory) {
    if (!output_root) return default_output_path(source);
    if (!target_is_directory) return string_duplicate(output_root);
    char *relative = relative_to_root(target_root, source);
    if (!relative) return NULL;
    char *relative_c = replace_extension(relative, ".c");
    free(relative);
    if (!relative_c) return NULL;
    char *output = path_join(output_root, relative_c);
    free(relative_c);
    return output;
}

static bool compile_one(const char *source_path, const char *output_path, bool write_output, bool quiet) {
    char *io_error = NULL;
    char *source = read_file(source_path, &io_error);
    if (!source) {
        fail(io_error ? io_error : "failed to read source");
        free(io_error);
        return false;
    }

    char *compile_error = NULL;
    char *generated = humc_compile(source, &compile_error);
    free(source);
    if (!generated) {
        fprintf(stderr, "Error: %s: %s\n", source_path,
                compile_error ? compile_error : "compilation failed");
        free(compile_error);
        return false;
    }
    free(compile_error);

    if (write_output) {
        if (!write_file(output_path, generated)) {
            free(generated);
            return false;
        }
        if (!quiet) printf("%s\n", output_path);
    }
    free(generated);
    return true;
}

static int command_compile(const char *source_path, const char *output_path) {
    StringList files = {0};
    bool directory = false;
    char *error = NULL;
    if (!resolve_targets(source_path, &files, &directory, &error)) {
        fail(error ? error : "invalid source path");
        free(error);
        list_free(&files);
        return 1;
    }
    if (directory || files.count != 1) {
        fail("specify a single .humc file; use 'humc build dir' for directories");
        list_free(&files);
        return 1;
    }
    char *chosen_output = output_path ? string_duplicate(output_path) : default_output_path(files.items[0]);
    if (!chosen_output) {
        fail("out of memory");
        list_free(&files);
        return 1;
    }
    bool okay = compile_one(files.items[0], chosen_output, true, false);
    free(chosen_output);
    list_free(&files);
    return okay ? 0 : 1;
}

static int command_build(const char *target, const char *output_root) {
    StringList files = {0};
    bool target_is_directory = false;
    char *error = NULL;
    if (!resolve_targets(target, &files, &target_is_directory, &error)) {
        fail(error ? error : "invalid build target");
        free(error);
        list_free(&files);
        return 1;
    }
    if (!files.count) {
        printf("No HumC files found.\n");
        list_free(&files);
        return 0;
    }
    size_t passed = 0;
    for (size_t i = 0; i < files.count; i++) {
        char *out = output_for_target(files.items[i], target, output_root, target_is_directory);
        if (!out) {
            fail("out of memory while creating output path");
            list_free(&files);
            return 1;
        }
        if (compile_one(files.items[i], out, true, true)) {
            passed++;
            printf("Built %s -> %s\n", files.items[i], out);
        }
        free(out);
    }
    size_t total = files.count;
    printf("\nBuilt %zu of %zu file(s).\n", passed, total);
    list_free(&files);
    return passed == total ? 0 : 1;
}

static int command_check(const char *target) {
    StringList files = {0};
    bool is_directory = false;
    char *error = NULL;
    if (!resolve_targets(target, &files, &is_directory, &error)) {
        fail(error ? error : "invalid check target");
        free(error);
        list_free(&files);
        return 1;
    }
    (void)is_directory;
    if (!files.count) {
        printf("No HumC files found.\n");
        list_free(&files);
        return 0;
    }
    size_t passed = 0;
    for (size_t i = 0; i < files.count; i++) {
        printf("Checking %s\n", files.items[i]);
        if (compile_one(files.items[i], NULL, false, true)) {
            passed++;
            printf("  OK\n");
        } else {
            printf("  FAILED\n");
        }
    }
    size_t total = files.count;
    printf("\nChecked %zu file(s): %zu passed, %zu failed.\n", total, passed, total - passed);
    list_free(&files);
    return passed == total ? 0 : 1;
}

static char *format_source(const char *source) {
    return humc_format(source);
}

static int command_fmt(const char *target) {
    StringList files = {0};
    bool is_directory = false;
    char *error = NULL;
    if (!resolve_targets(target, &files, &is_directory, &error)) {
        fail(error ? error : "invalid format target");
        free(error);
        list_free(&files);
        return 1;
    }
    (void)is_directory;
    int status = 0;
    for (size_t i = 0; i < files.count; i++) {
        char *read_error = NULL;
        char *source = read_file(files.items[i], &read_error);
        if (!source) {
            fail(read_error ? read_error : "cannot read source");
            free(read_error);
            status = 1;
            continue;
        }
        char *formatted = format_source(source);
        if (!formatted) {
            fail("out of memory while formatting");
            free(source);
            status = 1;
            continue;
        }
        if (strcmp(source, formatted) == 0) {
            printf("Already formatted %s\n", files.items[i]);
        } else if (write_file(files.items[i], formatted)) {
            printf("Formatted %s\n", files.items[i]);
        } else {
            status = 1;
        }
        free(source);
        free(formatted);
    }
    list_free(&files);
    return status;
}

static char *base_without_extension(const char *path) {
    char *base = replace_extension(path, "");
    if (!base) return NULL;
    if (!strchr(base, '/')) {
        char *relative = path_join(".", base);
        free(base);
        return relative;
    }
    return base;
}

static int wait_for_process(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int run_command(const char *source_path, int program_argc, char **program_argv) {
    char *c_path = default_output_path(source_path);
    char *binary_path = base_without_extension(source_path);
    if (!c_path || !binary_path) {
        fail("out of memory");
        free(c_path);
        free(binary_path);
        return 1;
    }
    if (!compile_one(source_path, c_path, true, true)) {
        free(c_path);
        free(binary_path);
        return 1;
    }

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    pid_t child = fork();
    if (child == 0) {
        execlp(cc, cc, c_path, "-o", binary_path, (char *)NULL);
        fprintf(stderr, "Error: cannot execute C compiler '%s': %s\n", cc, strerror(errno));
        _exit(127);
    }
    if (child < 0) {
        fprintf(stderr, "Error: cannot start C compiler: %s\n", strerror(errno));
        free(c_path);
        free(binary_path);
        return 1;
    }
    int status = wait_for_process(child);
    if (status != 0) {
        free(c_path);
        free(binary_path);
        return status;
    }

    child = fork();
    if (child == 0) {
        char **args = (char **)calloc((size_t)program_argc + 2, sizeof(char *));
        if (!args) _exit(2);
        args[0] = binary_path;
        for (int i = 0; i < program_argc; i++) args[i + 1] = program_argv[i];
        args[program_argc + 1] = NULL;
        execv(binary_path, args);
        fprintf(stderr, "Error: cannot execute '%s': %s\n", binary_path, strerror(errno));
        _exit(127);
    }
    if (child < 0) {
        fprintf(stderr, "Error: cannot start program: %s\n", strerror(errno));
        free(c_path);
        free(binary_path);
        return 1;
    }
    status = wait_for_process(child);
    free(c_path);
    free(binary_path);
    return status;
}

static int command_init(const char *directory) {
    char *src_directory = path_join(directory, "src");
    char *main_file = src_directory ? path_join(src_directory, "main.humc") : NULL;
    char *makefile = path_join(directory, "Makefile");
    if (!src_directory || !main_file || !makefile) {
        fail("out of memory");
        free(src_directory);
        free(main_file);
        free(makefile);
        return 1;
    }
    if (!make_directories(src_directory)) {
        fprintf(stderr, "Error: cannot create '%s': %s\n", src_directory, strerror(errno));
        free(src_directory);
        free(main_file);
        free(makefile);
        return 1;
    }
    struct stat st;
    if (stat(main_file, &st) != 0) {
        const char *sample = "fn main\n    print \"Hello from HumC!\"\n    return 0\n";
        if (!write_file(main_file, sample)) {
            free(src_directory);
            free(main_file);
            free(makefile);
            return 1;
        }
    }
    if (stat(makefile, &st) != 0) {
        const char *makefile_contents =
            "HUMC ?= humc\n"
            "CC ?= cc\n"
            "\n"
            "all: app\n"
            "\n"
            "main.c: src/main.humc\n"
            "\t$(HUMC) src/main.humc -o main.c\n"
            "\n"
            "app: main.c\n"
            "\t$(CC) main.c -o app\n"
            "\n"
            "clean:\n"
            "\trm -f main.c app\n";
        if (!write_file(makefile, makefile_contents)) {
            free(src_directory);
            free(main_file);
            free(makefile);
            return 1;
        }
    }
    printf("HumC project initialized in %s\n", directory);
    printf("Next: cd %s && HumC src/main.humc && cc main.c -o app\n", directory);
    free(src_directory);
    free(main_file);
    free(makefile);
    return 0;
}

static bool same_file_stamp(const FileStamp *a, const FileStamp *b) {
    return strcmp(a->path, b->path) == 0 && a->modified == b->modified &&
           a->modified_nanoseconds == b->modified_nanoseconds && a->size == b->size;
}

static FileStamp *make_stamps(const StringList *files) {
    FileStamp *stamps = (FileStamp *)calloc(files->count ? files->count : 1, sizeof(*stamps));
    if (!stamps) return NULL;
    for (size_t i = 0; i < files->count; i++) {
        struct stat st;
        stamps[i].path = string_duplicate(files->items[i]);
        if (!stamps[i].path) {
            for (size_t j = 0; j < i; j++) free(stamps[j].path);
            free(stamps);
            return NULL;
        }
        if (stat(files->items[i], &st) == 0) {
            stamps[i].modified = st.st_mtime;
#if defined(__APPLE__)
            stamps[i].modified_nanoseconds = st.st_mtimespec.tv_nsec;
#else
            stamps[i].modified_nanoseconds = st.st_mtim.tv_nsec;
#endif
            stamps[i].size = st.st_size;
        }
    }
    return stamps;
}

static void free_stamps(FileStamp *stamps, size_t count) {
    if (stamps == NULL) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        free(stamps[i].path);
    }

    free(stamps);
}

static int command_watch(const char *target, const char *output_root) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("HumC watch mode\nWatching: %s\nPress Ctrl+C to stop.\n\n", target);

    StringList initial = {0};
    bool is_directory = false;
    char *error = NULL;
    if (!resolve_targets(target, &initial, &is_directory, &error)) {
        fail(error ? error : "invalid watch target");
        free(error);
        list_free(&initial);
        return 1;
    }
    list_sort(&initial);
    for (size_t i = 0; i < initial.count; i++) {
        char *out = output_for_target(initial.items[i], target, output_root, is_directory);
        if (!out || !compile_one(initial.items[i], out, true, true)) {
            free(out);
            list_free(&initial);
            return 1;
        }
        printf("Built %s\n", out);
        free(out);
    }

    FileStamp *previous = make_stamps(&initial);
    size_t previous_count = initial.count;
    if (!previous) {
        fail("out of memory while watching files");
        list_free(&initial);
        return 1;
    }
    list_free(&initial);

    while (!interrupted) {
        struct timespec delay = {0, 400000000L};
        (void)nanosleep(&delay, NULL);
        if (interrupted) break;

        StringList current = {0};
        error = NULL;
        if (!resolve_targets(target, &current, &is_directory, &error)) {
            fprintf(stderr, "Watch error: %s\n", error ? error : "cannot rescan target");
            free(error);
            continue;
        }
        list_sort(&current);
        FileStamp *now = make_stamps(&current);
        if (!now) {
            list_free(&current);
            continue;
        }

        bool changed = current.count != previous_count;
        if (!changed) {
            for (size_t i = 0; i < current.count; i++) {
                if (!same_file_stamp(&now[i], &previous[i])) {
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            printf("\nChange detected; rebuilding...\n");
            size_t passed = 0;
            for (size_t i = 0; i < current.count; i++) {
                char *out = output_for_target(current.items[i], target, output_root, is_directory);
                if (out && compile_one(current.items[i], out, true, true)) {
                    printf("Built %s\n", out);
                    passed++;
                }
                free(out);
            }
            printf("Build result: %zu/%zu file(s).\n", passed, current.count);
            free_stamps(previous, previous_count);
            previous = now;
            previous_count = current.count;
            now = NULL;
        }
        free_stamps(now, current.count);
        list_free(&current);
    }
    free_stamps(previous, previous_count);
    printf("\nStopped.\n");
    return 0;
}

static bool parse_output_option(int argc, char **argv, int start, const char **target, const char **output, bool allow_output) {
    *target = NULL;
    *output = NULL;
    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (!allow_output) {
                fprintf(stderr, "Error: -o is not supported for this command\n");
                return false;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: expected a path after %s\n", argv[i]);
                return false;
            }
            if (*output) {
                fprintf(stderr, "Error: output path specified more than once\n");
                return false;
            }
            *output = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            return false;
        } else if (!*target) {
            *target = argv[i];
        } else {
            fprintf(stderr, "Error: expected one file or directory\n");
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 1;
    }

    const char *first = argv[1];
    if (strcmp(first, "help") == 0 || strcmp(first, "--help") == 0 || strcmp(first, "-h") == 0) {
        if (argc > 2) {
            fail("help does not accept arguments");
            return 1;
        }
        usage(stdout);
        return 0;
    }
    if (strcmp(first, "version") == 0 || strcmp(first, "--version") == 0 || strcmp(first, "-v") == 0) {
        if (argc > 2) {
            fail("version does not accept arguments");
            return 1;
        }
        printf("%s\n", HUMC_VERSION);
        return 0;
    }
    if (strcmp(first, "init") == 0) {
        if (argc > 3) {
            fail("init accepts at most one directory");
            return 1;
        }
        return command_init(argc == 3 ? argv[2] : ".");
    }
    if (strcmp(first, "check") == 0 || strcmp(first, "fmt") == 0) {
        const char *target = NULL;
        const char *output = NULL;
        if (!parse_output_option(argc, argv, 2, &target, &output, false)) return 1;
        (void)output;
        if (!target) target = ".";
        return strcmp(first, "check") == 0 ? command_check(target) : command_fmt(target);
    }
    if (strcmp(first, "build") == 0) {
        const char *target = NULL;
        const char *output = NULL;
        if (!parse_output_option(argc, argv, 2, &target, &output, true)) return 1;
        if (!target) target = ".";
        return command_build(target, output);
    }
    if (strcmp(first, "watch") == 0) {
        const char *target = NULL;
        const char *output = NULL;
        if (!parse_output_option(argc, argv, 2, &target, &output, true)) return 1;
        if (!target) target = ".";
        return command_watch(target, output);
    }
    if (strcmp(first, "run") == 0) {
        if (argc < 3) {
            fail("run requires a .humc file");
            return 1;
        }
        if (!has_humc_extension(argv[2])) {
            fail("source file must have a .humc extension");
            return 1;
        }
        return run_command(argv[2], argc - 3, argv + 3);
    }

    const char *output = NULL;
    const char *source = first;
    if (!has_humc_extension(source)) {
        usage(stderr);
        return 1;
    }
    if (argc > 2) {
        if (argc == 4 && (strcmp(argv[2], "-o") == 0 || strcmp(argv[2], "--output") == 0)) output = argv[3];
        else {
            fail("expected optional -o output.c");
            return 1;
        }
    }
    return command_compile(source, output);
}
