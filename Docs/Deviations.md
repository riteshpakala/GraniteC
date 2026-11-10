# Deviations

Each is deliberate. Where the Swift code and `Schema/GraniteAgent.md` disagree, GraniteC follows the standard, as
GraniteRust does; where GraniteRust deviates from Swift, GraniteC follows GraniteRust unless it says otherwise here.

1. **A schema is declared, not read off a type.** C has no deserializer to probe: a package declares its
   `granite_schema` field by field. Its data form and JSON Schema form are Rust's.
2. **Input is checked against the declared schema.** It is refused as kind `input` with exit 3, as in Rust. The sentence
   for JSON that does not parse is serde's, word for word, line and column included. The sentence for JSON that does
   not fit is serde's words without the position ("missing field `text`", where Rust's typed deserializer says
   "missing field `text` at line 1 column 2"). Interop compares the rest of such a call.
3. **Events and answers are JSON.** A run delivers `json_object`s; `output` reads the answer off them, the last event
   when a package says nothing. An answer is built with `granite_answer_*`, which writes what Rust's `Answer<T>`
   writes.
4. **One concurrency policy.** Every package call has its own key, so the engine is parallel up to the package's lanes.
   Not ported: supersede, drop, merge, coalescing, waits by prefix, `RunScope`, child runs and `context.spawn`, a
   resolver per input. Not ported either: Center and its watch, reducers, effects, services and relays, tickets, lanes,
   registries, persistence, worker specs. A C agent is a package; the rest of Granite's app architecture is Swift's
   and Rust's.
5. **The lock rule is held at run time.** A thread that takes a second lock, releases an object, frees a JSON tree or
   calls the author's code while holding a `granite_lock` aborts with a sentence ([Kernel.md](Kernel.md)). Swift and
   Rust hold most of this at compile time.
6. **Cancelling is cooperative.** A run is never killed. Its flag is set, an ask of the model it waits on wakes ("the
   call was cancelled"), and its later events are dropped. Rust drops the run's future, and Swift's is cooperative.
7. **The wire is the runner's alone.** The runner's wire descriptor is close-on-exec, so an agent's children do not
   inherit it, and its stdin is `/dev/null` once the channel has taken the real one. In Rust the descriptor is
   inherited and stdin is the channel.
8. **SIGTERM is caught, not blocked.** A runner catches it with a handler that writes to a pipe. A blocked signal would
   be inherited, still blocked, by every child an agent starts.
9. **A call's processes go with it.** A runner starts with an empty signal mask, default dispositions and, on Apple,
   no inherited descriptor. Once it is reaped, anything left in its process group is killed. Rust's leaves a grandchild
   running.
10. **An interrupted `granite run` ends its runner.** SIGINT or SIGTERM cancels the runner, reaps it, and ends the
    command by the same signal. Rust's leaves the runner behind.
11. **C agent repositories.** The installer reads `granite-agent.json` and builds with make and pkg-config where Rust's
    reads Cargo metadata and builds with cargo. Its sentences say C's words: "… is not a C agent repository …", a
    C toolchain (make, cc, pkg-config), "… does not build against GraniteC: it needs GraniteC 0.2, and pkg-config
    finds 0.1.0". A folder with a Cargo.toml is pointed at GraniteRust.
12. **JSON keys are C strings.** json-c cuts a key at an embedded NUL, so a key containing `\u0000` is shortened.
    Values keep every byte.
13. **Shortest digits by round trip.** A double is written with the fewest digits that read back through `strtod`,
    laid out as serde_json's float writer (zmij) lays them out. That equals serde on every value of the JSON corpus
    and of interop's mirror. A tie the two algorithms break differently is possible in principle, and none has been
    seen.
14. **Records read without positions.** A shelf's `installation.json` that does not fit its record is said without
    serde's position. One that does not parse is said exactly as Rust says it.
15. **`granite check` reads a subset.** It reads the keywords the standard's schemas use ([Hosting.md](Hosting.md)),
    with jsonschema-rs's sentences and order, and a census test fails if a schema reaches past them. A `$ref` it cannot
    resolve is a problem, not a build error.
