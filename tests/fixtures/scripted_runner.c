/* A runner that says what a test scripts, byte for byte: the edge cases a host meets and a real
 * agent never says. The scenario is GRANITE_SCRIPT, since a host passes a runner no arguments for a
 * call. Test-only. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void say(const char *line) {
    size_t len = strlen(line);
    while (len) {
        ssize_t n = write(STDOUT_FILENO, line, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        line += n;
        len -= (size_t)n;
    }
}

static void hello(const char *wire) {
    char line[160];
    snprintf(line, sizeof line, "{\"hello\":{\"pid\":%d,\"wire\":\"%s\"}}\n", (int)getpid(), wire);
    say(line);
}

/* Reads stdin up to the end of the first line (the perform), or its end. */
static void read_perform(void) {
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {
    }
}

static void forever(void) {
    for (;;) pause();
}

static int fcntl_is_open(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

/* What the runner inherited from its host: its blocked signals, the ones it ignores, whether it
 * leads its own group, its folder, its open descriptors past stderr, and GRANITE_AGENT. */
static void report(void) {
    char line[8192];
    int k = snprintf(line, sizeof line, "{\"output\":{\"blocked\":[");
    sigset_t mask;
    sigprocmask(SIG_BLOCK, NULL, &mask);
    int first = 1;
    for (int s = 1; s < 32; s++)
        if (sigismember(&mask, s) == 1) k += snprintf(line + k, sizeof line - (size_t)k, "%s%d", first ? "" : ",", s), first = 0;
    k += snprintf(line + k, sizeof line - (size_t)k, "],\"ignored\":[");
    const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE};
    first = 1;
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        struct sigaction action;
        sigaction(signals[i], NULL, &action);
        if (action.sa_handler == SIG_IGN)
            k += snprintf(line + k, sizeof line - (size_t)k, "%s%d", first ? "" : ",", signals[i]), first = 0;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
    k += snprintf(line + k, sizeof line - (size_t)k, "],\"leads\":%s,\"cwd\":\"%s\",\"fds\":[",
                  getpgrp() == getpid() ? "true" : "false", cwd);
    first = 1;
    for (int fd = 3; fd < 256; fd++) {
        if (fcntl_is_open(fd)) k += snprintf(line + k, sizeof line - (size_t)k, "%s%d", first ? "" : ",", fd), first = 0;
    }
    const char *agent = getenv("GRANITE_AGENT");
    snprintf(line + k, sizeof line - (size_t)k, "],\"agent\":\"%s\"}}\n", agent ? agent : "");
    say(line);
}

int main(void) {
    const char *script = getenv("GRANITE_SCRIPT");
    script = script ? script : "report";
    if (strcmp(script, "exit-early") == 0) return 7;
    if (strcmp(script, "misplaced") == 0) {
        say("{\"output\":{}}\n");
        forever();
    }
    if (strcmp(script, "major2") == 0) {
        hello("granite-agent/2");
        forever();
    }
    if (strcmp(script, "noisy") == 0) {
        say("not json\n{\"weather\":1}\n\n");
    }
    hello("granite-agent/1");
    read_perform();
    if (strcmp(script, "report") == 0) {
        report();
    } else if (strcmp(script, "noisy") == 0) {
        say("{\"output\":{\"heard\":true}}\n");
    } else if (strcmp(script, "stubborn") == 0) {
        signal(SIGTERM, SIG_IGN);
        say("{\"note\":\"ignoring SIGTERM\"}\n");
        forever();
    } else if (strcmp(script, "grandchild") == 0) {
        pid_t child = fork();
        if (child == 0) {
            for (;;) sleep(60);
        }
        char line[128];
        snprintf(line, sizeof line, "{\"output\":{\"grandchild\":%d}}\n", (int)child);
        say(line);
    } else if (strcmp(script, "silent") == 0) {
        return 0;
    } else if (strcmp(script, "long-note") == 0) {
        static char note[140000];
        memcpy(note, "{\"note\":\"", 9);
        memset(note + 9, 'x', 100000);
        memcpy(note + 100009, "\"}\n", 4);
        say(note);
        say("{\"output\":{\"long\":true}}\n");
    } else if (strcmp(script, "hang") == 0) {
        forever();
    }
    return 0;
}
