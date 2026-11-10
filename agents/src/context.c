/* The cancel flag, the host, and a run's context. See granitec/context.h. */
#include "granitec/context.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "granitec/buf.h"

/* --- cancelling ---------------------------------------------------------------------------- */

struct granite_cancel {
    granite_object header;
    atomic_bool set;
    granite_emitter *emitter;
};

static void cancel_deinit(void *self) {
    granite_release(((granite_cancel *)self)->emitter);
}

static const granite_class cancel_class = {"cancel flag", sizeof(granite_cancel), cancel_deinit, NULL};

granite_cancel *granite_cancel_new(void) {
    granite_cancel *cancel = granite_object_new(&cancel_class);
    atomic_init(&cancel->set, false);
    cancel->emitter = granite_emitter_new("a cancel flag's listeners");
    return cancel;
}

void granite_cancel_set(granite_cancel *cancel) {
    if (atomic_exchange(&cancel->set, true)) return;
    granite_emitter_send(cancel->emitter, NULL);
}

bool granite_cancel_is_set(const granite_cancel *cancel) {
    return cancel && atomic_load(&((granite_cancel *)cancel)->set);
}

void granite_cancel_on(granite_cancel *cancel, void *receiver, granite_handler handler, void *arg,
                       void (*arg_release)(void *)) {
    granite_emitter_on(cancel->emitter, receiver, NULL, handler, arg, arg_release);
    /* Set before the subscription was there to hear it: say so now. A wake may come twice. */
    if (granite_cancel_is_set(cancel)) handler(receiver, NULL, arg);
}

/* --- the host ------------------------------------------------------------------------------ */

struct granite_host {
    granite_object header;
    char *directory;
    char *ask;
    granite_model *model;
    void (*note)(void *arg, const char *line);
    void *note_arg;
    void (*note_release)(void *);
};

static void host_deinit(void *self) {
    granite_host *host = self;
    free(host->directory);
    free(host->ask);
    granite_release(host->model);
    if (host->note_release) host->note_release(host->note_arg);
}

static const granite_class host_class = {"host", sizeof(granite_host), host_deinit, NULL};

granite_host *granite_host_new(const char *directory, const char *ask, granite_model *model,
                               void (*note)(void *arg, const char *line), void *note_arg, void (*note_release)(void *)) {
    granite_host *host = granite_object_new(&host_class);
    host->directory = granite_strdup(directory);
    host->ask = granite_strdup(ask);
    host->model = granite_retain(model);
    host->note = note;
    host->note_arg = note_arg;
    host->note_release = note_release;
    return host;
}

granite_host *granite_host_none(void) {
    return granite_host_new(NULL, NULL, NULL, NULL, NULL, NULL);
}

const char *granite_host_directory(const granite_host *host) {
    return host ? host->directory : NULL;
}

const char *granite_host_ask(const granite_host *host) {
    return host ? host->ask : NULL;
}

granite_model *granite_host_model(const granite_host *host) {
    return host ? host->model : NULL;
}

void granite_host_note(const granite_host *host, const char *line) {
    if (host && host->note) host->note(host->note_arg, line);
}

/* --- the context --------------------------------------------------------------------------- */

struct granite_context {
    granite_object header;
    char *run;
    char *word;
    granite_host *host;
    granite_cancel *cancel;
    void *compute;
    void (*sink)(void *arg, json_object *event);
    void *sink_arg;
    void (*sink_release)(void *);
};

static void context_deinit(void *self) {
    granite_context *context = self;
    free(context->run);
    free(context->word);
    granite_release(context->host);
    granite_release(context->cancel);
    if (context->sink_release) context->sink_release(context->sink_arg);
}

static const granite_class context_class = {"context", sizeof(granite_context), context_deinit, NULL};

granite_context *granite_context_new(const char *run, const char *word, granite_host *host, granite_cancel *cancel,
                                     void *compute, void (*sink)(void *arg, json_object *event), void *sink_arg,
                                     void (*sink_release)(void *)) {
    granite_context *context = granite_object_new(&context_class);
    context->run = granite_strdup(run ? run : "");
    context->word = granite_strdup(word ? word : "");
    context->host = host ? granite_retain(host) : granite_host_none();
    context->cancel = cancel ? granite_retain(cancel) : granite_cancel_new();
    context->compute = compute;
    context->sink = sink;
    context->sink_arg = sink_arg;
    context->sink_release = sink_release;
    return context;
}

void granite_deliver(granite_context *context, json_object *event) {
    if (context->sink) context->sink(context->sink_arg, event);
    else granite_json_put(event);
}

void granite_note(granite_context *context, const char *fmt, ...) {
    granite_buf line = {0};
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&line, fmt, args);
    va_end(args);
    granite_host_note(context->host, granite_buf_str(&line));
    granite_buf_free(&line);
}

bool granite_context_is_cancelled(const granite_context *context) {
    return granite_cancel_is_set(context->cancel);
}

granite_cancel *granite_context_cancel(const granite_context *context) {
    return context->cancel;
}

const char *granite_context_run(const granite_context *context) {
    return context->run;
}

const char *granite_context_word(const granite_context *context) {
    return context->word;
}

granite_host *granite_context_host(const granite_context *context) {
    return context->host;
}

const char *granite_context_directory(const granite_context *context) {
    return granite_host_directory(context->host);
}

const char *granite_context_ask(const granite_context *context) {
    return granite_host_ask(context->host);
}

void *granite_context_compute(const granite_context *context) {
    return context->compute;
}

granite_error *granite_ask_object(granite_context *context, const json_object *schema, const char *system,
                                  const char *user, uint32_t max_tokens, double seconds, json_object **object) {
    *object = NULL;
    granite_model *model = granite_host_model(context->host);
    if (!model) return granite_host_no_model(context->word);
    if (granite_context_is_cancelled(context)) return granite_host_no_answer("the call was cancelled");
    return model->methods->object(model, schema, system ? system : "", user ? user : "", max_tokens, seconds,
                                  context->cancel, object);
}

granite_error *granite_ask_text(granite_context *context, const char *system, const char *user, uint32_t max_tokens,
                                double seconds, char **text) {
    *text = NULL;
    granite_model *model = granite_host_model(context->host);
    if (!model) return granite_host_no_model(context->word);
    if (granite_context_is_cancelled(context)) return granite_host_no_answer("the call was cancelled");
    return model->methods->text(model, system ? system : "", user ? user : "", max_tokens, seconds, context->cancel,
                                text);
}
