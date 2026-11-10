/* A C agent repository installed, called, installed again and removed, as GraniteRust's installer
 * test does with its hello agent; and each way an install is refused, in its sentence. Builds the
 * fixture repository against this build tree through pkg-config (make test puts it on the path). */
#include <stdarg.h>
#include <unistd.h>

#include "granite_host/files.h"
#include "granite_host/install.h"
#include "granite_host/process.h"
#include "granite_host/shelf.h"
#include "granite_test.h"

static char repository[1024];

typedef struct stages {
    granite_buf said;
    int lines;
} stages;

static void progress(void *arg, const char *stage, const char *line) {
    stages *s = arg;
    if (line) s->lines++;
    else granite_buf_appendf(&s->said, "%s|", stage);
}

/* A copy of the fixture repository at `folder`, its manifest naming it `name`. */
static void copy_repository(const char *folder, const char *name, bool broken) {
    granite_mkdirs(folder);
    const char *files[] = {"Makefile", "parrot.c", NULL};
    for (const char **file = files; *file; file++) {
        char *from = granite_path_join(repository, *file), *to = granite_path_join(folder, *file);
        size_t len = 0;
        char *bytes = granite_file_read(from, &len);
        if (broken && strcmp(*file, "parrot.c") == 0) granite_file_write(to, "this is not C;\n", 15);
        else granite_file_write(to, bytes, len);
        free(bytes);
        free(from);
        free(to);
    }
    char *manifest = granite_path_join(folder, GRANITE_AGENT_MANIFEST);
    char *text = granite_strdupf("{\"name\":\"%s\",\"runner\":\"agent\",\"granite\":\"0.1.0\"}\n", name);
    granite_file_write(manifest, text, strlen(text));
    free(text);
    free(manifest);
}

static char *install_said(granite_install_error *error) {
    char *text = error ? granite_install_text(error) : granite_strdup("(installed)");
    granite_install_error_free(error);
    return text;
}

static granite_install_error *install_path(const char *shelf, const char *path, stages *s, granite_installation **record) {
    granite_source source = granite_source_parse(path, "/");
    granite_install_error *error = granite_install(shelf, &source, s ? progress : NULL, s, record);
    granite_source_free(&source);
    return error;
}

GRANITE_TEST(a_repository_is_installed_called_reinstalled_and_removed) {
    char *shelf = granite_temporary_folder("install-shelf");
    stages s = {{0}, 0};
    granite_installation *record = NULL;
    granite_install_error *error = install_path(shelf, repository, &s, &record);
    char *said = install_said(error);
    GRANITE_ASSERT_STR(said, "(installed)");
    free(said);
    if (!record) return;
    GRANITE_ASSERT_STR(granite_buf_str(&s.said), "reading the package|building parrot-c|asking parrot-c what it carries|"
                                                 "installing parrot-c|installed parrot-c: parrot|");
    GRANITE_ASSERT_STR(record->name, "parrot-c");
    GRANITE_ASSERT_STR(record->executable, "agent");
    GRANITE_ASSERT_STR(record->source.path, repository);
    GRANITE_ASSERT(record->granite && strncmp(record->granite, "0.1.0", 5) == 0);
    GRANITE_ASSERT_STR(granite_installation_word(record), "parrot");

    granite_installation *found = granite_shelf_find(shelf, "PARROT");
    GRANITE_ASSERT(found != NULL);
    GRANITE_ASSERT(found && granite_shelf_verify(shelf, found) == NULL);
    char *executable = found ? granite_shelf_executable_of(shelf, found) : NULL;
    granite_process *process = granite_process_new(executable, NULL);
    char *output = NULL;
    granite_remote_error *call = granite_process_perform(process, "parrot", "{\"text\":\"hi\"}", 13, NULL, NULL, &output, NULL, NULL);
    GRANITE_ASSERT(call == NULL && output && strstr(output, "\"said\":\"hi\"") != NULL);
    granite_remote_free(call);
    free(output);
    granite_process_free(process);
    free(executable);
    granite_installation_free(found);

    /* Again: the same repository replaces itself whole. */
    error = install_path(shelf, repository, NULL, NULL);
    said = install_said(error);
    GRANITE_ASSERT_STR(said, "(installed)");
    free(said);
    granite_shelf_read read = granite_shelf_read_all(shelf);
    GRANITE_ASSERT_EQ(read.count, 1);
    granite_shelf_read_free(&read);

    /* Another repository that carries `parrot` is refused. */
    char *other = granite_temporary_folder("install-other");
    copy_repository(other, "parrot-twin", false);
    error = install_path(shelf, other, NULL, NULL);
    said = install_said(error);
    char *clash = granite_strdupf("an agent called `parrot` is already installed, from %s", repository);
    GRANITE_ASSERT_STR(said, clash);
    free(clash);
    free(said);

    granite_installation *removed = NULL;
    error = granite_install_remove(shelf, "parrot-c", &removed);
    GRANITE_ASSERT(error == NULL && removed != NULL);
    granite_installation_free(removed);
    said = install_said(granite_install_remove(shelf, "parrot-c", NULL));
    GRANITE_ASSERT_STR(said, "no agent installed here is called parrot-c");
    free(said);
    said = install_said(granite_install_update(shelf, "parrot-c", NULL, NULL, NULL));
    GRANITE_ASSERT_STR(said, "no agent installed here is called parrot-c");
    free(said);

    granite_installation_free(record);
    granite_buf_free(&s.said);
    granite_remove_tree(other);
    granite_remove_tree(shelf);
    free(other);
    free(shelf);
}

GRANITE_TEST(each_way_an_install_is_refused_is_said) {
    char *shelf = granite_temporary_folder("install-refused");
    char *empty = granite_temporary_folder("install-empty");
    char *said = install_said(install_path(shelf, empty, NULL, NULL));
    char *expected = granite_strdupf("%s is not a C agent repository: it has no granite-agent.json", empty);
    GRANITE_ASSERT_STR(said, expected);
    free(said);
    free(expected);

    char *cargo = granite_path_join(empty, "Cargo.toml");
    granite_file_write(cargo, "[package]\n", 10);
    said = install_said(install_path(shelf, empty, NULL, NULL));
    expected = granite_strdupf("%s is not a C agent repository: it has no granite-agent.json (it has a Cargo.toml: "
                               "GraniteRust's granite installs Rust agents)", empty);
    GRANITE_ASSERT_STR(said, expected);
    free(said);
    free(expected);
    free(cargo);

    char *broken = granite_temporary_folder("install-broken");
    copy_repository(broken, "broken-parrot", true);
    granite_install_error *error = install_path(shelf, broken, NULL, NULL);
    GRANITE_ASSERT(error && error->type == GRANITE_INSTALL_BUILD);
    if (error) {
        char *text = granite_install_text(error);
        GRANITE_ASSERT(strncmp(text, "broken-parrot did not build (the whole log is at ", 49) == 0);
        GRANITE_ASSERT(granite_exists(error->path));
        free(text);
    }
    granite_install_error_free(error);

    char *newer = granite_temporary_folder("install-newer");
    copy_repository(newer, "newer-parrot", false);
    char *manifest = granite_path_join(newer, GRANITE_AGENT_MANIFEST);
    granite_file_write(manifest, "{\"name\":\"newer-parrot\",\"granite\":\"9.0\"}", 39);
    said = install_said(install_path(shelf, newer, NULL, NULL));
    GRANITE_ASSERT_STR(said, "newer-parrot does not build against GraniteC: it needs GraniteC 9.0, and pkg-config finds 0.1.0");
    free(said);
    free(manifest);

    granite_shelf_read read = granite_shelf_read_all(shelf);
    GRANITE_ASSERT_EQ(read.count, 0);
    granite_shelf_read_free(&read);
    granite_remove_tree(empty);
    granite_remove_tree(broken);
    granite_remove_tree(newer);
    granite_remove_tree(shelf);
    free(empty);
    free(broken);
    free(newer);
    free(shelf);
}

static bool git(const char *folder, ...) {
    const char *arguments[12] = {"git"};
    int n = 1;
    va_list args;
    va_start(args, folder);
    const char *a;
    while (n < 11 && (a = va_arg(args, const char *))) arguments[n++] = a;
    va_end(args);
    arguments[n] = NULL;
    granite_command_said said = granite_command_run(arguments, folder, NULL, NULL);
    bool ok = said.status == 0;
    granite_command_said_free(&said);
    return ok;
}

GRANITE_TEST(a_repository_is_fetched_from_git_and_its_revision_kept) {
    char *shelf = granite_temporary_folder("install-git-shelf");
    char *origin = granite_temporary_folder("install-git-origin");
    copy_repository(origin, "parrot-git", false);
    bool made = git(origin, "init", "--quiet", NULL) && git(origin, "add", ".", NULL) &&
                git(origin, "-c", "user.name=granite", "-c", "user.email=granite@example.invalid", "commit", "--quiet", "-m", "parrot", NULL);
    GRANITE_ASSERT(made);
    char *url = granite_strdupf("file://%s", origin);
    stages s = {{0}, 0};
    granite_installation *record = NULL;
    granite_source source = granite_source_parse(url, "/");
    char *said = install_said(granite_install(shelf, &source, progress, &s, &record));
    GRANITE_ASSERT_STR(said, "(installed)");
    char *fetching = granite_strdupf("fetching %s|", url);
    GRANITE_ASSERT(strncmp(granite_buf_str(&s.said), fetching, strlen(fetching)) == 0);
    GRANITE_ASSERT(record && record->revision && strlen(record->revision) == 40);
    GRANITE_ASSERT(record && record->source.url && strcmp(record->source.url, url) == 0);
    free(fetching);
    free(said);
    granite_source_free(&source);
    granite_source missing = granite_source_parse("file:///nowhere/at/all.git", "/");
    granite_install_error *error = granite_install(shelf, &missing, NULL, NULL, NULL);
    GRANITE_ASSERT(error && error->type == GRANITE_INSTALL_FETCH);
    granite_install_error_free(error);
    granite_source_free(&missing);
    granite_installation_free(record);
    granite_buf_free(&s.said);
    granite_remove_tree(origin);
    granite_remove_tree(shelf);
    free(url);
    free(origin);
    free(shelf);
}

int main(void) {
    char path[1024];
    granite_test_path("tests/fixtures/agent-repo", path, sizeof path);
    if (!realpath(path, repository)) snprintf(repository, sizeof repository, "%s", path);
    GRANITE_RUN(a_repository_is_installed_called_reinstalled_and_removed);
    GRANITE_RUN(each_way_an_install_is_refused_is_said);
    GRANITE_RUN(a_repository_is_fetched_from_git_and_its_revision_kept);
    GRANITE_TEST_MAIN_END();
}
