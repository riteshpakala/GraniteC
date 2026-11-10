/* The test harness every package shares: Mary's mary_test.h under Granite's names. Each
 * <pkg>/tests/test_<x>.c defines cases with GRANITE_TEST(name) and runs them from main() with
 * GRANITE_RUN(name); the exit status is the number of failed cases. Case names follow the
 * Swift or Rust twin's test names where one exists, so the suites read side by side. */
#ifndef GRANITE_TEST_H
#define GRANITE_TEST_H

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int granite_test_failures = 0;
static int granite_test_case_failed = 0;

#define GRANITE_TEST(name) static void name(void)
#define GRANITE_RUN(name) do { granite_test_case_failed = 0; name(); \
    printf("%s %s\n", granite_test_case_failed ? "FAIL" : "ok  ", #name); fflush(stdout); \
    if (granite_test_case_failed) granite_test_failures++; } while (0)
#define GRANITE_FAIL(...) do { granite_test_case_failed = 1; \
    fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } while (0)
#define GRANITE_ASSERT(cond) do { if (!(cond)) GRANITE_FAIL("assertion failed: %s", #cond); } while (0)
#define GRANITE_ASSERT_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) GRANITE_FAIL("%s = %lld, expected %lld", #a, _a, _b); } while (0)
#define GRANITE_ASSERT_STR(a, b) do { const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) \
        GRANITE_FAIL("%s = \"%s\", expected \"%s\"", #a, _a ? _a : "(null)", _b ? _b : "(null)"); } while (0)
#define GRANITE_ASSERT_NEAR(a, b, eps) do { double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (eps)) GRANITE_FAIL("%s = %g, expected %g (±%g)", #a, _a, _b, (double)(eps)); } while (0)
#define GRANITE_TEST_MAIN_END() return granite_test_failures

/* A path inside the source tree: `make test` exports GRANITE_SRC. */
static inline const char *granite_test_path(const char *relative, char *out, size_t n) {
    const char *root = getenv("GRANITE_SRC");
    snprintf(out, n, "%s/%s", root && *root ? root : ".", relative);
    return out;
}

/* A path inside this build: `make test` exports GRANITE_BUILD (the fixture runners, the command). */
static inline const char *granite_test_build_path(const char *relative, char *out, size_t n) {
    const char *root = getenv("GRANITE_BUILD");
    snprintf(out, n, "%s/%s", root && *root ? root : "build", relative);
    return out;
}

/* Whether `body` aborts, in a child, saying something that contains `said` on stderr: how a test
 * shows a rule the runtime holds (a nested lock, a release under one) fails the way it should. */
static inline int granite_test_aborts(void (*body)(void), const char *said) {
    int pipes[2];
    if (pipe(pipes) != 0) return 0;
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child < 0) return 0;
    if (child == 0) {
        dup2(pipes[1], 2);
        close(pipes[0]);
        close(pipes[1]);
        signal(SIGABRT, SIG_DFL);
        body();
        _exit(0);
    }
    close(pipes[1]);
    char text[4096];
    size_t used = 0;
    ssize_t n;
    while (used + 1 < sizeof text && (n = read(pipes[0], text + used, sizeof text - 1 - used)) > 0) used += (size_t)n;
    text[used] = '\0';
    close(pipes[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        fprintf(stderr, "  the child did not abort (status %d); it said: %s\n", status, text);
        return 0;
    }
    if (said && !strstr(text, said)) {
        fprintf(stderr, "  the child aborted saying: %s(expected it to say \"%s\")\n", text, said);
        return 0;
    }
    return 1;
}

#endif
