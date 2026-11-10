/* The engine behind a package's calls: GraniteRust's RunContainer (Swift's AgentRunContainer),
 * with the one policy package calls need, parallel up to the package's lanes, since every call
 * has a key of its own.
 *
 * PIN: EVERY CHANGE IS MAIN'S. Starting, cancelling, draining and settling are turns on
 * granite.main, as a Granite send is; a run's thread only ever enqueues.
 * PIN: A WAIT SETTLES AFTER EVERYTHING IS SAID. A finished run's waiter resumes in the drain that
 * read its lifecycle, after its last event reached the taps; a cancelled one's in the cancel's turn.
 * PIN: A CANCEL IS SAID AT ONCE. The run's flag is set, its waiter settles cancelled, and whatever
 * its thread enqueues after is dropped.
 * PIN: THE RUN'S OWN ERROR IS KEPT. What the run returned is handed to its waiter as it was, so a
 * refusal keeps its kind. */
#ifndef GRANITEC_RUNS_H
#define GRANITEC_RUNS_H

#include <stdint.h>

#include "granitec/context.h"

typedef struct granite_runs granite_runs;

/* Makes the compute every run shares, on a run's thread, blocking (the package's compute()). */
typedef granite_error *(*granite_runs_compute)(void *owner, void **compute);
/* The run, on its own thread, blocking (the package's run()). `job` is what perform was given. */
typedef granite_error *(*granite_runs_body)(void *owner, granite_context *context, void *job);

/* What the engine counted, as GraniteRust's PackageState. */
typedef struct granite_runs_state {
    uint64_t runs, finished, cancelled, failed, events;
    char *last_failure; /* the caller frees it */
} granite_runs_state;

/* An engine for `word`'s calls: at most `max` at once (below 1: no bound), runs named
 * `{label}-{n}`. `owner` is handed to `compute` and `body`. */
granite_runs *granite_runs_new(const char *word, const char *label, int max, void *owner,
                               granite_runs_compute compute, granite_runs_body body);

/* One event a tap hears: whose run it is, and the event (borrowed for the call). */
typedef struct granite_run_event {
    const char *key;
    const json_object *event;
} granite_run_event;

/* Every event of every run, as it is drained, on main, while `receiver` lives. */
void granite_runs_tap(granite_runs *runs, void *receiver, granite_handler handler, void *arg,
                      void (*arg_release)(void *));

/* Starts a run keyed `key` (or queues it for a lane), offered `host`, with `job` (released with
 * `job_release` when the run is over), and blocks until it settles: NULL when it finished, else
 * what it ended with (the caller frees it). Never call it from main. */
granite_error *granite_runs_perform(granite_runs *runs, const char *key, granite_host *host, void *job,
                                    void (*job_release)(void *));
/* Cancels the run keyed `key`, live or waiting for a lane, from any thread. */
void granite_runs_cancel(granite_runs *runs, const char *key);
/* What the engine has counted, read on main. */
granite_runs_state granite_runs_read_state(granite_runs *runs);
/* How many runs are live and how many wait for a lane, read on main. */
void granite_runs_count(granite_runs *runs, size_t *live, size_t *waiting);

#endif
