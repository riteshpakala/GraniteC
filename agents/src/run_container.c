/* The engine behind a package's calls. See runs.h. */
#include "runs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "granitec/buf.h"
#include "granitec/lock.h"
#include "granitec/main.h"
#include "granitec/thread.h"

/* A run's thread gets a stack big enough for what an author writes on it. */
#define RUN_STACK (8u * 1024u * 1024u)

typedef struct waiter {
    granite_lock lock;
    pthread_cond_t settled_cond;
    bool settled;
    granite_error *result;
} waiter;

/* One run, live or waiting for a lane: an object, since its thread and main both hold it. */
typedef struct run_record {
    granite_object header;
    granite_runs *runs;
    char *raw;
    char *key;
    granite_cancel *cancel;
    granite_host *host;
    granite_cancel *requested;
    void *job;
    void (*job_release)(void *);
    waiter *waiter; /* main-only: whoever performs it, until it settles */
} run_record;

typedef enum frame_type { FRAME_EVENT, FRAME_ENDED } frame_type;

/* What a run's thread hands main: an event, or how it ended. */
typedef struct frame {
    run_record *run;
    frame_type type;
    json_object *event;
    bool cancelled;
    granite_error *error; /* FRAME_ENDED: NULL when it finished */
    struct frame *next;
} frame;

struct granite_runs {
    granite_object header;
    char *word;
    char *label;
    int max;
    void *owner;
    granite_runs_compute compute;
    granite_runs_body body;
    granite_emitter *taps;
    /* main-only */
    run_record **live;
    size_t live_count, live_capacity;
    run_record **waiting;
    size_t waiting_count, waiting_capacity;
    uint64_t counter;
    granite_runs_state state;
    /* the inbox: any thread in, main out */
    granite_lock lock;
    frame *head, *tail;
    bool drain_scheduled;
};

static void runs_deinit(void *self) {
    granite_runs *runs = self;
    free(runs->word);
    free(runs->label);
    granite_release(runs->taps);
    free(runs->live);
    free(runs->waiting);
    free(runs->state.last_failure);
    granite_lock_destroy(&runs->lock);
}

static const granite_class runs_class = {"run engine", sizeof(granite_runs), runs_deinit, NULL};

static void record_deinit(void *self) {
    run_record *r = self;
    if (r->job_release) r->job_release(r->job);
    granite_release(r->host);
    granite_release(r->requested);
    granite_release(r->cancel);
    granite_release(r->runs);
    free(r->raw);
    free(r->key);
}

static const granite_class record_class = {"run", sizeof(run_record), record_deinit, NULL};

granite_runs *granite_runs_new(const char *word, const char *label, int max, void *owner,
                               granite_runs_compute compute, granite_runs_body body) {
    granite_runs *runs = granite_object_new(&runs_class);
    runs->word = granite_strdup(word);
    runs->label = granite_strdup(label ? label : "PackageRun");
    runs->max = max;
    runs->owner = owner;
    runs->compute = compute;
    runs->body = body;
    runs->taps = granite_emitter_new("a run engine's taps");
    granite_lock_init(&runs->lock, "a run engine's inbox");
    return runs;
}

void granite_runs_tap(granite_runs *runs, void *receiver, granite_handler handler, void *arg,
                      void (*arg_release)(void *)) {
    granite_emitter_on(runs->taps, receiver, NULL, handler, arg, arg_release);
}

/* --- main: the live set and the queue ------------------------------------------------------ */

static void push(run_record ***list, size_t *count, size_t *capacity, run_record *r) {
    if (*count == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 4;
        *list = granite_realloc(*list, *capacity * sizeof **list);
    }
    (*list)[(*count)++] = r;
}

/* Takes `r` out of `list`, keeping the order; false when it was not there. */
static bool take(run_record **list, size_t *count, const run_record *r) {
    for (size_t i = 0; i < *count; i++) {
        if (list[i] == r) {
            memmove(list + i, list + i + 1, (*count - i - 1) * sizeof *list);
            (*count)--;
            return true;
        }
    }
    return false;
}

static bool is_live(const granite_runs *runs, const run_record *r) {
    for (size_t i = 0; i < runs->live_count; i++)
        if (runs->live[i] == r) return true;
    return false;
}

static void set_failure(granite_runs *runs, const char *said) {
    free(runs->state.last_failure);
    runs->state.last_failure = granite_strdup(said);
}

/* Main: the waiter resumes with `result` (taken). */
static void settle(run_record *r, granite_error *result) {
    waiter *w = r->waiter;
    r->waiter = NULL;
    if (!w) {
        granite_error_free(result);
        return;
    }
    granite_lock_acquire(&w->lock);
    w->result = result;
    w->settled = true;
    pthread_cond_signal(&w->settled_cond);
    granite_lock_release(&w->lock);
}

static void launch(granite_runs *runs, run_record *r);

/* Main: starts what waits for a lane while there is room. */
static void start_waiting(granite_runs *runs) {
    while (runs->waiting_count && (runs->max < 1 || runs->live_count < (size_t)runs->max)) {
        run_record *next = runs->waiting[0];
        take(runs->waiting, &runs->waiting_count, next);
        launch(runs, next);
    }
}

/* --- the run's thread ---------------------------------------------------------------------- */

static void drain(void *pointer);

static void enqueue(granite_runs *runs, frame *f) {
    granite_lock_acquire(&runs->lock);
    if (runs->tail) runs->tail->next = f;
    else runs->head = f;
    runs->tail = f;
    bool schedule = !runs->drain_scheduled;
    runs->drain_scheduled = true;
    granite_lock_release(&runs->lock);
    if (schedule) granite_main_enqueue(drain, runs);
}

static void deliver_event(void *arg, json_object *event) {
    run_record *r = arg;
    frame *f = granite_calloc(1, sizeof *f);
    f->run = granite_retain(r);
    f->type = FRAME_EVENT;
    f->event = event;
    enqueue(r->runs, f);
}

static void release_record(void *r) {
    granite_release(r);
}

static void run_thread(void *pointer) {
    run_record *r = pointer;
    granite_runs *runs = r->runs;
    frame *ended = granite_calloc(1, sizeof *ended);
    ended->run = r; /* the thread's reference, handed to the frame */
    ended->type = FRAME_ENDED;
    void *compute = NULL;
    granite_lock_check("started a run");
    granite_error *made = runs->compute ? runs->compute(runs->owner, &compute) : NULL;
    if (made) {
        /* Not the run's own error: the engine says it, as Rust's AgentRunError::Failed. */
        if (granite_cancel_is_set(r->cancel)) ended->cancelled = true;
        else ended->error = granite_fault("%s failed: %s", r->raw, made->said);
        granite_error_free(made);
    } else {
        granite_context *context = granite_context_new(r->key, runs->word, r->host, r->cancel, compute, deliver_event,
                                                       granite_retain(r), release_record);
        granite_error *error = runs->body(runs->owner, context, r->job);
        granite_release(context);
        if (granite_cancel_is_set(r->cancel)) {
            ended->cancelled = true;
            granite_error_free(error);
        } else {
            ended->error = error;
        }
    }
    enqueue(runs, ended);
}

/* Main: the run takes a lane and its thread starts. */
static void launch(granite_runs *runs, run_record *r) {
    push(&runs->live, &runs->live_count, &runs->live_capacity, r);
    runs->state.runs++;
    granite_retain(r); /* the thread's */
    if (!granite_thread_spawn("granite.run", RUN_STACK, run_thread, r)) {
        frame *ended = granite_calloc(1, sizeof *ended);
        ended->run = r;
        ended->type = FRAME_ENDED;
        ended->error = granite_fault("%s failed: a thread could not start (%s)", r->raw, strerror(errno));
        enqueue(runs, ended);
    }
}

/* --- main: draining ------------------------------------------------------------------------ */

static void drain(void *pointer) {
    granite_runs *runs = pointer;
    granite_lock_acquire(&runs->lock);
    frame *f = runs->head;
    runs->head = runs->tail = NULL;
    runs->drain_scheduled = false;
    granite_lock_release(&runs->lock);
    while (f) {
        frame *next = f->next;
        run_record *r = f->run;
        /* A frame of a run that was cancelled is dropped: the run is over for whoever listens. */
        if (is_live(runs, r)) {
            if (f->type == FRAME_EVENT) {
                runs->state.events++;
                granite_run_event heard = {r->key, f->event};
                granite_emitter_send(runs->taps, &heard);
            } else {
                take(runs->live, &runs->live_count, r);
                if (f->cancelled) {
                    runs->state.cancelled++;
                    settle(r, granite_cancelled());
                } else if (f->error) {
                    runs->state.failed++;
                    set_failure(runs, f->error->said);
                    settle(r, f->error);
                    f->error = NULL;
                } else {
                    runs->state.finished++;
                    settle(r, NULL);
                }
                granite_release(r); /* the live set's */
                start_waiting(runs);
            }
        }
        granite_json_put(f->event);
        granite_error_free(f->error);
        granite_release(f->run);
        free(f);
        f = next;
    }
}

/* --- performing and cancelling ------------------------------------------------------------- */

typedef struct start {
    granite_runs *runs;
    run_record *record;
} start;

static void start_turn(void *pointer) {
    start *s = pointer;
    granite_runs *runs = s->runs;
    run_record *r = s->record;
    runs->counter++;
    r->raw = granite_strdupf("%s-%llu", runs->label, (unsigned long long)runs->counter);
    /* Cancelled before it could start: it ends as a queued run cancelled would. */
    if (granite_cancel_is_set(r->requested)) {
        runs->state.cancelled++;
        settle(r, granite_cancelled());
        granite_release(r);
        return;
    }
    /* A key that is live, or waiting, is the same call asked for again: the newer replaces it. */
    for (size_t i = 0; i < runs->live_count; i++) {
        if (strcmp(runs->live[i]->key, r->key) == 0) {
            granite_runs_cancel(runs, r->key);
            break;
        }
    }
    for (size_t i = 0; i < runs->waiting_count; i++) {
        run_record *queued = runs->waiting[i];
        if (strcmp(queued->key, r->key) == 0) {
            take(runs->waiting, &runs->waiting_count, queued);
            runs->state.cancelled++;
            settle(queued, granite_cancelled());
            granite_release(queued);
            break;
        }
    }
    if (runs->max >= 1 && runs->live_count >= (size_t)runs->max) {
        push(&runs->waiting, &runs->waiting_count, &runs->waiting_capacity, r);
        return;
    }
    launch(runs, r);
}

granite_error *granite_runs_perform(granite_runs *runs, const char *key, granite_host *host, granite_cancel *requested,
                                    void *job, void (*job_release)(void *)) {
    if (granite_main_is_current()) {
        if (job_release) job_release(job);
        return granite_fault("a run was performed from main, which would wait on itself");
    }
    waiter w = {.settled = false, .result = NULL};
    granite_lock_init(&w.lock, "a perform's wait");
    pthread_cond_init(&w.settled_cond, NULL);
    run_record *r = granite_object_new(&record_class);
    r->runs = granite_retain(runs);
    r->key = granite_strdup(key);
    r->cancel = granite_cancel_new();
    r->host = host ? granite_retain(host) : granite_host_none();
    r->requested = granite_retain(requested);
    r->job = job;
    r->job_release = job_release;
    r->waiter = &w;
    /* One main turn starts the run and opens its wait, so nothing settles in between. The live set
     * or the queue now holds the record. */
    start s = {runs, r};
    granite_main_call(start_turn, &s);
    granite_lock_acquire(&w.lock);
    while (!w.settled) granite_lock_wait(&w.lock, &w.settled_cond);
    granite_lock_release(&w.lock);
    pthread_cond_destroy(&w.settled_cond);
    granite_lock_destroy(&w.lock);
    return w.result;
}

typedef struct cancel_request {
    granite_runs *runs;
    char *key;
} cancel_request;

static void cancel_turn(void *pointer) {
    cancel_request *c = pointer;
    granite_runs *runs = c->runs;
    for (size_t i = 0; i < runs->live_count; i++) {
        run_record *r = runs->live[i];
        if (strcmp(r->key, c->key) != 0) continue;
        take(runs->live, &runs->live_count, r);
        granite_cancel_set(r->cancel);
        runs->state.cancelled++;
        settle(r, granite_cancelled());
        granite_release(r);
        start_waiting(runs);
        goto done;
    }
    for (size_t i = 0; i < runs->waiting_count; i++) {
        run_record *r = runs->waiting[i];
        if (strcmp(r->key, c->key) != 0) continue;
        take(runs->waiting, &runs->waiting_count, r);
        runs->state.cancelled++;
        settle(r, granite_cancelled());
        granite_release(r);
        break;
    }
done:
    granite_release(runs);
    free(c->key);
    free(c);
}

void granite_runs_cancel(granite_runs *runs, const char *key) {
    cancel_request *c = granite_calloc(1, sizeof *c);
    c->runs = granite_retain(runs);
    c->key = granite_strdup(key);
    granite_main_run(cancel_turn, c);
}

typedef struct state_read {
    granite_runs *runs;
    granite_runs_state state;
    size_t live, waiting;
} state_read;

static void read_turn(void *pointer) {
    state_read *read = pointer;
    read->state = read->runs->state;
    read->state.last_failure = granite_strdup(read->runs->state.last_failure);
    read->live = read->runs->live_count;
    read->waiting = read->runs->waiting_count;
}

granite_runs_state granite_runs_read_state(granite_runs *runs) {
    state_read read = {.runs = runs};
    granite_main_call(read_turn, &read);
    return read.state;
}

void granite_runs_count(granite_runs *runs, size_t *live, size_t *waiting) {
    state_read read = {.runs = runs};
    granite_main_call(read_turn, &read);
    free(read.state.last_failure);
    if (live) *live = read.live;
    if (waiting) *waiting = read.waiting;
}
