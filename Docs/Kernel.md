# The kernel

`core/` is Granite's event layer and concurrency core in C: what the rest stands on. It follows Granite's design
since `fec78b1`/`8291f7a`: no signals, scopes or cancellables; emitters hold their receivers weakly; nothing is
released while a lock is held; one lock type is the only primitive that guards state.

| Swift Granite | GraniteRust | GraniteC | Notes |
| --- | --- | --- | --- |
| ARC, `weak` | `Arc`, `Weak` | `granite_object`, `granite_weak` | atomic strong and weak counts in a header; a weak load is a compare-and-swap that succeeds only while the object lives, so nothing torn down comes back |
| `GraniteLock<State>` | `Locked<T>` | `granite_lock` | a mutex whose state is the fields after it; the only place GraniteC takes a mutex (`make lint`) |
| `withExtendedLifetime(removed)` | a body returning what it removed | `granite_removal` | what a locked section removes, released in order after the unlock |
| `GraniteEmitter`, `GraniteEmitterKey` | `Emitter<T>`, `EmitterKey` | `granite_emitter` | receivers held weakly and handed to their handler; a key replaces and moves last; a displaced receiver is told after the unlock |
| `GraniteMain`, `MainActor` | `Main` | `granite_main_*` | one thread, `granite.main`, turns in the order sent; `enqueue` always defers, `run` is inline on main, `call` blocks |
| threads | `tokio` tasks | `granite_thread_spawn` | named, with a stack size (runs get 8 MiB) |

## The rule, held at run time

Swift's compiler and Rust's borrow checker hold most of Granite's concurrency rules. C holds them while it runs: a
thread-local count of the `granite_lock` each thread holds makes these abort at once, with a sentence naming the
lock, every time rather than once in a while:

- taking a second lock while holding one;
- releasing an object (its last reference or not: a release under a lock fails the same way whichever it is);
- freeing a JSON tree (`granite_json_put`);
- finishing a removal;
- calling into the author's code (the run, the compute, the output, a handler).

```c
granite_removal removal = {0};
granite_lock_acquire(&list->lock);
granite_removal_add(&removal, list->items[i]);   /* kept, not released */
list->items[i] = list->items[--list->count];
granite_lock_release(&list->lock);
granite_removal_finish(&removal);                /* released here, nothing held */
```

## Events

`granite_emitter_on(emitter, receiver, key, handler, arg, arg_release)` subscribes `receiver` (an object) weakly.
The subscription lasts as long as the receiver: nothing is stored, cancelled, or torn down in a deinit, and
`granite_emitter_off` is never needed for correctness. A send copies the list under the lock and delivers on the
sender's thread with it released, so a handler may send, subscribe or unsubscribe on the same emitter; a receiver
released earlier in the same send is not reached. `arg` goes to the handler on every send and `arg_release(arg)`
runs when the subscription goes, after the unlock: an arg whose release sends to its own emitter is safe (the
EmitterTests' release cases, ported in `core/tests/test_emitter.c`).

The run engine taps a package's events through an emitter; a context's cancel flag wakes blocked model asks
through one.

## Objects and ownership

Only what crosses threads is an object: a run record, a context, a host, a model, a cancel flag, a channel, an
emitter and its entries, a child process. Everything else is a value with one owner, and what lives as long as the
process (main's queue, a package's engine) is never freed. A json-c tree has one owner thread at a time: json-c's
counts are not atomic, so trees are handed across threads, never shared.
