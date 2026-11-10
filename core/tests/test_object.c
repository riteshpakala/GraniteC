/* Objects, the lock and its rule, and removals: what the rest of GraniteC stands on. */
#include <pthread.h>
#include <stdatomic.h>

#include "granite_test.h"
#include "granitec/buf.h"
#include "granitec/lock.h"
#include "granitec/object.h"
#include "granitec/thread.h"

typedef struct probe {
    granite_object header;
    atomic_int *deinits;
} probe;

static void probe_deinit(void *self) {
    probe *p = self;
    if (p->deinits) atomic_fetch_add(p->deinits, 1);
}

static const granite_class probe_class = {"probe", sizeof(probe), probe_deinit, NULL};

static probe *probe_new(atomic_int *deinits) {
    probe *p = granite_object_new(&probe_class);
    p->deinits = deinits;
    return p;
}

GRANITE_TEST(the_last_release_runs_the_deinit_once) {
    atomic_int deinits = 0;
    long before = granite_objects_alive();
    probe *p = probe_new(&deinits);
    granite_retain(p);
    GRANITE_ASSERT_EQ(granite_strong_count(p), 2);
    granite_release(p);
    GRANITE_ASSERT_EQ(atomic_load(&deinits), 0);
    granite_release(p);
    GRANITE_ASSERT_EQ(atomic_load(&deinits), 1);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

GRANITE_TEST(a_weak_reference_does_not_keep_its_object_and_never_revives_it) {
    atomic_int deinits = 0;
    long before = granite_objects_alive();
    probe *p = probe_new(&deinits);
    granite_weak weak = {0};
    granite_weak_set(&weak, p);
    probe *loaded = granite_weak_load(&weak);
    GRANITE_ASSERT(loaded == p);
    GRANITE_ASSERT_EQ(granite_strong_count(p), 2);
    granite_release(loaded);
    GRANITE_ASSERT(granite_weak_alive(&weak));
    granite_release(p);
    GRANITE_ASSERT_EQ(atomic_load(&deinits), 1);
    GRANITE_ASSERT(!granite_weak_alive(&weak));
    GRANITE_ASSERT(granite_weak_load(&weak) == NULL);
    GRANITE_ASSERT(granite_weak_is(&weak, p));
    GRANITE_ASSERT_EQ(granite_objects_alive(), before + 1); /* the weak reference keeps the memory */
    granite_weak_clear(&weak);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

typedef struct race {
    granite_weak weak;
    atomic_int go;
    atomic_int loaded;
} race;

static void *load_while_released(void *pointer) {
    race *r = pointer;
    while (!atomic_load(&r->go)) {
    }
    for (int i = 0; i < 2000; i++) {
        probe *p = granite_weak_load(&r->weak);
        if (p) {
            atomic_fetch_add(&r->loaded, 1);
            granite_release(p);
        }
    }
    return NULL;
}

GRANITE_TEST(a_weak_load_racing_the_last_release_hands_back_a_live_object_or_nothing) {
    long before = granite_objects_alive();
    for (int round = 0; round < 200; round++) {
        atomic_int deinits = 0;
        race r = {0};
        probe *p = probe_new(&deinits);
        granite_weak_set(&r.weak, p);
        pthread_t threads[4];
        for (int i = 0; i < 4; i++) pthread_create(&threads[i], NULL, load_while_released, &r);
        atomic_store(&r.go, 1);
        granite_release(p);
        for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);
        GRANITE_ASSERT_EQ(atomic_load(&deinits), 1);
        GRANITE_ASSERT(granite_weak_load(&r.weak) == NULL);
        granite_weak_clear(&r.weak);
    }
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

static granite_lock first, second;

static void takes_two_locks(void) {
    granite_lock_acquire(&first);
    granite_lock_acquire(&second);
}

static void releases_under_a_lock(void) {
    probe *p = probe_new(NULL);
    granite_retain(p);
    granite_lock_acquire(&first);
    granite_release(p); /* not the last: the rule holds anyway */
}

static void calls_out_under_a_lock(void) {
    granite_lock_acquire(&first);
    granite_lock_check("called the author's handler");
}

static void finishes_a_removal_under_a_lock(void) {
    granite_removal removal = {0};
    granite_removal_add(&removal, probe_new(NULL));
    granite_lock_acquire(&first);
    granite_removal_finish(&removal);
}

static void lets_go_of_a_lock_it_does_not_hold(void) {
    granite_lock_release(&first);
}

GRANITE_TEST(the_lock_rule_fails_the_same_way_every_time) {
    GRANITE_ASSERT(granite_test_aborts(takes_two_locks, "took the second lock while holding the first lock"));
    GRANITE_ASSERT(granite_test_aborts(releases_under_a_lock, "released a probe while holding the first lock"));
    GRANITE_ASSERT(granite_test_aborts(calls_out_under_a_lock, "called the author's handler while holding the first lock"));
    GRANITE_ASSERT(granite_test_aborts(finishes_a_removal_under_a_lock, "while holding the first lock"));
    GRANITE_ASSERT(granite_test_aborts(lets_go_of_a_lock_it_does_not_hold, "which this thread does not hold"));
}

/* A deinit that takes the lock its object was removed under: fine after the unlock. */
typedef struct relocker {
    granite_object header;
    granite_lock *lock;
    int *reached;
} relocker;

static void relocker_deinit(void *self) {
    relocker *r = self;
    granite_lock_acquire(r->lock);
    (*r->reached)++;
    granite_lock_release(r->lock);
}

static const granite_class relocker_class = {"relocker", sizeof(relocker), relocker_deinit, NULL};

static void free_text(void *text) {
    free(text);
}

GRANITE_TEST(what_a_locked_section_removes_goes_after_it_in_order) {
    int reached = 0;
    relocker *r = granite_object_new(&relocker_class);
    r->lock = &first;
    r->reached = &reached;
    granite_removal removal = {0};
    granite_lock_acquire(&first);
    granite_removal_add(&removal, r);
    granite_removal_add(&removal, NULL);
    granite_removal_add_with(&removal, free_text, granite_strdup("text"));
    for (int i = 0; i < 20; i++) granite_removal_add(&removal, probe_new(NULL));
    GRANITE_ASSERT_EQ(removal.count, 22);
    granite_lock_release(&first);
    long before = granite_objects_alive();
    granite_removal_finish(&removal);
    GRANITE_ASSERT_EQ(reached, 1);
    GRANITE_ASSERT_EQ(removal.count, 0);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before - 21);
}

typedef struct waiter {
    granite_lock lock;
    pthread_cond_t cond;
    int ready;
} waiter;

static void signal_later(void *pointer) {
    waiter *w = pointer;
    granite_sleep(0.02);
    granite_lock_acquire(&w->lock);
    w->ready = 1;
    pthread_cond_broadcast(&w->cond);
    granite_lock_release(&w->lock);
}

GRANITE_TEST(a_wait_lets_the_lock_go_and_takes_it_back) {
    waiter w = {.ready = 0};
    granite_lock_init(&w.lock, "the waiter's lock");
    pthread_cond_init(&w.cond, NULL);
    granite_lock_acquire(&w.lock);
    GRANITE_ASSERT(!granite_lock_wait_for(&w.lock, &w.cond, 0.01));
    GRANITE_ASSERT(granite_thread_spawn("granite.test", 0, signal_later, &w));
    while (!w.ready) granite_lock_wait(&w.lock, &w.cond);
    GRANITE_ASSERT(granite_lock_held());
    granite_lock_release(&w.lock);
    GRANITE_ASSERT(!granite_lock_held());
    granite_sleep(0.01);
    granite_lock_destroy(&w.lock);
    pthread_cond_destroy(&w.cond);
}

GRANITE_TEST(a_buffer_grows_and_hands_its_text_over) {
    granite_buf buf = {0};
    GRANITE_ASSERT_STR(granite_buf_str(&buf), "");
    for (int i = 0; i < 100; i++) granite_buf_appendf(&buf, "%d,", i);
    granite_buf_append_char(&buf, '!');
    GRANITE_ASSERT(buf.len == 291);
    char *text = granite_buf_take(&buf);
    GRANITE_ASSERT(strncmp(text, "0,1,2,", 6) == 0);
    GRANITE_ASSERT(text[290] == '!');
    GRANITE_ASSERT(buf.data == NULL && buf.len == 0);
    free(text);
    char *made = granite_strdupf("%s-%08x", "parrot", 0xabcu);
    GRANITE_ASSERT_STR(made, "parrot-00000abc");
    free(made);
}

int main(void) {
    granite_lock_init(&first, "the first lock");
    granite_lock_init(&second, "the second lock");
    GRANITE_RUN(the_last_release_runs_the_deinit_once);
    GRANITE_RUN(a_weak_reference_does_not_keep_its_object_and_never_revives_it);
    GRANITE_RUN(a_weak_load_racing_the_last_release_hands_back_a_live_object_or_nothing);
    GRANITE_RUN(the_lock_rule_fails_the_same_way_every_time);
    GRANITE_RUN(what_a_locked_section_removes_goes_after_it_in_order);
    GRANITE_RUN(a_wait_lets_the_lock_go_and_takes_it_back);
    GRANITE_RUN(a_buffer_grows_and_hands_its_text_over);
    GRANITE_TEST_MAIN_END();
}
