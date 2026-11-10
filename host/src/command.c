/* A command the installer runs: git, make, pkg-config. See granite_host/install.h. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "granite_host/install.h"
#include "granitec/buf.h"

extern char **environ;

/* posix_spawn's chdir, as child.c reaches it. */
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

/* This process's environment without what must not reach a build: Apple's loader paths, and the
 * flags and jobserver of a make that runs us. */
static char **clean_environment(void) {
    size_t count = 0;
    while (environ[count]) count++;
    char **kept = granite_calloc(count + 1, sizeof *kept);
    size_t k = 0;
    for (size_t i = 0; i < count; i++) {
        const char *e = environ[i];
        if (strncmp(e, "DYLD_", 5) == 0 || strncmp(e, "MAKEFLAGS=", 10) == 0 || strncmp(e, "MFLAGS=", 7) == 0 ||
            strncmp(e, "MAKELEVEL=", 10) == 0 || strncmp(e, "MAKEOVERRIDES=", 14) == 0)
            continue;
        kept[k++] = (char *)e;
    }
    return kept;
}

/* Splits what arrived into lines for `line`; what is left of a line waits in `pending`. */
static void lines_of(granite_buf *pending, void (*line)(void *, const char *), void *arg, bool flush) {
    size_t start = 0;
    for (size_t i = 0; i < pending->len; i++) {
        if (pending->data[i] != '\n') continue;
        pending->data[i] = '\0';
        if (line) line(arg, pending->data + start);
        pending->data[i] = '\n';
        start = i + 1;
    }
    if (flush && start < pending->len) {
        if (line) line(arg, pending->data + start);
        start = pending->len;
    }
    memmove(pending->data ? pending->data : "", pending->data ? pending->data + start : "", pending->len - start);
    pending->len -= start;
    if (pending->data) pending->data[pending->len] = '\0';
}

granite_command_said granite_command_run(const char *const *arguments, const char *folder,
                                         void (*line)(void *arg, const char *text), void *arg) {
    granite_command_said said = {0};
    int out[2], err[2];
    if (pipe(out) || pipe(err)) {
        said.status = 1;
        said.stdout_text = granite_strdup("");
        said.output = granite_strdup("no pipe for the command");
        return said;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    posix_spawn_file_actions_addclose(&actions, err[0]);
    if (folder) add_chdir(&actions, folder);
    char **environment = clean_environment();
    pid_t pid;
    int spawned = posix_spawnp(&pid, arguments[0], &actions, NULL, (char *const *)arguments, environment);
    free(environment);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    close(err[1]);
    if (spawned != 0) {
        close(out[0]);
        close(err[0]);
        said.status = 127;
        said.stdout_text = granite_strdup("");
        said.output = spawned == ENOENT ? granite_strdupf("%s was not found", arguments[0])
                                        : granite_strdupf("%s could not start: %s", arguments[0], strerror(spawned));
        return said;
    }
    granite_buf stdout_text = {0}, output = {0}, out_pending = {0}, err_pending = {0};
    struct pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    int open_fds = 2;
    char chunk[8192];
    while (open_fds) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(fds[i].fd, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
                open_fds--;
                continue;
            }
            granite_buf_append(&output, chunk, (size_t)n);
            if (i == 0) granite_buf_append(&stdout_text, chunk, (size_t)n);
            granite_buf *pending = i == 0 ? &out_pending : &err_pending;
            granite_buf_append(pending, chunk, (size_t)n);
            lines_of(pending, line, arg, false);
        }
    }
    lines_of(&out_pending, line, arg, true);
    lines_of(&err_pending, line, arg, true);
    granite_buf_free(&out_pending);
    granite_buf_free(&err_pending);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    said.status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    said.stdout_text = granite_buf_take(&stdout_text);
    said.output = granite_buf_take(&output);
    return said;
}

void granite_command_said_free(granite_command_said *said) {
    free(said->stdout_text);
    free(said->output);
    said->stdout_text = said->output = NULL;
}
