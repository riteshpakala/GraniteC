/* Where granite-agent/1 messages travel: one line each way. GraniteRust's AgentChannel and its
 * PipeChannel, over two descriptors (a runner's stdin and its wire, or a host's ends of a child's).
 *
 * PIN: ONE READER. A channel's thread (granite.channel) reads 64 KiB at a time, splits lines, skips
 * blank ones, and reads each as a message; one that does not read is said to `unreadable` and
 * dropped. granite_channel_recv hands the messages out in order.
 * PIN: GRANITE_AGENT_TRACE=1 TRACES EVERY LINE to stderr, as "[granite-agent PID] read …" and
 * "[granite-agent PID] sent …", the first 300 bytes of each. */
#ifndef GRANITEC_CHANNEL_H
#define GRANITEC_CHANNEL_H

#include "granitec/wire.h"

typedef struct granite_channel granite_channel;

/* A channel reading `read_fd` and writing `write_fd` (both its own from here on). `unreadable(arg,
 * line, why)` is told of a line that is not a message (NULL: said nowhere). */
granite_channel *granite_pipe_channel_new(int read_fd, int write_fd,
                                          void (*unreadable)(void *arg, const char *line, const char *why), void *arg);
/* Two channels joined to each other, over pipes: what a test talks across. */
void granite_channel_pair(granite_channel **a, granite_channel **b);
/* Writes `message` as one line; false, with why in `*error` (optional; the caller frees it), when
 * the channel is closed or the line could not be written. Never under a lock. */
bool granite_channel_send(granite_channel *channel, const granite_wire *message, char **error);
/* The next message, blocking; NULL once the other end has said all it will. */
granite_wire *granite_channel_recv(granite_channel *channel);
/* Closes the writing end: the other end reads to its end. Reading goes on. */
void granite_channel_close(granite_channel *channel);

/* What GRANITE_AGENT_TRACE=1 says for one line (`arrow` is "read" or "sent"). */
void granite_trace(const char *arrow, const char *line, size_t len);

#endif
