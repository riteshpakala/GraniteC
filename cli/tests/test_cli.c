/* The granite command against GraniteRust's: every case in cli/tests/cases.jsonl run through this
 * build's granite and said as GraniteRust's granite says it (rust.expected, from
 * scripts/cli-goldens.sh), byte for byte once pids, costs and places are made the same; and an
 * agent added, listed, run and removed through the command. */
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "granite_host/files.h"
#include "granite_test.h"
#include "granitec/buf.h"
#include "granitec/json.h"
#include "granitec/thread.h"

typedef struct ran {
    char *out;
    char *err;
    int code;
} ran;

static char cli[1024];

static char *drain(int fd) {
    granite_buf text = {0};
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) granite_buf_append(&text, chunk, (size_t)n);
    close(fd);
    return granite_buf_take(&text);
}

/* Runs the command with `args` in `folder`, `input` on its stdin, GRANITE_SHELF unset. */
static ran run_cli(char **args, const char *input, const char *folder) {
    int in[2], out[2], err[2];
    if (pipe(in) || pipe(out) || pipe(err)) abort();
    pid_t child = fork();
    if (child == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        close(in[1]);
        close(out[0]);
        close(err[0]);
        if (chdir(folder) != 0) _exit(126);
        unsetenv("GRANITE_SHELF");
        execv(cli, args);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (*input && write(in[1], input, strlen(input)) < 0) {
    }
    close(in[1]);
    ran r;
    r.out = drain(out[0]);
    r.err = drain(err[0]);
    int status = 0;
    waitpid(child, &status, 0);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return r;
}

/* Every `from` in `text` made `to`. */
static char *replace_all(char *text, const char *from, const char *to) {
    if (!*from) return text;
    granite_buf out = {0};
    const char *at = text, *found;
    while ((found = strstr(at, from))) {
        granite_buf_append(&out, at, (size_t)(found - at));
        granite_buf_append_str(&out, to);
        at = found + strlen(from);
    }
    granite_buf_append_str(&out, at);
    free(text);
    return granite_buf_take(&out);
}

/* After `prefix`, a run of digits made `1`. */
static char *one_digit_after(char *text, const char *prefix) {
    granite_buf out = {0};
    const char *at = text, *found;
    while ((found = strstr(at, prefix))) {
        found += strlen(prefix);
        granite_buf_append(&out, at, (size_t)(found - at));
        const char *digits = found;
        while (*digits >= '0' && *digits <= '9') digits++;
        granite_buf_append_str(&out, digits > found ? "1" : "");
        at = digits;
    }
    granite_buf_append_str(&out, at);
    free(text);
    return granite_buf_take(&out);
}

/* cli/tests/normalize.py, in C. */
static char *normalize(char *text, char *const *places, char *const *names) {
    for (int i = 0; places[i]; i++) {
        char *placeholder = granite_strdupf("{%s}", names[i]);
        text = replace_all(text, places[i], placeholder);
        free(placeholder);
    }
    text = one_digit_after(text, "\"pid\":");
    text = one_digit_after(text, "as pid ");
    granite_buf out = {0};
    const char *at = text, *found;
    while ((found = strstr(at, "released · peak "))) {
        granite_buf_append(&out, at, (size_t)(found - at));
        granite_buf_append_str(&out, "released · <cost>");
        const char *end = strstr(found, " s CPU in ");
        if (end) {
            end += strlen(" s CPU in ");
            while ((*end >= '0' && *end <= '9') || *end == '.') end++;
            end += strncmp(end, " s", 2) == 0 ? 2 : 0;
        } else {
            end = found + strlen("released · peak ");
        }
        at = end;
    }
    granite_buf_append_str(&out, at);
    free(text);
    return granite_buf_take(&out);
}

GRANITE_TEST(every_case_is_said_as_graniterusts_granite_says_it) {
    char path[1024];
    size_t len;
    char *cases = granite_file_read(granite_test_path("cli/tests/cases.jsonl", path, sizeof path), &len);
    char *expected = granite_file_read(granite_test_path("cli/tests/rust.expected", path, sizeof path), &len);
    GRANITE_ASSERT(cases && expected);
    if (!cases || !expected) return;
    char parrot[1024], schema[1024];
    granite_test_build_path("fixtures/parrot_runner", parrot, sizeof parrot);
    granite_test_path("Schema", schema, sizeof schema);
    char *work = granite_temporary_folder("cli"), *shelf = granite_path_join(work, "shelf");
    granite_mkdirs(shelf);
    char *places[] = {parrot, schema, shelf, NULL}, *names[] = {"PARROT", "SCHEMA", "SHELF", NULL};
    char *case_save = NULL, *expected_save = NULL;
    int seen = 0;
    for (char *c = strtok_r(cases, "\n", &case_save), *e = strtok_r(expected, "\n", &expected_save); c && e;
         c = strtok_r(NULL, "\n", &case_save), e = strtok_r(NULL, "\n", &expected_save)) {
        seen++;
        json_object *one = NULL, *want = NULL;
        granite_json_read(c, strlen(c), &one, NULL);
        granite_json_read(e, strlen(e), &want, NULL);
        json_object *arguments = granite_json_get(one, "args");
        size_t n = json_object_array_length(arguments);
        char **args = granite_calloc(n + 2, sizeof *args);
        args[0] = cli;
        for (size_t i = 0; i < n; i++) {
            char *arg = granite_strdup(json_object_get_string(json_object_array_get_idx(arguments, i)));
            for (int p = 0; places[p]; p++) {
                char *placeholder = granite_strdupf("{%s}", names[p]);
                arg = replace_all(arg, placeholder, places[p]);
                free(placeholder);
            }
            args[i + 1] = arg;
        }
        ran r = run_cli(args, granite_json_get_string(one, "stdin"), work);
        r.out = normalize(r.out, places, names);
        r.err = normalize(r.err, places, names);
        int code = json_object_get_int(granite_json_get(want, "code"));
        const char *out = granite_json_get_string(want, "stdout"), *err = granite_json_get_string(want, "stderr");
        if (r.code != code || strcmp(r.out, out) != 0 || strcmp(r.err, err) != 0)
            GRANITE_FAIL("case %d (%s):\n  GraniteC %d out[%s] err[%s]\n  Rust     %d out[%s] err[%s]", seen, c, r.code, r.out,
                         r.err, code, out, err);
        for (size_t i = 1; i <= n; i++) free(args[i]);
        free(args);
        free(r.out);
        free(r.err);
        granite_json_put(one);
        granite_json_put(want);
    }
    GRANITE_ASSERT(seen >= 30);
    granite_remove_tree(work);
    free(work);
    free(shelf);
    free(cases);
    free(expected);
}

GRANITE_TEST(an_agent_is_added_listed_run_and_removed_through_the_command) {
    char repository[1024], real[1024];
    granite_test_path("tests/fixtures/agent-repo", repository, sizeof repository);
    if (!realpath(repository, real)) snprintf(real, sizeof real, "%s", repository);
    char *work = granite_temporary_folder("cli-agents"), *shelf = granite_path_join(work, "shelf");
    char *add[] = {cli, "agents", "add", real, "--shelf", shelf, NULL};
    ran added = run_cli(add, "", work);
    GRANITE_ASSERT_EQ(added.code, 0);
    GRANITE_ASSERT(strstr(added.out, "\"name\": \"parrot-c\"") != NULL);
    GRANITE_ASSERT(strstr(added.err, "installed parrot-c: parrot") != NULL);
    char *list[] = {cli, "agents", "list", "--shelf", shelf, NULL};
    ran listed = run_cli(list, "", work);
    /* The fixture sits inside this repository's checkout, so its folder has a revision, as in Rust. */
    GRANITE_ASSERT(strncmp(listed.out, "parrot       parrot-c@", 22) == 0);
    GRANITE_ASSERT(strstr(listed.out, " · says its text back, in a process of its own.\n") != NULL);
    char *runner = granite_strdupf("%s/parrot-c/agent", shelf);
    char *call[] = {cli, "run", runner, "-", NULL};
    ran called = run_cli(call, "{\"word\":\"parrot\",\"input\":{\"text\":\"hi\"}}", work);
    GRANITE_ASSERT_EQ(called.code, 0);
    GRANITE_ASSERT(strstr(called.out, "\"said\":\"hi\"") != NULL && strstr(called.out, "\"status\":\"exit 0\"") != NULL);
    char *rm[] = {cli, "agents", "rm", "parrot", "--shelf", shelf, NULL};
    ran removed = run_cli(rm, "", work);
    GRANITE_ASSERT_EQ(removed.code, 0);
    GRANITE_ASSERT_STR(removed.err, "removed parrot-c\n");
    ran empty = run_cli(list, "", work);
    GRANITE_ASSERT_STR(empty.out, "");
    ran *all[] = {&added, &listed, &called, &removed, &empty};
    for (size_t i = 0; i < 5; i++) {
        free(all[i]->out);
        free(all[i]->err);
    }
    free(runner);
    granite_remove_tree(work);
    free(work);
    free(shelf);
}

GRANITE_TEST(an_interrupted_run_ends_its_runner_and_then_itself) {
    char parrot[1024];
    granite_test_build_path("fixtures/parrot_runner", parrot, sizeof parrot);
    int in[2], out[2];
    if (pipe(in) || pipe(out)) abort();
    pid_t child = fork();
    if (child == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 2);
        close(in[1]);
        close(out[0]);
        execl(cli, cli, "run", parrot, "-", (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    const char *input = "{\"word\":\"parrot\",\"input\":{\"text\":\"x\",\"delay\":30}}";
    if (write(in[1], input, strlen(input)) < 0) {
    }
    close(in[1]);
    granite_sleep(0.5);
    kill(child, SIGINT);
    int status = 0;
    waitpid(child, &status, 0);
    GRANITE_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
    char *said = drain(out[0]);
    GRANITE_ASSERT_STR(said, "");
    free(said);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *built = getenv("GRANITE_CLI");
    snprintf(cli, sizeof cli, "%s", built ? built : "build/bin/granite");
    GRANITE_RUN(every_case_is_said_as_graniterusts_granite_says_it);
    GRANITE_RUN(an_agent_is_added_listed_run_and_removed_through_the_command);
    GRANITE_RUN(an_interrupted_run_ends_its_runner_and_then_itself);
    GRANITE_TEST_MAIN_END();
}
