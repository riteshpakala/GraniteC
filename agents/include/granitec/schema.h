/* What an input takes: each field's name and kind, whether it must be there, and for a field with a
 * fixed set of values, the values. GraniteRust's GraniteSchema, declared rather than read off a type:
 *
 *     static const granite_field fields[] = {
 *         {"text", {GRANITE_SCHEMA_STRING}, true, "what to say back"},
 *         {"times", {GRANITE_SCHEMA_INTEGER}, false, NULL},
 *         {NULL},
 *     };
 *     static const granite_schema input = {GRANITE_SCHEMA_OBJECT, .fields = fields};
 *
 * PIN: NOT JSON SCHEMA. The data form (granite_schema_value) crosses processes as Rust writes it
 * ("string", {"object":[…]}, "any", field order kept); granite_schema_json_schema is what a model
 * is shown.
 * PIN: A FIELD NAMED `directory` IS THE HOST'S. A host fills it with the folder it works in and
 * does not show it to a model. */
#ifndef GRANITEC_SCHEMA_H
#define GRANITEC_SCHEMA_H

#include <stdbool.h>

#include "granitec/json.h"

typedef enum granite_schema_type {
    GRANITE_SCHEMA_ANY, /* zero: a schema left out takes anything */
    GRANITE_SCHEMA_STRING,
    GRANITE_SCHEMA_INTEGER,
    GRANITE_SCHEMA_NUMBER,
    GRANITE_SCHEMA_BOOLEAN,
    GRANITE_SCHEMA_CHOICE, /* one of `choices`, a NULL-terminated list of strings */
    GRANITE_SCHEMA_ARRAY,  /* a list of `items` */
    GRANITE_SCHEMA_OBJECT, /* `fields`, ended by a field with no name */
} granite_schema_type;

typedef struct granite_field granite_field;

typedef struct granite_schema {
    granite_schema_type type;
    const char *const *choices;
    const struct granite_schema *items;
    const granite_field *fields;
} granite_schema;

struct granite_field {
    const char *name;
    granite_schema schema;
    bool required;
    /* The line said beside the field (Rust's `fields`), or NULL. */
    const char *note;
};

/* The field a host fills with its folder. */
#define GRANITE_DIRECTORY_FIELD "directory"

/* The data form: "string", {"choice":[…]}, {"array":…}, {"object":[{"name","required","schema"}]}. */
json_object *granite_schema_value(const granite_schema *schema);
/* As JSON Schema, each field's note said as its description; with `hide_directory`, the host's
 * directory field left out. */
json_object *granite_schema_json_schema(const granite_schema *schema, bool hide_directory);
/* The notes of an object's fields, as Rust's `fields` map ({} when none). */
json_object *granite_schema_notes(const granite_schema *schema);
/* Whether the schema has the host's directory field. */
bool granite_schema_takes_directory(const granite_schema *schema);
/* Whether `value` reads as `schema`: NULL when it does, else what serde would say of it ("missing
 * field `text`", "invalid type: integer `5`, expected a string"), for the caller to free. Unknown
 * fields are ignored and a field that is not required may be null, as serde reads an Option. */
char *granite_schema_check(const granite_schema *schema, const json_object *value);

/* How serde names a JSON value it did not expect: "integer `5`", "string \"x\"", "map", … */
char *granite_json_unexpected(const json_object *value);

#endif
