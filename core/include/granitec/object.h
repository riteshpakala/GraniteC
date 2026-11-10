/* Shared things with a count: what ARC is to Swift Granite and Arc/Weak to GraniteRust.
 *
 * Only what crosses threads is an object: a run, a context, a host, a model, a cancel flag, a
 * channel, an emitter's entries. Everything else is a value with one owner, and what lives as
 * long as the process is never freed.
 *
 * PIN: NOTHING IS RELEASED UNDER A LOCK. A release may run a deinit, and a deinit may run what the
 * author wrote and reach the same lock again. granite_release aborts when the calling thread
 * holds a granite_lock, whether or not the count reaches zero, so the rule fails the same way
 * every time instead of once in a while. Release what a locked section removed after it, through
 * a granite_removal.
 * PIN: A WEAK LOAD NEVER REVIVES. granite_weak_load hands back a strong reference only while the
 * strong count is above zero (a compare-and-swap), so a thing being torn down stays gone. */
#ifndef GRANITEC_OBJECT_H
#define GRANITEC_OBJECT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct granite_object granite_object;

/* What kind of object it is: its name (for the abort messages), its size, and what runs when the
 * last strong reference goes. `displaced` is optional: an emitter calls it, after its unlock, on
 * a receiver whose subscription a key replaced or remove_all took (Swift's
 * GraniteEmitterDisplaceable). */
typedef struct granite_class {
    const char *name;
    size_t size;
    void (*deinit)(void *self);
    void (*displaced)(void *self);
} granite_class;

/* The header every object starts with. */
struct granite_object {
    const granite_class *granite_class;
    atomic_size_t strong;
    /* The weak references, plus one held by all the strong ones together: the memory goes when
     * this reaches zero, after the deinit. */
    atomic_size_t weak;
};

/* A new object of `cls`, zeroed past its header, with one strong reference. */
void *granite_object_new(const granite_class *cls);
void *granite_retain(void *object);
/* Drops a strong reference; the last runs the deinit, then lets the memory go once no weak
 * reference remains. NULL is ignored. Aborts under a granite_lock. */
void granite_release(void *object);
size_t granite_strong_count(const void *object);
const granite_class *granite_class_of(const void *object);
/* Objects made and not yet freed, process-wide: what a test reads to say nothing leaked. */
long granite_objects_alive(void);

/* A reference that does not keep its object alive. Zero-initialized, it points nowhere. */
typedef struct granite_weak {
    granite_object *object;
} granite_weak;

/* Points `weak` at `object` (or nowhere, for NULL), clearing what it pointed at before. */
void granite_weak_set(granite_weak *weak, void *object);
/* A strong reference while the object lives, else NULL. Release what it returns. */
void *granite_weak_load(const granite_weak *weak);
/* Whether the object still lives. Never takes a reference: a load here could make the caller
 * the object's last owner. */
bool granite_weak_alive(const granite_weak *weak);
/* Whether `weak` points at `object`, alive or not: an address compare, never a load. */
bool granite_weak_is(const granite_weak *weak, const void *object);
void granite_weak_clear(granite_weak *weak);

#endif
