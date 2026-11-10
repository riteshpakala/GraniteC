/* A runner as a child process. See granite_host/child.h. */
#include "granite_host/child.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "granitec/buf.h"
#include "granitec/lock.h"
#include "granitec/object.h"
#include "granitec/thread.h"

#define TAIL_BYTES 8192

struct granite_child {
    granite_object header;
    pid_t pid;
    granite_channel *channel;
    double started;
    granite_lock lock;
    pthread_cond_t ended_cond;
    char tail[TAIL_BYTES];
    size_t tail_len;
    bool ended;
    granite_child_ended end;
};

static void child_deinit(void *self) {
    granite_child *child = self;
    granite_release(child->channel);
    granite_lock_destroy(&child->lock);
}

static const granite_class child_class = {"child process", sizeof(granite_child), child_deinit, NULL};

char *granite_status_text(int status) {
    if (WIFSIGNALED(status)) return granite_strdupf("signal %d", WTERMSIG(status));
    return granite_strdupf("exit %d", WIFEXITED(status) ? WEXITSTATUS(status) : 0);
}

/* posix_spawn's chdir: the POSIX 2024 call where Apple has it (macOS 26), its old _np name before. */
static int add_chdir(posix_spawn_file_actions_t *actions, const char *directory) {
#if defined(__APPLE__)
    if (__builtin_available(macOS 26.0, *)) return posix_spawn_file_actions_addchdir(actions, directory);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return posix_spawn_file_actions_addchdir_np(actions, directory);
#pragma clang diagnostic pop
#else
    return posix_spawn_file_actions_addchdir_np(actions, directory);
#endif
}

typedef struct tail_reader {
    granite_child *child;
    int fd;
} tail_reader;

static void read_tail(void *pointer) {
    tail_reader *r = pointer;
    granite_child *child = r->child;
    char chunk[4096];
    for (;;) {
        ssize_t n = read(r->fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        granite_lock_acquire(&child->lock);
        for (ssize_t i = 0; i < n; i++) {
            if (child->tail_len == TAIL_BYTES) {
                memmove(child->tail, child->tail + 1, TAIL_BYTES - 1);
                child->tail_len--;
            }
            child->tail[child->tail_len++] = chunk[i];
        }
        granite_lock_release(&child->lock);
    }
    close(r->fd);
    granite_release(child);
    free(r);
}

static void reap(void *pointer) {
    granite_child *child = pointer;
    int status = 0;
    struct rusage usage;
    memset(&usage, 0, sizeof usage);
    while (wait4(child->pid, &status, 0, &usage) < 0 && errno == EINTR) {
    }
    /* What is left of its group goes with it: a grandchild must not outlive the call. */
    if (kill(-child->pid, 0) == 0) kill(-child->pid, SIGKILL);
    granite_child_ended end;
    end.status = status;
    end.cpu_seconds = (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
                      (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
#if defined(__linux__)
    end.peak_bytes = (int64_t)usage.ru_maxrss * 1024;
#else
    end.peak_bytes = (int64_t)usage.ru_maxrss;
#endif
    granite_lock_acquire(&child->lock);
    child->end = end;
    child->ended = true;
    pthread_cond_broadcast(&child->ended_cond);
    granite_lock_release(&child->lock);
    granite_release(child);
}

static void unreadable_line(void *arg, const char *line, const char *why) {
    fprintf(stderr, "granite-agent: an unreadable line from the runner: %s\n", line);
}

static void cloexec_pipe(int fds[2]) {
    if (pipe(fds) != 0) {
        perror("granite: no pipe");
        abort();
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
}

granite_child *granite_child_spawn(const char *executable, const char *const *arguments, const char *directory,
                                   char *const *environment, granite_remote_error **error) {
    *error = NULL;
    int in[2], out[2], err[2];
    cloexec_pipe(in);
    cloexec_pipe(out);
    cloexec_pipe(err);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    if (directory) add_chdir(&actions, directory);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t empty, defaults;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attributes, &empty);
    sigemptyset(&defaults);
    const int reset[] = {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE};
    for (size_t i = 0; i < sizeof reset / sizeof reset[0]; i++) sigaddset(&defaults, reset[i]);
    posix_spawnattr_setsigdefault(&attributes, &defaults);

    size_t count = 0;
    while (arguments && arguments[count]) count++;
    char **argv = granite_calloc(count + 2, sizeof *argv);
    argv[0] = (char *)executable;
    for (size_t i = 0; i < count; i++) argv[i + 1] = (char *)arguments[i];

    pid_t pid = 0;
    int status = posix_spawn(&pid, executable, &actions, &attributes, argv, environment);
    free(argv);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (status != 0) {
        close(in[1]);
        close(out[0]);
        close(err[0]);
        *error = granite_remote_new(GRANITE_REMOTE_SPAWN, NULL);
        (*error)->path = granite_strdup(executable);
        (*error)->reason = granite_strdup(strerror(status));
        return NULL;
    }

    granite_child *child = granite_object_new(&child_class);
    child->pid = pid;
    child->started = granite_now();
    granite_lock_init(&child->lock, "a child's tail and end");
    pthread_cond_init(&child->ended_cond, NULL);
    child->channel = granite_pipe_channel_new(out[0], in[1], unreadable_line, NULL);
    tail_reader *tail = granite_calloc(1, sizeof *tail);
    tail->child = granite_retain(child);
    tail->fd = err[0];
    if (!granite_thread_spawn("granite.stderr", 0, read_tail, tail) ||
        !granite_thread_spawn("granite.reaper", 0, reap, granite_retain(child))) {
        perror("granite: a child's readers could not start");
        abort();
    }
    return child;
}

granite_channel *granite_child_channel(granite_child *child) {
    return child->channel;
}

pid_t granite_child_pid(const granite_child *child) {
    return child->pid;
}

granite_child_ended granite_child_wait(granite_child *child) {
    granite_lock_acquire(&child->lock);
    while (!child->ended) granite_lock_wait(&child->lock, &child->ended_cond);
    granite_child_ended end = child->end;
    granite_lock_release(&child->lock);
    return end;
}

bool granite_child_has_exited(granite_child *child) {
    granite_lock_acquire(&child->lock);
    bool ended = child->ended;
    granite_lock_release(&child->lock);
    return ended;
}

typedef struct killing {
    granite_child *child;
    double grace;
} killing;

static void kill_after_grace(void *pointer) {
    killing *k = pointer;
    double deadline = granite_now() + k->grace;
    granite_lock_acquire(&k->child->lock);
    while (!k->child->ended && granite_now() < deadline)
        granite_lock_wait_for(&k->child->lock, &k->child->ended_cond, deadline - granite_now());
    bool ended = k->child->ended;
    granite_lock_release(&k->child->lock);
    if (!ended) kill(-k->child->pid, SIGKILL);
    granite_release(k->child);
    free(k);
}

void granite_child_terminate(granite_child *child, double grace) {
    if (granite_child_has_exited(child)) return;
    kill(-child->pid, SIGTERM);
    killing *k = granite_calloc(1, sizeof *k);
    k->child = granite_retain(child);
    k->grace = grace;
    if (!granite_thread_spawn("granite.kill", 0, kill_after_grace, k)) {
        kill(-child->pid, SIGKILL);
        granite_release(child);
        free(k);
    }
}

char *granite_child_log(granite_child *child) {
    granite_lock_acquire(&child->lock);
    char *tail = granite_strdupf("%.*s", (int)child->tail_len, child->tail);
    granite_lock_release(&child->lock);
    /* The last six lines, as Rust's lines() counts them (a final newline ends a line, not starts one). */
    size_t len = strlen(tail);
    while (len && tail[len - 1] == '\n') len--;
    size_t start = len;
    int lines = 0;
    while (start > 0) {
        if (tail[start - 1] == '\n' && ++lines == 6) break;
        start--;
    }
    char *kept = granite_strdupf("%.*s", (int)(len - start), tail + start);
    free(tail);
    /* Trimmed of the whitespace around it. */
    size_t begin = 0, end = strlen(kept);
    while (begin < end && (kept[begin] == ' ' || kept[begin] == '\n' || kept[begin] == '\t' || kept[begin] == '\r')) begin++;
    while (end > begin && (kept[end - 1] == ' ' || kept[end - 1] == '\n' || kept[end - 1] == '\t' || kept[end - 1] == '\r')) end--;
    char *trimmed = granite_strdupf("%.*s", (int)(end - begin), kept + begin);
    free(kept);
    return trimmed;
}

double granite_child_seconds(const granite_child *child) {
    return granite_now() - child->started;
}
