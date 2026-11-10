/* The JSON Schema behind `granite check`: the subset of draft 2020-12 the standard's schemas use,
 * read the way GraniteRust's check reads them (jsonschema-rs 0.26), with its sentences.
 *
 * Keywords: $ref (within a document, and to a registered $id), $defs, $id, type (and unions),
 * properties, required, items, additionalProperties, oneOf, allOf, if/then/else, enum, const,
 * pattern (POSIX extended), minimum. format, title, description, examples and $schema say nothing
 * here, as in Rust. A schema that uses any other keyword is caught by the census test.
 *
 * PIN: THE SAME PROBLEMS, IN THE SAME ORDER. A schema's keywords are visited in key order (a
 * serde_json map is sorted), each problem said as "{sentence} at {instance path}". */
#ifndef GRANITE_JSONSCHEMA_VALIDATE_H
#define GRANITE_JSONSCHEMA_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>

#include "granitec/json.h"

typedef struct granite_schemas granite_schemas;

granite_schemas *granite_schemas_new(void);
void granite_schemas_free(granite_schemas *schemas);
/* Registers every *.json in `folder` that has an $id, by that $id and by its file name. False, with
 * why in `*error`, when the folder cannot be read. */
bool granite_schemas_add_folder(granite_schemas *schemas, const char *folder, char **error);
/* Registers `document` (taken) by `name`, and by its $id when it has one. */
void granite_schemas_add(granite_schemas *schemas, const char *name, json_object *document);
/* The schema registered as `name` ($id or file name), borrowed; NULL when none. */
const json_object *granite_schemas_find(const granite_schemas *schemas, const char *name);

/* Every way `instance` breaks `schema`, one "{sentence} at {path}" each; `*count` of them (the
 * caller frees each and the array). */
char **granite_schema_validate(const granite_schemas *schemas, const json_object *schema, const json_object *instance,
                               size_t *count);

/* The keywords this validator reads, and those it lets pass as annotations: what the census holds
 * every schema to. NULL-terminated. */
extern const char *const granite_jsonschema_keywords[];

#endif
