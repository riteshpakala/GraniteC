/* An agent that ships as a package of its own: a word, what it does, an input it declares, a run
 * that streams events, and an answer read off them. Swift Granite's GraniteAgentPackage and
 * GraniteRust's AgentPackage.
 *
 *     static granite_error *run(granite_context *context, const json_object *input) {
 *         json_object *answer = granite_answer_new();
 *         granite_json_set_string(answer, "said", granite_json_get_string(input, "text"));
 *         granite_deliver(context, answer);
 *         return NULL;
 *     }
 *     const granite_package parrot = {.word = "parrot", .summary = "Says it back.",
 *                                     .module = "Parrot", .input = &input, .run = run};
 *
 * PIN: COMPUTE IS MADE ON THE FIRST CALL, never when the process starts: a package that holds a
 * device or a model costs nothing until it is asked for, and --describe makes none. It is made
 * once per process; a compute that fails is tried again on the next call.
 * PIN: A RUN BLOCKS, ON ITS OWN THREAD. It streams with granite_deliver, checks
 * granite_context_is_cancelled, and returns NULL or why it did not answer. It touches no state. */
#ifndef GRANITEC_PACKAGE_H
#define GRANITEC_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "granitec/answer.h"
#include "granitec/context.h"
#include "granitec/error.h"
#include "granitec/json.h"
#include "granitec/schema.h"

/* `lanes` for a package with no bound on how many of its calls run at once. */
#define GRANITE_LANES_UNBOUNDED (-1)

typedef struct granite_package {
    /* The word a host calls it by: lowercase, one word (^[a-z][a-z0-9-]*$), unique among what a host runs. */
    const char *word;
    /* What it does, in a sentence a host shows a person or a model. */
    const char *summary;
    /* The module a host names it by beside its word. */
    const char *module;
    /* Its type, as Module.Type (NULL: module.word). */
    const char *type;
    /* How to use it well, in its own words (NULL: none). */
    const char *guide;
    /* How many calls run at once: 0 for two, GRANITE_LANES_UNBOUNDED for no bound. */
    int lanes;
    /* What it makes, one kind each, ended by a make with no kind (NULL: nothing). */
    const granite_make *makes;
    /* What its input takes (NULL: anything). */
    const granite_schema *input;
    /* Makes the compute every run of this process shares, on the first call (NULL: none). */
    granite_error *(*compute)(void **compute);
    /* The run, on its own thread: blocking, streaming events, returning NULL or why it did not answer. */
    granite_error *(*run)(granite_context *context, const json_object *input);
    /* The answer, read off the run's events, a new object (NULL: no answer). NULL: the last event. */
    json_object *(*output)(const json_object *events);
} granite_package;

/* The package's type as a host names it: `type`, or module.word. */
const char *granite_package_type(const granite_package *package, char *buffer, size_t size);
/* How many calls run at once, or -1 for no bound. */
int granite_package_lanes(const granite_package *package);

/* What --describe says of the package: its entry, as Rust's Entry::of writes it. */
json_object *granite_package_entry(const granite_package *package);
/* Every entry of a roster (NULL-terminated; the first package of a word wins), as an array. */
json_object *granite_roster_entries(const granite_package *const *roster);
/* The roster's package called `word`, or NULL. */
const granite_package *granite_roster_find(const granite_package *const *roster, const char *word);

/* One call, in this process: `input` (JSON bytes) read and checked against the package's schema,
 * the run hosted on the package's own engine and offered `host`, the answer checked, stamped and
 * written. NULL with the answer's canonical bytes in `*answer` (the caller frees them), or why the
 * call was not answered: an input or answer refusal, the run's own error, or a cancel. Setting
 * `cancel` (optional) cancels the call. Blocks; never call it from granite.main. */
granite_error *granite_package_perform(const granite_package *package, const char *input, size_t len,
                                       granite_host *host, granite_cancel *cancel, char **answer, size_t *answer_len);

#endif
