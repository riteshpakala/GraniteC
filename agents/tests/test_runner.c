/* The parrot's runner as a process: its --describe is the Rust parrot's bytes, and a call over its
 * stdin and stdout ends with Rust's lines and exit codes. Test plumbing only: the host's own process
 * layer is host/. */
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "granite_test.h"
#include "granitec/buf.h"
#include "granitec/thread.h"

typedef struct ran {
    char *out;
    char *err;
    int status;
} ran;

static char *read_all(int fd) {
    granite_buf text = {0};
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) granite_buf_append(&text, chunk, (size_t)n);
    close(fd);
    return granite_buf_take(&text);
}

/* Runs the parrot with `argument` (or none), `input` on its stdin; sends SIGTERM after `term_after`
 * seconds when it is positive. */
static ran run_parrot(const char *argument, const char *input, double term_after) {
    char path[1024];
    granite_test_build_path("fixtures/parrot_runner", path, sizeof path);
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
        execl(path, path, argument, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (input && write(in[1], input, strlen(input)) < 0) {
    }
    if (term_after > 0) {
        granite_sleep(term_after);
        kill(child, SIGTERM);
    }
    close(in[1]);
    ran r;
    r.out = read_all(out[0]);
    r.err = read_all(err[0]);
    waitpid(child, &r.status, 0);
    return r;
}

static void forget(ran *r) {
    free(r->out);
    free(r->err);
}

/* The runner's lines with its pid made 1, so they compare. */
static void pid_free(char *text) {
    for (char *at = strstr(text, "\"pid\":"); at; at = strstr(at + 1, "\"pid\":")) {
        char *digits = at + 6, *end = digits;
        while (*end >= '0' && *end <= '9') end++;
        if (end > digits) {
            *digits = '1';
            memmove(digits + 1, end, strlen(end) + 1);
        }
    }
}

GRANITE_TEST(describe_is_the_rust_parrots_bytes) {
    char golden_path[1024];
    granite_test_path("tests/fixtures/parrot.describe", golden_path, sizeof golden_path);
    FILE *golden = fopen(golden_path, "rb");
    GRANITE_ASSERT(golden != NULL);
    if (!golden) return;
    char *expected = read_all(fileno(golden));
    ran r = run_parrot("--describe", NULL, 0);
    GRANITE_ASSERT(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    GRANITE_ASSERT_STR(r.out, expected);
    free(expected);
    forget(&r);
}

GRANITE_TEST(a_call_answers_on_the_wire_and_nothing_else_reaches_it) {
    ran r = run_parrot(NULL,
                       "{\"perform\":{\"ask\":\"say\",\"directory\":\"/p\",\"input\":{\"notes\":1,\"text\":\"hi\"},"
                       "\"model\":false,\"word\":\"parrot\"}}\n",
                       0);
    GRANITE_ASSERT(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    pid_free(r.out);
    GRANITE_ASSERT_STR(r.out,
                       "{\"hello\":{\"answer\":\"granite-answer/1\",\"pid\":1,\"wire\":\"granite-agent/1\"}}\n"
                       "{\"note\":\"note 1\"}\n"
                       "{\"output\":{\"agent\":{\"word\":\"parrot\"},\"ask\":\"say\",\"directory\":null,\"expected\":[],"
                       "\"format\":\"granite-answer/1\",\"hostDirectory\":\"/p\",\"made\":[],\"modelSaid\":null,\"path\":null,"
                       "\"paths\":[],\"payloads\":[],\"pid\":1,\"problems\":[],\"said\":\"hi\"}}\n");
    GRANITE_ASSERT(strstr(r.err, "this line must not reach the wire") != NULL);
    forget(&r);
}

static void expect_ending(const char *input, int code, const char *last_line) {
    ran r = run_parrot(NULL, input, 0);
    if (!WIFEXITED(r.status) || WEXITSTATUS(r.status) != code)
        GRANITE_FAIL("%s: ended %d, expected exit %d", input, r.status, code);
    const char *last = r.out + strlen(r.out);
    if (last > r.out) last--;
    while (last > r.out && last[-1] != '\n') last--;
    if (strcmp(last, last_line) != 0) GRANITE_FAIL("%s: said %s", input, last);
    forget(&r);
}

GRANITE_TEST(each_ending_is_rusts_line_and_exit) {
    expect_ending("{\"perform\":{\"input\":{\"fail\":true,\"text\":\"x\"},\"model\":false,\"word\":\"parrot\"}}\n", 3,
                  "{\"failed\":\"told to fail\",\"refused\":{\"kind\":\"call\",\"reasons\":[\"told to fail: leave out `fail`\"]}}\n");
    expect_ending("{\"perform\":{\"input\":{\"fault\":true,\"text\":\"x\"},\"model\":false,\"word\":\"parrot\"}}\n", 1,
                  "{\"failed\":\"it broke\"}\n");
    expect_ending("{\"perform\":{\"input\":{\"text\":\"x\",\"askModel\":true},\"model\":false,\"word\":\"parrot\"}}\n", 3,
                  "{\"failed\":\"parrot asks the host's model, and this host has none loaded\",\"refused\":{\"kind\":\"model\","
                  "\"reasons\":[\"parrot asks the host's model, and this host has none loaded\"]}}\n");
    expect_ending("{\"perform\":{\"input\":{},\"model\":false,\"word\":\"comic\"}}\n", 1,
                  "{\"failed\":\"no agent is called `comic` in this process; it carries `parrot`\"}\n");
    expect_ending("", 2, "{\"failed\":\"the host left before making a call\"}\n");
    expect_ending("{\"cancel\":{}}\n", 130, "{\"failed\":\"cancelled before the call began\"}\n");
}

GRANITE_TEST(a_crash_ends_with_its_signal_and_sigterm_cancels) {
    ran crash = run_parrot(NULL, "{\"perform\":{\"input\":{\"crash\":true,\"text\":\"x\"},\"model\":false,\"word\":\"parrot\"}}\n", 0);
    GRANITE_ASSERT(WIFSIGNALED(crash.status) && WTERMSIG(crash.status) == SIGABRT);
    forget(&crash);
    ran term = run_parrot(NULL, "{\"perform\":{\"input\":{\"delay\":5,\"text\":\"x\"},\"model\":false,\"word\":\"parrot\"}}\n", 0.3);
    GRANITE_ASSERT(WIFEXITED(term.status) && WEXITSTATUS(term.status) == 130);
    GRANITE_ASSERT(strstr(term.out, "{\"note\":\"waiting 5 s as pid ") != NULL);
    GRANITE_ASSERT(strstr(term.out, "{\"failed\":\"cancelled\"}\n") != NULL);
    forget(&term);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    GRANITE_RUN(describe_is_the_rust_parrots_bytes);
    GRANITE_RUN(a_call_answers_on_the_wire_and_nothing_else_reaches_it);
    GRANITE_RUN(each_ending_is_rusts_line_and_exit);
    GRANITE_RUN(a_crash_ends_with_its_signal_and_sigterm_cancels);
    GRANITE_TEST_MAIN_END();
}
