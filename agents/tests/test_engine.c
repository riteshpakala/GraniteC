/* The run engine: one writer on main, events drained to the taps before a wait settles, lanes,
 * cancels said at once, and the run's own error kept. */
#include <pthread.h>
#include <stdatomic.h>

#include "../src/runs.h"
#include "granite_test.h"
#include "granitec/lock.h"
#include "granitec/main.h"
#include "granitec/thread.h"

/* What a test asks a run to do. */
typedef struct job {
    int events;
    double every;
    enum { FINISH, REFUSE, FAULT, WAIT_FOR_CANCEL } then;
} job;

static atomic_int running = 0, most_at_once = 0, computes = 0, fail_compute = 0;

static granite_error *compute(void *owner, void **made) {
    atomic_fetch_add(&computes, 1);
    if (atomic_load(&fail_compute)) return granite_fault("no GPU here");
    *made = owner;
    return NULL;
}

static granite_error *body(void *owner, granite_context *context, void *pointer) {
    job *j = pointer;
    int now = atomic_fetch_add(&running, 1) + 1;
    int most = atomic_load(&most_at_once);
    while (now > most && !atomic_compare_exchange_weak(&most_at_once, &most, now)) {
    }
    if (granite_main_is_current()) GRANITE_FAIL("a run ran on main");
    for (int i = 1; i <= j->events; i++) {
        granite_sleep(j->every);
        json_object *event = json_object_new_object();
        granite_json_set(event, "n", json_object_new_int(i));
        granite_deliver(context, event);
    }
    granite_error *error = NULL;
    if (j->then == REFUSE) {
        error = granite_refuse(GRANITE_REFUSAL_CALL, "%s needs a title", granite_context_word(context));
    } else if (j->then == FAULT) {
        error = granite_fault("the disk is full");
    } else if (j->then == WAIT_FOR_CANCEL) {
        while (!granite_context_is_cancelled(context)) granite_sleep(0.002);
        json_object *late = json_object_new_object();
        granite_json_set(late, "late", json_object_new_boolean(1));
        granite_deliver(context, late);
    }
    atomic_fetch_sub(&running, 1);
    return error;
}

static job *make_job(int events, double every, int then) {
    job *j = granite_calloc(1, sizeof *j);
    j->events = events;
    j->every = every;
    j->then = then;
    return j;
}

static void free_job(void *j) {
    free(j);
}

/* A tap that keeps "key:n" for every event it hears (on main). */
typedef struct heard {
    granite_buf text;
    atomic_int count; /* polled from the test's thread */
} heard;

static void hear(void *receiver, const void *value, void *arg) {
    const granite_run_event *e = value;
    heard *h = arg;
    if (!granite_main_is_current()) GRANITE_FAIL("a tap heard off main");
    json_object *n = granite_json_get(e->event, "n");
    granite_buf_appendf(&h->text, "%s:%d;", e->key, n ? json_object_get_int(n) : -1);
    atomic_fetch_add(&h->count, 1);
}

static int owner_token;

GRANITE_TEST(a_finished_run_settles_after_its_events_reached_the_taps) {
    granite_runs *runs = granite_runs_new("plate", "PackageRun", 2, &owner_token, compute, body);
    heard h = {{0}, 0};
    void *tap = granite_anchor_new();
    granite_runs_tap(runs, tap, hear, &h, NULL);
    granite_error *error = granite_runs_perform(runs, "plate-1", NULL, make_job(3, 0.001, FINISH), free_job);
    GRANITE_ASSERT(error == NULL);
    GRANITE_ASSERT_STR(granite_buf_str(&h.text), "plate-1:1;plate-1:2;plate-1:3;");
    granite_runs_state state = granite_runs_read_state(runs);
    GRANITE_ASSERT_EQ(state.runs, 1);
    GRANITE_ASSERT_EQ(state.finished, 1);
    GRANITE_ASSERT_EQ(state.events, 3);
    free(state.last_failure);
    granite_release(tap);
    granite_buf_free(&h.text);
    granite_release(runs);
}

GRANITE_TEST(the_runs_own_refusal_keeps_its_kind_and_a_fault_is_said_as_it_is) {
    granite_runs *runs = granite_runs_new("plate", "PackageRun", 2, &owner_token, compute, body);
    granite_error *refused = granite_runs_perform(runs, "a", NULL, make_job(1, 0, REFUSE), free_job);
    GRANITE_ASSERT(refused && refused->type == GRANITE_ERROR_REFUSED);
    GRANITE_ASSERT_EQ(refused->kind, GRANITE_REFUSAL_CALL);
    GRANITE_ASSERT_STR(refused->said, "plate needs a title");
    granite_error *fault = granite_runs_perform(runs, "b", NULL, make_job(0, 0, FAULT), free_job);
    GRANITE_ASSERT(fault && fault->type == GRANITE_ERROR_FAULT);
    GRANITE_ASSERT_STR(fault->said, "the disk is full");
    granite_runs_state state = granite_runs_read_state(runs);
    GRANITE_ASSERT_EQ(state.failed, 2);
    GRANITE_ASSERT_STR(state.last_failure, "the disk is full");
    free(state.last_failure);
    granite_error_free(refused);
    granite_error_free(fault);
    granite_release(runs);
}

GRANITE_TEST(a_compute_that_fails_is_the_engines_sentence) {
    granite_runs *runs = granite_runs_new("plate", "PackageRun", 2, &owner_token, compute, body);
    atomic_store(&fail_compute, 1);
    granite_error *first = granite_runs_perform(runs, "a", NULL, make_job(1, 0, FINISH), free_job);
    granite_error *second = granite_runs_perform(runs, "b", NULL, make_job(1, 0, FINISH), free_job);
    atomic_store(&fail_compute, 0);
    GRANITE_ASSERT(first && first->type == GRANITE_ERROR_FAULT);
    GRANITE_ASSERT_STR(first->said, "PackageRun-1 failed: no GPU here");
    GRANITE_ASSERT_STR(second->said, "PackageRun-2 failed: no GPU here");
    granite_error_free(first);
    granite_error_free(second);
    granite_release(runs);
}

typedef struct performer {
    granite_runs *runs;
    char key[16];
    job *job;
    granite_error *result;
} performer;

static void *perform_on_a_thread(void *pointer) {
    performer *p = pointer;
    p->result = granite_runs_perform(p->runs, p->key, NULL, p->job, free_job);
    return NULL;
}

GRANITE_TEST(lanes_bound_how_many_run_at_once_and_queued_runs_start_in_order) {
    granite_runs *runs = granite_runs_new("plate", "PackageRun", 2, &owner_token, compute, body);
    atomic_store(&most_at_once, 0);
    performer performers[6];
    pthread_t threads[6];
    for (int i = 0; i < 6; i++) {
        performers[i] = (performer){runs, "", make_job(3, 0.004, FINISH), NULL};
        snprintf(performers[i].key, sizeof performers[i].key, "call-%d", i);
        pthread_create(&threads[i], NULL, perform_on_a_thread, &performers[i]);
    }
    for (int i = 0; i < 6; i++) pthread_join(threads[i], NULL);
    for (int i = 0; i < 6; i++) GRANITE_ASSERT(performers[i].result == NULL);
    GRANITE_ASSERT_EQ(atomic_load(&most_at_once), 2);
    size_t live = 9, waiting = 9;
    granite_runs_count(runs, &live, &waiting);
    GRANITE_ASSERT_EQ(live, 0);
    GRANITE_ASSERT_EQ(waiting, 0);
    granite_release(runs);
}

GRANITE_TEST(a_cancel_settles_at_once_and_drops_what_the_run_says_after) {
    granite_runs *runs = granite_runs_new("plate", "PackageRun", 1, &owner_token, compute, body);
    heard h = {{0}, 0};
    void *tap = granite_anchor_new();
    granite_runs_tap(runs, tap, hear, &h, NULL);
    performer live = {runs, "live", make_job(1, 0, WAIT_FOR_CANCEL), NULL};
    performer queued = {runs, "queued", make_job(1, 0, FINISH), NULL};
    pthread_t first, second;
    pthread_create(&first, NULL, perform_on_a_thread, &live);
    for (int i = 0; i < 200 && atomic_load(&h.count) < 1; i++) granite_sleep(0.005);
    pthread_create(&second, NULL, perform_on_a_thread, &queued);
    size_t lanes = 0, waiting = 0;
    for (int i = 0; i < 200 && waiting < 1; i++) {
        granite_sleep(0.005);
        granite_runs_count(runs, &lanes, &waiting);
    }
    GRANITE_ASSERT_EQ(waiting, 1);
    granite_runs_cancel(runs, "queued");
    pthread_join(second, NULL);
    GRANITE_ASSERT(queued.result && queued.result->type == GRANITE_ERROR_CANCELLED);
    granite_runs_cancel(runs, "live");
    pthread_join(first, NULL);
    GRANITE_ASSERT(live.result && live.result->type == GRANITE_ERROR_CANCELLED);
    GRANITE_ASSERT_STR(live.result->said, "cancelled");
    granite_sleep(0.05);
    granite_main_turn();
    GRANITE_ASSERT_STR(granite_buf_str(&h.text), "live:1;"); /* the late event was dropped */
    granite_runs_state state = granite_runs_read_state(runs);
    GRANITE_ASSERT_EQ(state.cancelled, 2);
    GRANITE_ASSERT_EQ(state.runs, 1);
    free(state.last_failure);
    granite_error_free(live.result);
    granite_error_free(queued.result);
    granite_release(tap);
    granite_buf_free(&h.text);
    granite_release(runs);
}

static void perform_from_main(void *pointer) {
    granite_runs *runs = pointer;
    granite_error *error = granite_runs_perform(runs, "x", NULL, make_job(0, 0, FINISH), free_job);
    if (!error || error->type != GRANITE_ERROR_FAULT) GRANITE_FAIL("performing from main was not refused");
    granite_error_free(error);
}

GRANITE_TEST(performing_from_main_is_refused_rather_than_waiting_on_itself) {
    granite_runs *runs = granite_runs_new("plate", "PackageRun", 2, &owner_token, compute, body);
    granite_main_call(perform_from_main, runs);
    granite_release(runs);
}

int main(void) {
    long before = granite_objects_alive();
    GRANITE_RUN(a_finished_run_settles_after_its_events_reached_the_taps);
    GRANITE_RUN(the_runs_own_refusal_keeps_its_kind_and_a_fault_is_said_as_it_is);
    GRANITE_RUN(a_compute_that_fails_is_the_engines_sentence);
    GRANITE_RUN(lanes_bound_how_many_run_at_once_and_queued_runs_start_in_order);
    GRANITE_RUN(a_cancel_settles_at_once_and_drops_what_the_run_says_after);
    GRANITE_RUN(performing_from_main_is_refused_rather_than_waiting_on_itself);
    granite_sleep(0.05);
    granite_main_turn();
    if (granite_objects_alive() != before) {
        fprintf(stderr, "  %ld objects outlived the tests\n", granite_objects_alive() - before);
        granite_test_failures++;
    }
    GRANITE_TEST_MAIN_END();
}
