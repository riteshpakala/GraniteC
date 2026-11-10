/* The main queue: one thread, turns in the order they were sent, deferred or inline. */
#include <pthread.h>
#include <stdatomic.h>

#include "granite_test.h"
#include "granitec/lock.h"
#include "granitec/main.h"

static int order[4000];
static int ordered = 0; /* written on main only */

static void append(void *arg) {
    if (!granite_main_is_current()) GRANITE_FAIL("a turn ran off main");
    order[ordered++] = (int)(intptr_t)arg;
}

GRANITE_TEST(turns_run_on_main_in_the_order_they_were_sent) {
    ordered = 0;
    GRANITE_ASSERT(!granite_main_is_current());
    for (int i = 0; i < 100; i++) granite_main_enqueue(append, (void *)(intptr_t)i);
    granite_main_turn();
    GRANITE_ASSERT_EQ(ordered, 100);
    int in_order = 1;
    for (int i = 0; i < 100; i++) in_order &= order[i] == i;
    GRANITE_ASSERT(in_order);
}

static void *send_many(void *arg) {
    int base = (int)(intptr_t)arg;
    for (int i = 0; i < 500; i++) granite_main_enqueue(append, (void *)(intptr_t)(base + i));
    return NULL;
}

GRANITE_TEST(each_senders_turns_keep_their_order_across_threads) {
    ordered = 0;
    pthread_t threads[8];
    for (int t = 0; t < 8; t++) pthread_create(&threads[t], NULL, send_many, (void *)(intptr_t)(t * 1000));
    for (int t = 0; t < 8; t++) pthread_join(threads[t], NULL);
    granite_main_turn();
    GRANITE_ASSERT_EQ(ordered, 4000);
    int last[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int in_order = 1;
    for (int i = 0; i < ordered; i++) {
        int sender = order[i] / 1000, n = order[i] % 1000;
        in_order &= n > last[sender];
        last[sender] = n;
    }
    GRANITE_ASSERT(in_order);
}

/* From main: run is inline, enqueue is after the current turn. */
static void from_main(void *unused) {
    ordered = 0;
    granite_main_enqueue(append, (void *)(intptr_t)2);
    granite_main_run(append, (void *)(intptr_t)1);
    granite_main_call(append, (void *)(intptr_t)0); /* inline, as run */
}

GRANITE_TEST(on_main_run_and_call_are_inline_and_enqueue_waits_its_turn) {
    granite_main_enqueue(from_main, NULL);
    granite_main_turn(); /* from_main has run, and queued its own turn after this one */
    granite_main_turn();
    GRANITE_ASSERT_EQ(ordered, 3);
    GRANITE_ASSERT_EQ(order[0], 1);
    GRANITE_ASSERT_EQ(order[1], 0);
    GRANITE_ASSERT_EQ(order[2], 2);
}

static void compute(void *arg) {
    *(int *)arg = granite_main_is_current() ? 42 : -1;
}

GRANITE_TEST(a_call_returns_once_main_has_run_it) {
    int value = 0;
    granite_main_call(compute, &value);
    GRANITE_ASSERT_EQ(value, 42);
}

static void slow(void *arg) {
    granite_lock_check("main ran a turn");
    atomic_store((atomic_int *)arg, 1);
}

GRANITE_TEST(off_main_run_is_deferred_to_main) {
    atomic_int ran = 0;
    granite_main_run(slow, &ran);
    granite_main_turn();
    GRANITE_ASSERT_EQ(atomic_load(&ran), 1);
}

int main(void) {
    GRANITE_RUN(turns_run_on_main_in_the_order_they_were_sent);
    GRANITE_RUN(each_senders_turns_keep_their_order_across_threads);
    GRANITE_RUN(on_main_run_and_call_are_inline_and_enqueue_waits_its_turn);
    GRANITE_RUN(a_call_returns_once_main_has_run_it);
    GRANITE_RUN(off_main_run_is_deferred_to_main);
    GRANITE_TEST_MAIN_END();
}
