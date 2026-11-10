/* The single writer's thread. See granitec/main.h. */
#include "granitec/main.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "granitec/buf.h"
#include "granitec/lock.h"
#include "granitec/thread.h"

typedef struct queued {
    granite_turn turn;
    void *arg;
    struct queued *next;
} queued;

/* Process-lifetime: made once, never freed. */
static struct {
    granite_lock lock;
    pthread_cond_t ready;
    queued *head;
    queued *tail;
} queue;

static pthread_once_t started = PTHREAD_ONCE_INIT;
static _Thread_local bool on_main = false;

static void main_loop(void *unused) {
    on_main = true;
    for (;;) {
        granite_lock_acquire(&queue.lock);
        while (!queue.head) granite_lock_wait(&queue.lock, &queue.ready);
        queued *next = queue.head;
        queue.head = next->next;
        if (!queue.head) queue.tail = NULL;
        granite_lock_release(&queue.lock);
        next->turn(next->arg);
        free(next);
    }
}

static void start(void) {
    granite_lock_init(&queue.lock, "granite.main's queue");
    pthread_cond_init(&queue.ready, NULL);
    if (!granite_thread_spawn("granite.main", 0, main_loop, NULL)) {
        perror("granite: the main thread could not start");
        abort();
    }
}

void granite_main_enqueue(granite_turn turn, void *arg) {
    pthread_once(&started, start);
    queued *item = granite_calloc(1, sizeof *item);
    item->turn = turn;
    item->arg = arg;
    granite_lock_acquire(&queue.lock);
    if (queue.tail) queue.tail->next = item;
    else queue.head = item;
    queue.tail = item;
    pthread_cond_signal(&queue.ready);
    granite_lock_release(&queue.lock);
}

void granite_main_run(granite_turn turn, void *arg) {
    if (on_main) turn(arg);
    else granite_main_enqueue(turn, arg);
}

bool granite_main_is_current(void) {
    return on_main;
}

typedef struct call {
    granite_turn turn;
    void *arg;
    granite_lock lock;
    pthread_cond_t done_cond;
    bool done;
} call;

static void run_call(void *pointer) {
    call *c = pointer;
    c->turn(c->arg);
    granite_lock_acquire(&c->lock);
    c->done = true;
    pthread_cond_signal(&c->done_cond);
    granite_lock_release(&c->lock);
}

void granite_main_call(granite_turn turn, void *arg) {
    if (on_main) {
        turn(arg);
        return;
    }
    call c = {.turn = turn, .arg = arg, .done = false};
    granite_lock_init(&c.lock, "a call to main");
    pthread_cond_init(&c.done_cond, NULL);
    granite_main_enqueue(run_call, &c);
    granite_lock_acquire(&c.lock);
    while (!c.done) granite_lock_wait(&c.lock, &c.done_cond);
    granite_lock_release(&c.lock);
    pthread_cond_destroy(&c.done_cond);
    granite_lock_destroy(&c.lock);
}

static void nothing(void *unused) {
}

void granite_main_turn(void) {
    if (on_main) {
        fprintf(stderr, "granite: main waited for its own turn\n");
        abort();
    }
    granite_main_call(nothing, NULL);
}
