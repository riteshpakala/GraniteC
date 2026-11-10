/* The emitter's contract, ported from Granite's EmitterTests: receivers are held weakly and end
 * with their owners, keys replace and tell who they displaced, delivery is ordered and
 * re-entrant, nothing is released under the lock, and concurrent use stays consistent. */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>

#include "granite_test.h"
#include "granitec/buf.h"
#include "granitec/emitter.h"
#include "granitec/lock.h"

/* What the handlers saw, in order. */
typedef struct recorder {
    granite_lock lock;
    char items[1100][24];
    int count;
} recorder;

static void recorder_init(recorder *r) {
    memset(r, 0, sizeof *r);
    granite_lock_init(&r->lock, "the recorder's lock");
}

static void record(recorder *r, const char *fmt, ...) {
    char item[24];
    va_list args;
    va_start(args, fmt);
    vsnprintf(item, sizeof item, fmt, args);
    va_end(args);
    granite_lock_acquire(&r->lock);
    if (r->count < 1100) memcpy(r->items[r->count++], item, sizeof item);
    granite_lock_release(&r->lock);
}

static int recorded(recorder *r) {
    granite_lock_acquire(&r->lock);
    int count = r->count;
    granite_lock_release(&r->lock);
    return count;
}

/* "a,b,c": what was recorded, joined. */
static const char *joined(recorder *r, char *out, size_t n) {
    out[0] = '\0';
    granite_lock_acquire(&r->lock);
    for (int i = 0; i < r->count; i++) {
        if (i) strncat(out, ",", n - strlen(out) - 1);
        strncat(out, r->items[i], n - strlen(out) - 1);
    }
    granite_lock_release(&r->lock);
    return out;
}

/* A receiver that counts how often an emitter displaced it. */
typedef struct displaced_probe {
    granite_object header;
    atomic_int displaced;
} displaced_probe;

static void probe_displaced(void *self) {
    atomic_fetch_add(&((displaced_probe *)self)->displaced, 1);
}

static const granite_class displaced_probe_class = {"displaced probe", sizeof(displaced_probe), NULL, probe_displaced};

/* A handler's arg: which recorder, under which label. */
typedef struct labelled {
    recorder *log;
    char label[16];
} labelled;

static labelled *label(recorder *log, const char *name) {
    labelled *l = granite_calloc(1, sizeof *l);
    l->log = log;
    snprintf(l->label, sizeof l->label, "%s", name);
    return l;
}

static void free_arg(void *arg) {
    free(arg);
}

static void log_label_and_value(void *receiver, const void *value, void *arg) {
    labelled *l = arg;
    record(l->log, "%s %d", l->label, *(const int *)value);
}

static void log_label(void *receiver, const void *value, void *arg) {
    labelled *l = arg;
    record(l->log, "%s", l->label);
}

static void log_value(void *receiver, const void *value, void *arg) {
    record(arg, "%d", *(const int *)value);
}

static void ignore(void *receiver, const void *value, void *arg) {
}

static void send_int(granite_emitter *emitter, int value) {
    granite_emitter_send(emitter, &value);
}

GRANITE_TEST(receivers_run_in_subscription_order) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *first = granite_anchor_new(), *second = granite_anchor_new();
    granite_emitter_on(emitter, first, NULL, log_label_and_value, label(&log, "first"), free_arg);
    granite_emitter_on(emitter, second, NULL, log_label_and_value, label(&log, "second"), free_arg);
    send_int(emitter, 1);
    char text[256];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "first 1,second 1");
    granite_release(first);
    granite_release(second);
    granite_release(emitter);
}

static void check_handed(void *receiver, const void *value, void *arg) {
    record(arg, "%s", receiver == *(void *const *)value ? "true" : "false");
}

GRANITE_TEST(the_handler_is_handed_its_receiver) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *receiver = granite_anchor_new();
    granite_emitter_on(emitter, receiver, NULL, check_handed, &log, NULL);
    granite_emitter_send(emitter, &receiver);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "true");
    granite_release(receiver);
    granite_release(emitter);
}

GRANITE_TEST(releasing_the_receiver_ends_the_subscription) {
    long before = granite_objects_alive();
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *receiver = granite_anchor_new();
    granite_emitter_on(emitter, receiver, NULL, log_value, &log, NULL);
    send_int(emitter, 1);
    GRANITE_ASSERT_EQ(granite_emitter_receiver_count(emitter), 1);
    granite_release(receiver);
    GRANITE_ASSERT_EQ(granite_emitter_receiver_count(emitter), 0);
    send_int(emitter, 2);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "1");
    granite_release(emitter);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

GRANITE_TEST(a_key_replaces_its_subscription_and_moves_it_last) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *first = granite_anchor_new(), *middle = granite_anchor_new(), *last = granite_anchor_new();
    granite_emitter_on(emitter, first, "slot", log_label, label(&log, "first"), free_arg);
    granite_emitter_on(emitter, middle, NULL, log_label, label(&log, "middle"), free_arg);
    granite_emitter_on(emitter, last, "slot", log_label, label(&log, "last"), free_arg);
    send_int(emitter, 1);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "middle,last");
    GRANITE_ASSERT_EQ(granite_emitter_receiver_count(emitter), 2);
    granite_release(first);
    granite_release(middle);
    granite_release(last);
    granite_release(emitter);
}

GRANITE_TEST(replacing_a_key_tells_the_displaced_receiver) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    displaced_probe *first = granite_object_new(&displaced_probe_class);
    displaced_probe *second = granite_object_new(&displaced_probe_class);
    granite_emitter_on(emitter, first, "slot", ignore, NULL, NULL);
    granite_emitter_on(emitter, second, "slot", ignore, NULL, NULL);
    GRANITE_ASSERT_EQ(atomic_load(&first->displaced), 1);
    GRANITE_ASSERT_EQ(atomic_load(&second->displaced), 0);
    granite_emitter_on(emitter, second, "slot", ignore, NULL, NULL);
    GRANITE_ASSERT_EQ(atomic_load(&second->displaced), 0); /* replacing its own subscription is not a displacement */
    granite_release(first);
    granite_release(second);
    granite_release(emitter);
}

GRANITE_TEST(removing_all_tells_every_receiver) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    displaced_probe *first = granite_object_new(&displaced_probe_class);
    displaced_probe *second = granite_object_new(&displaced_probe_class);
    granite_emitter_on(emitter, first, NULL, ignore, NULL, NULL);
    granite_emitter_on(emitter, second, NULL, ignore, NULL, NULL);
    granite_emitter_remove_all(emitter);
    GRANITE_ASSERT_EQ(granite_emitter_receiver_count(emitter), 0);
    GRANITE_ASSERT_EQ(atomic_load(&first->displaced), 1);
    GRANITE_ASSERT_EQ(atomic_load(&second->displaced), 1);
    granite_release(first);
    granite_release(second);
    granite_release(emitter);
}

GRANITE_TEST(off_matches_the_receiver_and_the_key) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *receiver = granite_anchor_new(), *other = granite_anchor_new();
    granite_emitter_on(emitter, receiver, "a", log_label, label(&log, "a"), free_arg);
    granite_emitter_on(emitter, receiver, "b", log_label, label(&log, "b"), free_arg);
    granite_emitter_on(emitter, other, "c", log_label, label(&log, "c"), free_arg);
    granite_emitter_off(emitter, other, "a");
    granite_emitter_off(emitter, receiver, "a");
    send_int(emitter, 1);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "b,c");
    granite_emitter_off(emitter, receiver, NULL);
    send_int(emitter, 2);
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "b,c,c");
    granite_release(receiver);
    granite_release(other);
    granite_release(emitter);
}

/* The first handler lets the second receiver go. */
typedef struct letting_go {
    recorder *log;
    void *held;
} letting_go;

static void log_and_let_go(void *receiver, const void *value, void *arg) {
    letting_go *l = arg;
    record(l->log, "first");
    void *held = l->held;
    l->held = NULL;
    granite_release(held);
}

GRANITE_TEST(a_receiver_released_earlier_in_the_same_send_is_not_reached) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *first = granite_anchor_new();
    letting_go holder = {&log, granite_anchor_new()};
    granite_emitter_on(emitter, first, NULL, log_and_let_go, &holder, NULL);
    granite_emitter_on(emitter, holder.held, NULL, log_label, label(&log, "second"), free_arg);
    send_int(emitter, 1);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "first");
    granite_release(first);
    granite_release(emitter);
}

typedef struct offing {
    recorder *log;
    granite_emitter *emitter;
    void *target;
} offing;

static void log_and_off(void *receiver, const void *value, void *arg) {
    offing *o = arg;
    record(o->log, "first %d", *(const int *)value);
    granite_emitter_off(o->emitter, o->target, NULL);
}

GRANITE_TEST(off_during_a_send_takes_effect_from_the_next_send) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *first = granite_anchor_new(), *second = granite_anchor_new();
    offing o = {&log, emitter, second};
    granite_emitter_on(emitter, first, NULL, log_and_off, &o, NULL);
    granite_emitter_on(emitter, second, NULL, log_label_and_value, label(&log, "second"), free_arg);
    send_int(emitter, 1);
    send_int(emitter, 2);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "first 1,second 1,first 2");
    granite_release(first);
    granite_release(second);
    granite_release(emitter);
}

typedef struct reentrant {
    recorder *log;
    granite_emitter *emitter;
    void *late;
} reentrant;

static void log_times_ten(void *receiver, const void *value, void *arg) {
    record(arg, "%d", *(const int *)value * 10);
}

static void log_then_subscribe_and_send(void *receiver, const void *value, void *arg) {
    reentrant *r = arg;
    int v = *(const int *)value;
    record(r->log, "%d", v);
    if (v == 1) {
        granite_emitter_on(r->emitter, r->late, NULL, log_times_ten, r->log, NULL);
        send_int(r->emitter, 2);
    }
}

GRANITE_TEST(handlers_may_send_and_subscribe_reentrantly) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *receiver = granite_anchor_new(), *late = granite_anchor_new();
    reentrant r = {&log, emitter, late};
    granite_emitter_on(emitter, receiver, NULL, log_then_subscribe_and_send, &r, NULL);
    send_int(emitter, 1);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "1,2,20");
    granite_release(receiver);
    granite_release(late);
    granite_release(emitter);
}

static void log_true(void *receiver, const void *value, void *arg) {
    record(arg, "true");
}

GRANITE_TEST(a_value_less_emitter_sends) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *receiver = granite_anchor_new();
    granite_emitter_on(emitter, receiver, NULL, log_true, &log, NULL);
    granite_emitter_send(emitter, NULL);
    GRANITE_ASSERT_EQ(recorded(&log), 1);
    granite_release(receiver);
    granite_release(emitter);
}

/* Nothing is released under the lock. A handler's arg is let go when its subscription goes; if
 * letting it go sends on the same emitter, the emitter's lock must already be free. Under it, the
 * send would take the lock again and the held-lock check would abort the test. */
typedef struct sends_on_release {
    granite_emitter *emitter;
    int value;
} sends_on_release;

static sends_on_release *sends(int value, granite_emitter *emitter) {
    sends_on_release *s = granite_calloc(1, sizeof *s);
    s->emitter = granite_retain(emitter);
    s->value = value;
    return s;
}

static void send_on_release(void *arg) {
    sends_on_release *s = arg;
    send_int(s->emitter, s->value);
    granite_release(s->emitter);
    free(s);
}

GRANITE_TEST(a_handler_released_by_a_replaced_key_may_send_to_its_emitter) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *first = granite_anchor_new(), *second = granite_anchor_new();
    granite_emitter_on(emitter, first, "slot", ignore, sends(99, emitter), send_on_release);
    granite_emitter_on(emitter, second, "slot", log_value, &log, NULL);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "99");
    granite_release(first);
    granite_release(second);
    granite_release(emitter);
}

GRANITE_TEST(a_handler_released_by_off_may_send_to_its_emitter) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *leaving = granite_anchor_new(), *staying = granite_anchor_new();
    granite_emitter_on(emitter, staying, NULL, log_value, &log, NULL);
    granite_emitter_on(emitter, leaving, NULL, ignore, sends(7, emitter), send_on_release);
    granite_emitter_off(emitter, leaving, NULL);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "7");
    granite_release(leaving);
    granite_release(staying);
    granite_release(emitter);
}

GRANITE_TEST(a_handler_released_by_remove_all_may_send_to_its_emitter) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    void *receiver = granite_anchor_new();
    granite_emitter_on(emitter, receiver, NULL, ignore, sends(1, emitter), send_on_release);
    granite_emitter_remove_all(emitter);
    GRANITE_ASSERT_EQ(granite_emitter_receiver_count(emitter), 0);
    granite_release(receiver);
    granite_release(emitter);
}

GRANITE_TEST(a_handler_released_when_its_receiver_is_compacted_may_send_to_its_emitter) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *staying = granite_anchor_new(), *gone = granite_anchor_new();
    granite_emitter_on(emitter, staying, NULL, log_value, &log, NULL);
    granite_emitter_on(emitter, gone, NULL, ignore, sends(5, emitter), send_on_release);
    granite_release(gone);
    /* send finds the released receiver, lets its snapshot go, then compacts after the unlock. */
    send_int(emitter, 0);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "0,5");
    granite_release(staying);
    granite_release(emitter);
}

/* The old signals deadlocked here: a release that changes the emitter it is leaving. */
typedef struct changes_on_release {
    granite_emitter *emitter;
    void *late;
    recorder *log;
    int clear;
} changes_on_release;

static void change_on_release(void *arg) {
    changes_on_release *c = arg;
    if (c->clear) {
        granite_emitter_remove_all(c->emitter);
    } else {
        granite_emitter_on(c->emitter, c->late, NULL, log_times_ten, c->log, NULL);
        granite_emitter_off(c->emitter, c->late, "none");
    }
    granite_release(c->emitter);
    free(c);
}

GRANITE_TEST(a_release_that_changes_its_emitter_does_not_trip_the_lock) {
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *leaving = granite_anchor_new(), *staying = granite_anchor_new(), *late = granite_anchor_new();
    granite_emitter_on(emitter, staying, NULL, log_value, &log, NULL);
    changes_on_release *subscribe = granite_calloc(1, sizeof *subscribe);
    *subscribe = (changes_on_release){granite_retain(emitter), late, &log, 0};
    granite_emitter_on(emitter, leaving, NULL, ignore, subscribe, change_on_release);
    granite_emitter_off(emitter, leaving, NULL);
    send_int(emitter, 1);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "1,10");

    changes_on_release *clear = granite_calloc(1, sizeof *clear);
    *clear = (changes_on_release){granite_retain(emitter), NULL, &log, 1};
    granite_emitter_on(emitter, leaving, NULL, ignore, clear, change_on_release);
    granite_emitter_off(emitter, leaving, NULL);
    GRANITE_ASSERT_EQ(granite_emitter_receiver_count(emitter), 0);
    granite_release(leaving);
    granite_release(staying);
    granite_release(late);
    granite_release(emitter);
}

GRANITE_TEST(an_emitter_let_go_lets_its_handlers_go) {
    long before = granite_objects_alive();
    granite_emitter *emitter = granite_emitter_new(NULL);
    granite_emitter *other = granite_emitter_new(NULL);
    recorder log;
    recorder_init(&log);
    void *receiver = granite_anchor_new(), *watching = granite_anchor_new();
    granite_emitter_on(other, watching, NULL, log_value, &log, NULL);
    granite_emitter_on(emitter, receiver, NULL, ignore, sends(4, other), send_on_release);
    granite_release(emitter);
    char text[64];
    GRANITE_ASSERT_STR(joined(&log, text, sizeof text), "4");
    granite_release(receiver);
    granite_release(watching);
    granite_release(other);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

typedef struct worker {
    granite_emitter *emitter;
    int base;
} worker;

static void *churn(void *pointer) {
    worker *w = pointer;
    for (int step = 0; step < 125; step++) {
        int index = w->base + step;
        void *transient = granite_anchor_new();
        char key[24];
        snprintf(key, sizeof key, "transient %d", index % 7);
        granite_emitter_on(w->emitter, transient, key, ignore, NULL, NULL);
        send_int(w->emitter, index);
        granite_release(transient);
    }
    return NULL;
}

GRANITE_TEST(concurrent_send_subscribe_and_release_stay_consistent) {
    long before = granite_objects_alive();
    granite_emitter *emitter = granite_emitter_new(NULL);
    recorder *delivered = granite_calloc(1, sizeof *delivered);
    recorder_init(delivered);
    void *steady = granite_anchor_new();
    granite_emitter_on(emitter, steady, NULL, log_value, delivered, NULL);
    pthread_t threads[8];
    worker workers[8];
    for (int i = 0; i < 8; i++) {
        workers[i] = (worker){emitter, i * 125};
        pthread_create(&threads[i], NULL, churn, &workers[i]);
    }
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);
    send_int(emitter, -1);
    GRANITE_ASSERT_EQ(recorded(delivered), 1001);
    GRANITE_ASSERT_EQ(granite_emitter_receiver_count(emitter), 1);
    granite_release(steady);
    granite_release(emitter);
    free(delivered);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

int main(void) {
    GRANITE_RUN(receivers_run_in_subscription_order);
    GRANITE_RUN(the_handler_is_handed_its_receiver);
    GRANITE_RUN(releasing_the_receiver_ends_the_subscription);
    GRANITE_RUN(a_key_replaces_its_subscription_and_moves_it_last);
    GRANITE_RUN(replacing_a_key_tells_the_displaced_receiver);
    GRANITE_RUN(removing_all_tells_every_receiver);
    GRANITE_RUN(off_matches_the_receiver_and_the_key);
    GRANITE_RUN(a_receiver_released_earlier_in_the_same_send_is_not_reached);
    GRANITE_RUN(off_during_a_send_takes_effect_from_the_next_send);
    GRANITE_RUN(handlers_may_send_and_subscribe_reentrantly);
    GRANITE_RUN(a_value_less_emitter_sends);
    GRANITE_RUN(a_handler_released_by_a_replaced_key_may_send_to_its_emitter);
    GRANITE_RUN(a_handler_released_by_off_may_send_to_its_emitter);
    GRANITE_RUN(a_handler_released_by_remove_all_may_send_to_its_emitter);
    GRANITE_RUN(a_handler_released_when_its_receiver_is_compacted_may_send_to_its_emitter);
    GRANITE_RUN(a_release_that_changes_its_emitter_does_not_trip_the_lock);
    GRANITE_RUN(an_emitter_let_go_lets_its_handlers_go);
    GRANITE_RUN(concurrent_send_subscribe_and_release_stay_consistent);
    GRANITE_TEST_MAIN_END();
}
