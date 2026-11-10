/* The agent's side of granite-agent/1. See granitec/server.h.
 *
 * One call: `hello`, then the host's `perform` started on the roster's package on a thread of its
 * own, the host's answers matched to the run's asks of its model, a `cancel` or the host's leaving
 * handled, and exactly one `output` or `failed` said at the end, in GraniteRust's sentences. */
#include "granitec/server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "granitec/buf.h"
#include "granitec/lock.h"
#include "granitec/main.h"
#include "granitec/thread.h"

/* --- the host's model, asked across the channel -------------------------------------------- */

typedef struct ask_wait {
    uint64_t id;
    granite_wire *answer;
    bool cancelled;
    struct ask_wait *next;
} ask_wait;

typedef struct channel_model {
    granite_model base;
    granite_channel *channel;
    granite_lock lock;
    pthread_cond_t answered;
    uint64_t next_id;
    ask_wait *waiting;
    bool gone;
} channel_model;

static void channel_model_deinit(void *self) {
    channel_model *model = self;
    granite_release(model->channel);
    granite_lock_destroy(&model->lock);
}

static const granite_class channel_model_class = {"the host's model", sizeof(channel_model), channel_model_deinit, NULL};

/* The host's answer to an ask; one nobody waits for is dropped. */
static void model_resolve(channel_model *model, granite_wire *answer) {
    granite_lock_acquire(&model->lock);
    for (ask_wait *w = model->waiting; w; w = w->next) {
        if (w->id == answer->id && !w->answer) {
            w->answer = answer;
            answer = NULL;
            pthread_cond_broadcast(&model->answered);
            break;
        }
    }
    granite_lock_release(&model->lock);
    granite_wire_free(answer);
}

/* The host is gone: every ask waiting, and every ask after, ends without an answer. */
static void model_leave(channel_model *model) {
    granite_lock_acquire(&model->lock);
    model->gone = true;
    pthread_cond_broadcast(&model->answered);
    granite_lock_release(&model->lock);
}

typedef struct cancel_wake {
    channel_model *model;
    ask_wait *wait;
} cancel_wake;

static void wake_on_cancel(void *receiver, const void *value, void *arg) {
    cancel_wake *wake = arg;
    granite_lock_acquire(&wake->model->lock);
    wake->wait->cancelled = true;
    pthread_cond_broadcast(&wake->model->answered);
    granite_lock_release(&wake->model->lock);
}

static void free_wake(void *arg) {
    free(arg);
}

/* One ask: sent under a new id, then waited on until the host answers it, leaves, or the call is
 * cancelled. NULL with the answer, or why there is none. */
static granite_error *ask(channel_model *model, granite_wire *message, granite_cancel *cancel, granite_wire **answer) {
    *answer = NULL;
    ask_wait *wait = granite_calloc(1, sizeof *wait);
    granite_lock_acquire(&model->lock);
    if (model->gone) {
        granite_lock_release(&model->lock);
        free(wait);
        granite_wire_free(message);
        return granite_host_no_answer("the host is gone");
    }
    wait->id = ++model->next_id;
    wait->next = model->waiting;
    model->waiting = wait;
    granite_lock_release(&model->lock);
    message->id = wait->id;

    void *listening = granite_anchor_new();
    if (cancel) {
        cancel_wake *wake = granite_calloc(1, sizeof *wake);
        *wake = (cancel_wake){model, wait};
        granite_cancel_on(cancel, listening, wake_on_cancel, wake, free_wake);
    }
    char *why = NULL;
    granite_error *error = NULL;
    if (!granite_channel_send(model->channel, message, &why)) {
        error = granite_host_no_answer("the ask could not be sent: %s", why);
        free(why);
    }
    granite_wire_free(message);

    granite_lock_acquire(&model->lock);
    while (!error && !wait->answer && !wait->cancelled && !model->gone) granite_lock_wait(&model->lock, &model->answered);
    for (ask_wait **at = &model->waiting; *at; at = &(*at)->next) {
        if (*at == wait) {
            *at = wait->next;
            break;
        }
    }
    granite_wire *got = wait->answer;
    bool cancelled = wait->cancelled;
    granite_lock_release(&model->lock);
    granite_release(listening);
    free(wait);
    if (error) {
        granite_wire_free(got);
        return error;
    }
    if (got) {
        *answer = got;
        return NULL;
    }
    return cancelled ? granite_host_no_answer("the call was cancelled") : granite_host_no_answer("the host is gone");
}

static granite_wire *ask_message(granite_ask_kind kind, const char *system, const char *user, uint32_t max_tokens,
                                 double seconds) {
    granite_wire *message = granite_wire_new(GRANITE_WIRE_MODEL);
    message->ask_kind = kind;
    message->system = granite_strdup(system);
    message->user = granite_strdup(user);
    message->max_tokens = max_tokens;
    message->seconds = seconds;
    return message;
}

static granite_error *model_object(granite_model *base, const json_object *schema, const char *system,
                                   const char *user, uint32_t max_tokens, double seconds, granite_cancel *cancel,
                                   json_object **object) {
    channel_model *model = (channel_model *)base;
    granite_wire *message = ask_message(GRANITE_ASK_OBJECT, system, user, max_tokens, seconds);
    message->schema = granite_json_copy(schema);
    granite_wire *answer = NULL;
    granite_error *error = ask(model, message, cancel, &answer);
    if (error) return error;
    if (answer->error) error = granite_host_no_answer("%s", answer->error);
    else if (!answer->object) error = granite_host_no_answer("the host answered no object");
    else {
        *object = answer->object;
        answer->object = NULL;
    }
    granite_wire_free(answer);
    return error;
}

static granite_error *model_text(granite_model *base, const char *system, const char *user, uint32_t max_tokens,
                                 double seconds, granite_cancel *cancel, char **text) {
    channel_model *model = (channel_model *)base;
    granite_wire *answer = NULL;
    granite_error *error = ask(model, ask_message(GRANITE_ASK_TEXT, system, user, max_tokens, seconds), cancel, &answer);
    if (error) return error;
    if (answer->error) {
        error = granite_host_no_answer("%s", answer->error);
    } else {
        *text = answer->text ? answer->text : granite_strdup("");
        answer->text = NULL;
    }
    granite_wire_free(answer);
    return error;
}

static const granite_model_methods channel_model_methods = {model_object, model_text};

static channel_model *channel_model_new(granite_channel *channel) {
    channel_model *model = granite_object_new(&channel_model_class);
    model->base.methods = &channel_model_methods;
    model->channel = granite_retain(channel);
    granite_lock_init(&model->lock, "the host's model");
    pthread_cond_init(&model->answered, NULL);
    return model;
}

/* --- one call ------------------------------------------------------------------------------ */

typedef struct server_call {
    granite_channel *channel;
    const granite_package *const *roster;
    granite_lock lock;
    pthread_cond_t ended_cond;
    /* under lock */
    bool started;
    bool cancelled;
    bool ended;
    granite_exit exit;
    channel_model *model;
    granite_cancel *cancel; /* the run's: set by a cancel */
} server_call;

/* Says `message` (taken) and ends the call with `exit`, once. */
static void finish(server_call *call, granite_exit exit, granite_wire *message) {
    granite_lock_acquire(&call->lock);
    bool first = !call->ended;
    call->ended = true;
    if (first) call->exit = exit;
    granite_lock_release(&call->lock);
    if (first) granite_channel_send(call->channel, message, NULL);
    granite_wire_free(message);
    if (!first) return;
    granite_lock_acquire(&call->lock);
    pthread_cond_broadcast(&call->ended_cond);
    granite_lock_release(&call->lock);
}

static void finish_said(server_call *call, granite_exit exit, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void finish_said(server_call *call, granite_exit exit, const char *fmt, ...) {
    granite_buf said = {0};
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&said, fmt, args);
    va_end(args);
    granite_error *failure = granite_fault("%s", granite_buf_str(&said));
    finish(call, exit, granite_wire_failed(failure));
    granite_error_free(failure);
    granite_buf_free(&said);
}

/* Ends the call: a run is stopped, and says so; before a run, the call ends as cancelled. */
static void cancel_call(server_call *call) {
    granite_lock_acquire(&call->lock);
    call->cancelled = true;
    bool started = call->started;
    granite_cancel *cancel = call->cancel ? granite_retain(call->cancel) : NULL;
    granite_lock_release(&call->lock);
    if (cancel) {
        granite_cancel_set(cancel);
        granite_release(cancel);
    }
    if (!started) finish_said(call, GRANITE_EXIT_CANCELLED, "cancelled before the call began");
}

typedef struct running {
    server_call *call;
    const granite_package *package;
    granite_wire *perform;
    granite_host *host;
    granite_cancel *cancel;
} running;

static void send_note(void *arg, const char *line) {
    granite_wire *note = granite_wire_note(line);
    granite_channel_send(arg, note, NULL);
    granite_wire_free(note);
}

static void release_channel(void *arg) {
    granite_release(arg);
}

static void run_call(void *pointer) {
    running *r = pointer;
    server_call *call = r->call;
    char *input = granite_json_write(r->perform->input, NULL);
    char *answer = NULL;
    size_t answer_len = 0;
    granite_error *error = granite_package_perform(r->package, input, strlen(input), r->host, r->cancel, &answer, &answer_len);
    free(input);
    granite_lock_acquire(&call->lock);
    bool cancelled = call->cancelled;
    granite_lock_release(&call->lock);
    if (!error) {
        json_object *output = NULL;
        if (granite_json_read(answer, answer_len, &output, NULL)) {
            finish(call, GRANITE_EXIT_ANSWERED, granite_wire_output(output));
        } else {
            finish_said(call, GRANITE_EXIT_FAILED, "%s answered something that is not JSON", r->package->word);
        }
    } else if (error->type == GRANITE_ERROR_CANCELLED) {
        finish_said(call, GRANITE_EXIT_CANCELLED, "cancelled");
    } else {
        granite_exit exit = cancelled ? GRANITE_EXIT_CANCELLED
                            : error->type == GRANITE_ERROR_REFUSED ? GRANITE_EXIT_REFUSED : GRANITE_EXIT_FAILED;
        finish(call, exit, granite_wire_failed(error));
    }
    granite_error_free(error);
    free(answer);
    granite_wire_free(r->perform);
    granite_release(r->host);
    granite_release(r->cancel);
    free(r);
}

static void start_call(server_call *call, granite_wire *perform) {
    granite_lock_acquire(&call->lock);
    bool again = call->started || call->ended;
    call->started = true;
    granite_lock_release(&call->lock);
    if (again) {
        granite_wire_free(perform);
        finish_said(call, GRANITE_EXIT_UNREADABLE, "a process answers one call; the host sent a second `perform`");
        return;
    }
    const granite_package *package = granite_roster_find(call->roster, perform->word);
    if (!package) {
        granite_buf carried = {0};
        for (const granite_package *const *p = call->roster; p && *p; p++)
            if (granite_roster_find(call->roster, (*p)->word) == *p)
                granite_buf_appendf(&carried, "%s`%s`", carried.len ? ", " : "", (*p)->word);
        finish_said(call, GRANITE_EXIT_FAILED, "no agent is called `%s` in this process; it carries %s", perform->word,
                    carried.len ? granite_buf_str(&carried) : "none");
        granite_buf_free(&carried);
        granite_wire_free(perform);
        return;
    }
    channel_model *model = perform->model ? channel_model_new(call->channel) : NULL;
    running *r = granite_calloc(1, sizeof *r);
    r->call = call;
    r->package = package;
    r->perform = perform;
    r->host = granite_host_new(perform->directory, perform->ask, model ? &model->base : NULL, send_note,
                               granite_retain(call->channel), release_channel);
    r->cancel = granite_retain(call->cancel);
    granite_lock_acquire(&call->lock);
    call->model = model;
    bool cancelled = call->cancelled;
    granite_lock_release(&call->lock);
    if (cancelled) granite_cancel_set(r->cancel);
    if (!granite_thread_spawn("granite.call", 8u * 1024u * 1024u, run_call, r)) {
        finish_said(call, GRANITE_EXIT_FAILED, "the call could not start a thread: %s", strerror(errno));
        granite_wire_free(r->perform);
        granite_release(r->host);
        granite_release(r->cancel);
        free(r);
    }
}

/* Reads the host until it has said all it will. */
static void listen_to_host(server_call *call) {
    granite_wire *message;
    while ((message = granite_channel_recv(call->channel))) {
        switch (message->kind) {
        case GRANITE_WIRE_PERFORM: start_call(call, message); break;
        case GRANITE_WIRE_ANSWER: {
            granite_lock_acquire(&call->lock);
            channel_model *model = call->model ? granite_retain(call->model) : NULL;
            granite_lock_release(&call->lock);
            if (model) {
                model_resolve(model, message);
                granite_release(model);
            } else {
                granite_wire_free(message);
            }
            break;
        }
        case GRANITE_WIRE_CANCEL:
            granite_wire_free(message);
            cancel_call(call);
            break;
        default:
            finish_said(call, GRANITE_EXIT_UNREADABLE, "the host said `%s`, which is not a host's message",
                        granite_wire_kind_name(message->kind));
            granite_wire_free(message);
        }
    }
    /* The host sends nothing more: a call it made runs on, but no ask of the model can be answered.
     * A host that closes its end at once (echo '{"perform": …}' | runner) is served; only `cancel`
     * or SIGTERM cancels. A host that left before calling ends the process. */
    granite_lock_acquire(&call->lock);
    channel_model *model = call->model ? granite_retain(call->model) : NULL;
    bool called = call->started || call->ended;
    granite_lock_release(&call->lock);
    if (model) {
        model_leave(model);
        granite_release(model);
    }
    if (!called) finish_said(call, GRANITE_EXIT_UNREADABLE, "the host left before making a call");
}

static void listen_thread(void *pointer) {
    listen_to_host(pointer);
}

typedef struct outside_cancel {
    server_call *call;
} outside_cancel;

static void cancel_from_outside(void *receiver, const void *value, void *arg) {
    cancel_call(arg);
}

granite_exit granite_server_serve(granite_channel *channel, const granite_package *const *roster, granite_cancel *cancel) {
    server_call *call = granite_calloc(1, sizeof *call);
    call->channel = granite_retain(channel);
    call->roster = roster;
    call->cancel = granite_cancel_new();
    granite_lock_init(&call->lock, "a server's call");
    pthread_cond_init(&call->ended_cond, NULL);

    granite_wire *hello = granite_wire_hello((int32_t)getpid(), true);
    granite_channel_send(channel, hello, NULL);
    granite_wire_free(hello);

    void *watching = NULL;
    if (cancel) {
        watching = granite_anchor_new();
        granite_cancel_on(cancel, watching, cancel_from_outside, call, NULL);
    }
    /* The listener outlives the call when the host never closes its end: the process exits under it. */
    if (!granite_thread_spawn("granite.listen", 0, listen_thread, call)) {
        perror("granite: the server could not listen");
        abort();
    }
    granite_lock_acquire(&call->lock);
    while (!call->ended) granite_lock_wait(&call->lock, &call->ended_cond);
    granite_exit exit = call->exit;
    granite_lock_release(&call->lock);
    granite_release(watching);
    /* The call and its listener stay: the listener may still be reading. A server answers one call. */
    return exit;
}

/* --- the process --------------------------------------------------------------------------- */

static int terminate_pipe[2] = {-1, -1};

static void on_terminate(int signal_number) {
    int saved = errno;
    char byte = 1;
    if (write(terminate_pipe[1], &byte, 1) < 0) {
    }
    errno = saved;
}

static void watch_terminate(void *pointer) {
    char byte;
    while (read(terminate_pipe[0], &byte, 1) < 0 && errno == EINTR) {
    }
    granite_cancel_set(pointer);
}

static void unreadable_line(void *arg, const char *line, const char *why) {
    fprintf(stderr, "granite-agent: an unreadable line from the host: %s\n", line);
}

int granite_server_main(int argc, char **argv, const granite_package *const *roster) {
    /* The wire is stdout's own descriptor, kept from children; what the process calls stdout is
     * stderr; stdin is the channel's, with /dev/null where it was. */
    int wire = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    int reading = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    if (wire < 0 || reading < 0) {
        perror("granite-agent: the wire could not be set aside");
        exit(GRANITE_EXIT_FAILED);
    }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        if (null != STDIN_FILENO) close(null);
    }
    signal(SIGPIPE, SIG_IGN);
    granite_channel *channel = granite_pipe_channel_new(reading, wire, unreadable_line, NULL);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--describe") == 0) {
            granite_wire *entries = granite_wire_new(GRANITE_WIRE_ENTRIES);
            entries->entries = granite_roster_entries(roster);
            granite_channel_send(channel, entries, NULL);
            granite_wire_free(entries);
            granite_channel_close(channel);
            exit(GRANITE_EXIT_ANSWERED);
        }
    }

    /* SIGTERM cancels the call. A handler that writes to a pipe, not a blocked signal: a mask
     * would be inherited by every child an agent starts. */
    granite_cancel *terminated = granite_cancel_new();
    if (pipe(terminate_pipe) == 0) {
        fcntl(terminate_pipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(terminate_pipe[1], F_SETFD, FD_CLOEXEC);
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_handler = on_terminate;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &action, NULL);
        granite_thread_spawn("granite.term", 0, watch_terminate, granite_retain(terminated));
    }
    granite_exit code = granite_server_serve(channel, roster, terminated);
    granite_channel_close(channel);
    fflush(stderr);
    exit(code);
}
