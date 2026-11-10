/* State behind a lock: Swift's GraniteLock<State> and GraniteRust's Locked<T>.
 *
 * A granite_lock is a mutex whose state is, by convention, the fields after it. It is the one
 * place GraniteC takes a mutex (`make lint` holds that), so it can count what each thread holds.
 *
 * PIN: ONE LOCK AT A TIME, AND NOTHING CALLED UNDER IT. A thread holding a granite_lock aborts,
 * with a sentence saying which, when it takes another, releases an object, frees a json-c tree,
 * or calls into what the author wrote. Swift's compiler and Rust's borrow checker hold most of
 * this rule; here it is held at run time, every time (Docs/Deviations.md).
 * PIN: WHAT A LOCKED SECTION REMOVES GOES AFTER IT. Put it in a granite_removal under the lock and
 * finish the removal after the unlock. */
#ifndef GRANITEC_LOCK_H
#define GRANITEC_LOCK_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct granite_lock {
    pthread_mutex_t mutex;
    const char *name;
} granite_lock;

void granite_lock_init(granite_lock *lock, const char *name);
void granite_lock_destroy(granite_lock *lock);
/* Takes `lock`. Aborts when this thread already holds a granite_lock. */
void granite_lock_acquire(granite_lock *lock);
void granite_lock_release(granite_lock *lock);
/* Waits on `cond` with `lock` held: the one way to let a lock go and take it back. */
void granite_lock_wait(granite_lock *lock, pthread_cond_t *cond);
/* As granite_lock_wait, for at most `seconds`; false when the time ran out. */
bool granite_lock_wait_for(granite_lock *lock, pthread_cond_t *cond, double seconds);

/* Whether this thread holds a granite_lock. */
bool granite_lock_held(void);
/* Aborts, saying `what` was done while holding a lock, when this thread holds one. Called before
 * a release, a json-c free, and every call into the author's code. */
void granite_lock_check(const char *what);

/* Things to let go after the unlock. Zero-initialized, it is empty. Adding allocates, which is
 * safe under a lock: malloc runs nobody's code. */
typedef struct granite_removal {
    struct granite_removal_item *items;
    size_t count;
    size_t capacity;
} granite_removal;

/* Keeps a strong reference to release later. NULL is ignored. */
void granite_removal_add(granite_removal *removal, void *object);
/* Keeps `arg` for `release(arg)` later. A NULL arg is ignored. */
void granite_removal_add_with(granite_removal *removal, void (*release)(void *), void *arg);
/* After the unlock: lets everything go, in the order it was added, and empties the removal. */
void granite_removal_finish(granite_removal *removal);

#endif
