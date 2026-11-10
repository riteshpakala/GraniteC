/* The messages of granite-agent/1: JSON objects, one per line, each named by its one key.
 *
 *   runner → host   {"hello":{"answer":"granite-answer/1","pid":4512,"wire":"granite-agent/1"}}
 *   host → runner   {"perform":{"ask":"…","directory":"/p","input":{…},"model":true,"word":"plate"}}
 *   runner → host   {"note":"round 1: measured clean"}
 *   runner → host   {"model":{"id":1,"kind":"object","maxTokens":3000,"schema":{…},"seconds":300,"system":"…","user":"…"}}
 *   host → runner   {"answer":{"id":1,"object":{…}}}
 *   runner → host   {"output":{…}}   or   {"failed":"a sentence","refused":{"kind":"call","reasons":[…]}}
 *   host → runner   {"cancel":{}}
 *   runner → host   {"entries":[…]}  (only with --describe)
 *
 * PIN: UNKNOWN FIELDS ARE IGNORED, UNKNOWN MESSAGES ARE NOT. A line whose one key is none of these
 * does not read; a reader drops it and says so.
 * PIN: THE SAME SENTENCES AS RUST. A line that does not read says why as GraniteRust's
 * AgentWire::read does: serde's words for the JSON and for each message's fields. */
#ifndef GRANITEC_WIRE_H
#define GRANITEC_WIRE_H

#include <stdbool.h>
#include <stdint.h>

#include "granitec/error.h"
#include "granitec/json.h"

#define GRANITE_WIRE_VERSION "granite-agent/1"
#define GRANITE_WIRE_MAJOR 1

typedef enum granite_wire_kind {
    GRANITE_WIRE_HELLO,
    GRANITE_WIRE_PERFORM,
    GRANITE_WIRE_NOTE,
    GRANITE_WIRE_MODEL,
    GRANITE_WIRE_ANSWER,
    GRANITE_WIRE_OUTPUT,
    GRANITE_WIRE_FAILED,
    GRANITE_WIRE_CANCEL,
    GRANITE_WIRE_ENTRIES,
} granite_wire_kind;

typedef enum granite_ask_kind { GRANITE_ASK_OBJECT, GRANITE_ASK_TEXT } granite_ask_kind;

/* One message. Only the fields of its kind are used; what it holds it owns. */
typedef struct granite_wire {
    granite_wire_kind kind;
    /* hello */
    char *wire;
    bool has_pid;
    int32_t pid;
    char *answer_standard;
    /* perform */
    char *word;
    json_object *input;
    char *directory;
    char *ask;
    bool model;
    /* note */
    char *note;
    /* model, and answer's id */
    uint64_t id;
    granite_ask_kind ask_kind;
    json_object *schema;
    char *system;
    char *user;
    uint32_t max_tokens;
    double seconds;
    /* answer */
    json_object *object; /* NULL: none */
    char *text;
    char *error;
    /* output */
    json_object *output;
    /* failed: said as the error's sentence; a refusal when it is one */
    granite_error *failure;
    /* entries: an array of entry objects */
    json_object *entries;
} granite_wire;

/* "hello", "perform", …: the message's key. */
const char *granite_wire_kind_name(granite_wire_kind kind);

granite_wire *granite_wire_new(granite_wire_kind kind);
/* The runner's first message: this wire, `pid`, and the answer standard. */
granite_wire *granite_wire_hello(int32_t pid, bool keeps_answers);
granite_wire *granite_wire_note(const char *note);
granite_wire *granite_wire_output(json_object *output);
/* A failed call: its sentence and, for a refusal, its kind and reasons (copied). A cancel is said
 * as "cancelled", with no refusal. */
granite_wire *granite_wire_failed(const granite_error *failure);
granite_wire *granite_wire_cancel(void);
void granite_wire_free(granite_wire *message);

/* The message as one line of sorted JSON, newline included. The caller frees it. */
char *granite_wire_line(const granite_wire *message, size_t *len);
/* The message one line holds (without or with its newline), or NULL with why in `*error`. Keys are
 * tried in Swift Granite's order; a field a reader does not know is ignored, and a malformed
 * `refused` is dropped, leaving the sentence. */
granite_wire *granite_wire_read(const char *line, size_t len, char **error);

#endif
