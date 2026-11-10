/* A runner as a child process: started in a group of its own, its stdin and stdout a channel, its
 * stderr kept as a tail, and its end reaped with what it cost. GraniteRust's AgentChild.
 *
 * PIN: THE CHILD STARTS CLEAN. posix_spawn puts it in its own process group, with an empty signal
 * mask and TERM, INT, HUP, QUIT and PIPE at their defaults, whatever the host blocks or ignores;
 * on Apple, no descriptor but its three is inherited.
 * PIN: ITS GROUP GOES WITH IT. Terminating sends SIGTERM to the group and SIGKILL after the grace;
 * once the runner is reaped, anything left in its group is killed (Rust's leaves it). */
#ifndef GRANITE_HOST_CHILD_H
#define GRANITE_HOST_CHILD_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "granite_host/remote.h"
#include "granitec/channel.h"

typedef struct granite_child granite_child;

typedef struct granite_child_ended {
    int status; /* as wait4 says it */
    double cpu_seconds;
    int64_t peak_bytes;
} granite_child_ended;

/* Starts `executable` with `arguments` (NULL-terminated, after argv[0]), in `directory` (NULL:
 * here), with exactly `environment` (NULL-terminated KEY=VALUE). NULL, with a spawn error in
 * `*error`, when it could not start. */
granite_child *granite_child_spawn(const char *executable, const char *const *arguments, const char *directory,
                                   char *const *environment, granite_remote_error **error);
granite_channel *granite_child_channel(granite_child *child);
pid_t granite_child_pid(const granite_child *child);
/* Blocks until the child is reaped. */
granite_child_ended granite_child_wait(granite_child *child);
bool granite_child_has_exited(granite_child *child);
/* SIGTERM to its group, and SIGKILL after `grace` seconds when it has not exited. */
void granite_child_terminate(granite_child *child, double grace);
/* The last six lines it printed off the wire, trimmed. The caller frees it. */
char *granite_child_log(granite_child *child);
/* Seconds since it started. */
double granite_child_seconds(const granite_child *child);
/* "exit 1", "signal 9": a wait status, as Rust says it. The caller frees it. */
char *granite_status_text(int status);

#endif
