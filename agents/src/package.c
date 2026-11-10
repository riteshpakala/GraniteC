/* A package, its one engine in a process, and a call through it. See granitec/package.h.
 *
 * The pipeline is GraniteRust's AnyAgentPackage::perform step for step: read the input, check it,
 * tap the run's events, perform, map what it ended with, read the answer off its events, check it
 * against granite-answer/1, stamp it, and write it sorted. */
#include "granitec/package.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "granitec/buf.h"
#include "granitec/lock.h"
#include "runs.h"

const char *granite_package_type(const granite_package *package, char *buffer, size_t size) {
    if (package->type) return package->type;
    snprintf(buffer, size, "%s.%s", package->module ? package->module : "", package->word);
    return buffer;
}

int granite_package_lanes(const granite_package *package) {
    if (package->lanes == 0) return 2;
    return package->lanes < 0 ? -1 : package->lanes;
}

/* --- describing ---------------------------------------------------------------------------- */

static json_object *make_entry(const granite_make *make) {
    json_object *entry = json_object_new_object(), *into = json_object_new_array();
    granite_json_set_string(entry, "kind", make->kind);
    granite_json_set_string(entry, "what", make->what ? make->what : "");
    for (const char *const *where = make->into; where && *where; where++)
        json_object_array_add(into, json_object_new_string(*where));
    granite_json_set(entry, "into", into);
    granite_json_set_string(entry, "never", make->never);
    return entry;
}

json_object *granite_package_entry(const granite_package *package) {
    char type[512];
    json_object *entry = json_object_new_object();
    granite_json_set_string(entry, "kind", "package");
    granite_json_set_string(entry, "word", package->word);
    granite_json_set_string(entry, "summary", package->summary ? package->summary : "");
    granite_json_set_string(entry, "module", package->module ? package->module : "");
    granite_json_set_string(entry, "type", granite_package_type(package, type, sizeof type));
    granite_json_set(entry, "schema", granite_schema_value(package->input));
    granite_json_set(entry, "fields", granite_schema_notes(package->input));
    int lanes = granite_package_lanes(package);
    if (lanes > 0) granite_json_set(entry, "lanes", json_object_new_int(lanes));
    if (package->guide && *package->guide) granite_json_set_string(entry, "guide", package->guide);
    if (package->makes && package->makes->kind) {
        json_object *makes = json_object_new_array();
        for (const granite_make *make = package->makes; make->kind; make++) json_object_array_add(makes, make_entry(make));
        granite_json_set(entry, "makes", makes);
    }
    return entry;
}

const granite_package *granite_roster_find(const granite_package *const *roster, const char *word) {
    for (const granite_package *const *p = roster; p && *p; p++)
        if (strcmp((*p)->word, word) == 0) return *p;
    return NULL;
}

json_object *granite_roster_entries(const granite_package *const *roster) {
    json_object *entries = json_object_new_array();
    for (const granite_package *const *p = roster; p && *p; p++)
        if (granite_roster_find(roster, (*p)->word) == *p) json_object_array_add(entries, granite_package_entry(*p));
    return entries;
}

/* --- the package's host: one engine, and its compute made once ----------------------------- */

typedef struct package_host {
    const granite_package *package;
    granite_runs *runs;
    granite_lock lock;
    pthread_cond_t made_cond;
    enum { COMPUTE_IDLE, COMPUTE_MAKING, COMPUTE_MADE } compute_state;
    void *compute;
    struct package_host *next;
} package_host;

/* Process-lifetime: a package's host is made on its first call and kept. */
static struct {
    granite_lock lock;
    package_host *hosts;
} hosts;
static pthread_once_t hosts_started = PTHREAD_ONCE_INIT;

static void start_hosts(void) {
    granite_lock_init(&hosts.lock, "the package hosts");
}

/* Rust's OnceCell::get_or_try_init: one caller makes it while the rest wait; a failure leaves it
 * for the next caller to try. */
static granite_error *make_compute(void *owner, void **compute) {
    package_host *host = owner;
    if (!host->package->compute) {
        *compute = NULL;
        return NULL;
    }
    granite_lock_acquire(&host->lock);
    while (host->compute_state == COMPUTE_MAKING) granite_lock_wait(&host->lock, &host->made_cond);
    if (host->compute_state == COMPUTE_MADE) {
        *compute = host->compute;
        granite_lock_release(&host->lock);
        return NULL;
    }
    host->compute_state = COMPUTE_MAKING;
    granite_lock_release(&host->lock);
    void *made = NULL;
    granite_error *error = host->package->compute(&made);
    granite_lock_acquire(&host->lock);
    if (error) {
        host->compute_state = COMPUTE_IDLE;
    } else {
        host->compute = made;
        host->compute_state = COMPUTE_MADE;
    }
    pthread_cond_broadcast(&host->made_cond);
    granite_lock_release(&host->lock);
    *compute = made;
    return error;
}

static granite_error *run_body(void *owner, granite_context *context, void *job) {
    package_host *host = owner;
    return host->package->run(context, job);
}

static package_host *host_of(const granite_package *package) {
    pthread_once(&hosts_started, start_hosts);
    granite_lock_acquire(&hosts.lock);
    package_host *found = hosts.hosts;
    while (found && found->package != package) found = found->next;
    if (!found) {
        found = granite_calloc(1, sizeof *found);
        found->package = package;
        granite_lock_init(&found->lock, "a package's compute");
        pthread_cond_init(&found->made_cond, NULL);
        found->runs = granite_runs_new(package->word, "PackageRun", granite_package_lanes(package), found, make_compute,
                                       run_body);
        found->next = hosts.hosts;
        hosts.hosts = found;
    }
    granite_lock_release(&hosts.lock);
    return found;
}

/* --- a call -------------------------------------------------------------------------------- */

/* `{word}-{8 hex}`, mixed as Rust mixes it: the clock's nanoseconds, the process, and a count. */
static char *call_id(const char *word) {
    static atomic_ulong next = 1;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t nanos = (uint64_t)now.tv_nsec;
    uint64_t count = atomic_fetch_add(&next, 1);
    uint64_t mixed = (nanos ^ ((uint64_t)getpid() << 16)) + count * 0x9E3779B9u;
    return granite_strdupf("%s-%08llx", word, (unsigned long long)(mixed & 0xffffffffu));
}

/* The tap that keeps a copy of every event of one call, on main. Owned by its subscription, not
 * by the call: main may still be in the handler, hearing another call's event, as the call ends. */
typedef struct collected {
    char *key;
    json_object *events; /* taken by the call once its run has settled */
} collected;

static void collect(void *receiver, const void *value, void *arg) {
    const granite_run_event *heard = value;
    collected *c = arg;
    if (strcmp(heard->key, c->key) == 0) json_object_array_add(c->events, granite_json_copy(heard->event));
}

static void free_collected(void *arg) {
    collected *c = arg;
    free(c->key);
    granite_json_put(c->events);
    free(c);
}

typedef struct cancelling {
    granite_runs *runs;
    char *key;
} cancelling;

static void cancel_call(void *receiver, const void *value, void *arg) {
    cancelling *c = arg;
    granite_runs_cancel(c->runs, c->key);
}

static void free_cancelling(void *arg) {
    cancelling *c = arg;
    free(c->key);
    free(c);
}

static void put_job(void *job) {
    granite_json_put(job);
}

static granite_error *input_refusal(const char *word, char *reason) {
    char *said = granite_strdupf("%s could not read its input: %s", word, reason);
    const char *reasons[] = {reason};
    granite_error *refused = granite_refuse_said(GRANITE_REFUSAL_INPUT, said, reasons, 1);
    free(said);
    free(reason);
    return refused;
}

granite_error *granite_package_perform(const granite_package *package, const char *bytes, size_t len,
                                       granite_host *host, granite_cancel *cancel, char **answer, size_t *answer_len) {
    const char *word = package->word;
    *answer = NULL;
    if (answer_len) *answer_len = 0;

    json_object *input = NULL;
    char *reason = NULL;
    if (!granite_json_read(bytes, len, &input, &reason)) return input_refusal(word, reason);
    reason = granite_schema_check(package->input, input);
    if (reason) {
        granite_json_put(input);
        return input_refusal(word, reason);
    }

    package_host *ours = host_of(package);
    char *id = call_id(word);
    /* The tap before the run, so no event is missed; it ends with the call. */
    collected *heard = granite_calloc(1, sizeof *heard);
    heard->key = granite_strdup(id);
    heard->events = json_object_new_array();
    void *tap = granite_anchor_new();
    granite_runs_tap(ours->runs, tap, collect, heard, free_collected);
    void *watching = NULL;
    if (cancel) {
        watching = granite_anchor_new();
        cancelling *c = granite_calloc(1, sizeof *c);
        c->runs = ours->runs;
        c->key = granite_strdup(id);
        granite_cancel_on(cancel, watching, cancel_call, c, free_cancelling);
    }
    granite_error *ended = granite_runs_perform(ours->runs, id, host, cancel, input, put_job);
    granite_release(watching);
    /* Settled: every event of this call reached the tap, and none comes after. */
    json_object *events = heard->events;
    heard->events = NULL;
    granite_release(tap);
    /* The engine says a run's end as what the run returned: a refusal keeps its kind, a fault is
     * said as it is, and a cancel is a cancel. */
    if (ended) {
        granite_json_put(events);
        free(id);
        return ended;
    }

    json_object *output = NULL;
    if (package->output) {
        granite_lock_check("asked a package for its output");
        output = package->output(events);
    } else {
        size_t n = json_object_array_length(events);
        output = n ? granite_json_copy(json_object_array_get_idx(events, n - 1)) : NULL;
    }
    bool answered = package->output ? output != NULL : json_object_array_length(events) > 0;
    granite_json_put(events);
    free(id);
    if (!answered) return granite_refuse(GRANITE_REFUSAL_ANSWER, "%s finished without an answer", word);
    if (json_object_get_type(output) != json_type_object) {
        granite_json_put(output);
        return granite_refuse(GRANITE_REFUSAL_ANSWER, "%s answered something that is not an object", word);
    }

    size_t count = 0;
    char **problems = granite_answer_problems(output, word, package->makes, granite_host_directory(host), &count);
    if (count) {
        char *text = granite_refusal_text((const char *const *)problems, count);
        char *said = granite_strdupf("%s's answer breaks %s: %s", word, GRANITE_ANSWER_FORMAT, text);
        granite_error *refused = granite_refuse_said(GRANITE_REFUSAL_ANSWER, said, (const char *const *)problems, count);
        free(said);
        free(text);
        for (size_t i = 0; i < count; i++) free(problems[i]);
        free(problems);
        granite_json_put(output);
        return refused;
    }
    free(problems);
    json_object *stamped = granite_answer_stamped(output, word);
    granite_json_put(output);
    *answer = granite_json_write(stamped, answer_len);
    granite_json_put(stamped);
    return NULL;
}
