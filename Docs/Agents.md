# Writing an agent in C

An agent is a `granite_package`: a word, what it does, the input it takes, a run that streams events, and an answer
read off them. A runner is a process whose `main` is a roster of packages:

```c
static const granite_package *const roster[] = {&echo, &plate, NULL};   /* the first package of a word wins */
int main(int argc, char **argv) { return granite_server_main(argc, argv, roster); }
```

With `--describe` the runner prints its entries and exits; otherwise it answers one call on its stdin and stdout
and exits 0 answered, 1 failed, 2 unreadable, 3 refused, 130 cancelled.

## The package

| Field | What it says |
| --- | --- |
| `word` | what a host calls it by: lowercase, one word (`^[a-z][a-z0-9-]*$`) |
| `summary` | a sentence a host shows a person or a model |
| `module`, `type` | how a host names it beside its word (`type` defaults to `module.word`) |
| `guide` | how to use it well, in its own words |
| `lanes` | how many calls run at once: `0` for two, `GRANITE_LANES_UNBOUNDED` for no bound |
| `makes` | what it makes: `{kind, what, into, never}`, ended by a make with no kind |
| `input` | a declared `granite_schema` (`NULL`: anything) |
| `compute` | what every run shares, made on the first call, once per process, tried again after a failure |
| `run` | the run: blocking, on a thread of its own (8 MiB stack) |
| `output` | the answer read off the run's events (`NULL`: the last event) |

### The input

The schema is declared, field by field, and kept in order:

```c
static const char *const moods[] = {"calm", "loud", NULL};
static const granite_schema tag = {GRANITE_SCHEMA_STRING};
static const granite_field fields[] = {
    {"text", {GRANITE_SCHEMA_STRING}, true, "what to say"},
    {"mood", {GRANITE_SCHEMA_CHOICE, .choices = moods}, false, NULL},
    {"tags", {GRANITE_SCHEMA_ARRAY, .items = &tag}, false, NULL},
    {"directory", {GRANITE_SCHEMA_STRING}, false, NULL},   /* the host fills it with its folder */
    {NULL},
};
```

A call's input is read strictly and checked against it before the run starts; one that does not read is refused as
kind `input` ("echo could not read its input: missing field `text`") and the run never starts.

### The run

```c
static granite_error *run(granite_context *context, const json_object *input) {
    for (int round = 1; round <= 3; round++) {
        if (granite_context_is_cancelled(context)) return NULL;   /* cancelling is cooperative */
        granite_note(context, "round %d", round);
        /* … work … */
    }
    json_object *answer = granite_answer_new();
    granite_answer_path(answer, "cards/ada.card.json");
    granite_answer_made(answer, "card", "ada", "cards/ada.card.json", NULL);
    granite_deliver(context, answer);
    return NULL;
}
```

- `granite_deliver(context, event)` streams an event (the event is taken). Safe from any thread.
- `granite_note` is a line of progress the host shows.
- `granite_context_directory`, `granite_context_ask`: what the host offers; either may be `NULL`.
- `granite_context_compute`: what `compute` made.
- A run touches no shared state and holds no lock when it returns.

### Asking the host's model

```c
json_object *object = NULL;
granite_error *error = granite_ask_schema(context, &card_schema, "Make a card.", user, 3000, 300, &object);
if (error) return error;   /* a model refusal: no model, no answer, or the call was cancelled */
```

`granite_ask_object` (any object, a JSON Schema), `granite_ask_schema` (an object that must read as a declared
schema) and `granite_ask_text` block until the host answers, the host leaves, or the call is cancelled. A host that
offers no model refuses the ask, naming the package.

### Saying no

| Return | The host reads |
| --- | --- |
| `NULL` | the answer, checked against granite-answer/1, stamped (`format`, `agent`) and written sorted |
| `granite_refuse(kind, "fmt", …)` | a refusal of that kind, one reason, exit 3 |
| `granite_refuse_reasons(kind, reasons, n)` | several reasons: "N problems, so nothing was done:" and a list |
| `granite_refuse_said(kind, said, reasons, n)` | a refusal in the package's own sentence |
| `granite_fault("fmt", …)` | the agent's own fault, said as it is, exit 1 |

Refusal kinds: `input`, `call`, `build`, `device`, `model`, `parse`, `reach`, `answer`, `host`. An answer that
breaks the standard is refused as `answer` with every problem as AgentAnswerCheck says it.

## The repository

A C agent repository builds its runner with make and finds GraniteC through pkg-config alone:

```text
granite-agent.json   {"name": "comic", "runner": "agent", "granite": "0.1.0"}
Makefile             make O=<folder> agent  →  <folder>/agent
Schema/examples/     *.perform.json and *.refusal.json, the calls containers/check.sh makes
```

```make
O ?= build
agent: $(O)/agent
$(O)/agent: comic.c
	@mkdir -p $(O)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags granitec) -o $@ comic.c $(shell pkg-config --libs granitec)
```

`tests/fixtures/agent-repo` is one, and `granite agents add <folder or git URL[@revision]> --shelf <dir>` installs
it. Don't name sources `*.mod*`: the GitHub C `.gitignore` drops them.

## In one process

`granite_package_perform(package, input, len, host, cancel, &answer, &len)` makes one call in this process, through
the same pipeline the runner uses: what a test, or a host that links its agents, calls. Never call it from
`granite.main`.
