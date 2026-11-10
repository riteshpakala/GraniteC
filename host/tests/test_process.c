/* The host's side of a call, against real runners: the parrot (GraniteRust's twin) for every way a
 * call ends, and the scripted runner for what a real agent never says. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "granite_host/files.h"
#include "granite_host/process.h"
#include "granite_test.h"
#include "granitec/thread.h"

static char parrot_path[1024], scripted_path[1024];

static char *read_golden_entries(void) {
    char path[1024];
    granite_test_path("tests/fixtures/parrot.describe", path, sizeof path);
    size_t len = 0;
    char *line = granite_file_read(path, &len);
    if (!line) return NULL;
    /* {"entries":[…]}\n → […] */
    char *start = strchr(line, '['), *end = strrchr(line, ']');
    char *entries = granite_strdupf("%.*s", (int)(end - start + 1), start);
    free(line);
    return entries;
}

GRANITE_TEST(describe_says_what_it_carries_and_makes_nothing) {
    granite_process *process = granite_process_new(parrot_path, NULL);
    granite_remote_error *error = NULL;
    json_object *entries = granite_process_describe(process, &error);
    GRANITE_ASSERT(entries != NULL && error == NULL);
    char *written = granite_json_write(entries, NULL), *golden = read_golden_entries();
    GRANITE_ASSERT_STR(written, golden);
    free(written);
    free(golden);
    granite_json_put(entries);
    granite_process_free(process);
}

typedef struct heard {
    granite_buf notes;
} heard;

static void hear(void *arg, const char *line) {
    granite_buf_appendf(&((heard *)arg)->notes, "%s|", line);
}

typedef struct counting_model {
    granite_model base;
} counting_model;

static granite_error *four(granite_model *model, const json_object *schema, const char *system, const char *user,
                           uint32_t max_tokens, double seconds, granite_cancel *cancel, json_object **object) {
    *object = json_object_new_object();
    granite_json_set(*object, "n", json_object_new_int(4));
    return NULL;
}

static granite_error *no_text(granite_model *model, const char *system, const char *user, uint32_t max_tokens,
                              double seconds, granite_cancel *cancel, char **text) {
    return granite_host_no_answer("text is not asked here");
}

static const granite_model_methods four_methods = {four, no_text};
static const granite_class four_class = {"model of four", sizeof(counting_model), NULL, NULL};

static granite_remote_error *perform(granite_process *process, const char *input, granite_host *host, char **output,
                                     granite_process_cost *cost) {
    return granite_process_perform(process, "parrot", input, strlen(input), host, NULL, output, NULL, cost);
}

GRANITE_TEST(one_call_is_answered_its_notes_heard_its_ask_answered_and_its_cost_measured) {
    granite_process *process = granite_process_new(parrot_path, NULL);
    heard h = {{0}};
    counting_model *model = granite_object_new(&four_class);
    model->base.methods = &four_methods;
    char *folder = granite_temporary_folder("process");
    granite_host *host = granite_host_new(folder, "count", &model->base, hear, &h, NULL);
    char *output = NULL;
    granite_process_cost cost = {0};
    granite_remote_error *error = perform(process, "{\"text\":\"hi\",\"notes\":2,\"askModel\":true}", host, &output, &cost);
    GRANITE_ASSERT(error == NULL);
    json_object *answer = NULL;
    GRANITE_ASSERT(output && granite_json_read(output, strlen(output), &answer, NULL));
    GRANITE_ASSERT_STR(granite_json_get_string(answer, "said"), "hi");
    GRANITE_ASSERT_EQ(json_object_get_int(granite_json_get(answer, "modelSaid")), 4);
    GRANITE_ASSERT_STR(granite_json_get_string(answer, "hostDirectory"), folder);
    GRANITE_ASSERT_STR(granite_json_get_string(answer, "ask"), "count");
    GRANITE_ASSERT_STR(granite_buf_str(&h.notes), "note 1|note 2|");
    GRANITE_ASSERT_STR(cost.status, "exit 0");
    GRANITE_ASSERT_EQ(cost.asks, 1);
    char *sentence = granite_cost_sentence(&cost);
    GRANITE_ASSERT(strncmp(sentence, "released · peak ", 17) == 0 && strstr(sentence, " s CPU in ") != NULL);
    free(sentence);
    granite_cost_free(&cost);
    granite_json_put(answer);
    free(output);
    granite_release(host);
    granite_release(model);
    granite_buf_free(&h.notes);
    granite_remove_tree(folder);
    free(folder);
    granite_process_free(process);
}

static char *said_by(granite_remote_error *error) {
    return error ? granite_remote_text(error) : granite_strdup("(answered)");
}

static void expect(const char *input, granite_remote_type type, const char *sentence) {
    granite_process *process = granite_process_new(parrot_path, NULL);
    char *output = NULL;
    granite_remote_error *error = perform(process, input, NULL, &output, NULL);
    char *said = said_by(error);
    if (!error || error->type != type) GRANITE_FAIL("%s: %s", input, said);
    else if (sentence && strncmp(said, sentence, strlen(sentence)) != 0) GRANITE_FAIL("%s: said %s", input, said);
    free(said);
    free(output);
    granite_remote_free(error);
    granite_process_free(process);
}

GRANITE_TEST(each_way_a_call_ends_is_classified_as_rust_classifies_it) {
    expect("{\"text\":\"x\",\"fail\":true}", GRANITE_REMOTE_REFUSED, "parrot refused (call): told to fail");
    expect("{\"notes\":1}", GRANITE_REMOTE_REFUSED, "parrot refused (input): parrot could not read its input: missing field `text`");
    expect("{\"text\":\"x\",\"askModel\":true}", GRANITE_REMOTE_REFUSED,
           "parrot refused (model): parrot asks the host's model, and this host has none loaded");
    expect("{\"text\":\"x\",\"fault\":true}", GRANITE_REMOTE_FAULT, "parrot failed by its own fault: it broke");
    expect("{\"text\":\"x\",\"crash\":true}", GRANITE_REMOTE_ENDED,
           "parrot's process ended (signal 6) without answering: this line must not reach the wire");
    granite_process *process = granite_process_new(parrot_path, NULL);
    char *output = NULL;
    granite_remote_error *error = granite_process_perform(process, "comic", "{}", 2, NULL, NULL, &output, NULL, NULL);
    char *said = said_by(error);
    GRANITE_ASSERT_STR(said, "comic failed by its own fault: no agent is called `comic` in this process; it carries `parrot`");
    free(said);
    granite_remote_free(error);
    error = granite_process_perform(process, "parrot", "not json", 8, NULL, NULL, &output, NULL, NULL);
    said = said_by(error);
    GRANITE_ASSERT_STR(said, "parrot's process broke the wire: the input is not JSON");
    free(said);
    granite_remote_free(error);
    granite_process_free(process);
    granite_process *missing = granite_process_new("/nowhere/runner", NULL);
    error = granite_process_perform(missing, "parrot", "{}", 2, NULL, NULL, &output, NULL, NULL);
    said = said_by(error);
    GRANITE_ASSERT_STR(said, "could not start /nowhere/runner: No such file or directory");
    free(said);
    granite_remote_free(error);
    granite_process_free(missing);
}

typedef struct cancelled_call {
    granite_process *process;
    granite_cancel *cancel;
    granite_remote_error *error;
    double took;
} cancelled_call;

static void *perform_until_cancelled(void *pointer) {
    cancelled_call *c = pointer;
    char *output = NULL;
    double start = granite_now();
    const char *input = "{\"text\":\"x\",\"delay\":30}";
    c->error = granite_process_perform(c->process, "parrot", input, strlen(input), NULL, c->cancel, &output, NULL, NULL);
    c->took = granite_now() - start;
    free(output);
    return NULL;
}

GRANITE_TEST(a_cancelled_call_ends_its_process) {
    cancelled_call c = {granite_process_new(parrot_path, NULL), granite_cancel_new(), NULL, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, perform_until_cancelled, &c);
    granite_sleep(0.3);
    granite_cancel_set(c.cancel);
    pthread_join(thread, NULL);
    GRANITE_ASSERT(c.error != NULL);
    GRANITE_ASSERT(c.took < 5);
    granite_remote_free(c.error);
    granite_release(c.cancel);
    granite_process_free(c.process);
}

static granite_process *scripted(const char *script) {
    char *environment[] = {granite_strdupf("GRANITE_SCRIPT=%s", script), granite_strdup("PATH=/usr/bin:/bin"), NULL};
    granite_process *process = granite_process_new(scripted_path, environment);
    free(environment[0]);
    free(environment[1]);
    return process;
}

static granite_remote_error *scripted_call(const char *script, granite_host *host, granite_cancel *cancel, char **output) {
    granite_process *process = scripted(script);
    granite_remote_error *error = granite_process_perform(process, "x", "{}", 2, host, cancel, output, NULL, NULL);
    granite_process_free(process);
    return error;
}

GRANITE_TEST(a_runner_starts_clean_whatever_its_host_blocks_or_ignores) {
    sigset_t block, before;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &before);
    struct sigaction ignore, kept;
    memset(&ignore, 0, sizeof ignore);
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore, &kept);
    char *folder = granite_temporary_folder("clean"), real[1024];
    if (!realpath(folder, real)) strcpy(real, folder);
    granite_host *host = granite_host_new(real, NULL, NULL, NULL, NULL, NULL);
    char *output = NULL;
    granite_remote_error *error = scripted_call("report", host, NULL, &output);
    pthread_sigmask(SIG_SETMASK, &before, NULL);
    sigaction(SIGINT, &kept, NULL);
    GRANITE_ASSERT(error == NULL);
    json_object *report = NULL;
    GRANITE_ASSERT(output && granite_json_read(output, strlen(output), &report, NULL));
    char *blocked = granite_json_write(granite_json_get(report, "blocked"), NULL);
    char *ignored = granite_json_write(granite_json_get(report, "ignored"), NULL);
    char *fds = granite_json_write(granite_json_get(report, "fds"), NULL);
    GRANITE_ASSERT_STR(blocked, "[]");
    GRANITE_ASSERT_STR(ignored, "[]");
    GRANITE_ASSERT_STR(fds, "[]");
    GRANITE_ASSERT(json_object_get_boolean(granite_json_get(report, "leads")));
    GRANITE_ASSERT_STR(granite_json_get_string(report, "cwd"), real);
    GRANITE_ASSERT_STR(granite_json_get_string(report, "agent"), "1");
    free(blocked);
    free(ignored);
    free(fds);
    granite_json_put(report);
    free(output);
    granite_release(host);
    granite_remove_tree(folder);
    free(folder);
}

static void expect_scripted(const char *script, granite_remote_type type, const char *sentence, const char *answer) {
    char *output = NULL;
    granite_remote_error *error = scripted_call(script, NULL, NULL, &output);
    char *said = said_by(error);
    if (answer) {
        if (error || !output || strcmp(output, answer) != 0) GRANITE_FAIL("%s: %s / %s", script, said, output ? output : "");
    } else if (!error || error->type != type || strcmp(said, sentence) != 0) {
        GRANITE_FAIL("%s: said %s", script, said);
    }
    free(said);
    free(output);
    granite_remote_free(error);
}

GRANITE_TEST(what_a_real_agent_never_says_is_read_as_rust_reads_it) {
    expect_scripted("major2", GRANITE_REMOTE_UNREADABLE, "x's process broke the wire: it speaks granite-agent/2, and this host granite-agent/1", NULL);
    expect_scripted("misplaced", 0, NULL, "{}");
    expect_scripted("noisy", 0, NULL, "{\"heard\":true}");
    expect_scripted("silent", GRANITE_REMOTE_ENDED, "x's process ended (exit 0) without answering", NULL);
    expect_scripted("exit-early", GRANITE_REMOTE_ENDED, "x's process ended (exit 7) without answering", NULL);
    expect_scripted("long-note", 0, NULL, "{\"long\":true}");
}

GRANITE_TEST(a_runner_that_ignores_sigterm_is_killed_after_the_grace) {
    granite_cancel *cancel = granite_cancel_new();
    cancelled_call c = {scripted("stubborn"), cancel, NULL, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, perform_until_cancelled, &c);
    granite_sleep(0.3);
    granite_cancel_set(cancel);
    pthread_join(thread, NULL);
    char *said = said_by(c.error);
    GRANITE_ASSERT_STR(said, "parrot's process ended (signal 9) without answering");
    GRANITE_ASSERT(c.took > 1.5 && c.took < 5);
    free(said);
    granite_remote_free(c.error);
    granite_release(cancel);
    granite_process_free(c.process);
}

GRANITE_TEST(a_grandchild_does_not_outlive_the_call) {
    char *output = NULL;
    granite_remote_error *error = scripted_call("grandchild", NULL, NULL, &output);
    GRANITE_ASSERT(error == NULL);
    json_object *said = NULL;
    GRANITE_ASSERT(output && granite_json_read(output, strlen(output), &said, NULL));
    pid_t grandchild = (pid_t)json_object_get_int(granite_json_get(said, "grandchild"));
    GRANITE_ASSERT(grandchild > 0);
    bool gone = false;
    for (int i = 0; i < 100 && !gone; i++) {
        gone = kill(grandchild, 0) != 0 && errno == ESRCH;
        if (!gone) granite_sleep(0.02);
    }
    GRANITE_ASSERT(gone);
    granite_json_put(said);
    free(output);
    granite_remote_free(error);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    granite_test_build_path("fixtures/parrot_runner", parrot_path, sizeof parrot_path);
    granite_test_build_path("fixtures/scripted_runner", scripted_path, sizeof scripted_path);
    GRANITE_RUN(describe_says_what_it_carries_and_makes_nothing);
    GRANITE_RUN(one_call_is_answered_its_notes_heard_its_ask_answered_and_its_cost_measured);
    GRANITE_RUN(each_way_a_call_ends_is_classified_as_rust_classifies_it);
    GRANITE_RUN(a_cancelled_call_ends_its_process);
    GRANITE_RUN(a_runner_starts_clean_whatever_its_host_blocks_or_ignores);
    GRANITE_RUN(what_a_real_agent_never_says_is_read_as_rust_reads_it);
    GRANITE_RUN(a_runner_that_ignores_sigterm_is_killed_after_the_grace);
    GRANITE_RUN(a_grandchild_does_not_outlive_the_call);
    GRANITE_TEST_MAIN_END();
}
