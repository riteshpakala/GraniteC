# Hosting agents

The host's side is the `granite` command and the library behind it (`host/`, never installed): one runner process per
call, a shelf of installed runners, and an installer for C agent repositories. Its usage, outputs and exit codes are
GraniteRust's `granite`, byte for byte (`cli/tests` holds them to Rust's).

```text
granite agents add <name | git URL[@revision] | folder> --shelf <dir>
granite agents update <name> --shelf <dir>
granite agents rm <name> --shelf <dir>
granite agents list --shelf <dir> [--json]
granite describe <runner>
granite run <runner> <perform.json | -> [--directory <dir>] [--ask <text>] [--word <word>]
granite check --schemas <dir>... --schema <file or $id> <instance.json | ->
```

A shelf is `--shelf` or `GRANITE_SHELF`, never a default. `run` is a host with no model: it prints one line,
`{"output": …}` or `{"failed": …, "refused": …}` with the runner's `status`, the cost on stderr, and exits 0 answered,
3 refused, 1 failed, 2 when the runner broke the wire or died. SIGINT or SIGTERM during a run cancels the runner,
reaps it, and ends the command by the same signal.

## A call

`granite_process_perform` starts the runner and:

1. starts it clean: its own process group, an empty signal mask, TERM, INT, HUP, QUIT and PIPE at their defaults,
   no inherited descriptor (close-on-exec everywhere, `POSIX_SPAWN_CLOEXEC_DEFAULT` on Apple), `GRANITE_AGENT=1`,
   the host's folder as its working folder;
2. waits for `hello` of major 1, sends the call once, relays notes, and answers each ask of the model on a thread of
   its own (a host without one says "this host has no model loaded");
3. reads the one `output` or `failed`: refused (with its kind), a fault (the runner named the answer standard), or a
   plain failure; a broken wire is unreadable; a runner that ends without answering is ended, with its status and
   the last six lines it printed off the wire;
4. closes its stdin, terminates it if it is still there five seconds later (SIGTERM to its group, SIGKILL two seconds
   after), reaps it with `wait4`, and kills whatever is left of its group.

A cancel says `cancel` and terminates the runner after two seconds. `GRANITE_AGENT_TRACE=1` traces every line either
side sends or reads, to stderr.

## The shelf

One folder per installation, each with its runner (`agent`) and `installation.json`, the same record GraniteRust
writes: a C host reads a Rust host's shelf and the other way around. A runner whose size or modification time moved
is digested before a call, and one whose SHA-256 is not the recorded one is refused: install it again. Only
`agents add` writes to a shelf.

## The installer

```text
fetch → granite-agent.json → make O=<work>/build <runner> → <runner> --describe → install
```

The stages are Swift's words (fetching, reading the package, building, asking … what it carries, installing,
installed). The build runs with stdin empty, without `DYLD_*` or a parent make's flags and jobserver, and finds
GraniteC through pkg-config; the record's `granite` is `pkg-config --modversion granitec` and its `commit`. One word
is one installation; installing the same repository again replaces it whole (staged, then renamed). A failed build
keeps its whole log beside the shelf.
