/* Values sent to receivers held weakly: Swift's GraniteEmitter, GraniteRust's Emitter<T>.
 *
 * PIN: NO TOKENS. granite_emitter_on keeps its receiver weakly and hands it back to the handler on
 * every send, so the handler need not hold it. The subscription ends when the receiver is
 * released: nothing is stored, cancelled, or torn down in a deinit. granite_emitter_off removes
 * one early and is never needed for correctness.
 * PIN: DELIVERY IS SYNCHRONOUS, IN ORDER, LOCK RELEASED. A send copies the list under the lock and
 * delivers on the sender's thread with it released, so a handler may send, subscribe or
 * unsubscribe on the same emitter. A receiver released by an earlier handler in the same send is
 * not reached; an off or a replaced key takes effect from the next send.
 * PIN: NOTHING IS RELEASED UNDER THE LOCK. What a change removes (an entry, its handler's arg, a
 * receiver loaded to be told it was displaced) goes after the unlock.
 * PIN: AN ARG THAT HOLDS ITS OWN RECEIVER, OR ITS OWN EMITTER, KEEPS IT ALIVE. Reach what the
 * handler needs through the receiver it is handed. */
#ifndef GRANITEC_EMITTER_H
#define GRANITEC_EMITTER_H

#include <stddef.h>

#include "granitec/object.h"

typedef struct granite_emitter granite_emitter;

/* Called with the live receiver, the value sent, and the arg given to granite_emitter_on. */
typedef void (*granite_handler)(void *receiver, const void *value, void *arg);

/* A new emitter, an object: release it to let it go. `name` is for the lock's messages. */
granite_emitter *granite_emitter_new(const char *name);

/* Subscribes `receiver` (an object, held weakly) until it is released. With a `key`, the
 * subscription already under it is removed first and this one added last, whoever its receiver
 * was; the receiver it displaced is told, if its class says how. `arg` goes to the handler on
 * every send; `arg_release(arg)` runs when the subscription goes. */
void granite_emitter_on(granite_emitter *emitter, void *receiver, const char *key, granite_handler handler, void *arg,
                        void (*arg_release)(void *));
/* Removes `receiver`'s subscriptions: all of them (NULL key), or only the one under `key`. */
void granite_emitter_off(granite_emitter *emitter, const void *receiver, const char *key);
/* Sends `value` to every live receiver, in subscription order, on this thread. */
void granite_emitter_send(granite_emitter *emitter, const void *value);
/* How many subscriptions still have a live receiver. */
size_t granite_emitter_receiver_count(granite_emitter *emitter);
/* Removes every subscription, telling the receivers that can be told: an owner taking over. */
void granite_emitter_remove_all(granite_emitter *emitter);
/* Drops the subscriptions whose receivers are gone: what a send does when it finds one. */
void granite_emitter_compact(granite_emitter *emitter);

/* A bare object to receive with: an anchor whose life is the subscription's (Rust's Arc<()>). */
void *granite_anchor_new(void);

#endif
