# GraniteC

Granite's agent kernel and the `granite-agent/1` standard in C: what you link to write an agent in C, and the
`granite` command a host uses to install, describe, run and check one, in its own container or on the machine. The
Swift original is [Granite](https://github.com/riteshpakala/Granite); the Rust port is
[GraniteRust](https://github.com/riteshpakala/GraniteRust). The wire is language-neutral and the bytes are the same:
a C host drives Rust runners and a Rust host drives C runners (`make interop` holds that).

```c
#include <granitec/granitec.h>
#include <granitec/server.h>

static const granite_field fields[] = {{"text", {GRANITE_SCHEMA_STRING}, true, "what to say"}, {NULL}};
static const granite_schema input = {GRANITE_SCHEMA_OBJECT, .fields = fields};

static granite_error *run(granite_context *context, const json_object *in) {
    granite_note(context, "saying it back");
    json_object *answer = granite_answer_new();
    granite_json_set_string(answer, "said", granite_json_get_string(in, "text"));
    granite_deliver(context, answer);
    return NULL;
}

static const granite_package echo = {.word = "echo", .summary = "Says it back.", .module = "Echo",
                                     .input = &input, .run = run};
static const granite_package *const roster[] = {&echo, NULL};

int main(int argc, char **argv) { return granite_server_main(argc, argv, roster); }
```

```sh
cc echo.c $(pkg-config --cflags --libs granitec) -o agent
./agent --describe
echo '{"perform":{"word":"echo","input":{"text":"hi"},"model":false}}' | ./agent
granite run ./agent - <<< '{"word":"echo","input":{"text":"hi"}}'
```

## Building

Needs a C11 compiler with pthreads, GNU make (3.81 or later), pkg-config and json-c 0.15 or later.

```sh
make                         # libgranitec.a and the granite command, under build/
make test                    # every package's tests (WERROR=1, SAN=address,undefined, SAN=thread)
make install PREFIX=/usr/local
scripts/check.sh             # the repository's one check: all three builds, the lint, an install that builds an agent
```

| | |
| --- | --- |
| `core/` | objects with strong and weak counts, the lock and its rule, the emitter, granite.main ([Docs/Kernel.md](Docs/Kernel.md)) |
| `agents/` | JSON as serde_json reads and writes it, errors, contexts, the run engine, packages, answers, the wire, the runner ([Docs/Agents.md](Docs/Agents.md)) |
| `host/` | the host's side of a call, the shelf, the installer: the `granite` command's own ([Docs/Hosting.md](Docs/Hosting.md)) |
| `jsonschema/` | the validator behind `granite check` |
| `cli/` | the `granite` command |
| `containers/` | the check image and `check.sh`, which checks a C agent repository in a container or natively ([Docs/Containers.md](Docs/Containers.md)) |
| `Schema/` | the standard and its JSON Schemas, verbatim from Granite |
| `tests/fixtures/` | the parrot (GraniteRust's twin), a scripted runner and a mirror for the host's and interop's tests, and an agent repository |

What GraniteC does differently from Swift and Rust, and why, is [Docs/Deviations.md](Docs/Deviations.md);
[PORTING.md](PORTING.md) maps every Swift and Rust file to its C home or says it was not ported.
