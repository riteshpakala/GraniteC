/* Shared things with a count. See granitec/object.h. */
#include "granitec/object.h"

#include <stdio.h>
#include <stdlib.h>

#include "granitec/buf.h"
#include "granitec/lock.h"

static atomic_long objects_alive = 0;

static void object_abort(const char *what, const granite_object *object) {
    fprintf(stderr, "granite: %s a %s\n", what,
            object && object->granite_class ? object->granite_class->name : "(unknown object)");
    abort();
}

void *granite_object_new(const granite_class *cls) {
    if (!cls || cls->size < sizeof(granite_object)) {
        fprintf(stderr, "granite: the class %s is smaller than an object's header\n", cls ? cls->name : "(null)");
        abort();
    }
    granite_object *object = granite_calloc(1, cls->size);
    object->granite_class = cls;
    atomic_init(&object->strong, 1);
    atomic_init(&object->weak, 1);
    atomic_fetch_add_explicit(&objects_alive, 1, memory_order_relaxed);
    return object;
}

void *granite_retain(void *pointer) {
    granite_object *object = pointer;
    if (!object) return NULL;
    if (atomic_fetch_add_explicit(&object->strong, 1, memory_order_relaxed) == 0)
        object_abort("retained, after its last release,", object);
    return object;
}

/* Drops one weak reference: the memory goes with the last. Runs nobody's code, so it is safe
 * under a lock. */
static void weak_drop(granite_object *object) {
    /* acq_rel, not release + an acquire fence: the same order, and one ThreadSanitizer can see. */
    if (atomic_fetch_sub_explicit(&object->weak, 1, memory_order_acq_rel) == 1) {
        atomic_fetch_sub_explicit(&objects_alive, 1, memory_order_relaxed);
        free(object);
    }
}

void granite_release(void *pointer) {
    granite_object *object = pointer;
    if (!object) return;
    if (granite_lock_held()) {
        char what[160];
        snprintf(what, sizeof what, "released a %s", object->granite_class->name);
        granite_lock_check(what);
    }
    size_t before = atomic_fetch_sub_explicit(&object->strong, 1, memory_order_acq_rel);
    if (before == 1) {
        if (object->granite_class->deinit) object->granite_class->deinit(object);
        weak_drop(object);
    } else if (before == 0) {
        object_abort("released, once more than it was retained,", object);
    }
}

size_t granite_strong_count(const void *pointer) {
    const granite_object *object = pointer;
    return object ? atomic_load_explicit(&((granite_object *)object)->strong, memory_order_acquire) : 0;
}

const granite_class *granite_class_of(const void *pointer) {
    const granite_object *object = pointer;
    return object ? object->granite_class : NULL;
}

long granite_objects_alive(void) {
    return atomic_load_explicit(&objects_alive, memory_order_acquire);
}

void granite_weak_set(granite_weak *weak, void *pointer) {
    granite_object *object = pointer;
    if (object) atomic_fetch_add_explicit(&object->weak, 1, memory_order_relaxed);
    granite_object *before = weak->object;
    weak->object = object;
    if (before) weak_drop(before);
}

void *granite_weak_load(const granite_weak *weak) {
    granite_object *object = weak->object;
    if (!object) return NULL;
    size_t strong = atomic_load_explicit(&object->strong, memory_order_relaxed);
    while (strong > 0) {
        if (atomic_compare_exchange_weak_explicit(&object->strong, &strong, strong + 1, memory_order_acquire,
                                                  memory_order_relaxed))
            return object;
    }
    return NULL;
}

bool granite_weak_alive(const granite_weak *weak) {
    return weak->object && atomic_load_explicit(&weak->object->strong, memory_order_acquire) > 0;
}

bool granite_weak_is(const granite_weak *weak, const void *object) {
    return weak->object == object;
}

void granite_weak_clear(granite_weak *weak) {
    granite_weak_set(weak, NULL);
}
