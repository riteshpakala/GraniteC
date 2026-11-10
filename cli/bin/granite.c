/* The granite command: GraniteRust's granite-cli, its usage, outputs and exit codes byte for byte.
 *
 *   granite agents add <name | git URL[@revision] | folder> --shelf <dir>
 *   granite agents update <name> --shelf <dir>
 *   granite agents rm <name> --shelf <dir>
 *   granite agents list --shelf <dir> [--json]
 *   granite describe <runner>
 *   granite run <runner> <perform.json | -> [--directory <dir>] [--ask <text>] [--word <word>]
 *   granite check --schemas <dir>… --schema <file or $id> <instance.json | ->
 *
 * PIN: NO DEFAULT SHELF. A shelf is --shelf or GRANITE_SHELF, never a host's own folder by accident.
 * PIN: `run` IS A HOST WITH NO MODEL. It prints one line, {"output": …} or {"failed": …, "refused": …},
 * with the process's status, and exits 0 answered, 3 refused, 1 failed, 2 when the runner broke the
 * wire or died. SIGINT or SIGTERM during a run cancels the runner and reaps it, then ends this
 * process by the same signal (Rust's leaves the runner). */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "granite_host/files.h"
#include "granite_host/install.h"
#include "granite_host/process.h"
#include "granite_host/shelf.h"
#include "granite_jsonschema/validate.h"
#include "granitec/granitec.h"

static const char USAGE[] =
    "usage:\n"
    "  granite agents add <name | git URL[@revision] | folder> --shelf <dir>\n"
    "  granite agents update <name> --shelf <dir>\n"
    "  granite agents rm <name> --shelf <dir>\n"
    "  granite agents list --shelf <dir> [--json]\n"
    "  granite describe <runner>\n"
    "  granite run <runner> <perform.json | -> [--directory <dir>] [--ask <text>] [--word <word>]\n"
    "  granite check --schemas <dir>... --schema <file or $id> <instance.json | ->";

typedef struct option {
    char *name;
    char *value;
} option;

typedef struct arguments {
    char **words;
    size_t word_count;
    option *options;
    size_t option_count;
    bool json;
} arguments;

/* The last value of `--name`, or NULL. */
static const char *one(const arguments *a, const char *name) {
    const char *found = NULL;
    for (size_t i = 0; i < a->option_count; i++)
        if (strcmp(a->options[i].name, name) == 0) found = a->options[i].value;
    return found;
}

/* A failure: said on stderr after "granite: ", with its exit code. */
static int fail(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static int fail(int code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("granite: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    return code;
}

/* Rust's io::Error Display: the system's sentence and its number. */
static char *os_error(int number) {
    return granite_strdupf("%s (os error %d)", strerror(number), number);
}

static const char *shelf_of(const arguments *a) {
    const char *shelf = one(a, "shelf");
    if (!shelf) shelf = getenv("GRANITE_SHELF");
    return shelf;
}

static void print_pretty(const json_object *value) {
    char *text = granite_json_write_pretty(value, NULL);
    printf("%s\n", text);
    free(text);
}

static void progress(void *arg, const char *stage, const char *line) {
    if (line) fprintf(stderr, "  %s\n", line);
    else fprintf(stderr, "%s\n", stage);
}

/* --- agents -------------------------------------------------------------------------------- */

static int add(const arguments *a, const char *said_source) {
    const char *shelf = shelf_of(a);
    if (!shelf) return fail(2, "say which shelf: --shelf <dir> or GRANITE_SHELF");
    char here[PATH_MAX];
    if (!getcwd(here, sizeof here)) strcpy(here, "/");
    granite_source source = granite_source_parse(said_source, here);
    granite_installation *installed = NULL;
    granite_install_error *error = granite_install(shelf, &source, progress, NULL, &installed);
    granite_source_free(&source);
    if (error) {
        char *text = granite_install_text(error);
        int code = fail(1, "%s", text);
        free(text);
        granite_install_error_free(error);
        return code;
    }
    json_object *json = granite_installation_json(installed);
    print_pretty(json);
    granite_json_put(json);
    granite_installation_free(installed);
    return 0;
}

static int update(const arguments *a, const char *name) {
    const char *shelf = shelf_of(a);
    if (!shelf) return fail(2, "say which shelf: --shelf <dir> or GRANITE_SHELF");
    granite_installation *installed = NULL;
    granite_install_error *error = granite_install_update(shelf, name, progress, NULL, &installed);
    if (error) {
        char *text = granite_install_text(error);
        int code = fail(1, "%s", text);
        free(text);
        granite_install_error_free(error);
        return code;
    }
    fprintf(stderr, "updated %s at %s\n", installed->name, installed->revision ? installed->revision : "its folder");
    granite_installation_free(installed);
    return 0;
}

static int remove_installed(const arguments *a, const char *name) {
    const char *shelf = shelf_of(a);
    if (!shelf) return fail(2, "say which shelf: --shelf <dir> or GRANITE_SHELF");
    granite_installation *removed = NULL;
    granite_install_error *error = granite_install_remove(shelf, name, &removed);
    if (error) {
        char *text = granite_install_text(error);
        int code = fail(1, "%s", text);
        free(text);
        granite_install_error_free(error);
        return code;
    }
    fprintf(stderr, "removed %s\n", removed->name);
    granite_installation_free(removed);
    return 0;
}

/* Rust's {:<12}: padded to twelve characters (not bytes). */
static void print_padded(const char *word) {
    size_t characters = 0;
    for (const unsigned char *c = (const unsigned char *)word; *c; c++)
        if ((*c & 0xC0) != 0x80) characters++;
    fputs(word, stdout);
    for (size_t i = characters; i < 12; i++) fputc(' ', stdout);
}

static int list(const arguments *a) {
    const char *shelf = shelf_of(a);
    if (!shelf) return fail(2, "say which shelf: --shelf <dir> or GRANITE_SHELF");
    granite_shelf_read read = granite_shelf_read_all(shelf);
    if (a->json) {
        json_object *all = json_object_new_array();
        for (size_t i = 0; i < read.count; i++) json_object_array_add(all, granite_installation_json(read.installations[i]));
        print_pretty(all);
        granite_json_put(all);
    } else {
        for (size_t i = 0; i < read.count; i++) {
            const granite_installation *installation = read.installations[i];
            char at[8];
            if (installation->revision) snprintf(at, sizeof at, "%.7s", installation->revision);
            else snprintf(at, sizeof at, "folder");
            for (size_t e = 0; e < json_object_array_length(installation->entries); e++) {
                json_object *entry = json_object_array_get_idx(installation->entries, e);
                const char *word = granite_json_get_string(entry, "word"), *summary = granite_json_get_string(entry, "summary");
                print_padded(word ? word : "");
                printf(" %s@%s · %s\n", installation->name, at, summary ? summary : "");
            }
        }
        for (size_t i = 0; i < read.problem_count; i++) fprintf(stderr, "%s\n", read.problems[i]);
    }
    granite_shelf_read_free(&read);
    return 0;
}

/* --- describe and run ---------------------------------------------------------------------- */

static int describe(const char *runner) {
    granite_process *process = granite_process_new(runner, NULL);
    granite_remote_error *error = NULL;
    json_object *entries = granite_process_describe(process, &error);
    granite_process_free(process);
    if (!entries) {
        char *text = granite_remote_text(error);
        int code = fail(2, "%s", text);
        free(text);
        granite_remote_free(error);
        return code;
    }
    print_pretty(entries);
    granite_json_put(entries);
    return 0;
}

/* `-` (stdin) or a file's bytes; NULL with Rust's sentence for why. */
static char *read_input(const char *said, size_t *len, char **error) {
    if (strcmp(said, "-") == 0) {
        granite_buf bytes = {0};
        char chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) granite_buf_append(&bytes, chunk, n);
        *len = bytes.len;
        return granite_buf_take(&bytes);
    }
    char *bytes = granite_file_read(said, len);
    if (!bytes) {
        char *why = os_error(errno);
        *error = granite_strdupf("%s: %s", said, why);
        free(why);
    }
    return bytes;
}

static void note(void *arg, const char *line) {
    fprintf(stderr, "note: %s\n", line);
}

/* SIGINT and SIGTERM during a run: the runner is cancelled and reaped, then this process ends by the
 * same signal. The handler only records it; the watcher does the rest. */
static volatile sig_atomic_t interrupted = 0;
static int interrupt_pipe[2] = {-1, -1};

static void on_interrupt(int number) {
    int saved = errno;
    interrupted = number;
    char byte = (char)number;
    if (write(interrupt_pipe[1], &byte, 1) < 0) {
    }
    errno = saved;
}

static void watch_interrupt(void *pointer) {
    char byte;
    while (read(interrupt_pipe[0], &byte, 1) < 0 && errno == EINTR) {
    }
    granite_cancel_set(pointer);
}

static int run(const arguments *a, const char *runner, const char *perform) {
    size_t len = 0;
    char *error = NULL;
    char *bytes = read_input(perform, &len, &error);
    if (!bytes) {
        int code = fail(2, "%s", error);
        free(error);
        return code;
    }
    json_object *said = NULL;
    if (!granite_json_read(bytes, len, &said, &error)) {
        int code = fail(2, "%s: %s", perform, error);
        free(error);
        free(bytes);
        return code;
    }
    free(bytes);
    json_object *call = granite_json_has(said, "perform") ? granite_json_get(said, "perform") : said;
    const char *word = one(a, "word");
    if (!word) word = granite_json_get_string(call, "word");
    if (!word) {
        granite_json_put(said);
        return fail(2, "say which agent: a perform's `word`, or --word");
    }
    json_object *input = granite_json_has(call, "input") ? granite_json_get(call, "input") : NULL;
    char *input_bytes = granite_json_has(call, "input") ? granite_json_write(input, NULL) : granite_strdup("{}");
    const char *directory = one(a, "directory");
    if (!directory) directory = granite_json_get_string(call, "directory");
    char *absolute = NULL;
    if (directory) {
        char here[PATH_MAX];
        absolute = directory[0] == '/' || !getcwd(here, sizeof here) ? granite_strdup(directory)
                                                                     : granite_strdupf("%s/%s", here, directory);
    }
    const char *ask = one(a, "ask");
    if (!ask) ask = granite_json_get_string(call, "ask");
    granite_host *host = granite_host_new(absolute, ask, NULL, note, NULL, NULL);

    granite_cancel *cancel = granite_cancel_new();
    if (pipe(interrupt_pipe) == 0) {
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_handler = on_interrupt;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        granite_thread_spawn("granite.interrupt", 0, watch_interrupt, granite_retain(cancel));
    }

    granite_process *process = granite_process_new(runner, NULL);
    char *output = NULL;
    size_t output_len = 0;
    granite_process_cost cost = {0};
    granite_remote_error *failed =
        granite_process_perform(process, word, input_bytes, strlen(input_bytes), host, cancel, &output, &output_len, &cost);
    if (cost.status) {
        char *sentence = granite_cost_sentence(&cost);
        fprintf(stderr, "%s\n", sentence);
        free(sentence);
    }
    if (interrupted) {
        /* Cancelled and reaped: end as the signal would have ended us. */
        signal(interrupted, SIG_DFL);
        raise(interrupted);
    }

    granite_wire *line = NULL;
    int code;
    if (!failed) {
        json_object *answer = NULL;
        granite_json_read(output, output_len, &answer, NULL);
        line = granite_wire_output(answer);
        code = 0;
    } else if (failed->type == GRANITE_REMOTE_REFUSED) {
        granite_error *refused = granite_refuse_said(failed->refusal->kind, failed->message,
                                                     (const char *const *)failed->refusal->reasons,
                                                     failed->refusal->reason_count);
        line = granite_wire_failed(refused);
        granite_error_free(refused);
        code = 3;
    } else if (failed->type == GRANITE_REMOTE_FAULT || failed->type == GRANITE_REMOTE_FAILED) {
        granite_error *fault = granite_fault("%s", failed->message);
        line = granite_wire_failed(fault);
        granite_error_free(fault);
        code = 1;
    } else {
        char *text = granite_remote_text(failed);
        granite_error *other = granite_fault("%s", text);
        line = granite_wire_failed(other);
        granite_error_free(other);
        free(text);
        code = 2;
    }
    char *written = granite_wire_line(line, NULL);
    json_object *printed = NULL;
    granite_json_read(written, strlen(written), &printed, NULL);
    granite_json_set_string(printed, "status", cost.status ? cost.status : "");
    char *sorted = granite_json_write(printed, NULL);
    printf("%s\n", sorted);
    free(sorted);
    free(written);
    granite_json_put(printed);
    granite_wire_free(line);
    granite_remote_free(failed);
    free(output);
    granite_cost_free(&cost);
    granite_process_free(process);
    granite_release(host);
    granite_release(cancel);
    free(absolute);
    free(input_bytes);
    granite_json_put(said);
    return code;
}

/* --- check --------------------------------------------------------------------------------- */

static int check(const arguments *a, const char *instance) {
    const char *name = one(a, "schema");
    if (!name) return fail(2, "say which schema: --schema <file or $id>");
    granite_schemas *schemas = granite_schemas_new();
    for (size_t i = 0; i < a->option_count; i++) {
        if (strcmp(a->options[i].name, "schemas") != 0) continue;
        char *error = NULL;
        if (!granite_schemas_add_folder(schemas, a->options[i].value, &error)) {
            /* Rust says the folder and its io error. */
            char *why = os_error(errno);
            int code = fail(2, "%s: %s", a->options[i].value, why);
            free(why);
            free(error);
            granite_schemas_free(schemas);
            return code;
        }
    }
    json_object *owned = NULL;
    const json_object *schema = granite_schemas_find(schemas, name);
    if (!schema) {
        size_t len = 0;
        char *bytes = granite_file_read(name, &len);
        char *error = NULL;
        if (!bytes) {
            char *why = os_error(errno);
            int code = fail(2, "%s: %s", name, why);
            free(why);
            granite_schemas_free(schemas);
            return code;
        }
        if (!granite_json_read(bytes, len, &owned, &error)) {
            int code = fail(2, "%s: %s", name, error);
            free(error);
            free(bytes);
            granite_schemas_free(schemas);
            return code;
        }
        free(bytes);
        schema = owned;
    }
    size_t len = 0;
    char *error = NULL;
    char *bytes = read_input(instance, &len, &error);
    json_object *value = NULL;
    int code = 0;
    if (!bytes) {
        code = fail(2, "%s", error);
    } else if (!granite_json_read(bytes, len, &value, &error)) {
        code = fail(2, "%s: %s", instance, error);
    } else {
        size_t count = 0;
        char **problems = granite_schema_validate(schemas, schema, value, &count);
        if (!count) fprintf(stderr, "valid: %s against %s\n", instance, name);
        for (size_t i = 0; i < count; i++) {
            fprintf(stderr, "%s\n", problems[i]);
            free(problems[i]);
        }
        free(problems);
        if (count) code = fail(1, "%s does not keep %s: %zu problems", instance, name, count);
    }
    free(error);
    free(bytes);
    granite_json_put(value);
    granite_json_put(owned);
    granite_schemas_free(schemas);
    return code;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    arguments a = {0};
    a.words = granite_calloc((size_t)argc + 1, sizeof *a.words);
    a.options = granite_calloc((size_t)argc + 1, sizeof *a.options);
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        if (strncmp(argument, "--", 2) == 0) {
            const char *flag = argument + 2;
            if (strcmp(flag, "json") == 0 || strcmp(flag, "yes") == 0) {
                if (strcmp(flag, "json") == 0) a.json = true;
                continue;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "--%s needs a value\n%s\n", flag, USAGE);
                return 2;
            }
            a.options[a.option_count++] = (option){(char *)flag, argv[++i]};
        } else {
            a.words[a.word_count++] = (char *)argument;
        }
    }
    char **w = a.words;
    size_t n = a.word_count;
    int code;
    if (n == 3 && strcmp(w[0], "agents") == 0 && strcmp(w[1], "add") == 0) code = add(&a, w[2]);
    else if (n == 3 && strcmp(w[0], "agents") == 0 && strcmp(w[1], "update") == 0) code = update(&a, w[2]);
    else if (n == 3 && strcmp(w[0], "agents") == 0 && strcmp(w[1], "rm") == 0) code = remove_installed(&a, w[2]);
    else if ((n == 2 && strcmp(w[0], "agents") == 0 && strcmp(w[1], "list") == 0) || (n == 1 && strcmp(w[0], "agents") == 0))
        code = list(&a);
    else if (n == 2 && strcmp(w[0], "describe") == 0) code = describe(w[1]);
    else if (n == 3 && strcmp(w[0], "run") == 0) code = run(&a, w[1], w[2]);
    else if (n == 2 && strcmp(w[0], "check") == 0) code = check(&a, w[1]);
    else code = fail(2, "%s", USAGE);
    free(a.words);
    free(a.options);
    fflush(stdout);
    return code;
}
