/* The host's side of granite-agent/1: one runner process per call. GraniteRust's AgentProcess.
 *
 * PIN: ONE PROCESS, ONE CALL. describe starts the runner with --describe and reads its entries;
 * perform starts it, waits for hello (major 1), sends the call, relays notes, answers its asks of
 * the model, and reads its one output or failed. Once the call ends its stdin is closed, and a
 * runner still there after five seconds is terminated (SIGTERM, then SIGKILL after two).
 * PIN: GRANITE_AGENT=1 IS SET for every runner. */
#ifndef GRANITE_HOST_PROCESS_H
#define GRANITE_HOST_PROCESS_H

#include <stdint.h>

#include "granite_host/remote.h"
#include "granitec/context.h"

/* What a call cost: AgentProcessCost. */
typedef struct granite_process_cost {
    double seconds;
    double cpu_seconds;
    int64_t peak_bytes;
    char *status;
    uint32_t asks;
} granite_process_cost;

/* "released · peak 12 MB · 0.1 s CPU in 0.4 s". The caller frees it. */
char *granite_cost_sentence(const granite_process_cost *cost);
json_object *granite_cost_json(const granite_process_cost *cost);
void granite_cost_free(granite_process_cost *cost);

typedef struct granite_process granite_process;

/* A runner at `executable` (relative with a slash: under this folder), started with exactly
 * `environment` (NULL: this process's own) plus GRANITE_AGENT=1. */
granite_process *granite_process_new(const char *executable, char *const *environment);
void granite_process_free(granite_process *process);
const char *granite_process_executable(const granite_process *process);

/* What the runner carries: its entries (an array), or NULL with why in `*error`. */
json_object *granite_process_describe(granite_process *process, granite_remote_error **error);

/* One call of `word` with `input` (JSON bytes), offered `host` (its folder is the runner's working
 * folder; its notes hear the runner's; its model answers the runner's asks, or "this host has no
 * model loaded"). NULL with the output's sorted bytes in `*output`, or why there is none. Setting
 * `cancel` (optional) says `cancel` to the runner and terminates it after two seconds. `cost`
 * (optional) is filled once the runner has ended. */
granite_remote_error *granite_process_perform(granite_process *process, const char *word, const char *input, size_t len,
                                              granite_host *host, granite_cancel *cancel, char **output, size_t *output_len,
                                              granite_process_cost *cost);

#endif
