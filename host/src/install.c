/* A C agent repository made a runner on this machine. See granite_host/install.h. */
#include "granite_host/install.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "granite_host/files.h"
#include "granite_host/process.h"
#include "granite_host/sha256.h"
#include "granite_host/shelf.h"
#include "granitec/buf.h"

char *granite_install_text(const granite_install_error *e) {
    switch (e->type) {
    case GRANITE_INSTALL_TOOLCHAIN: return granite_strdupf("a C toolchain (make, cc, pkg-config) is needed to build an agent: %s", e->reason);
    case GRANITE_INSTALL_FETCH: return granite_strdupf("could not fetch %s: %s", e->source, e->reason);
    case GRANITE_INSTALL_NOT_A_PACKAGE: return granite_strdupf("%s is not a C agent repository: %s", e->source, e->reason);
    case GRANITE_INSTALL_NOT_GRANITE: return granite_strdupf("%s does not build against GraniteC: %s", e->name, e->reason);
    case GRANITE_INSTALL_BUILD: return granite_strdupf("%s did not build (the whole log is at %s):\n%s", e->name, e->path, e->log);
    case GRANITE_INSTALL_NO_AGENT: return granite_strdupf("%s holds no agent: its runner describes none", e->name);
    case GRANITE_INSTALL_CLASH: return granite_strdupf("an agent called `%s` is already installed, from %s", e->word, e->installed);
    case GRANITE_INSTALL_NOT_INSTALLED: return granite_strdupf("no agent installed here is called %s", e->name);
    }
    return granite_strdup("");
}

void granite_install_error_free(granite_install_error *e) {
    if (!e) return;
    free(e->source);
    free(e->reason);
    free(e->name);
    free(e->log);
    free(e->path);
    free(e->word);
    free(e->installed);
    free(e);
}

static granite_install_error *failure(granite_install_type type) {
    granite_install_error *e = granite_calloc(1, sizeof *e);
    e->type = type;
    return e;
}

static granite_install_error *fetch_failed(const char *source, char *reason) {
    granite_install_error *e = failure(GRANITE_INSTALL_FETCH);
    e->source = granite_strdup(source);
    e->reason = reason;
    return e;
}

static granite_install_error *not_a_package(const char *source, char *reason) {
    granite_install_error *e = failure(GRANITE_INSTALL_NOT_A_PACKAGE);
    e->source = granite_strdup(source);
    e->reason = reason;
    return e;
}

/* The last `lines` lines of `text`, trimmed, as Rust's tail(). */
static char *tail(const char *text, int lines) {
    size_t len = strlen(text);
    while (len && text[len - 1] == '\n') len--;
    size_t start = len;
    int seen = 0;
    while (start > 0) {
        if (text[start - 1] == '\n' && ++seen == lines) break;
        start--;
    }
    size_t end = len;
    while (start < end && (text[start] == ' ' || text[start] == '\n' || text[start] == '\t')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\n' || text[end - 1] == '\t')) end--;
    return granite_strdupf("%.*s", (int)(end - start), text + start);
}

static void stage(granite_progress progress, void *arg, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void stage(granite_progress progress, void *arg, const char *fmt, ...) {
    if (!progress) return;
    granite_buf said = {0};
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&said, fmt, args);
    va_end(args);
    progress(arg, granite_buf_str(&said), NULL);
    granite_buf_free(&said);
}

/* The repository's commit, when it is one. */
static char *head_of(const char *folder) {
    const char *arguments[] = {"git", "rev-parse", "HEAD", NULL};
    granite_command_said said = granite_command_run(arguments, folder, NULL, NULL);
    char *revision = NULL;
    if (said.status == 0) {
        size_t n = strlen(said.stdout_text);
        while (n && (said.stdout_text[n - 1] == '\n' || said.stdout_text[n - 1] == ' ')) n--;
        if (n) revision = granite_strdupf("%.*s", (int)n, said.stdout_text);
    }
    granite_command_said_free(&said);
    return revision;
}

/* What `pkg-config` says of granitec: `--modversion`, or `--variable=commit`. NULL when it cannot. */
static char *pkg_config(const char *ask) {
    const char *pkg = getenv("PKG_CONFIG");
    const char *arguments[] = {pkg && *pkg ? pkg : "pkg-config", ask, "granitec", NULL};
    granite_command_said said = granite_command_run(arguments, NULL, NULL, NULL);
    char *answer = NULL;
    if (said.status == 0) {
        size_t n = strlen(said.stdout_text);
        while (n && (said.stdout_text[n - 1] == '\n' || said.stdout_text[n - 1] == ' ')) n--;
        answer = granite_strdupf("%.*s", (int)n, said.stdout_text);
    }
    granite_command_said_free(&said);
    return answer;
}

/* "1.2.3" against "1.10": the numbers in order, a missing one a zero. */
static int compare_versions(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9'))) break;
    }
    return 0;
}

static bool folder_name(const char *name) {
    if (!*name || name[0] == '.' || name[0] == '-') return false;
    for (const char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' ||
              *c == '.'))
            return false;
    return true;
}

typedef struct manifest {
    char *name;
    char *runner;
    char *granite;
} manifest;

static void manifest_free(manifest *m) {
    free(m->name);
    free(m->runner);
    free(m->granite);
}

/* granite-agent.json, read; NULL, or why the repository is not one. */
static char *read_manifest(const char *package, manifest *m) {
    memset(m, 0, sizeof *m);
    char *path = granite_path_join(package, GRANITE_AGENT_MANIFEST);
    size_t len = 0;
    char *bytes = granite_file_read(path, &len);
    free(path);
    if (!bytes) {
        char *cargo = granite_path_join(package, "Cargo.toml");
        bool rust = granite_exists(cargo);
        free(cargo);
        return granite_strdupf("it has no %s%s", GRANITE_AGENT_MANIFEST,
                               rust ? " (it has a Cargo.toml: GraniteRust's granite installs Rust agents)" : "");
    }
    json_object *value = NULL;
    char *why = NULL;
    if (!granite_json_read(bytes, len, &value, &why)) {
        char *reason = granite_strdupf("its %s does not read: %s", GRANITE_AGENT_MANIFEST, why);
        free(why);
        free(bytes);
        return reason;
    }
    free(bytes);
    const char *name = granite_json_get_string(value, "name");
    const char *runner = granite_json_get_string(value, "runner");
    const char *granite = granite_json_get_string(value, "granite");
    char *reason = NULL;
    if (!name) reason = granite_strdupf("its %s names no `name`", GRANITE_AGENT_MANIFEST);
    else if (!folder_name(name)) reason = granite_strdupf("`name` is not a folder name: %s", name);
    else if (runner && !folder_name(runner)) reason = granite_strdupf("`runner` is not a make target: %s", runner);
    if (!reason) {
        m->name = granite_strdup(name);
        m->runner = granite_strdup(runner ? runner : "agent");
        m->granite = granite_strdup(granite);
    }
    granite_json_put(value);
    return reason;
}

typedef struct building {
    granite_progress progress;
    void *arg;
    char *stage;
} building;

static void build_line(void *pointer, const char *text) {
    building *b = pointer;
    if (b->progress) b->progress(b->arg, b->stage, text);
}

static bool copy_file(const char *from, const char *to, mode_t mode) {
    size_t len = 0;
    char *bytes = granite_file_read(from, &len);
    if (!bytes) return false;
    bool ok = granite_file_write(to, bytes, len) && chmod(to, mode) == 0;
    free(bytes);
    return ok;
}

granite_install_error *granite_install(const char *folder, const granite_source *source, granite_progress progress,
                                       void *arg, granite_installation **installed) {
    if (installed) *installed = NULL;
    char *said_source = granite_source_text(source);
    char *builds = granite_path_join(folder, ".builds");
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    char *work = granite_strdupf("%s/%llx", builds, (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec);
    granite_install_error *error = NULL;
    char *package = NULL, *revision = NULL, *built = NULL, *granite = NULL;
    manifest m = {0};
    json_object *entries = NULL;
    granite_installation *record = NULL;
    if (!granite_mkdirs(work)) {
        error = fetch_failed(said_source, granite_strdupf("could not make %s", work));
        goto done;
    }

    /* 1. Fetch. */
    if (source->url) {
        stage(progress, arg, "fetching %s", source->url);
        const char *url = source->url;
        size_t end = strlen(url);
        while (end && url[end - 1] == '/') end--;
        size_t start = end;
        while (start && url[start - 1] != '/' && url[start - 1] != ':') start--;
        char *name = granite_strdupf("%.*s", (int)(end - start), url + start);
        size_t n = strlen(name);
        if (n >= 4 && strcmp(name + n - 4, ".git") == 0) name[n - 4] = '\0';
        package = granite_path_join(work, *name ? name : "package");
        free(name);
        const char *clone[] = {"git", "clone", "--quiet", url, package, NULL};
        granite_command_said said = granite_command_run(clone, work, NULL, NULL);
        if (said.status != 0) {
            error = fetch_failed(url, tail(said.output, 4));
            granite_command_said_free(&said);
            goto done;
        }
        granite_command_said_free(&said);
        if (source->revision) {
            const char *checkout[] = {"git", "checkout", "--quiet", source->revision, NULL};
            said = granite_command_run(checkout, package, NULL, NULL);
            if (said.status != 0) {
                char *wanted = granite_strdupf("%s@%s", url, source->revision);
                error = fetch_failed(wanted, tail(said.output, 4));
                free(wanted);
                granite_command_said_free(&said);
                goto done;
            }
            granite_command_said_free(&said);
        }
    } else {
        package = granite_strdup(source->path);
        if (!granite_is_directory(package)) {
            error = not_a_package(said_source, granite_strdup("it is not a folder"));
            goto done;
        }
    }
    revision = head_of(package);

    /* 2. Read the repository. */
    stage(progress, arg, "reading the package");
    char *reason = read_manifest(package, &m);
    if (reason) {
        error = not_a_package(said_source, reason);
        goto done;
    }
    char *version = pkg_config("--modversion");
    if (!version) {
        error = failure(GRANITE_INSTALL_TOOLCHAIN);
        error->reason = granite_strdup("pkg-config cannot find granitec: install GraniteC (make install) or set PKG_CONFIG_PATH");
        goto done;
    }
    if (m.granite && compare_versions(version, m.granite) < 0) {
        error = failure(GRANITE_INSTALL_NOT_GRANITE);
        error->name = granite_strdup(m.name);
        error->reason = granite_strdupf("it needs GraniteC %s, and pkg-config finds %s", m.granite, version);
        free(version);
        goto done;
    }
    char *commit = pkg_config("--variable=commit");
    granite = commit && *commit && strcmp(commit, "unknown") != 0 ? granite_strdupf("%s@%s", version, commit) : granite_strdup(version);
    free(commit);
    free(version);

    /* 3. Build: the repository's own runner, into this install's work folder. */
    stage(progress, arg, "building %s", m.name);
    char *out = granite_path_join(work, "build");
    char *o = granite_strdupf("O=%s", out);
    building b = {progress, arg, granite_strdupf("building %s", m.name)};
    const char *make_program = getenv("GRANITE_MAKE");
    const char *make[] = {make_program && *make_program ? make_program : "make", o, m.runner, NULL};
    granite_command_said said = granite_command_run(make, package, build_line, &b);
    free(b.stage);
    free(o);
    if (said.status == 127) {
        error = failure(GRANITE_INSTALL_TOOLCHAIN);
        error->reason = granite_strdupf("`%s` was not found", make[0]);
    } else if (said.status != 0) {
        char *log = granite_strdupf("%s/%s.log", builds, m.name);
        granite_file_write(log, said.output, strlen(said.output));
        error = failure(GRANITE_INSTALL_BUILD);
        error->name = granite_strdup(m.name);
        error->log = tail(said.output, 12);
        error->path = log;
    }
    granite_command_said_free(&said);
    built = granite_path_join(out, m.runner);
    free(out);
    if (error) goto done;

    /* 4. Ask it what it carries, and check no other installation carries the same. */
    stage(progress, arg, "asking %s what it carries", m.name);
    granite_process *process = granite_process_new(built, NULL);
    granite_remote_error *described = NULL;
    entries = granite_process_describe(process, &described);
    granite_process_free(process);
    if (!entries || json_object_array_length(entries) == 0) {
        error = failure(GRANITE_INSTALL_NO_AGENT);
        if (described) {
            char *text = granite_remote_text(described);
            error->name = granite_strdupf("%s (%s)", m.name, text);
            free(text);
        } else {
            error->name = granite_strdup(m.name);
        }
        granite_remote_free(described);
        goto done;
    }
    granite_shelf_read shelf = granite_shelf_read_all(folder);
    for (size_t i = 0; i < shelf.count && !error; i++) {
        granite_installation *other = shelf.installations[i];
        if (strcmp(other->name, m.name) == 0) continue;
        for (size_t e = 0; e < json_object_array_length(entries) && !error; e++) {
            const char *word = granite_json_get_string(json_object_array_get_idx(entries, e), "word");
            for (size_t o2 = 0; word && o2 < json_object_array_length(other->entries); o2++) {
                const char *theirs = granite_json_get_string(json_object_array_get_idx(other->entries, o2), "word");
                if (theirs && strcmp(theirs, word) == 0) {
                    error = failure(GRANITE_INSTALL_CLASH);
                    error->word = granite_strdup(word);
                    error->installed = granite_source_text(&other->source);
                    break;
                }
            }
        }
    }
    granite_shelf_read_free(&shelf);
    if (error) goto done;

    /* 5. Install, whole or not at all. */
    stage(progress, arg, "installing %s", m.name);
    char *staged_name = granite_strdupf(".%s.new", m.name);
    char *staged = granite_path_join(folder, staged_name);
    free(staged_name);
    granite_remove_tree(staged);
    char *executable = granite_path_join(staged, "agent");
    record = granite_calloc(1, sizeof *record);
    char sha[65], built_at[21];
    bool ok = granite_mkdirs(staged) && copy_file(built, executable, 0755) &&
              granite_stamp_of(executable, &record->size, &record->modified) && granite_sha256_file(executable, sha);
    if (ok) {
        record->format = granite_strdup(GRANITE_INSTALLATION_FORMAT);
        record->name = granite_strdup(m.name);
        record->source.path = granite_strdup(source->path);
        record->source.url = granite_strdup(source->url);
        record->source.revision = granite_strdup(source->revision);
        record->revision = revision;
        revision = NULL;
        record->granite = granite;
        granite = NULL;
        record->executable = granite_strdup("agent");
        record->sha256 = granite_strdup(sha);
        record->has_modified = true;
        granite_now_iso8601(built_at);
        record->built_at = granite_strdup(built_at);
        record->entries = entries;
        entries = NULL;
        json_object *json = granite_installation_json(record);
        char *text = granite_json_write_pretty(json, NULL);
        granite_json_put(json);
        char *path = granite_path_join(staged, GRANITE_INSTALLATION_FILE);
        ok = granite_file_write(path, text, strlen(text));
        free(path);
        free(text);
    }
    char *destination = granite_path_join(folder, m.name);
    if (ok) {
        granite_remove_tree(destination);
        ok = rename(staged, destination) == 0;
    }
    if (!ok) {
        granite_remove_tree(staged);
        error = fetch_failed(said_source, granite_strdup("could not install: the shelf could not be written"));
        granite_installation_free(record);
        record = NULL;
    }
    free(destination);
    free(executable);
    free(staged);
    if (error) goto done;
    granite_buf words = {0};
    for (size_t e = 0; e < json_object_array_length(record->entries); e++)
        granite_buf_appendf(&words, "%s%s", e ? ", " : "",
                            granite_json_get_string(json_object_array_get_idx(record->entries, e), "word"));
    stage(progress, arg, "installed %s: %s", m.name, granite_buf_str(&words));
    granite_buf_free(&words);

done:
    granite_remove_tree(work);
    if (installed && !error) *installed = record;
    else granite_installation_free(record);
    manifest_free(&m);
    granite_json_put(entries);
    free(said_source);
    free(builds);
    free(work);
    free(package);
    free(revision);
    free(built);
    free(granite);
    return error;
}

granite_install_error *granite_install_update(const char *folder, const char *name, granite_progress progress, void *arg,
                                              granite_installation **installed) {
    granite_installation *found = granite_shelf_find(folder, name);
    if (!found) {
        granite_install_error *e = failure(GRANITE_INSTALL_NOT_INSTALLED);
        e->name = granite_strdup(name);
        return e;
    }
    granite_source newest = granite_source_newest(&found->source);
    granite_install_error *error = granite_install(folder, &newest, progress, arg, installed);
    granite_source_free(&newest);
    granite_installation_free(found);
    return error;
}

granite_install_error *granite_install_remove(const char *folder, const char *name, granite_installation **removed) {
    if (removed) *removed = NULL;
    granite_installation *found = granite_shelf_find(folder, name);
    if (!found) {
        granite_install_error *e = failure(GRANITE_INSTALL_NOT_INSTALLED);
        e->name = granite_strdup(name);
        return e;
    }
    char *inside = granite_shelf_folder_of(folder, found);
    bool gone = granite_remove_tree(inside);
    free(inside);
    if (!gone) {
        granite_install_error *e = fetch_failed(found->name, granite_strdup("it could not be removed"));
        granite_installation_free(found);
        return e;
    }
    if (removed) *removed = found;
    else granite_installation_free(found);
    return NULL;
}
