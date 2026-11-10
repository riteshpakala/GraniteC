/* The agent's side of granite-agent/1: the whole main of a process that carries packages. With
 * --describe it says what it carries and exits; otherwise it answers one call on stdin and its
 * wire, offering the run the host's folder, ask, notes and model over the same pipe, and exits.
 * GraniteRust's AgentServer and ServerCall (Swift's AgentServerCall).
 *
 * PIN: THE WIRE IS STDOUT'S, AND NOBODY ELSE'S. Before anything runs, stdout is moved to a
 * descriptor of the wire's own (not inherited by children), descriptor 1 is pointed at stderr, and
 * stdin is moved aside for the channel with /dev/null left in its place: a package's printf can
 * never put a line on the wire, nor its reads take one off.
 * PIN: EXACTLY ONE `output` OR `failed`, then the exit: 0 answered, 1 failed, 2 the host said
 * something that is not a call or left before making one, 3 refused, 130 cancelled (a `cancel`,
 * or SIGTERM). */
#ifndef GRANITEC_SERVER_H
#define GRANITEC_SERVER_H

#include "granitec/channel.h"
#include "granitec/package.h"

typedef enum granite_exit {
    GRANITE_EXIT_ANSWERED = 0,
    GRANITE_EXIT_FAILED = 1,
    GRANITE_EXIT_UNREADABLE = 2,
    GRANITE_EXIT_REFUSED = 3,
    GRANITE_EXIT_CANCELLED = 130,
} granite_exit;

/* A runner's main: `return granite_server_main(argc, argv, roster);`. The roster ends with NULL;
 * the first package of a word wins. Never returns. */
int granite_server_main(int argc, char **argv, const granite_package *const *roster);

/* One call on `channel`, answered from `roster`: what main runs, without the process around it.
 * `cancel` (optional), set from anywhere, ends the call as a host's `cancel` does. */
granite_exit granite_server_serve(granite_channel *channel, const granite_package *const *roster,
                                  granite_cancel *cancel);

#endif
