/* State behind a lock, and the count of what each thread holds. See granitec/lock.h. */
#include "granitec/lock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "granitec/buf.h"
#include "granitec/object.h"

/* The lock this thread holds: one at most. */
static _Thread_local granite_lock *held = NULL;

void granite_lock_init(granite_lock *lock, const char *name) {
    pthread_mutex_init(&lock->mutex, NULL);
    lock->name = name ? name : "a lock";
}

void granite_lock_destroy(granite_lock *lock) {
    pthread_mutex_destroy(&lock->mutex);
}

void granite_lock_acquire(granite_lock *lock) {
    if (held) {
        fprintf(stderr, "granite: took %s while holding %s\n", lock->name, held->name);
        abort();
    }
    pthread_mutex_lock(&lock->mutex);
    held = lock;
}

void granite_lock_release(granite_lock *lock) {
    if (held != lock) {
        fprintf(stderr, "granite: let go of %s, which this thread does not hold\n", lock->name);
        abort();
    }
    held = NULL;
    pthread_mutex_unlock(&lock->mutex);
}

void granite_lock_wait(granite_lock *lock, pthread_cond_t *cond) {
    if (held != lock) {
        fprintf(stderr, "granite: waited on %s without holding it\n", lock->name);
        abort();
    }
    pthread_cond_wait(cond, &lock->mutex);
}

bool granite_lock_wait_for(granite_lock *lock, pthread_cond_t *cond, double seconds) {
    if (held != lock) {
        fprintf(stderr, "granite: waited on %s without holding it\n", lock->name);
        abort();
    }
    if (seconds < 0) seconds = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    long long nanos = (long long)deadline.tv_nsec + (long long)((seconds - (double)(long long)seconds) * 1e9);
    deadline.tv_sec += (time_t)seconds + (time_t)(nanos / 1000000000LL);
    deadline.tv_nsec = (long)(nanos % 1000000000LL);
    return pthread_cond_timedwait(cond, &lock->mutex, &deadline) != ETIMEDOUT;
}

bool granite_lock_held(void) {
    return held != NULL;
}

void granite_lock_check(const char *what) {
    if (held) {
        fprintf(stderr, "granite: %s while holding %s\n", what, held->name);
        abort();
    }
}

struct granite_removal_item {
    void (*release)(void *);
    void *arg;
};

static void release_object(void *object) {
    granite_release(object);
}

void granite_removal_add(granite_removal *removal, void *object) {
    granite_removal_add_with(removal, release_object, object);
}

void granite_removal_add_with(granite_removal *removal, void (*release)(void *), void *arg) {
    if (!arg) return;
    if (removal->count == removal->capacity) {
        removal->capacity = removal->capacity ? removal->capacity * 2 : 8;
        removal->items = granite_realloc(removal->items, removal->capacity * sizeof *removal->items);
    }
    removal->items[removal->count++] = (struct granite_removal_item){release, arg};
}

void granite_removal_finish(granite_removal *removal) {
    granite_lock_check("let go of what a locked section removed");
    /* Taken first: a release may reach code that adds to a fresh removal of its own. */
    struct granite_removal_item *items = removal->items;
    size_t count = removal->count;
    memset(removal, 0, sizeof *removal);
    for (size_t i = 0; i < count; i++) items[i].release(items[i].arg);
    free(items);
}
