/* The messages of granite-agent/1. See granitec/wire.h.
 *
 * Reading follows GraniteRust's AgentWire::read: keys tried in Swift Granite's order, then each
 * message read as serde reads its struct. Fields are visited in key order (serde_json's map is
 * sorted) and the first that does not read is said; then the first required field missing, in the
 * order the struct declares them. Every sentence is prefixed with the message's key. */
#include "granitec/wire.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "granitec/buf.h"
#include "granitec/schema.h"

static const char *const KIND_NAMES[] = {"hello", "perform", "note", "model", "answer", "output", "failed", "cancel", "entries"};

const char *granite_wire_kind_name(granite_wire_kind kind) {
    return (unsigned)kind <= GRANITE_WIRE_ENTRIES ? KIND_NAMES[kind] : "unknown";
}

granite_wire *granite_wire_new(granite_wire_kind kind) {
    granite_wire *message = granite_calloc(1, sizeof *message);
    message->kind = kind;
    return message;
}

granite_wire *granite_wire_hello(int32_t pid, bool keeps_answers) {
    granite_wire *hello = granite_wire_new(GRANITE_WIRE_HELLO);
    hello->wire = granite_strdup(GRANITE_WIRE_VERSION);
    hello->has_pid = true;
    hello->pid = pid;
    if (keeps_answers) hello->answer_standard = granite_strdup("granite-answer/1");
    return hello;
}

granite_wire *granite_wire_note(const char *note) {
    granite_wire *message = granite_wire_new(GRANITE_WIRE_NOTE);
    message->note = granite_strdup(note);
    return message;
}

granite_wire *granite_wire_output(json_object *output) {
    granite_wire *message = granite_wire_new(GRANITE_WIRE_OUTPUT);
    message->output = output;
    return message;
}

granite_wire *granite_wire_failed(const granite_error *failure) {
    granite_wire *message = granite_wire_new(GRANITE_WIRE_FAILED);
    message->failure = failure->type == GRANITE_ERROR_CANCELLED ? granite_fault("%s", failure->said) : granite_error_copy(failure);
    return message;
}

granite_wire *granite_wire_cancel(void) {
    return granite_wire_new(GRANITE_WIRE_CANCEL);
}

void granite_wire_free(granite_wire *m) {
    if (!m) return;
    free(m->wire);
    free(m->answer_standard);
    free(m->word);
    granite_json_put(m->input);
    free(m->directory);
    free(m->ask);
    free(m->note);
    granite_json_put(m->schema);
    free(m->system);
    free(m->user);
    granite_json_put(m->object);
    free(m->text);
    free(m->error);
    granite_json_put(m->output);
    granite_error_free(m->failure);
    granite_json_put(m->entries);
    free(m);
}

/* --- writing ------------------------------------------------------------------------------- */

/* A borrowed tree set into a message being written: its count is raised, and lowered with it. */
static json_object *shared(const json_object *value) {
    return value ? json_object_get((json_object *)value) : NULL;
}

char *granite_wire_line(const granite_wire *m, size_t *len) {
    json_object *body = NULL, *line = json_object_new_object();
    switch (m->kind) {
    case GRANITE_WIRE_HELLO:
        body = json_object_new_object();
        granite_json_set_string(body, "wire", m->wire ? m->wire : GRANITE_WIRE_VERSION);
        if (m->has_pid) granite_json_set(body, "pid", json_object_new_int(m->pid));
        granite_json_set_string(body, "answer", m->answer_standard);
        break;
    case GRANITE_WIRE_PERFORM:
        body = json_object_new_object();
        granite_json_set_string(body, "word", m->word ? m->word : "");
        granite_json_set(body, "input", shared(m->input));
        granite_json_set_string(body, "directory", m->directory);
        granite_json_set_string(body, "ask", m->ask);
        granite_json_set(body, "model", json_object_new_boolean(m->model));
        break;
    case GRANITE_WIRE_NOTE: body = json_object_new_string(m->note ? m->note : ""); break;
    case GRANITE_WIRE_MODEL:
        body = json_object_new_object();
        granite_json_set(body, "id", json_object_new_uint64(m->id));
        granite_json_set_string(body, "kind", m->ask_kind == GRANITE_ASK_TEXT ? "text" : "object");
        if (m->schema) granite_json_set(body, "schema", shared(m->schema));
        granite_json_set_string(body, "system", m->system ? m->system : "");
        granite_json_set_string(body, "user", m->user ? m->user : "");
        granite_json_set(body, "maxTokens", json_object_new_int64(m->max_tokens));
        granite_json_set(body, "seconds", granite_json_number(m->seconds));
        break;
    case GRANITE_WIRE_ANSWER:
        body = json_object_new_object();
        granite_json_set(body, "id", json_object_new_uint64(m->id));
        if (m->object) granite_json_set(body, "object", shared(m->object));
        granite_json_set_string(body, "text", m->text);
        granite_json_set_string(body, "error", m->error);
        break;
    case GRANITE_WIRE_OUTPUT: body = shared(m->output); break;
    case GRANITE_WIRE_FAILED: {
        const granite_error *f = m->failure;
        if (f && f->type == GRANITE_ERROR_REFUSED) {
            json_object *refused = json_object_new_object(), *reasons = json_object_new_array();
            granite_json_set_string(refused, "kind", granite_refusal_kind_name(f->kind));
            for (size_t i = 0; i < f->reason_count; i++) json_object_array_add(reasons, json_object_new_string(f->reasons[i]));
            granite_json_set(refused, "reasons", reasons);
            granite_json_set(line, "refused", refused);
        }
        body = json_object_new_string(f && f->said ? f->said : "");
        break;
    }
    case GRANITE_WIRE_CANCEL: body = json_object_new_object(); break;
    case GRANITE_WIRE_ENTRIES: body = m->entries ? shared(m->entries) : json_object_new_array(); break;
    }
    granite_json_set(line, granite_wire_kind_name(m->kind), body);
    granite_buf text = {0};
    granite_json_append(&text, line);
    granite_buf_append_char(&text, '\n');
    granite_json_put(line);
    if (len) *len = text.len;
    return granite_buf_take(&text);
}

/* --- reading ------------------------------------------------------------------------------- */

typedef enum field_type {
    STRING,
    OPTIONAL_STRING,
    BOOLEAN,
    U64,
    U32,
    OPTIONAL_I32,
    F64,
    VALUE,
    OPTIONAL_VALUE,
    ASK_KIND,
    ENTRY_KIND,
    OPTIONAL_U32,
    OPTIONAL_BOOLEAN,
} field_type;

typedef struct field_spec {
    const char *name;
    field_type type;
    void *out;
    bool *present;
} field_spec;

static char *invalid_type(const json_object *value, const char *expected) {
    char *unexpected = granite_json_unexpected(value);
    char *said = granite_strdupf("invalid type: %s, expected %s", unexpected, expected);
    free(unexpected);
    return said;
}

static char *invalid_value(const json_object *value, const char *expected) {
    char *unexpected = granite_json_unexpected(value);
    char *said = granite_strdupf("invalid value: %s, expected %s", unexpected, expected);
    free(unexpected);
    return said;
}

static bool is_negative_integer(const json_object *value) {
    return granite_json_is_integer(value) && json_object_get_int64((json_object *)value) < 0;
}

static char *read_unsigned(const json_object *value, const char *expected, uint64_t max, uint64_t *out) {
    if (!granite_json_is_integer(value)) return invalid_type(value, expected);
    if (is_negative_integer(value)) return invalid_value(value, expected);
    int64_t i = json_object_get_int64((json_object *)value);
    uint64_t u = i == INT64_MAX ? json_object_get_uint64((json_object *)value) : (uint64_t)i;
    if (u > max) return invalid_value(value, expected);
    *out = u;
    return NULL;
}

static char *read_variant(const json_object *value, const char *a, const char *b, int *out) {
    if (json_object_get_type((json_object *)value) != json_type_string) return invalid_type(value, "string or map");
    const char *got = json_object_get_string((json_object *)value);
    if (strcmp(got, a) == 0) *out = 0;
    else if (strcmp(got, b) == 0) *out = 1;
    else return granite_strdupf("unknown variant `%s`, expected `%s` or `%s`", got, a, b);
    return NULL;
}

/* One field, read into its out. */
static char *read_field(const field_spec *f, json_object *value) {
    json_type type = json_object_get_type(value);
    uint64_t u;
    switch (f->type) {
    case STRING:
    case OPTIONAL_STRING:
        if (f->type == OPTIONAL_STRING && !value) return NULL;
        if (type != json_type_string) return invalid_type(value, "a string");
        *(char **)f->out = granite_strdup(json_object_get_string(value));
        break;
    case BOOLEAN:
    case OPTIONAL_BOOLEAN:
        if (f->type == OPTIONAL_BOOLEAN && !value) return NULL;
        if (type != json_type_boolean) return invalid_type(value, "a boolean");
        if (f->out) *(bool *)f->out = json_object_get_boolean(value);
        break;
    case U64: {
        char *said = read_unsigned(value, "u64", UINT64_MAX, &u);
        if (said) return said;
        *(uint64_t *)f->out = u;
        break;
    }
    case U32:
    case OPTIONAL_U32: {
        if (f->type == OPTIONAL_U32 && !value) return NULL;
        char *said = read_unsigned(value, "u32", UINT32_MAX, &u);
        if (said) return said;
        if (f->out) *(uint32_t *)f->out = (uint32_t)u;
        break;
    }
    case OPTIONAL_I32: {
        if (!value) return NULL;
        if (!granite_json_is_integer(value)) return invalid_type(value, "i32");
        int64_t i = json_object_get_int64(value);
        if (i == INT64_MAX || i > INT32_MAX || i < INT32_MIN) return invalid_value(value, "i32");
        *(int32_t *)f->out = (int32_t)i;
        break;
    }
    case F64:
        if (!granite_json_is_number(value)) return invalid_type(value, "f64");
        *(double *)f->out = json_object_get_double(value);
        break;
    case VALUE:
    case OPTIONAL_VALUE:
        if (f->type == OPTIONAL_VALUE && !value) return NULL;
        if (f->out) *(json_object **)f->out = value ? json_object_get(value) : NULL;
        break;
    case ASK_KIND: {
        int which;
        char *said = read_variant(value, "object", "text", &which);
        if (said) return said;
        *(granite_ask_kind *)f->out = which ? GRANITE_ASK_TEXT : GRANITE_ASK_OBJECT;
        break;
    }
    case ENTRY_KIND: {
        int which;
        return read_variant(value, "package", "worker", &which);
    }
    }
    if (f->present) *f->present = true;
    return NULL;
}

static bool optional(field_type type) {
    return type == OPTIONAL_STRING || type == OPTIONAL_I32 || type == OPTIONAL_VALUE || type == OPTIONAL_U32 ||
           type == OPTIONAL_BOOLEAN;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Reads `value` as serde reads struct `name` from a map: NULL, or why it does not read. */
static char *read_struct(json_object *value, const char *name, const field_spec *fields, size_t count) {
    if (json_object_get_type(value) != json_type_object) {
        char *expected = granite_strdupf("struct %s", name);
        char *said = invalid_type(value, expected);
        free(expected);
        return said;
    }
    size_t n = (size_t)json_object_object_length(value), k = 0;
    const char **keys = granite_malloc((n ? n : 1) * sizeof *keys);
    json_object_object_foreach(value, key, unused) {
        (void)unused;
        keys[k++] = key;
    }
    qsort(keys, k, sizeof *keys, compare_names);
    char *said = NULL;
    for (size_t i = 0; i < k && !said; i++)
        for (size_t f = 0; f < count; f++)
            if (strcmp(fields[f].name, keys[i]) == 0) {
                said = read_field(&fields[f], granite_json_get(value, keys[i]));
                break;
            }
    free(keys);
    if (said) return said;
    for (size_t f = 0; f < count; f++)
        if (!optional(fields[f].type) && !granite_json_has(value, fields[f].name))
            return granite_strdupf("missing field `%s`", fields[f].name);
    return NULL;
}

static char *read_entries(json_object *value, json_object **out) {
    if (json_object_get_type(value) != json_type_array) return invalid_type(value, "a sequence");
    for (size_t i = 0; i < json_object_array_length(value); i++) {
        char *ignored = NULL;
        const field_spec fields[] = {
            {"kind", ENTRY_KIND, NULL, NULL},       {"word", STRING, &ignored, NULL},
            {"summary", STRING, &ignored, NULL},    {"module", STRING, &ignored, NULL},
            {"type", STRING, &ignored, NULL},       {"schema", OPTIONAL_VALUE, NULL, NULL},
            {"fields", OPTIONAL_VALUE, NULL, NULL}, {"lanes", OPTIONAL_U32, NULL, NULL},
            {"guide", OPTIONAL_STRING, &ignored, NULL}, {"makes", OPTIONAL_VALUE, NULL, NULL},
            {"toolNames", OPTIONAL_VALUE, NULL, NULL}, {"budget", OPTIONAL_VALUE, NULL, NULL},
            {"shell", OPTIONAL_BOOLEAN, NULL, NULL}, {"reportRule", OPTIONAL_STRING, &ignored, NULL},
        };
        /* Each string field is read into the same place and let go: only whether it reads matters. */
        char *said = NULL;
        json_object *entry = json_object_array_get_idx(value, i);
        if (json_object_get_type(entry) != json_type_object) {
            said = invalid_type(entry, "struct Entry");
        } else {
            size_t n = (size_t)json_object_object_length(entry), k = 0;
            const char **keys = granite_malloc((n ? n : 1) * sizeof *keys);
            json_object_object_foreach(entry, key, unused) {
                (void)unused;
                keys[k++] = key;
            }
            qsort(keys, k, sizeof *keys, compare_names);
            for (size_t j = 0; j < k && !said; j++)
                for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++)
                    if (strcmp(fields[f].name, keys[j]) == 0) {
                        said = read_field(&fields[f], granite_json_get(entry, keys[j]));
                        free(ignored);
                        ignored = NULL;
                        break;
                    }
            free(keys);
            for (size_t f = 0; !said && f < sizeof fields / sizeof fields[0]; f++)
                if (!optional(fields[f].type) && !granite_json_has(entry, fields[f].name))
                    said = granite_strdupf("missing field `%s`", fields[f].name);
        }
        if (said) return said;
    }
    *out = json_object_get(value);
    return NULL;
}

/* A refusal as it crosses the wire, or NULL when it does not read (and is dropped). */
static granite_error *read_refusal(json_object *refused, const char *said) {
    if (json_object_get_type(refused) != json_type_object) return NULL;
    json_object *kind = granite_json_get(refused, "kind"), *reasons = granite_json_get(refused, "reasons");
    if (json_object_get_type(kind) != json_type_string || json_object_get_type(reasons) != json_type_array) return NULL;
    size_t n = json_object_array_length(reasons);
    const char **texts = granite_calloc(n ? n : 1, sizeof *texts);
    for (size_t i = 0; i < n; i++) {
        json_object *reason = json_object_array_get_idx(reasons, i);
        if (json_object_get_type(reason) != json_type_string) {
            free(texts);
            return NULL;
        }
        texts[i] = json_object_get_string(reason);
    }
    granite_error *refusal = granite_refuse_said(granite_refusal_kind_named(json_object_get_string(kind)), said, texts, n);
    free(texts);
    return refusal;
}

static bool present(json_object *map, const char *key) {
    return granite_json_get(map, key) != NULL;
}

/* Reads the message under `key` with `fields`, prefixing a sentence with the key. */
static char *take(json_object *map, const char *key, const char *name, const field_spec *fields, size_t count) {
    char *said = read_struct(granite_json_get(map, key), name, fields, count);
    if (!said) return NULL;
    char *prefixed = granite_strdupf("`%s`: %s", key, said);
    free(said);
    return prefixed;
}

#define FIELDS(...) ((const field_spec[]){__VA_ARGS__}), (sizeof((const field_spec[]){__VA_ARGS__}) / sizeof(field_spec))

granite_wire *granite_wire_read(const char *line, size_t len, char **error) {
    *error = NULL;
    while (len && (line[len - 1] == '\n')) len--;
    json_object *map = NULL;
    if (!granite_json_read(line, len, &map, error)) return NULL;
    granite_wire *m = NULL;
    char *said = NULL;
    if (json_object_get_type(map) != json_type_object) {
        said = granite_strdup("a message is a JSON object");
    } else if (present(map, "hello")) {
        m = granite_wire_new(GRANITE_WIRE_HELLO);
        said = take(map, "hello", "Hello",
                    FIELDS({"wire", STRING, &m->wire, NULL}, {"pid", OPTIONAL_I32, &m->pid, &m->has_pid},
                           {"answer", OPTIONAL_STRING, &m->answer_standard, NULL}));
    } else if (present(map, "perform")) {
        m = granite_wire_new(GRANITE_WIRE_PERFORM);
        said = take(map, "perform", "Perform",
                    FIELDS({"word", STRING, &m->word, NULL}, {"input", VALUE, &m->input, NULL},
                           {"directory", OPTIONAL_STRING, &m->directory, NULL}, {"ask", OPTIONAL_STRING, &m->ask, NULL},
                           {"model", BOOLEAN, &m->model, NULL}));
    } else if (present(map, "note")) {
        json_object *note = granite_json_get(map, "note");
        if (json_object_get_type(note) == json_type_string) {
            m = granite_wire_note(json_object_get_string(note));
        } else {
            char *why = invalid_type(note, "a string");
            said = granite_strdupf("`note`: %s", why);
            free(why);
        }
    } else if (present(map, "model")) {
        m = granite_wire_new(GRANITE_WIRE_MODEL);
        said = take(map, "model", "ModelAsk",
                    FIELDS({"id", U64, &m->id, NULL}, {"kind", ASK_KIND, &m->ask_kind, NULL},
                           {"schema", OPTIONAL_VALUE, &m->schema, NULL}, {"system", STRING, &m->system, NULL},
                           {"user", STRING, &m->user, NULL}, {"maxTokens", U32, &m->max_tokens, NULL},
                           {"seconds", F64, &m->seconds, NULL}));
    } else if (present(map, "answer")) {
        m = granite_wire_new(GRANITE_WIRE_ANSWER);
        said = take(map, "answer", "ModelAnswer",
                    FIELDS({"id", U64, &m->id, NULL}, {"object", OPTIONAL_VALUE, &m->object, NULL},
                           {"text", OPTIONAL_STRING, &m->text, NULL}, {"error", OPTIONAL_STRING, &m->error, NULL}));
    } else if (granite_json_has(map, "output")) {
        json_object *output = granite_json_get(map, "output");
        m = granite_wire_output(output ? json_object_get(output) : NULL);
    } else if (present(map, "failed")) {
        json_object *failed = granite_json_get(map, "failed");
        if (json_object_get_type(failed) != json_type_string) {
            char *why = invalid_type(failed, "a string");
            said = granite_strdupf("`failed`: %s", why);
            free(why);
        } else {
            m = granite_wire_new(GRANITE_WIRE_FAILED);
            const char *sentence = json_object_get_string(failed);
            m->failure = read_refusal(granite_json_get(map, "refused"), sentence);
            if (!m->failure) m->failure = granite_fault("%s", sentence);
        }
    } else if (granite_json_has(map, "cancel")) {
        m = granite_wire_cancel();
    } else if (present(map, "entries")) {
        m = granite_wire_new(GRANITE_WIRE_ENTRIES);
        char *why = read_entries(granite_json_get(map, "entries"), &m->entries);
        if (why) {
            said = granite_strdupf("`entries`: %s", why);
            free(why);
        }
    } else {
        said = granite_strdup("a message is one of hello, perform, note, model, answer, output, failed, cancel, entries");
    }
    granite_json_put(map);
    if (said) {
        granite_wire_free(m);
        *error = said;
        return NULL;
    }
    return m;
}
