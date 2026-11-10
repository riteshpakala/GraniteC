/* Values sent to receivers held weakly. See granitec/emitter.h. */
#include "granitec/emitter.h"

#include <stdlib.h>
#include <string.h>

#include "granitec/buf.h"
#include "granitec/lock.h"

/* One subscription: an object, so a send can hold it while the list changes under it. */
typedef struct entry {
    granite_object header;
    granite_weak receiver;
    char *key;
    granite_handler handler;
    void *arg;
    void (*arg_release)(void *);
} entry;

static void entry_deinit(void *self) {
    entry *e = self;
    granite_weak_clear(&e->receiver);
    free(e->key);
    if (e->arg_release) e->arg_release(e->arg);
}

static const granite_class entry_class = {"subscription", sizeof(entry), entry_deinit, NULL};

struct granite_emitter {
    granite_object header;
    granite_lock lock;
    entry **entries;
    size_t count;
    size_t capacity;
};

static void emitter_deinit(void *self) {
    granite_emitter *emitter = self;
    /* The last reference is gone, so no one else reads the list: no lock, and every entry is
     * released here, with nothing held. */
    entry **entries = emitter->entries;
    size_t count = emitter->count;
    emitter->entries = NULL;
    emitter->count = 0;
    for (size_t i = 0; i < count; i++) granite_release(entries[i]);
    free(entries);
    granite_lock_destroy(&emitter->lock);
}

static const granite_class emitter_class = {"emitter", sizeof(granite_emitter), emitter_deinit, NULL};

static const granite_class anchor_class = {"anchor", sizeof(granite_object), NULL, NULL};

void *granite_anchor_new(void) {
    return granite_object_new(&anchor_class);
}

granite_emitter *granite_emitter_new(const char *name) {
    granite_emitter *emitter = granite_object_new(&emitter_class);
    granite_lock_init(&emitter->lock, name ? name : "an emitter's lock");
    return emitter;
}

/* What a rewrite picks to remove, beyond the entries whose receiver is gone. Runs under the lock:
 * it compares, and calls nothing. */
typedef struct pick {
    enum { PICK_NONE, PICK_KEY, PICK_RECEIVER, PICK_ALL } kind;
    const char *key;
    const void *receiver;
} pick;

static int same_key(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static int picked(const pick *p, const entry *e) {
    switch (p->kind) {
    case PICK_NONE: return 0;
    case PICK_KEY: return same_key(e->key, p->key);
    case PICK_RECEIVER: return granite_weak_is(&e->receiver, p->receiver) && (!p->key || same_key(e->key, p->key));
    case PICK_ALL: return 1;
    }
    return 0;
}

/* The live receivers a change displaced, each loaded (a strong reference) to be told after the
 * unlock. Loading under the lock is safe; only letting go is not. */
typedef struct receivers {
    void **items;
    size_t count;
    size_t capacity;
} receivers;

static void receivers_add(receivers *list, void *receiver) {
    if (!receiver) return;
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 4;
        list->items = granite_realloc(list->items, list->capacity * sizeof *list->items);
    }
    list->items[list->count++] = receiver;
}

/* Rebuilds the list under the lock: drops the entries whose receiver is gone and those `p` picks,
 * then appends `added`. Every entry taken out goes into `removal`; with `displaced`, the live
 * receiver of each picked entry is loaded into it (but `except`), to be told after the unlock. */
static void rewrite(granite_emitter *emitter, entry *added, const pick *p, granite_removal *removal,
                    receivers *displaced, const void *except) {
    granite_lock_acquire(&emitter->lock);
    size_t kept = 0;
    for (size_t i = 0; i < emitter->count; i++) {
        entry *e = emitter->entries[i];
        if (!granite_weak_alive(&e->receiver)) {
            granite_removal_add(removal, e);
        } else if (picked(p, e)) {
            granite_removal_add(removal, e);
            if (displaced && !granite_weak_is(&e->receiver, except))
                receivers_add(displaced, granite_weak_load(&e->receiver));
        } else {
            emitter->entries[kept++] = e;
        }
    }
    emitter->count = kept;
    if (added) {
        if (emitter->count == emitter->capacity) {
            emitter->capacity = emitter->capacity ? emitter->capacity * 2 : 4;
            emitter->entries = granite_realloc(emitter->entries, emitter->capacity * sizeof *emitter->entries);
        }
        emitter->entries[emitter->count++] = added;
    }
    granite_lock_release(&emitter->lock);
}

/* After the unlock: tells each displaced receiver and lets it go, then the removed entries. */
static void finish(granite_removal *removal, receivers *displaced) {
    if (displaced) {
        for (size_t i = 0; i < displaced->count; i++) {
            const granite_class *cls = granite_class_of(displaced->items[i]);
            if (cls && cls->displaced) cls->displaced(displaced->items[i]);
            granite_release(displaced->items[i]);
        }
        free(displaced->items);
    }
    granite_removal_finish(removal);
}

void granite_emitter_on(granite_emitter *emitter, void *receiver, const char *key, granite_handler handler, void *arg,
                        void (*arg_release)(void *)) {
    entry *e = granite_object_new(&entry_class);
    granite_weak_set(&e->receiver, receiver);
    e->key = granite_strdup(key);
    e->handler = handler;
    e->arg = arg;
    e->arg_release = arg_release;
    pick p = {key ? PICK_KEY : PICK_NONE, key, NULL};
    granite_removal removal = {0};
    receivers displaced = {0};
    rewrite(emitter, e, &p, &removal, &displaced, receiver);
    finish(&removal, &displaced);
}

void granite_emitter_off(granite_emitter *emitter, const void *receiver, const char *key) {
    pick p = {PICK_RECEIVER, key, receiver};
    granite_removal removal = {0};
    rewrite(emitter, NULL, &p, &removal, NULL, NULL);
    finish(&removal, NULL);
}

void granite_emitter_remove_all(granite_emitter *emitter) {
    pick p = {PICK_ALL, NULL, NULL};
    granite_removal removal = {0};
    receivers displaced = {0};
    rewrite(emitter, NULL, &p, &removal, &displaced, NULL);
    finish(&removal, &displaced);
}

void granite_emitter_compact(granite_emitter *emitter) {
    pick p = {PICK_NONE, NULL, NULL};
    granite_removal removal = {0};
    rewrite(emitter, NULL, &p, &removal, NULL, NULL);
    finish(&removal, NULL);
}

void granite_emitter_send(granite_emitter *emitter, const void *value) {
    /* The snapshot holds each entry, so one an earlier handler removes still delivers (from the
     * next send it is gone) and is let go after this send, never under the lock. */
    granite_lock_acquire(&emitter->lock);
    size_t count = emitter->count;
    entry **snapshot = count ? granite_malloc(count * sizeof *snapshot) : NULL;
    for (size_t i = 0; i < count; i++) snapshot[i] = granite_retain(emitter->entries[i]);
    granite_lock_release(&emitter->lock);

    int released = 0;
    for (size_t i = 0; i < count; i++) {
        entry *e = snapshot[i];
        /* Loaded now, not when the snapshot was taken: a receiver released earlier in this same
         * send is not reached. */
        void *receiver = granite_weak_load(&e->receiver);
        if (!receiver) {
            released = 1;
            continue;
        }
        e->handler(receiver, value, e->arg);
        granite_release(receiver);
    }
    for (size_t i = 0; i < count; i++) granite_release(snapshot[i]);
    free(snapshot);
    if (released) granite_emitter_compact(emitter);
}

size_t granite_emitter_receiver_count(granite_emitter *emitter) {
    /* Counted without loading a receiver: a load could make this thread its last owner. */
    granite_lock_acquire(&emitter->lock);
    size_t live = 0;
    for (size_t i = 0; i < emitter->count; i++) live += granite_weak_alive(&emitter->entries[i]->receiver) ? 1 : 0;
    granite_lock_release(&emitter->lock);
    return live;
}
