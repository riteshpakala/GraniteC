/* What a run reaches the world through: whether it was asked to stop, the host that offered it
 * its folder, ask and model, and the way its events reach the one writer. GraniteRust's
 * CancelFlag, AgentHost, AgentHostModel and AgentContext.
 *
 * PIN: CANCELLING IS COOPERATIVE. A run is never killed: its flag is set, an ask of the host's model
 * it is blocked in wakes, and the run is expected to return. Events it delivers after are dropped.
 * PIN: OFFERED, NEVER ASSUMED. A host may offer no folder, no ask and no model; a run that cannot
 * work without the model asks, and the ask refuses (kind model) naming the package.
 * PIN: THE ASK IS NOT A PARAMETER. A model writes a call's input; it never writes what the person asked. */
#ifndef GRANITEC_CONTEXT_H
#define GRANITEC_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#include "granitec/emitter.h"
#include "granitec/error.h"
#include "granitec/json.h"
#include "granitec/object.h"

/* --- cancelling ---------------------------------------------------------------------------- */

typedef struct granite_cancel granite_cancel;

granite_cancel *granite_cancel_new(void);
/* Sets the flag (once; later sets do nothing) and tells whoever listens, on this thread. */
void granite_cancel_set(granite_cancel *cancel);
bool granite_cancel_is_set(const granite_cancel *cancel);
/* Calls `handler(receiver, NULL, arg)` when the flag is set, while `receiver` lives; at once, on
 * this thread, when it is set already. What a blocking wait listens with to wake. */
void granite_cancel_on(granite_cancel *cancel, void *receiver, granite_handler handler, void *arg,
                       void (*arg_release)(void *));

/* --- the host and its model ---------------------------------------------------------------- */

typedef struct granite_model granite_model;

/* How a model answers. Both block until it answers, gives up, or `cancel` is set; both return
 * NULL with the answer, or an error (granite_host_no_answer, granite_host_bad_answer). */
typedef struct granite_model_methods {
    /* A JSON object that fits `schema` (a JSON Schema). */
    granite_error *(*object)(granite_model *model, const json_object *schema, const char *system, const char *user,
                             uint32_t max_tokens, double seconds, granite_cancel *cancel, json_object **object);
    /* Text, whole (the caller frees it). */
    granite_error *(*text)(granite_model *model, const char *system, const char *user, uint32_t max_tokens,
                           double seconds, granite_cancel *cancel, char **text);
} granite_model_methods;

/* A model is an object whose struct starts with this. */
struct granite_model {
    granite_object header;
    const granite_model_methods *methods;
};

typedef struct granite_host granite_host;

/* What a host offers a run: its folder and the person's ask (either may be NULL, both copied), its
 * model (retained, or NULL), and where a line of progress goes (`note(arg, line)`, or NULL to drop
 * them; `note_release(arg)` when the host goes). */
granite_host *granite_host_new(const char *directory, const char *ask, granite_model *model,
                               void (*note)(void *arg, const char *line), void *note_arg, void (*note_release)(void *));
/* A host that offers nothing: a call made with JSON alone. */
granite_host *granite_host_none(void);
const char *granite_host_directory(const granite_host *host);
const char *granite_host_ask(const granite_host *host);
granite_model *granite_host_model(const granite_host *host);
void granite_host_note(const granite_host *host, const char *line);

/* --- the context --------------------------------------------------------------------------- */

typedef struct granite_context granite_context;

/* Delivers `event` (taken) into this run's stream. Safe from any thread. */
void granite_deliver(granite_context *context, json_object *event);
/* A line of progress, through the host. */
void granite_note(granite_context *context, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool granite_context_is_cancelled(const granite_context *context);
/* The flag behind granite_context_is_cancelled, for work handed to another thread (borrowed). */
granite_cancel *granite_context_cancel(const granite_context *context);
/* This run's id, `{word}-{8 hex}` for a package call. */
const char *granite_context_run(const granite_context *context);
/* The package's word. */
const char *granite_context_word(const granite_context *context);
/* What the host offers (borrowed). */
granite_host *granite_context_host(const granite_context *context);
const char *granite_context_directory(const granite_context *context);
const char *granite_context_ask(const granite_context *context);
/* The compute the package made on its first call (borrowed). */
void *granite_context_compute(const granite_context *context);

/* Asks the host's model for a JSON object that fits `schema`; blocks until it answers. NULL with
 * the object in `*object`, or a model refusal (no model, no answer, cancelled). */
granite_error *granite_ask_object(granite_context *context, const json_object *schema, const char *system,
                                  const char *user, uint32_t max_tokens, double seconds, json_object **object);
/* Asks the host's model for text; `*text` is the caller's to free. */
granite_error *granite_ask_text(granite_context *context, const char *system, const char *user, uint32_t max_tokens,
                                double seconds, char **text);

/* For the engine, a host, and tests: a context for `run` of `word`, on `host` (retained), with
 * `cancel` (retained), the package's `compute`, and where events go (`sink(sink_arg, event)`,
 * the event taken; `sink_release(sink_arg)` when the context goes). */
granite_context *granite_context_new(const char *run, const char *word, granite_host *host, granite_cancel *cancel,
                                     void *compute, void (*sink)(void *arg, json_object *event), void *sink_arg,
                                     void (*sink_release)(void *));

#endif
