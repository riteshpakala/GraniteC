/* The host's side of granite-agent/1. See granite_host/process.h. */
#include "granite_host/process.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "granite_host/child.h"
#include "granitec/buf.h"
#include "granitec/lock.h"
#include "granitec/thread.h"

extern char **environ;

char *granite_cost_sentence(const granite_process_cost *cost) {
    double megabytes = (double)cost->peak_bytes / 1048576.0;
    char peak[64];
    if (megabytes >= 1024.0) snprintf(peak, sizeof peak, "%.1f GB", megabytes / 1024.0);
    else snprintf(peak, sizeof peak, "%.0f MB", megabytes);
    return granite_strdupf("released · peak %s · %.1f s CPU in %.1f s", peak, cost->cpu_seconds, cost->seconds);
}

json_object *granite_cost_json(const granite_process_cost *cost) {
    json_object *json = json_object_new_object();
    granite_json_set(json, "seconds", json_object_new_double(cost->seconds));
    granite_json_set(json, "cpuSeconds", json_object_new_double(cost->cpu_seconds));
    granite_json_set(json, "peakBytes", json_object_new_int64(cost->peak_bytes));
    granite_json_set_string(json, "status", cost->status ? cost->status : "");
    granite_json_set(json, "asks", json_object_new_int64(cost->asks));
    return json;
}

void granite_cost_free(granite_process_cost *cost) {
    free(cost->status);
    cost->status = NULL;
}

struct granite_process {
    char *executable;
    char **environment;
};

granite_process *granite_process_new(const char *executable, char *const *environment) {
    granite_process *process = granite_calloc(1, sizeof *process);
    char here[PATH_MAX];
    if (executable[0] != '/' && strchr(executable, '/') && getcwd(here, sizeof here))
        process->executable = granite_strdupf("%s/%s", here, executable);
    else
        process->executable = granite_strdup(executable);
    char *const *from = environment ? environment : environ;
    size_t count = 0;
    while (from && from[count]) count++;
    process->environment = granite_calloc(count + 2, sizeof *process->environment);
    size_t k = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(from[i], "GRANITE_AGENT=", 14) != 0) process->environment[k++] = granite_strdup(from[i]);
    process->environment[k] = granite_strdup("GRANITE_AGENT=1");
    return process;
}

void granite_process_free(granite_process *process) {
    if (!process) return;
    for (char **e = process->environment; *e; e++) free(*e);
    free(process->environment);
    free(process->executable);
    free(process);
}

const char *granite_process_executable(const granite_process *process) {
    return process->executable;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static granite_remote_error *ended(const char *word, granite_child *child, const granite_child_ended *end) {
    granite_remote_error *error = granite_remote_new(GRANITE_REMOTE_ENDED, word);
    error->status = granite_status_text(end->status);
    error->log = granite_child_log(child);
    return error;
}

json_object *granite_process_describe(granite_process *process, granite_remote_error **error) {
    *error = NULL;
    const char *arguments[] = {"--describe", NULL};
    granite_child *child = granite_child_spawn(process->executable, arguments, NULL, process->environment, error);
    if (!child) return NULL;
    granite_channel *channel = granite_child_channel(child);
    granite_channel_close(channel);
    json_object *entries = NULL;
    granite_wire *message;
    while (!entries && (message = granite_channel_recv(channel))) {
        if (message->kind == GRANITE_WIRE_ENTRIES) {
            entries = message->entries;
            message->entries = NULL;
        }
        granite_wire_free(message);
    }
    granite_child_ended end = granite_child_wait(child);
    if (!entries) *error = ended(base_name(process->executable), child, &end);
    granite_release(child);
    return entries;
}

/* --- answering the runner's asks ----------------------------------------------------------- */

typedef struct answering {
    granite_channel *channel;
    granite_host *host;
    granite_wire *ask;
} answering;

static void answer_ask(void *pointer) {
    answering *a = pointer;
    granite_wire *answer = granite_wire_new(GRANITE_WIRE_ANSWER);
    answer->id = a->ask->id;
    granite_model *model = granite_host_model(a->host);
    if (!model) {
        answer->error = granite_strdup("this host has no model loaded");
    } else {
        granite_error *error;
        if (a->ask->ask_kind == GRANITE_ASK_OBJECT) {
            json_object *schema = a->ask->schema ? a->ask->schema : json_object_new_object();
            error = model->methods->object(model, schema, a->ask->system, a->ask->user, a->ask->max_tokens,
                                           a->ask->seconds, NULL, &answer->object);
            if (!a->ask->schema) granite_json_put(schema);
        } else {
            error = model->methods->text(model, a->ask->system, a->ask->user, a->ask->max_tokens, a->ask->seconds, NULL,
                                         &answer->text);
        }
        if (error) {
            granite_json_put(answer->object);
            answer->object = NULL;
            free(answer->text);
            answer->text = NULL;
            answer->error = granite_strdup(error->said);
            granite_error_free(error);
        }
    }
    granite_channel_send(a->channel, answer, NULL);
    granite_wire_free(answer);
    granite_wire_free(a->ask);
    granite_release(a->channel);
    granite_release(a->host);
    free(a);
}

/* --- cancelling ---------------------------------------------------------------------------- */

static void cancel_runner(void *receiver, const void *value, void *arg) {
    granite_child *child = arg;
    granite_wire *cancel = granite_wire_cancel();
    granite_channel_send(granite_child_channel(child), cancel, NULL);
    granite_wire_free(cancel);
    granite_child_terminate(child, 2.0);
}

static void release_child(void *arg) {
    granite_release(arg);
}

/* --- a call -------------------------------------------------------------------------------- */

static granite_remote_error *unreadable(const char *word, char *reason) {
    granite_remote_error *error = granite_remote_new(GRANITE_REMOTE_UNREADABLE, word);
    error->reason = reason;
    return error;
}

/* Reads the runner until its one output or failed; NULL with the output, or why there is none. */
static granite_remote_error *serve(granite_child *child, const char *word, granite_wire *call, granite_host *host,
                                   uint32_t *asks, json_object **output) {
    granite_channel *channel = granite_child_channel(child);
    bool sent = false, standard = false;
    granite_remote_error *result = NULL;
    for (;;) {
        granite_wire *message = granite_channel_recv(channel);
        if (!message) {
            result = granite_remote_new(GRANITE_REMOTE_ENDED, word);
            break;
        }
        bool done = true;
        switch (message->kind) {
        case GRANITE_WIRE_HELLO: {
            const char *wire = message->wire;
            const char *major = strncmp(wire, "granite-agent/", 14) == 0 ? wire + 14 : NULL;
            char *end = NULL;
            long number = major && *major ? strtol(major, &end, 10) : -1;
            if (!major || !*major || *end || number != GRANITE_WIRE_MAJOR) {
                result = unreadable(word, granite_strdupf("it speaks %s, and this host %s", wire, GRANITE_WIRE_VERSION));
                break;
            }
            done = false;
            if (sent) break;
            sent = true;
            standard = message->answer_standard != NULL;
            char *why = NULL;
            if (!granite_channel_send(channel, call, &why)) {
                result = unreadable(word, granite_strdupf("the call could not be sent: %s", why));
                free(why);
                done = true;
            }
            break;
        }
        case GRANITE_WIRE_NOTE:
            granite_host_note(host, message->note);
            done = false;
            break;
        case GRANITE_WIRE_MODEL: {
            (*asks)++;
            answering *a = granite_calloc(1, sizeof *a);
            a->channel = granite_retain(channel);
            a->host = granite_retain(host);
            a->ask = message;
            message = NULL;
            if (!granite_thread_spawn("granite.answer", 0, answer_ask, a)) answer_ask(a);
            done = false;
            break;
        }
        case GRANITE_WIRE_OUTPUT:
            *output = message->output;
            message->output = NULL;
            break;
        case GRANITE_WIRE_FAILED: {
            granite_error *failure = message->failure;
            if (failure->type == GRANITE_ERROR_REFUSED) {
                result = granite_remote_new(GRANITE_REMOTE_REFUSED, word);
                result->refusal = granite_error_copy(failure);
            } else {
                result = granite_remote_new(standard ? GRANITE_REMOTE_FAULT : GRANITE_REMOTE_FAILED, word);
            }
            result->message = granite_strdup(failure->said);
            break;
        }
        default:
            result = unreadable(word, granite_strdupf("it said `%s` during a call", granite_wire_kind_name(message->kind)));
        }
        granite_wire_free(message);
        if (done) break;
    }
    return result;
}

typedef struct straggler {
    granite_child *child;
} straggler;

/* A runner still there five seconds after its call ended is terminated. */
static void end_straggler(void *pointer) {
    granite_child *child = pointer;
    double deadline = granite_now() + 5.0;
    while (!granite_child_has_exited(child) && granite_now() < deadline) granite_sleep(0.02);
    if (!granite_child_has_exited(child)) granite_child_terminate(child, 2.0);
    granite_release(child);
}

granite_remote_error *granite_process_perform(granite_process *process, const char *word, const char *input, size_t len,
                                              granite_host *host, granite_cancel *cancel, char **output, size_t *output_len,
                                              granite_process_cost *cost) {
    *output = NULL;
    if (output_len) *output_len = 0;
    json_object *json = NULL;
    if (!granite_json_read(input, len, &json, NULL)) return unreadable(word, granite_strdup("the input is not JSON"));
    granite_host *offered = host ? granite_retain(host) : granite_host_none();
    granite_remote_error *error = NULL;
    granite_child *child =
        granite_child_spawn(process->executable, NULL, granite_host_directory(offered), process->environment, &error);
    if (!child) {
        granite_json_put(json);
        granite_release(offered);
        return error;
    }
    granite_wire *call = granite_wire_new(GRANITE_WIRE_PERFORM);
    call->word = granite_strdup(word);
    call->input = json;
    call->directory = granite_strdup(granite_host_directory(offered));
    call->ask = granite_strdup(granite_host_ask(offered));
    call->model = granite_host_model(offered) != NULL;

    void *watching = NULL;
    if (cancel) {
        watching = granite_anchor_new();
        granite_cancel_on(cancel, watching, cancel_runner, granite_retain(child), release_child);
    }
    uint32_t asks = 0;
    json_object *said = NULL;
    granite_remote_error *result = serve(child, word, call, offered, &asks, &said);
    granite_release(watching);
    granite_wire_free(call);
    granite_channel_close(granite_child_channel(child));
    if (!granite_thread_spawn("granite.straggler", 0, end_straggler, granite_retain(child))) granite_release(child);
    granite_child_ended end = granite_child_wait(child);
    if (cost) {
        cost->seconds = granite_child_seconds(child);
        cost->cpu_seconds = end.cpu_seconds;
        cost->peak_bytes = end.peak_bytes;
        cost->status = granite_status_text(end.status);
        cost->asks = asks;
    }
    if (result && result->type == GRANITE_REMOTE_ENDED) {
        granite_remote_free(result);
        result = ended(word, child, &end);
    }
    if (!result) *output = granite_json_write(said, output_len);
    granite_json_put(said);
    granite_release(child);
    granite_release(offered);
    return result;
}
